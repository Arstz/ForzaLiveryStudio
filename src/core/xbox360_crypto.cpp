#include "xbox360_crypto.h"

#include "binary_io.h"
#include "game_export.h"
#include "xbox360_vinyl.h"

#include <QMessageAuthenticationCode>
#include <QRandomGenerator>
#include <QtEndian>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <openssl/evp.h>
#endif

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace fls {
namespace {

constexpr int kSha1Size = 20;
constexpr int kSha256Size = 32;
constexpr int kAesBlockSize = 16;
constexpr int kAesKeySize = 16;
constexpr int kProfileIdSize = 8;
constexpr int kSeedSize = 4 + kAesBlockSize;
constexpr int kMaximumAssetSize = 16 * 1024 * 1024;

struct Envelope {
    QCryptographicHash::Algorithm algorithm;
    int digestSize;
    int macKeySize;
};

constexpr Envelope kSha1 = {QCryptographicHash::Sha1, kSha1Size, 16};
constexpr Envelope kSha256 = {QCryptographicHash::Sha256, kSha256Size, 32};

struct KeyPair {
    QByteArray aes;
    QByteArray mac;
};

struct KeyLocation {
    quint32 aesOffset;
    quint32 macOffset;
    int aesProfileOffset;
    int macProfileOffset;
};

constexpr KeyLocation kFM4LiveryKeys = {0xec80, 0xec60, 5, 1};
constexpr KeyLocation kFM4GroupKeys = {0x1d0c4, 0x1d0a4, 2, 7};
constexpr KeyLocation kFH1DiscLiveryKeys = {0xd643, 0x173e4, 1, 5};
constexpr KeyLocation kFH1UpdateLiveryKeys = {0x13a13, 0x1dfe4, 1, 5};

const Envelope &envelopeForGame(ExportGameVersion game) {
    switch (game) {
    case ExportGameVersion::Motorsport3:
        return kSha1;
    case ExportGameVersion::Motorsport4:
    case ExportGameVersion::Horizon1:
        return kSha256;
    default:
        throw std::runtime_error("unsupported Xbox 360 encryption profile");
    }
}

quint32 beWord(const QByteArray &bytes, qsizetype offset) {
    return qFromBigEndian<quint32>(bytes.constData() + offset);
}

quint32 seedLength(const QByteArray &seed) {
    quint32 value = 0;
    for (int offset = 0; offset < kSeedSize; offset += sizeof(quint32)) {
        value ^= beWord(seed, offset);
    }

    return value;
}

quint32 envelopeLength(const QByteArray &bytes, const Envelope &profile) {
    const qsizetype headerSize = profile.digestSize + kSeedSize;
    const qsizetype cipherSize = bytes.size() - headerSize;
    if (cipherSize < kAesBlockSize || cipherSize % kAesBlockSize != 0
        || bytes.size() > kMaximumAssetSize) {
        return 0;
    }
    const quint32 length = seedLength(bytes.mid(profile.digestSize, kSeedSize));

    return length >= 4 && length <= cipherSize && cipherSize - length < kAesBlockSize ? length : 0;
}

const char *magicForKind(Xbox360FileKind kind) {
    switch (kind) {
    case Xbox360FileKind::Livery:
        return detail::kXbox360LiveryMagic;
    case Xbox360FileKind::Group:
        return detail::kXbox360GroupMagic;
    default:
        throw std::runtime_error("unsupported Xbox 360 vinyl file type");
    }
}

QByteArray aesCbc(const QByteArray &data, const QByteArray &key, QByteArray iv, bool encrypt) {
    QByteArray output(data.size(), Qt::Uninitialized);
#ifdef _WIN32
    BCRYPT_ALG_HANDLE algorithmHandle = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithmHandle, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("could not open the AES provider");
    }
    const auto closeAlgorithm = [](BCRYPT_ALG_HANDLE handle) { BCryptCloseAlgorithmProvider(handle, 0); };
    const std::unique_ptr<void, decltype(closeAlgorithm)> algorithm(algorithmHandle, closeAlgorithm);
    wchar_t chainMode[] = BCRYPT_CHAIN_MODE_CBC;
    BCRYPT_KEY_HANDLE keyHandle = nullptr;
    if (BCryptSetProperty(algorithm.get(), BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(chainMode),
                          sizeof(chainMode), 0) < 0
        || BCryptGenerateSymmetricKey(algorithm.get(), &keyHandle, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<char *>(key.constData())), key.size(), 0) < 0) {
        throw std::runtime_error("could not initialize the AES-CBC key");
    }
    const auto destroyKey = [](BCRYPT_KEY_HANDLE handle) { BCryptDestroyKey(handle); };
    const std::unique_ptr<void, decltype(destroyKey)> cipher(keyHandle, destroyKey);
    ULONG written = 0;
    const auto operation = encrypt ? BCryptEncrypt : BCryptDecrypt;
    if (operation(cipher.get(), reinterpret_cast<PUCHAR>(const_cast<char *>(data.constData())),
                  data.size(), nullptr, reinterpret_cast<PUCHAR>(iv.data()), iv.size(),
                  reinterpret_cast<PUCHAR>(output.data()), output.size(), &written, 0) < 0
        || written != output.size()) {
        throw std::runtime_error("Xbox 360 AES-CBC operation failed");
    }
#else
    const std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
        EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int written = 0;
    int tail = 0;
    output.resize(data.size() + kAesBlockSize);
    if (!cipher || EVP_CipherInit_ex(cipher.get(), EVP_aes_128_cbc(), nullptr,
            reinterpret_cast<const unsigned char *>(key.constData()),
            reinterpret_cast<const unsigned char *>(iv.constData()), encrypt) != 1
        || EVP_CIPHER_CTX_set_padding(cipher.get(), 0) != 1
        || EVP_CipherUpdate(cipher.get(), reinterpret_cast<unsigned char *>(output.data()), &written,
            reinterpret_cast<const unsigned char *>(data.constData()), data.size()) != 1
        || EVP_CipherFinal_ex(cipher.get(), reinterpret_cast<unsigned char *>(output.data()) + written, &tail) != 1
        || written + tail != data.size()) {
        throw std::runtime_error("Xbox 360 AES-CBC operation failed");
    }
    output.truncate(written + tail);
#endif

    return output;
}

QByteArray deriveKey(const QByteArray &base, const QByteArray &profileId,
                     int profileOffset, bool horizon) {
    QByteArray result(base.size(), Qt::Uninitialized);
    for (int index = 0; index < base.size(); ++index) {
        const quint8 byte = static_cast<quint8>(base[index])
            ^ static_cast<quint8>(profileId[(index + profileOffset) % kProfileIdSize]);
        result[index] = static_cast<char>(byte + (horizon ? 2 * index : 0));
    }

    return result;
}

QByteArray readPeSection(const QByteArray &image, quint32 address, int length) {
    constexpr int kPeOffsetField = 0x3c;
    constexpr int kCoffHeaderSize = 24;
    constexpr int kSectionHeaderSize = 40;
    if (!image.startsWith("MZ") || image.size() < kPeOffsetField + 4) {
        return {};
    }
    const quint32 pe = detail::readLeU32(image, kPeOffsetField);
    if (pe > image.size() - kCoffHeaderSize || image.mid(pe, 4) != QByteArray("PE\0\0", 4)) {
        return {};
    }
    const int count = detail::readLeU16(image, pe + 6);
    int header = pe + kCoffHeaderSize + detail::readLeU16(image, pe + 20);
    for (int index = 0; index < count && header <= image.size() - kSectionHeaderSize;
         ++index, header += kSectionHeaderSize) {
        const quint32 rva = detail::readLeU32(image, header + 12);
        const quint32 size = detail::readLeU32(image, header + 16);
        const quint32 fileOffset = detail::readLeU32(image, header + 20);
        if (address >= rva && static_cast<quint64>(address - rva) + length <= size) {
            const quint64 offset = static_cast<quint64>(fileOffset) + address - rva;
            if (offset + length <= image.size()) {
                return image.mid(static_cast<qsizetype>(offset), length);
            }
        }
    }

    return {};
}

KeyPair deriveExecutableKeys(const Xbox360Credentials &credentials, const KeyLocation &location,
                             const Envelope &profile, bool horizon, bool peLayout) {
    const auto readKey = [&credentials, peLayout](quint32 offset, int size) {
        return peLayout ? readPeSection(credentials.keyData, offset, size)
                        : credentials.keyData.mid(offset, size);
    };
    const QByteArray aes = readKey(location.aesOffset, kAesKeySize);
    const QByteArray mac = readKey(location.macOffset, profile.macKeySize);
    if (aes.size() != kAesKeySize || mac.size() != profile.macKeySize) {
        return {};
    }

    return {deriveKey(aes, credentials.profileId, location.aesProfileOffset, horizon),
            deriveKey(mac, credentials.profileId, location.macProfileOffset, horizon)};
}

QList<KeyPair> candidateKeys(ExportGameVersion game, Xbox360FileKind kind,
                            const Xbox360Credentials &credentials, bool encrypt) {
    const Envelope &profile = envelopeForGame(game);
    if (credentials.source == Xbox360KeySource::ProfileKeys) {
        if (credentials.keyData.size() != kAesKeySize + profile.macKeySize) {
            throw std::runtime_error(QStringLiteral("expected a %1-byte Xbox 360 profile key file")
                .arg(kAesKeySize + profile.macKeySize).toStdString());
        }

        return {{credentials.keyData.left(kAesKeySize), credentials.keyData.mid(kAesKeySize)}};
    }
    if (!xbox360SupportsExecutableKeys(game, kind) || credentials.profileId.size() != kProfileIdSize) {
        throw std::runtime_error("Xbox 360 executable keys require a supported title and profile ID");
    }
    const bool horizon = game == ExportGameVersion::Horizon1;
    const bool titleUpdate = credentials.source == Xbox360KeySource::HorizonTitleUpdate;
    const KeyLocation &location = horizon ? (titleUpdate ? kFH1UpdateLiveryKeys : kFH1DiscLiveryKeys)
        : kind == Xbox360FileKind::Livery ? kFM4LiveryKeys : kFM4GroupKeys;
    QList<KeyPair> candidates;
    if (horizon) {
        candidates.append(deriveExecutableKeys(credentials, location, profile, true, true));
    }
    candidates.append(deriveExecutableKeys(credentials, location, profile, horizon, false));
    if (horizon && !encrypt) {
        const KeyLocation &alternate = titleUpdate ? kFH1DiscLiveryKeys : kFH1UpdateLiveryKeys;
        candidates.append(deriveExecutableKeys(credentials, alternate, profile, true, true));
        candidates.append(deriveExecutableKeys(credentials, alternate, profile, true, false));
    }
    candidates.removeIf([](const KeyPair &key) { return key.aes.isEmpty(); });
    if (candidates.isEmpty()) {
        throw std::runtime_error("the selected executable does not contain the Xbox 360 key locations");
    }

    return candidates;
}

bool matchesDigest(const QByteArray &expected, const QByteArray &actual) {
    quint8 difference = 0;
    for (qsizetype index = 0; index < expected.size(); ++index) {
        difference |= static_cast<quint8>(expected[index] ^ actual[index]);
    }

    return difference == 0;
}

} // namespace

Xbox360FileKind xbox360FileKind(const QString &fileName) {
    if (fileName.endsWith(QStringLiteral(".liv"), Qt::CaseInsensitive)) {
        return Xbox360FileKind::Livery;
    }
    if (fileName.endsWith(QStringLiteral(".lvg"), Qt::CaseInsensitive)
        || fileName.endsWith(QStringLiteral(".livgroup"), Qt::CaseInsensitive)) {
        return Xbox360FileKind::Group;
    }

    return Xbox360FileKind::Unknown;
}

Xbox360Envelope xbox360Envelope(const QByteArray &bytes) {
    if (detail::isMotorsport2LiveryContainer(bytes) || detail::isMotorsport2Livery(bytes)) {
        return Xbox360Envelope::Motorsport2;
    }
    if (envelopeLength(bytes, kSha256)) {
        return Xbox360Envelope::Sha256;
    }
    if (envelopeLength(bytes, kSha1)) {
        return Xbox360Envelope::Sha1;
    }

    return Xbox360Envelope::Unknown;
}

QByteArray xbox360ProfileId(const QString &hex) {
    const QByteArray digits = hex.trimmed().toLatin1();
    const QByteArray profile = QByteArray::fromHex(digits);
    if (digits.size() != kProfileIdSize * 2 || profile.toHex().compare(digits, Qt::CaseInsensitive) != 0) {
        throw std::runtime_error("the Xbox 360 profile ID must contain 16 hexadecimal digits");
    }

    return profile;
}

bool xbox360SupportsExecutableKeys(ExportGameVersion game, Xbox360FileKind kind) {
    return game == ExportGameVersion::Motorsport4
        || (game == ExportGameVersion::Horizon1 && kind == Xbox360FileKind::Livery);
}

QByteArray decryptXbox360Asset(const QByteArray &bytes, ExportGameVersion game,
                              Xbox360FileKind kind, const Xbox360Credentials &credentials) {
    const Envelope &profile = envelopeForGame(game);
    const char *magic = magicForKind(kind);
    const quint32 length = envelopeLength(bytes, profile);
    if (length == 0) {
        throw std::runtime_error("invalid Xbox 360 encrypted file length");
    }
    const QByteArray digest = bytes.left(profile.digestSize);
    const QByteArray authenticated = bytes.mid(profile.digestSize);
    for (const KeyPair &keys : candidateKeys(game, kind, credentials, false)) {
        const QByteArray computed = QMessageAuthenticationCode::hash(authenticated, keys.mac, profile.algorithm);
        if (!matchesDigest(digest, computed)) {
            continue;
        }
        QByteArray plain = aesCbc(authenticated.mid(kSeedSize), keys.aes,
                                  authenticated.mid(4, kAesBlockSize), false);
        plain.truncate(length);
        if (plain.startsWith(magic)) {
            return plain;
        }
    }

    throw std::runtime_error("Xbox 360 file authentication failed for the selected game and keys");
}

QByteArray encryptXbox360Asset(const QByteArray &plain, ExportGameVersion game,
                              Xbox360FileKind kind, const Xbox360Credentials &credentials) {
    const Envelope &profile = envelopeForGame(game);
    const KeyPair keys = candidateKeys(game, kind, credentials, true).front();
    QByteArray iv;
    QByteArray seed;
    QByteArray padded = plain;
    if (!plain.startsWith(magicForKind(kind)) || plain.size() > kMaximumAssetSize - 2 * kSeedSize - profile.digestSize) {
        throw std::runtime_error("invalid Xbox 360 vinyl payload for encryption");
    }
    quint32 lengthWord = static_cast<quint32>(plain.size());
    for (int offset = 0; offset < kAesBlockSize; offset += sizeof(quint32)) {
        const quint32 value = QRandomGenerator::system()->generate();
        detail::appendBeU32(iv, value);
        lengthWord ^= value;
    }
    detail::appendBeU32(seed, lengthWord);
    seed.append(iv);
    const char paddingByte = game == ExportGameVersion::Motorsport4 ? '\xbb' : '\0';
    padded.append(QByteArray((kAesBlockSize - plain.size() % kAesBlockSize) % kAesBlockSize, paddingByte));
    seed.append(aesCbc(padded, keys.aes, iv, true));

    return QMessageAuthenticationCode::hash(seed, keys.mac, profile.algorithm) + seed;
}

} // namespace fls
