#include "analyzer/analyzerstemseparation.h"

#ifdef __STEM__

#include <algorithm>
#include <cmath>

#include "analyzer/analyzertrack.h"
#include "track/track.h"
#include "util/logger.h"

#ifdef __STEMSEP_ONNX__
#include "analyzer/stemseparator.h"
#endif

namespace {
const mixxx::Logger kLogger("AnalyzerStemSeparation");

// ponytail: single config flag, no per-model settings yet. Add a Stems
// preferences pane when a second model or execution-provider choice ships.
constexpr char kConfigGroup[] = "[Library]";
constexpr char kConfigKey[] = "StemSeparationEnabled";
constexpr char kModelPathKey[] = "StemSeparationModelPath";
} // anonymous namespace

AnalyzerStemSeparation::AnalyzerStemSeparation(UserSettingsPointer pConfig)
        : m_pConfig(pConfig),
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
    // Demucs requires 44.1 kHz stereo. ponytail: skip (don't guess) when the
    // decoded signal doesn't match; wire a resampler here when we support
    // arbitrary sample rates / channel counts.
    if (m_channelCount.value() != 2 ||
            m_sampleRate.value() != StemSeparator::kModelSampleRate) {
        kLogger.warning() << "Skipping" << pTrack->getLocation()
                          << "- need 44.1 kHz stereo, got" << m_sampleRate
                          << "Hz /" << m_channelCount.value() << "ch";
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

    const auto stems = separator.separate(m_samples);
    if (stems.size() != StemSeparator::kNumStems) {
        kLogger.warning() << "Separation produced no stems for" << pTrack->getLocation();
        return;
    }

    // ponytail: STUB output. Real next step: encode these 4 stem buffers into a
    // native .stem.mp4 via the writer, cache it keyed by track hash, and mark
    // the track stem-capable so EngineDeck/SoundSourceSTEM loads it. Logging RMS
    // here proves inference actually ran end to end.
    for (int s = 0; s < StemSeparator::kNumStems; ++s) {
        double sumSq = 0.0;
        for (const CSAMPLE v : stems[s]) {
            sumSq += static_cast<double>(v) * v;
        }
        const double rms = stems[s].empty() ? 0.0 : std::sqrt(sumSq / stems[s].size());
        kLogger.info() << "Stem" << s << "RMS" << rms << "for" << pTrack->getLocation();
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
