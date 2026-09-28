#pragma once

#ifdef __STEM__

#include "analyzer/analyzer.h"
#include "preferences/usersettings.h"

/// Offline AI stem separation (vocals/drums/bass/other).
///
/// ponytail: STUB. This slice wires the separation stage into the existing
/// analysis pipeline (opt-in, skips tracks that are already stems) and proves
/// the plumbing end to end. Real Demucs/ONNX inference + .stem.mp4 writing land
/// in the next step, replacing the body of storeResults(). See epic
/// mixxxdj/mixxx#15495.
class AnalyzerStemSeparation : public Analyzer {
  public:
    explicit AnalyzerStemSeparation(UserSettingsPointer pConfig);
    ~AnalyzerStemSeparation() override = default;

    /// Opt-in and disabled by default: separation is expensive and quadruples
    /// disk use, so it never runs on ordinary imports unless the user enables it.
    static bool isEnabled(const UserSettingsPointer& pConfig);

    bool initialize(const AnalyzerTrack& track,
            mixxx::audio::SampleRate sampleRate,
            mixxx::audio::ChannelCount channelCount,
            SINT frameLength) override;
    bool processSamples(const CSAMPLE* pIn, SINT count) override;
    void storeResults(TrackPointer pTrack) override;
    void cleanup() override;

  private:
    UserSettingsPointer m_pConfig;
    mixxx::audio::SampleRate m_sampleRate;
    mixxx::audio::ChannelCount m_channelCount;
    SINT m_framesProcessed;
};

#endif // __STEM__
