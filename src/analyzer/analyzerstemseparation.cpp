#include "analyzer/analyzerstemseparation.h"

#ifdef __STEM__

#include <algorithm>
#include <cmath>

#include "analyzer/analyzertrack.h"
#include "track/track.h"
#include "util/logger.h"

#ifdef __STEMSEP_ONNX__
#include <QCryptographicHash>
#include <QDir>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

#include "analyzer/stemfilewriter.h"
#include "analyzer/stemseparator.h"
#include "control/controlobject.h"
#include "util/cmdlineargs.h"
#endif

namespace {
const mixxx::Logger kLogger("AnalyzerStemSeparation");

#ifdef __STEMSEP_ONNX__
// Resample interleaved float audio to the model's 44.1 kHz stereo format.
// Returns empty on failure; returns the input unchanged when already correct.
std::vector<CSAMPLE> resampleToStemFormat(
        const std::vector<CSAMPLE>& in, int srcRate, int srcChannels) {
    if (srcRate == StemSeparator::kModelSampleRate && srcChannels == 2) {
        return in;
    }
    SwrContext* swr = nullptr;
    AVChannelLayout inLayout;
    AVChannelLayout outLayout;
    av_channel_layout_default(&inLayout, srcChannels);
    av_channel_layout_default(&outLayout, 2);
    if (swr_alloc_set_opts2(&swr,
                &outLayout,
                AV_SAMPLE_FMT_FLT,
                StemSeparator::kModelSampleRate,
                &inLayout,
                AV_SAMPLE_FMT_FLT,
                srcRate,
                0,
                nullptr) < 0 ||
            swr_init(swr) < 0) {
        if (swr) {
            swr_free(&swr);
        }
        return {};
    }
    const int inFrames = static_cast<int>(in.size() / srcChannels);
    const int64_t outFramesEst = av_rescale_rnd(
            swr_get_delay(swr, srcRate) + inFrames,
            StemSeparator::kModelSampleRate,
            srcRate,
            AV_ROUND_UP);
    std::vector<CSAMPLE> out(static_cast<size_t>(outFramesEst) * 2);
    const uint8_t* inPtr = reinterpret_cast<const uint8_t*>(in.data());
    uint8_t* outPtr = reinterpret_cast<uint8_t*>(out.data());
    const int outFrames = swr_convert(
            swr, &outPtr, static_cast<int>(outFramesEst), &inPtr, inFrames);
    swr_free(&swr);
    if (outFrames < 0) {
        return {};
    }
    out.resize(static_cast<size_t>(outFrames) * 2);
    return out;
}
#endif

// ponytail: single config flag, no per-model settings yet. Add a Stems
// preferences pane when a second model or execution-provider choice ships.
constexpr char kConfigGroup[] = "[Library]";
constexpr char kConfigKey[] = "StemSeparationEnabled";
constexpr char kModelPathKey[] = "StemSeparationModelPath";
constexpr char kFfmpegPathKey[] = "StemSeparationFfmpegPath";
#ifdef __STEMSEP_ONNX__
// Global 0-100 readout of the running separation (created by AnalysisFeature).
const ConfigKey kProgressKey("[Library]", "stem_separation_progress");
#endif
} // anonymous namespace

AnalyzerStemSeparation::AnalyzerStemSeparation(UserSettingsPointer pConfig,
        std::function<void(AnalyzerProgress)> progressCb)
        : m_pConfig(pConfig),
          m_progressCb(std::move(progressCb)),
          m_framesProcessed(0) {
}

bool AnalyzerStemSeparation::isEnabled(const UserSettingsPointer& pConfig) {
    return pConfig->getValue(ConfigKey(kConfigGroup, kConfigKey), false);
}

bool AnalyzerStemSeparation::initialize(const AnalyzerTrack& track,
        mixxx::audio::SampleRate sampleRate,
        mixxx::audio::ChannelCount channelCount,
        SINT frameLength) {
    Q_UNUSED(frameLength);

    // Run only when the user asked: globally (separate-on-import) or per-track
    // via the "Separate stems" menu action. Keeps this off the hot path for
    // ordinary analysis.
    if (!isEnabled(m_pConfig) && !track.getOptions().separateStems) {
        return false;
    }

    // Never separate a track that is already stems.
    if (!track.getTrack()->getStemInfo().isEmpty()) {
        return false;
    }

    m_sampleRate = sampleRate;
    m_channelCount = channelCount;
    m_framesProcessed = 0;
    m_samples.clear();
    return true;
}

bool AnalyzerStemSeparation::processSamples(const CSAMPLE* pIn, SINT count) {
    m_framesProcessed += count / std::max<int>(m_channelCount.value(), 1);
    // Offline separation needs the whole track, so buffer it. Reserve lazily on
    // first call to avoid repeated reallocations.
    m_samples.insert(m_samples.end(), pIn, pIn + count);
    return true;
}

void AnalyzerStemSeparation::storeResults(TrackPointer pTrack) {
#ifdef __STEMSEP_ONNX__
    // Demucs requires 44.1 kHz stereo; resample whatever the decoder gave us.
    if (m_channelCount.value() < 1) {
        return;
    }
    const std::vector<CSAMPLE> samples = resampleToStemFormat(
            m_samples, m_sampleRate.value(), m_channelCount.value());
    if (samples.empty()) {
        kLogger.warning() << "Resampling failed for" << pTrack->getLocation();
        return;
    }

    const QString modelPath =
            m_pConfig->getValue(ConfigKey(kConfigGroup, kModelPathKey), QString());
    if (modelPath.isEmpty()) {
        kLogger.warning() << "No stem separation model configured ("
                          << kConfigGroup << kModelPathKey << ")";
        return;
    }

    StemSeparator separator;
    if (!separator.load(modelPath)) {
        return; // load() already logged
    }

    // Map the inference's 0..1 chunk progress into the analyzer's finalizing
    // band (95..100%) so the existing progress bar advances during inference,
    // and publish a 0..100 readout for the skin.
    ControlObject::set(kProgressKey, 1.0); // >0 so the readout appears immediately
    const auto stems = separator.separate(samples, [this](double frac) {
        ControlObject::set(kProgressKey, frac * 100.0);
        if (m_progressCb) {
            // Busy progress must stay strictly below Done (asserted by the
            // scheduler); the final Done is emitted by AnalyzerThread.
            m_progressCb(std::min(kAnalyzerProgressDone - 0.001,
                    kAnalyzerProgressFinalizing +
                            frac * (kAnalyzerProgressDone -
                                           kAnalyzerProgressFinalizing)));
        }
    });
    ControlObject::set(kProgressKey, 0.0); // hide the readout when finished
    if (stems.size() != StemSeparator::kNumStems) {
        kLogger.warning() << "Separation produced no stems for" << pTrack->getLocation();
        return;
    }

    // Cache the result as a playable .stem.mp4 keyed by the source path, so
    // reloads are instant and the user can load it to hear stems immediately.
    const QString cacheDir =
            QDir(CmdlineArgs::Instance().getSettingsPath()).filePath("stems");
    QDir().mkpath(cacheDir);
    const QString hash = QString::fromLatin1(
            QCryptographicHash::hash(pTrack->getLocation().toUtf8(),
                    QCryptographicHash::Sha1)
                    .toHex());
    const QString outPath = QDir(cacheDir).filePath(hash + ".stem.mp4");

    const QString ffmpegPath = m_pConfig->getValue(
            ConfigKey(kConfigGroup, kFfmpegPathKey), QStringLiteral("ffmpeg"));

    if (StemFileWriter::write(
                outPath, stems, StemSeparator::kModelSampleRate, ffmpegPath)) {
        kLogger.info() << "Stems ready:" << outPath
                       << "(load this file to play stems)";
        // ponytail: next step is auto-loading this onto the requesting deck.
        // For now the cached file is discoverable and manually loadable.
    }
#else
    kLogger.info() << "Stem separation stub (built without ONNX) for"
                   << pTrack->getLocation() << "-" << m_framesProcessed
                   << "frames @" << m_sampleRate << "Hz";
#endif
}

void AnalyzerStemSeparation::cleanup() {
    m_framesProcessed = 0;
    m_samples.clear();
    m_samples.shrink_to_fit();
}

#endif // __STEM__
