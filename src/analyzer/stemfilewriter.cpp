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

// Matches StemInfoImporter::kStemDefaultColor and the standard Demucs order.
struct StemMeta {
    const char* name;
    const char* color;
};
constexpr StemMeta kStems[kNumStems] = {
        {"Vocals", "#009E73"},
        {"Drums", "#D55E00"},
        {"Bass", "#CC79A7"},
        {"Other", "#56B4E9"},
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

    const QByteArray udta = makeBox("udta", makeBox("stem", manifest));
    const qint64 newMoovSize = static_cast<qint64>(moovSize) + udta.size();
    if (newMoovSize > 0xFFFFFFFFLL) {
        kLogger.warning() << "moov would exceed 32-bit size";
        return false;
    }

    // Append udta at EOF (== end of moov) and grow moov's size field.
    f.seek(fileSize);
    f.write(udta);
    f.seek(moovPos);
    quint32 be = qToBigEndian(static_cast<quint32>(newMoovSize));
    f.write(reinterpret_cast<const char*>(&be), 4);
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
    // Build a minimal file containing only a `moov` box (with a dummy child),
    // as ffmpeg leaves it when moov is the last box.
    const QByteArray moov = makeBox("moov", makeBox("mvhd", QByteArray(8, '\0')));
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

    const QByteArray manifest = manifestJson();
    if (!injectStemAtom(path, manifest)) {
        return false;
    }

    // Re-parse exactly like StemInfoImporter: descend moov -> udta -> stem.
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QByteArray blob = f.readAll();
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
    qint64 mb, me, ub, ue, sb, se;
    if (!findChild(blob, 0, blob.size(), "moov", mb, me) ||
            !findChild(blob, mb, me, "udta", ub, ue) ||
            !findChild(blob, ub, ue, "stem", sb, se)) {
        kLogger.warning() << "selfCheck: injected atom not found by parser";
        return false;
    }
    const bool ok = blob.mid(sb, se - sb) == manifest;
    if (!ok) {
        kLogger.warning() << "selfCheck: manifest payload mismatch";
    }
    return ok;
}

#endif // __STEMSEP_ONNX__
