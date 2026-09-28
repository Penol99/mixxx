#pragma once

#ifdef __STEMSEP_ONNX__

#include <QByteArray>
#include <QString>
#include <vector>

#include "util/types.h"

/// Writes 4 separated stems into a Native-Instruments-style `.stem.mp4` that
/// Mixxx's existing SoundSourceSTEM can play: 5 stereo AAC streams (a main mix
/// followed by the 4 stems) plus a `moov/udta/stem` JSON manifest so the track
/// is recognised as stems with names/colours (see StemInfoImporter).
///
/// Muxing is delegated to the `ffmpeg` CLI (robust, and Mixxx already ships
/// FFmpeg); the custom manifest atom — which no muxer writes — is injected here
/// and covered by selfCheck(). ponytail: CLI keeps this ~10x smaller than a
/// libavformat muxer; switch to libav if the binary dependency ever bites.
class StemFileWriter {
  public:
    /// stems: exactly 4 buffers, interleaved stereo, at sampleRate.
    /// Returns true and leaves a valid .stem.mp4 at outPath on success.
    static bool write(const QString& outPath,
            const std::vector<std::vector<CSAMPLE>>& stems,
            int sampleRate,
            const QString& ffmpegPath);

    /// The NI `stem` manifest JSON (version + 4 named/coloured stems).
    static QByteArray manifestJson();

    /// Appends a `moov/udta/stem` atom holding `manifest` to an mp4 file that
    /// was muxed with `-map_metadata -1` (i.e. has no existing udta). Returns
    /// false and leaves the file untouched on any unexpected layout.
    static bool injectStemAtom(const QString& mp4Path, const QByteArray& manifest);

    /// Builds a synthetic moov box, injects the atom, and re-parses it with the
    /// same logic StemInfoImporter uses. Returns true if the atom is found.
    /// Runnable check for the fiddly byte-level box surgery.
    static bool selfCheck();
};

#endif // __STEMSEP_ONNX__
