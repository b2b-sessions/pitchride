#include "library/rekordbox/rekordboxcollection.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPasswordDigestor>
#include <QStandardPaths>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstring>

namespace rekordboxcollection {

namespace {

// ------------------------------------------------------------- AES-256 --
// Decryption only (FIPS-197 inverse cipher), enough for CBC.

constexpr uint8_t kSbox[256] = {
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
        0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
        0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
        0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
        0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
        0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
        0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
        0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
        0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
        0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
        0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
        0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
        0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
        0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
        0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
        0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16};

struct Aes256 {
    std::array<uint8_t, 240> roundKeys{};
    std::array<uint8_t, 256> invSbox{};

    explicit Aes256(const uint8_t* key) {
        for (int i = 0; i < 256; ++i) {
            invSbox[kSbox[i]] = static_cast<uint8_t>(i);
        }
        // Key expansion: 15 round keys of 16 bytes (Nk = 8, Nr = 14).
        std::memcpy(roundKeys.data(), key, 32);
        uint8_t rcon = 1;
        for (int i = 8; i < 60; ++i) {
            uint8_t t[4];
            std::memcpy(t, &roundKeys[(i - 1) * 4], 4);
            if (i % 8 == 0) {
                const uint8_t first = t[0];
                t[0] = static_cast<uint8_t>(kSbox[t[1]] ^ rcon);
                t[1] = kSbox[t[2]];
                t[2] = kSbox[t[3]];
                t[3] = kSbox[first];
                rcon = xtime(rcon);
            } else if (i % 8 == 4) {
                for (uint8_t& b : t) {
                    b = kSbox[b];
                }
            }
            for (int j = 0; j < 4; ++j) {
                roundKeys[i * 4 + j] = roundKeys[(i - 8) * 4 + j] ^ t[j];
            }
        }
    }

    static uint8_t xtime(uint8_t x) {
        return static_cast<uint8_t>((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
    }
    static uint8_t mul(uint8_t x, uint8_t y) {
        uint8_t r = 0;
        while (y) {
            if (y & 1) {
                r ^= x;
            }
            x = xtime(x);
            y >>= 1;
        }
        return r;
    }

    void addRoundKey(uint8_t* s, int round) const {
        for (int i = 0; i < 16; ++i) {
            s[i] ^= roundKeys[round * 16 + i];
        }
    }
    void invSubShift(uint8_t* s) const {
        // State is column-major: s[r + 4c]. Inverse ShiftRows then InvSubBytes.
        uint8_t t[16];
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                t[r + 4 * ((c + r) % 4)] = invSbox[s[r + 4 * c]];
            }
        }
        std::memcpy(s, t, 16);
    }
    static void invMixColumns(uint8_t* s) {
        for (int c = 0; c < 4; ++c) {
            uint8_t* col = s + 4 * c;
            const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9);
            col[1] = mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13);
            col[2] = mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11);
            col[3] = mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14);
        }
    }
    void decryptBlock(uint8_t* block) const {
        addRoundKey(block, 14);
        for (int round = 13; round >= 1; --round) {
            invSubShift(block);
            addRoundKey(block, round);
            invMixColumns(block);
        }
        invSubShift(block);
        addRoundKey(block, 0);
    }
    void decryptCbc(const uint8_t* iv, const uint8_t* in, uint8_t* out, int length) const {
        uint8_t prev[16];
        std::memcpy(prev, iv, 16);
        for (int off = 0; off + 16 <= length; off += 16) {
            uint8_t block[16];
            std::memcpy(block, in + off, 16);
            decryptBlock(block);
            for (int i = 0; i < 16; ++i) {
                out[off + i] = block[i] ^ prev[i];
            }
            std::memcpy(prev, in + off, 16);
        }
    }
};

// The key rekordbox uses for every master.db, stored obfuscated the way
// pyrekordbox stores it (base85, XOR, zlib).
QByteArray databaseKey() {
    static const char kBlob[] =
            "PN_Pq^*N>(JYe*u^8;Yg76HuZ<mR13S?=>)b9;DpoTXV(6ItkU`}8*m6tx_I{Solh_N#dfe{v=";
    static const char kXor[] = "657f48f84c437cc1";
    // Base85 (RFC 1924 alphabet, as Python's b85decode).
    static const char kAlphabet[] =
            "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{|}~";
    int value[128];
    std::fill(std::begin(value), std::end(value), -1);
    for (int i = 0; i < 85; ++i) {
        value[static_cast<unsigned char>(kAlphabet[i])] = i;
    }
    QByteArray decoded;
    const int n = static_cast<int>(std::strlen(kBlob));
    for (int i = 0; i < n; i += 5) {
        uint32_t acc = 0;
        int count = 0;
        for (int j = 0; j < 5; ++j) {
            const int v = i + j < n ? value[static_cast<unsigned char>(kBlob[i + j])] : 84;
            acc = acc * 85 + static_cast<uint32_t>(v);
            count += i + j < n ? 1 : 0;
        }
        for (int j = 0; j < count - 1; ++j) {
            decoded.append(static_cast<char>((acc >> (24 - 8 * j)) & 0xff));
        }
    }
    for (int i = 0; i < decoded.size(); ++i) {
        decoded[i] = static_cast<char>(decoded[i] ^ kXor[i % 16]);
    }
    // zlib stream: qUncompress wants the uncompressed size first (any big
    // enough guess works for a stream this small).
    QByteArray withSize(4, '\0');
    withSize[0] = 0;
    withSize[1] = 0;
    withSize[1] = 1; // 64 KB: plenty
    withSize[2] = 0;
    withSize[3] = 0;
    return qUncompress(withSize + decoded);
}

} // namespace

bool isMasterDb(const QString& path) {
    return QFileInfo(path).isFile() && QFileInfo(path).fileName().compare(
                                               QStringLiteral("master.db"), Qt::CaseInsensitive) == 0;
}

QString findMasterDb() {
    QStringList candidates;
#if defined(Q_OS_WIN)
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        const QString options = appData + QStringLiteral("/Pioneer/rekordboxAgent/storage/options.json");
        QFile file(options);
        if (file.open(QIODevice::ReadOnly)) {
            const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
            for (const auto& option : json.value(QStringLiteral("options")).toArray()) {
                const QJsonArray pair = option.toArray();
                if (pair.size() == 2 && pair.at(0).toString() == QStringLiteral("db-path")) {
                    candidates << pair.at(1).toString();
                }
            }
        }
        candidates << appData + QStringLiteral("/Pioneer/rekordbox/master.db");
        candidates << appData + QStringLiteral("/Pioneer/rekordbox7/master.db");
    }
#elif defined(Q_OS_MACOS)
    const QString home = QDir::homePath();
    const QString options = home + QStringLiteral("/Library/Application Support/Pioneer/rekordboxAgent/storage/options.json");
    QFile file(options);
    if (file.open(QIODevice::ReadOnly)) {
        const QJsonObject json = QJsonDocument::fromJson(file.readAll()).object();
        for (const auto& option : json.value(QStringLiteral("options")).toArray()) {
            const QJsonArray pair = option.toArray();
            if (pair.size() == 2 && pair.at(0).toString() == QStringLiteral("db-path")) {
                candidates << pair.at(1).toString();
            }
        }
    }
    candidates << home + QStringLiteral("/Library/Pioneer/rekordbox/master.db");
    candidates << home + QStringLiteral("/Library/Pioneer/rekordbox7/master.db");
#endif
    for (QString path : candidates) {
        if (QFileInfo(path).isDir()) {
            path = QDir(path).filePath(QStringLiteral("master.db"));
        }
        if (isMasterDb(path)) {
            return QDir::cleanPath(path);
        }
    }
    return QString();
}

QString decrypt(const QString& masterDbPath, const QString& outPath) {
    QFile in(masterDbPath);
    if (!in.open(QIODevice::ReadOnly)) {
        return QStringLiteral("can't open %1").arg(masterDbPath);
    }
    const QByteArray data = in.readAll();
    // SQLCipher 4's defaults: 4096-byte pages, each ending in 80 reserved
    // bytes (16 IV, 64 HMAC); the first 16 bytes of the file are the salt.
    constexpr int kPage = 4096;
    constexpr int kReserve = 80;
    if (data.size() < kPage || data.size() % kPage != 0) {
        return QStringLiteral("not a rekordbox database (size %1)").arg(data.size());
    }
    if (data.startsWith("SQLite format 3")) {
        // Not encrypted (a copy someone already decrypted): use as it is.
        QFile::remove(outPath);
        return QFile::copy(masterDbPath, outPath) ? QString() : QStringLiteral("can't write %1").arg(outPath);
    }
    const QByteArray passphrase = databaseKey();
    if (passphrase.size() != 64) {
        return QStringLiteral("no key");
    }
    const QByteArray salt = data.left(16);
    const QByteArray key = QPasswordDigestor::deriveKeyPbkdf2(
            QCryptographicHash::Sha512, passphrase, salt, 256000, 32);
    const Aes256 aes(reinterpret_cast<const uint8_t*>(key.constData()));
    QByteArray out(data.size(), '\0');
    const int pages = static_cast<int>(data.size() / kPage);
    for (int p = 0; p < pages; ++p) {
        const auto* page = reinterpret_cast<const uint8_t*>(data.constData()) + static_cast<qsizetype>(p) * kPage;
        auto* plain = reinterpret_cast<uint8_t*>(out.data()) + static_cast<qsizetype>(p) * kPage;
        const int start = p == 0 ? 16 : 0;
        aes.decryptCbc(page + kPage - kReserve, page + start, plain + start, kPage - kReserve - start);
        if (p == 0) {
            std::memcpy(plain, "SQLite format 3\0", 16);
        }
    }
    // A right key gives SQLite's header: page size 4096, 80 reserved bytes.
    if (static_cast<uint8_t>(out[16]) != 0x10 || static_cast<uint8_t>(out[17]) != 0x00 ||
            static_cast<uint8_t>(out[20]) != kReserve) {
        return QStringLiteral("this rekordbox database could not be read (a newer rekordbox?)");
    }
    QFile::remove(outPath);
    QFile file(outPath);
    if (!file.open(QIODevice::WriteOnly) || file.write(out) != out.size()) {
        return QStringLiteral("can't write %1").arg(outPath);
    }
    return QString();
}

} // namespace rekordboxcollection
