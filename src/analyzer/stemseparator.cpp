#include "analyzer/stemseparator.h"

#ifdef __STEMSEP_ONNX__

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <thread>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("StemSeparator");

// HTDemucs is trained on ~7.8 s segments. We process the track in fixed,
// zero-padded chunks of this many frames per channel.
// ponytail: non-overlapping chunks — seams between chunks may click. Add
// windowed overlap-add (WOLA, 25% overlap) here if artifacts show up.
constexpr int kChunkFrames = StemSeparator::kModelSampleRate * 8;
constexpr int kChannels = 2;

#ifdef _WIN32
std::wstring toOrtPath(const QString& p) {
    return p.toStdWString();
}
#else
std::string toOrtPath(const QString& p) {
    return p.toStdString();
}
#endif
} // anonymous namespace

struct StemSeparator::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "mixxx-stemsep"};
    std::unique_ptr<Ort::Session> session;
    Ort::AllocatorWithDefaultOptions allocator;
    Ort::AllocatorWithDefaultOptions& alloc = allocator;
    std::string inputName;
    std::string outputName;
};

StemSeparator::StemSeparator()
        : m_pImpl(std::make_unique<Impl>()) {
}

StemSeparator::~StemSeparator() = default;

bool StemSeparator::isLoaded() const {
    return m_pImpl->session != nullptr;
}

bool StemSeparator::load(const QString& modelPath) {
    try {
        Ort::SessionOptions options;
        options.SetIntraOpNumThreads(
                std::max(1u, std::thread::hardware_concurrency()));
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        // ponytail: CPU only. Add OpenVINO/CUDA execution providers with CPU
        // fallback here once we detect them (epic #15495 lists this).
        m_pImpl->session = std::make_unique<Ort::Session>(
                m_pImpl->env, toOrtPath(modelPath).c_str(), options);

        m_pImpl->inputName =
                m_pImpl->session->GetInputNameAllocated(0, m_pImpl->alloc).get();
        m_pImpl->outputName =
                m_pImpl->session->GetOutputNameAllocated(0, m_pImpl->alloc).get();
    } catch (const Ort::Exception& e) {
        kLogger.warning() << "Failed to load model" << modelPath << ":" << e.what();
        m_pImpl->session.reset();
        return false;
    }
    return true;
}

std::vector<std::vector<CSAMPLE>> StemSeparator::separate(
        const std::vector<CSAMPLE>& interleavedStereo) {
    if (!isLoaded() || interleavedStereo.empty()) {
        return {};
    }

    const int totalFrames = static_cast<int>(interleavedStereo.size() / kChannels);
    std::vector<std::vector<CSAMPLE>> stems(
            kNumStems, std::vector<CSAMPLE>(totalFrames * kChannels, 0.0f));

    const char* inputNames[] = {m_pImpl->inputName.c_str()};
    const char* outputNames[] = {m_pImpl->outputName.c_str()};
    auto memInfo = Ort::MemoryInfo::CreateCpu(
            OrtArenaAllocator, OrtMemTypeDefault);

    // Deinterleave one chunk at a time into planar [channel][frame] and run
    // the model. Output shape is [batch=1, source=kNumStems, channel, frame].
    std::vector<float> planar(static_cast<size_t>(kChannels) * kChunkFrames);
    const std::array<int64_t, 3> inputShape = {1, kChannels, kChunkFrames};

    try {
        for (int start = 0; start < totalFrames; start += kChunkFrames) {
            const int frames = std::min(kChunkFrames, totalFrames - start);
            std::fill(planar.begin(), planar.end(), 0.0f);
            for (int ch = 0; ch < kChannels; ++ch) {
                for (int f = 0; f < frames; ++f) {
                    planar[static_cast<size_t>(ch) * kChunkFrames + f] =
                            interleavedStereo[(static_cast<size_t>(start + f)) *
                                            kChannels +
                                    ch];
                }
            }

            Ort::Value input = Ort::Value::CreateTensor<float>(
                    memInfo,
                    planar.data(),
                    planar.size(),
                    inputShape.data(),
                    inputShape.size());

            auto outputs = m_pImpl->session->Run(Ort::RunOptions{nullptr},
                    inputNames,
                    &input,
                    1,
                    outputNames,
                    1);

            const float* out = outputs[0].GetTensorMutableData<float>();
            // out layout: [source][channel][frame] within this chunk.
            for (int s = 0; s < kNumStems; ++s) {
                for (int ch = 0; ch < kChannels; ++ch) {
                    const float* src = out +
                            ((static_cast<size_t>(s) * kChannels + ch) *
                                    kChunkFrames);
                    for (int f = 0; f < frames; ++f) {
                        stems[s][(static_cast<size_t>(start + f)) * kChannels + ch] =
                                src[f];
                    }
                }
            }
        }
    } catch (const Ort::Exception& e) {
        kLogger.warning() << "Inference failed:" << e.what();
        return {};
    }

    return stems;
}

#endif // __STEMSEP_ONNX__
