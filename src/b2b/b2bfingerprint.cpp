#include "b2b/b2bfingerprint.h"

#include <QFile>

namespace mixxx::b2b {

namespace {

constexpr qint64 kChunk = 1024 * 1024;
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

void fnv(uint64_t* hash, const char* data, qint64 length) {
    for (qint64 i = 0; i < length; ++i) {
        *hash ^= static_cast<uint8_t>(data[i]);
        *hash *= kFnvPrime;
    }
}

} // namespace

uint64_t fingerprintFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return 0;
    }
    const qint64 size = file.size();
    uint64_t hash = kFnvOffset;
    fnv(&hash, reinterpret_cast<const char*>(&size), sizeof(size));

    QByteArray head = file.read(std::min(size, kChunk));
    fnv(&hash, head.constData(), head.size());
    if (size > kChunk) {
        const qint64 tailStart = std::max(kChunk, size - kChunk);
        file.seek(tailStart);
        QByteArray tail = file.read(size - tailStart);
        fnv(&hash, tail.constData(), tail.size());
    }
    return hash == 0 ? 1 : hash; // 0 means "no track"
}

QString fingerprintToHex(uint64_t fingerprint) {
    return QStringLiteral("%1").arg(fingerprint, 16, 16, QLatin1Char('0'));
}

uint64_t fingerprintFromHex(const QString& hex) {
    bool ok = false;
    const uint64_t value = hex.toULongLong(&ok, 16);
    return ok ? value : 0;
}

} // namespace mixxx::b2b
