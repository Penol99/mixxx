#pragma once

#include <optional>

#include "track/track_decl.h"

/// A scheduled not-null track with additional options for analysis.
class AnalyzerTrack {
  public:
    struct Options {
        /// If set, overrides whether the analysis should assume constant BPM.
        std::optional<bool> useFixedTempo;
        /// If true, run AI stem separation for this track even when the global
        /// "separate on import" setting is off (used by the "Separate stems"
        /// track menu action).
        bool separateStems = false;
    };

    // Two overloads instead of a `= Options()` default argument: a nested
    // struct with a default member initializer can't be used as a default
    // argument of its enclosing class (GCC complete-class-context error).
    explicit AnalyzerTrack(TrackPointer track);
    AnalyzerTrack(TrackPointer track, Options options);

    /// Fetches the (not-null) track to be analyzed.
    const TrackPointer& getTrack() const;

    /// Fetches the additional options.
    const Options& getOptions() const;

  private:
    /// The (not-null) track to be analyzed.
    TrackPointer m_track;
    /// The additional options.
    Options m_options;
};
