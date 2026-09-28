#pragma once

#ifdef __STEMSEP_ONNX__

#include <QString>
#include <memory>
#include <vector>

#include "util/types.h"

/// Runs a Demucs v4 (HTDemucs) ONNX model to split interleaved stereo audio
/// into 4 stems: [vocals, drums, bass, other].
///
/// ONNXRuntime is kept out of this header via pimpl so only analyzer TUs that
/// actually do inference pull in the heavy dependency. See epic
/// mixxxdj/mixxx#15495.
class StemSeparator {
  public:
    static constexpr int kNumStems = 4;
    /// Demucs is trained at 44.1 kHz; callers must resample to this first.
    static constexpr int kModelSampleRate = 44100;

    StemSeparator();
    ~StemSeparator();

    /// Loads the ONNX model from disk. Returns false and logs on failure.
    bool load(const QString& modelPath);
    bool isLoaded() const;

    /// Separates interleaved stereo samples (at kModelSampleRate) into
    /// kNumStems interleaved-stereo buffers of the same frame count.
    /// Returns an empty vector on failure or if not loaded.
    std::vector<std::vector<CSAMPLE>> separate(
            const std::vector<CSAMPLE>& interleavedStereo);

  private:
    struct Impl;
    std::unique_ptr<Impl> m_pImpl;
};

#endif // __STEMSEP_ONNX__
