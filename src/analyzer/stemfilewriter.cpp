#include "analyzer/stemfilewriter.h"

#ifdef __STEMSEP_ONNX__

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtEndian>

#include <algorithm>

#include "util/logger.h"

namespace {
const mixxx::Logger kLogger("StemFileWriter");

constexpr int kNumStems = 4;
constexpr int kChannels = 2;

// Order MUST match the HTDemucs ONNX output: [drums, bass, other, vocals].
// Labels/colours are attached per output index so each named stem holds the
// right audio.
struct StemMeta {
    const char* name;
    const char* color;
};
constexpr StemMeta kStems[kNumStems] = {
        {"Drums", "#D55E00"},
        {"Bass", "#CC79A7"},
        {"Other", "#56B4E9"},
        {"Vocals", "#009E73"},
};

void appendBE32(QByteArray& out, quint32 v) {
    quint32 be = qToBigEndian(v);
    out.append(reinterpret_cast<const char*>(&be), 4);
}

// Wraps payload in an MP4 box: [size:BE32][type][payload].
QByteArray makeBox(const char type[4], const QByteArray& payload) {
    QByteArray box;
    appendBE32(box, static_cast<quint32>(8 + payload.size()));
    box.append(type, 4);
    box.append(payload);
    return box;
}

// Writes interleaved-stereo float samples as a 32-bit IEEE-float WAV.
bool writeWavFloat(const QString& path,
        const std::vector<CSAMPLE>& interleaved,
        int sampleRate) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    const quint32 dataBytes = static_cast<quint32>(interleaved.size() * sizeof(float));
    const quint32 byteRate = static_cast<quint32>(sampleRate) * kChannels * sizeof(float);
    QByteArray h;
    auto le32 = [&](quint32 v) {
        quint32 le = qToLittleEndian(v);
        h.append(reinterpret_cast<const char*>(&le), 4);
    };
    auto le16 = [&](quint16 v) {
        quint16 le = qToLittleEndian(v);
        h.append(reinterpret_cast<const char*>(&le), 2);
    };
    h.append("RIFF", 4);
    le32(36 + dataBytes);
    h.append("WAVE", 4);
    h.append("fmt ", 4);
    le32(16);
    le16(3); // IEEE float
    le16(kChannels);
    le32(static_cast<quint32>(sampleRate));
    le32(byteRate);
    le16(kChannels * sizeof(float)); // block align
    le16(8 * sizeof(float));         // bits per sample
    h.append("data", 4);
    le32(dataBytes);
    f.write(h);
    static_assert(sizeof(CSAMPLE) == sizeof(float), "WAV writer assumes float samples");
    f.write(reinterpret_cast<const char*>(interleaved.data()),
            static_cast<qint64>(dataBytes));
    f.close();
    return true;
}
} // anonymous namespace

QByteArray StemFileWriter::manifestJson() {
    QJsonArray stems;
    for (const auto& s : kStems) {
        QJsonObject o;
        o.insert("name", QString::fromUtf8(s.name));
        o.insert("color", QString::fromUtf8(s.color));
        stems.append(o);
    }
    QJsonObject root;
    root.insert("version", 1);
    root.insert("stems", stems);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool StemFileWriter::injectStemAtom(const QString& mp4Path, const QByteArray& manifest) {
    QFile f(mp4Path);
    if (!f.open(QIODevice::ReadWrite)) {
        kLogger.warning() << "Cannot open for atom injection:" << mp4Path;
        return false;
    }
    const qint64 fileSize = f.size();

    // Walk top-level boxes to find `moov` and confirm it is the last box, so we
    // can grow it by appending at EOF without shifting any mdat chunk offsets.
    qint64 pos = 0;
    qint64 moovPos = -1;
    quint32 moovSize = 0;
    while (pos + 8 <= fileSize) {
        f.seek(pos);
        QByteArray hdr = f.read(8);
        quint32 size = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar*>(hdr.constData()));
        const QByteArray type = hdr.mid(4, 4);
        if (size == 1 || size < 8) {
            // 64-bit or degenerate box: bail rather than risk corruption.
            kLogger.warning() << "Unsupported box size in" << mp4Path;
            return false;
        }
        if (type == "moov") {
            moovPos = pos;
            moovSize = size;
        }
        pos += size;
    }
    if (moovPos < 0 || moovPos + static_cast<qint64>(moovSize) != fileSize) {
        kLogger.warning() << "moov not found or not last box; mux with "
                             "-map_metadata -1 and no +faststart. File:"
                          << mp4Path;
        return false;
    }

    // Find moov's last child. Muxers (ffmpeg) usually write their own `udta` as
    // the last child; mixxx's parser only inspects the FIRST udta, so we must put
    // the stem box inside that existing udta rather than appending a second one.
    const qint64 moovEnd = moovPos + static_cast<qint64>(moovSize); // == fileSize
    qint64 childPos = moovPos + 8;
    qint64 lastChildPos = -1;
    quint32 lastChildSize = 0;
    QByteArray lastChildType;
    while (childPos + 8 <= moovEnd) {
        f.seek(childPos);
        const QByteArray hdr = f.read(8);
        const quint32 csize = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar*>(hdr.constData()));
        if (csize < 8 || childPos + static_cast<qint64>(csize) > moovEnd) {
            kLogger.warning() << "Unexpected moov child box in" << mp4Path;
            return false;
        }
        lastChildPos = childPos;
        lastChildSize = csize;
        lastChildType = hdr.mid(4, 4);
        childPos += csize;
    }

    const QByteArray stemBox = makeBox("stem", manifest);
    auto writeBE32 = [&f](qint64 at, quint32 v) {
        const quint32 be = qToBigEndian(v);
        f.seek(at);
        f.write(reinterpret_cast<const char*>(&be), 4);
    };

    if (lastChildType == "udta" &&
            lastChildPos + static_cast<qint64>(lastChildSize) == fileSize) {
        // Extend the muxer's existing udta (it ends at EOF): append the stem box
        // and grow both udta and moov.
        const qint64 newUdtaSize = static_cast<qint64>(lastChildSize) + stemBox.size();
        const qint64 newMoovSize = static_cast<qint64>(moovSize) + stemBox.size();
        if (newUdtaSize > 0xFFFFFFFFLL || newMoovSize > 0xFFFFFFFFLL) {
            kLogger.warning() << "box would exceed 32-bit size";
            return false;
        }
        f.seek(fileSize);
        f.write(stemBox);
        writeBE32(lastChildPos, static_cast<quint32>(newUdtaSize));
        writeBE32(moovPos, static_cast<quint32>(newMoovSize));
    } else {
        // No trailing udta: append a fresh udta{stem} and grow moov.
        const QByteArray udta = makeBox("udta", stemBox);
        const qint64 newMoovSize = static_cast<qint64>(moovSize) + udta.size();
        if (newMoovSize > 0xFFFFFFFFLL) {
            kLogger.warning() << "moov would exceed 32-bit size";
            return false;
        }
        f.seek(fileSize);
        f.write(udta);
        writeBE32(moovPos, static_cast<quint32>(newMoovSize));
    }
    f.close();
    return true;
}

bool StemFileWriter::write(const QString& outPath,
        const std::vector<std::vector<CSAMPLE>>& stems,
        int sampleRate,
        const QString& ffmpegPath) {
    if (stems.size() != kNumStems) {
        kLogger.warning() << "Expected" << kNumStems << "stems, got" << stems.size();
        return false;
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        kLogger.warning() << "Cannot create temp dir for stem muxing";
        return false;
    }

    // Main mix (stream 0) is required but never decoded by Mixxx: sum the stems
    // and hard-limit to [-1, 1].
    const size_t n = stems[0].size();
    std::vector<CSAMPLE> mainMix(n, 0.0f);
    for (int s = 0; s < kNumStems; ++s) {
        const size_t m = std::min(n, stems[s].size());
        for (size_t i = 0; i < m; ++i) {
            mainMix[i] += stems[s][i];
        }
    }
    for (CSAMPLE& v : mainMix) {
        v = std::clamp(v, -1.0f, 1.0f);
    }

    QStringList args{"-y"};
    const QString mainWav = tmp.filePath("main.wav");
    if (!writeWavFloat(mainWav, mainMix, sampleRate)) {
        return false;
    }
    args << "-i" << mainWav;
    for (int s = 0; s < kNumStems; ++s) {
        const QString wav = tmp.filePath(QStringLiteral("stem%1.wav").arg(s));
        if (!writeWavFloat(wav, stems[s], sampleRate)) {
            return false;
        }
        args << "-i" << wav;
    }
    for (int i = 0; i < kNumStems + 1; ++i) {
        args << "-map" << QStringLiteral("%1:a").arg(i);
    }
    args << "-c:a" << "aac" << "-b:a" << "256k" << "-map_metadata" << "-1"
         << outPath;

    QProcess ffmpeg;
    ffmpeg.start(ffmpegPath, args);
    if (!ffmpeg.waitForStarted()) {
        kLogger.warning() << "Cannot start ffmpeg at" << ffmpegPath;
        return false;
    }
    ffmpeg.waitForFinished(-1);
    if (ffmpeg.exitStatus() != QProcess::NormalExit || ffmpeg.exitCode() != 0) {
        kLogger.warning() << "ffmpeg muxing failed:"
                          << ffmpeg.readAllStandardError();
        return false;
    }

    if (!injectStemAtom(outPath, manifestJson())) {
        return false;
    }
    kLogger.info() << "Wrote stem file" << outPath;
    return true;
}

bool StemFileWriter::selfCheck() {
    const QByteArray manifest = manifestJson();

    // Descends moov -> FIRST udta -> stem exactly like StemInfoImporter, and
    // confirms the manifest matches. Because mixxx only inspects the first udta,
    // this catches the "muxer already wrote a udta" case (a second udta would be
    // invisible to the parser).
    auto findChild = [](const QByteArray& data, qint64 begin, qint64 end,
                             const char* type, qint64& outBegin, qint64& outEnd) {
        qint64 p = begin;
        while (p + 8 <= end) {
            quint32 size = qFromBigEndian<quint32>(
                    reinterpret_cast<const uchar*>(data.constData() + p));
            if (size < 8 || p + size > end) {
                return false;
            }
            if (memcmp(data.constData() + p + 4, type, 4) == 0) {
                outBegin = p + 8;
                outEnd = p + size;
                return true;
            }
            p += size;
        }
        return false;
    };

    auto checkCase = [&](const QByteArray& moov) -> bool {
        QTemporaryDir tmp;
        if (!tmp.isValid()) {
            return false;
        }
        const QString path = tmp.filePath("t.mp4");
        {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) {
                return false;
            }
            f.write(moov);
        }
        if (!injectStemAtom(path, manifest)) {
            return false;
        }
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return false;
        }
        const QByteArray blob = f.readAll();
        qint64 mb, me, ub, ue, sb, se;
        if (!findChild(blob, 0, blob.size(), "moov", mb, me) ||
                !findChild(blob, mb, me, "udta", ub, ue) ||
                !findChild(blob, ub, ue, "stem", sb, se)) {
            kLogger.warning() << "selfCheck: injected atom not found by parser";
            return false;
        }
        if (blob.mid(sb, se - sb) != manifest) {
            kLogger.warning() << "selfCheck: manifest payload mismatch";
            return false;
        }
        return true;
    };

    // Case 1: moov with no udta (append a fresh udta).
    const QByteArray noUdta = makeBox("moov", makeBox("mvhd", QByteArray(8, '\0')));
    // Case 2: moov whose last child is a udta (as ffmpeg leaves it) — the stem
    // box must be merged into that existing udta, not added as a second one.
    const QByteArray withUdta = makeBox("moov",
            makeBox("mvhd", QByteArray(8, '\0')) +
                    makeBox("udta", makeBox("meta", QByteArray(4, '\0'))));
    return checkCase(noUdta) && checkCase(withUdta);
}

#endif // __STEMSEP_ONNX__
