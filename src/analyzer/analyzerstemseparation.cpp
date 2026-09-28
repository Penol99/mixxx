#include "analyzer/analyzerstemseparation.h"

#ifdef __STEM__

#include <algorithm>

#include "analyzer/analyzertrack.h"
#include "track/track.h"
#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("AnalyzerStemSeparation");

// ponytail: single config flag, no per-model settings yet. Add a Stems
// preferences pane when a second model or execution-provider choice ships.
constexpr char kConfigGroup[] = "[Library]";
constexpr char kConfigKey[] = "StemSeparationEnabled";
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
    return true;
}

bool AnalyzerStemSeparation::processSamples(const CSAMPLE* pIn, SINT count) {
    Q_UNUSED(pIn);
    // ponytail: real impl buffers/streams audio into the Demucs session here.
    // The stub only tracks length so progress + storeResults are honest.
    m_framesProcessed += count / std::max<int>(m_channelCount.value(), 1);
    return true;
}

void AnalyzerStemSeparation::storeResults(TrackPointer pTrack) {
    // ponytail: STUB. Replace with Demucs ONNX inference -> 4 stem buffers
    // [vocals, drums, bass, other] -> .stem.mp4 writer -> cache keyed by track
    // hash, then mark the track stem-capable so the existing stem-load path
    // (EngineDeck/SoundSourceSTEM) picks it up. See epic mixxxdj/mixxx#15495.
    kLogger.info() << "Stem separation (stub) would produce 4 stems for"
                   << pTrack->getLocation() << "-" << m_framesProcessed
                   << "frames @" << m_sampleRate << "Hz";
}

void AnalyzerStemSeparation::cleanup() {
    m_framesProcessed = 0;
}

#endif // __STEM__
