#pragma once

#include "accounts.hpp"

#include <cstring>
#include <memory>
#include <shlobj.h>

namespace twofa {

inline constexpr std::size_t kMaxPasswordLength = 256;
inline constexpr std::size_t kMinExportPasswordLength = 12;
inline constexpr std::size_t kMinLocalPasswordLength = 6;

namespace backup {

// All versions use UTF-16LE text and little-endian uint16 lengths.
inline constexpr std::array<std::uint8_t, 8> kLegacyMagic{'2', 'F', 'A', 'V', 'L', 'T', '0', '1'};
inline constexpr std::array<std::uint8_t, 8> kTagsMagic{'2', 'F', 'A', 'V', 'L', 'T', '0', '2'};
inline constexpr std::array<std::uint8_t, 8> kMagic{'2', 'F', 'A', 'V', 'L', 'T', '0', '3'};
inline constexpr std::array<std::uint8_t, 8> kFavoritesMagic{'2', 'F', 'A', 'V', 'L', 'T', '0', '4'};
inline constexpr std::array<std::uint8_t, 8> kLocalMagic{'2', 'F', 'A', 'L', 'V', 'T', '0', '4'};
inline constexpr std::array<std::uint8_t, 8> kLocalFavoritesMagic{'2', 'F', 'A', 'L', 'V', 'T', '0', '5'};
inline constexpr std::array<std::uint8_t, 8> kWindowsVaultMagic{'2', 'F', 'A', 'D', 'P', 'A', '0', '1'};
inline constexpr std::array<std::uint8_t, 8> kWindowsFavoritesMagic{'2', 'F', 'A', 'D', 'P', 'A', '0', '2'};
inline constexpr std::size_t kSaltSize = 16;
inline constexpr std::size_t kNonceSize = 12;
inline constexpr std::size_t kTagSize = 16;
inline constexpr std::size_t kHeaderSize = kMagic.size() + kSaltSize + kNonceSize;
inline constexpr std::size_t kMaxLegacyPayloadSize = 2 + kMaxAccounts * (12 + 6 * kMaxNameLength + kMaxSecretLength);
inline constexpr std::size_t kMaxPayloadSize = 2 + kMaxAccounts *
    (12 + 4 * kMaxNameLength + kMaxTags * (6 + 2 * kMaxNameLength) + kMaxSecretLength);
inline constexpr std::size_t kMaxFileSize = kHeaderSize + kMaxPayloadSize + kMaxAccounts + kTagSize;
inline constexpr std::size_t kUsageSize = sizeof(std::uint32_t) + sizeof(std::uint64_t);
inline constexpr std::size_t kMaxLocalPayloadSize = kMaxPayloadSize + kMaxAccounts * (kUsageSize + 1);
inline constexpr std::size_t kMaxLocalFileSize = kMaxLocalPayloadSize + 4096;
inline constexpr ULONGLONG kIterations = 600000;

struct Plaintext {
    Plaintext() = default;
    Plaintext(const Plaintext&) = delete;
    Plaintext& operator=(const Plaintext&) = delete;
    Plaintext(Plaintext&&) noexcept = default;
    ~Plaintext() {
        if (!bytes.empty()) {
            SecureZeroMemory(bytes.data(), bytes.size());
        }
    }

    std::vector<std::uint8_t> bytes;
};

struct CngAlgorithm {
    CngAlgorithm() = default;
    CngAlgorithm(const CngAlgorithm&) = delete;
    ~CngAlgorithm() {
        if (handle) {
            BCryptCloseAlgorithmProvider(handle, 0);
        }
    }
    BCRYPT_ALG_HANDLE handle = nullptr;
};

struct CngKey {
    CngKey() = default;
    CngKey(const CngKey&) = delete;
    ~CngKey() {
        if (handle) {
            BCryptDestroyKey(handle);
        }
    }
    BCRYPT_KEY_HANDLE handle = nullptr;
};

inline void WriteUint16(std::vector<std::uint8_t>& bytes, std::size_t number) {
    bytes.push_back(static_cast<std::uint8_t>(number));
    bytes.push_back(static_cast<std::uint8_t>(number >> 8));
}

inline void WriteUnsigned(std::vector<std::uint8_t>& bytes, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value));
        value >>= 8;
    }
}

inline void WriteName(std::vector<std::uint8_t>& bytes, std::wstring_view value) {
    static_assert(sizeof(wchar_t) == 2);
    WriteUint16(bytes, value.size());
    const auto* start = reinterpret_cast<const std::uint8_t*>(value.data());
    if (!value.empty()) {
        bytes.insert(bytes.end(), start, start + value.size() * sizeof(wchar_t));
    }
}

class PayloadReader {
public:
    explicit PayloadReader(std::span<const std::uint8_t> bytes) : remaining_(bytes) {}

    std::span<const std::uint8_t> Take(std::size_t length) {
        if (length > remaining_.size()) {
            valid_ = false;
            return {};
        }
        const auto part = remaining_.first(length);
        remaining_ = remaining_.subspan(length);
        return part;
    }

    bool Good() const { return valid_; }
    bool AtEnd() const { return valid_ && remaining_.empty(); }

    unsigned ReadUint16() {
        const auto bytes = Take(2);
        return bytes.empty() ? 0 : bytes[0] | (static_cast<unsigned>(bytes[1]) << 8);
    }

    std::uint64_t ReadUnsigned(std::size_t count) {
        std::uint64_t value = 0;
        const auto bytes = Take(count);
        for (std::size_t i = bytes.size(); i > 0; --i) {
            value = (value << 8) | bytes[i - 1];
        }
        return value;
    }

    std::wstring ReadName() {
        const auto count = ReadUint16();
        if (count > kMaxNameLength) {
            valid_ = false;
            return {};
        }
        const auto bytes = Take(count * sizeof(wchar_t));
        std::wstring result(count, L'\0');
        if (!bytes.empty()) {
            std::memcpy(result.data(), bytes.data(), bytes.size());
        }
        return result;
    }

private:
    std::span<const std::uint8_t> remaining_;
    bool valid_ = true;
};

inline std::optional<std::vector<Account>> Deserialize(std::span<const std::uint8_t> bytes, unsigned version = 3,
                                                       bool allowEmpty = false, bool hasFavorites = false) {
    PayloadReader reader(bytes);
    const auto count = reader.ReadUint16();
    if (!reader.Good() || (!allowEmpty && count == 0) || count > kMaxAccounts) {
        return std::nullopt;
    }
    std::vector<Account> accounts;
    accounts.reserve(count);
    for (unsigned i = 0; i < count; ++i) {
        Account account;
        account.issuer = reader.ReadName();
        account.label = reader.ReadName();
        if (version == 1) {
            auto group = reader.ReadName();
            if (!group.empty()) {
                account.tags.push_back(std::move(group));
            }
        } else {
            const auto tagCount = reader.ReadUint16();
            if (tagCount > kMaxTags) {
                return std::nullopt;
            }
            account.tags.reserve(tagCount);
            for (unsigned tag = 0; tag < tagCount; ++tag) {
                account.tags.push_back(reader.ReadName());
                if (version >= 3) {
                    const auto low = reader.ReadUint16();
                    account.tagColors[tag] = low | (reader.ReadUint16() << 16);
                }
            }
        }
        const auto settings = reader.Take(2);
        if (settings.empty() || settings[0] > 2) {
            return std::nullopt;
        }
        account.algorithm = static_cast<Algorithm>(settings[0]);
        account.digits = settings[1];
        account.period = reader.ReadUint16();
        const auto secretLength = reader.ReadUint16();
        if (secretLength > kMaxSecretLength) {
            return std::nullopt;
        }
        const auto secret = reader.Take(secretLength);
        if (version >= 4) {
            account.useCount = static_cast<std::uint32_t>(reader.ReadUnsigned(sizeof(account.useCount)));
            account.lastUsed = reader.ReadUnsigned(sizeof(account.lastUsed));
        }
        if (hasFavorites) {
            const auto favorite = reader.ReadUnsigned(1);
            if (favorite > 1) {
                return std::nullopt;
            }
            account.favorite = favorite == 1;
        }
        SecretBytes decoded;
        std::ranges::copy(secret, decoded.data.begin());
        decoded.size = secret.size();
        if (!reader.Good() || !account.secret.Store(std::move(decoded)) || !ValidAccount(account)) {
            return std::nullopt;
        }
        accounts.push_back(std::move(account));
    }
    return reader.AtEnd() ? std::optional(std::move(accounts)) : std::nullopt;
}

inline bool Serialize(std::span<const Account> accounts, Plaintext& plaintext, bool allowEmpty = false,
                      bool includeUsage = false, bool includeFavorites = false) {
    if (!plaintext.bytes.empty() || (!allowEmpty && accounts.empty()) || accounts.size() > kMaxAccounts ||
        !std::ranges::all_of(accounts, ValidAccount)) {
        return false;
    }
    std::size_t size = 2 + accounts.size() * ((includeUsage ? kUsageSize : 0) + (includeFavorites ? 1 : 0));
    for (const auto& account : accounts) {
        size += 12 + sizeof(wchar_t) * (account.issuer.size() + account.label.size()) + account.secret.Size();
        for (const auto& tag : account.tags) {
            size += 6 + sizeof(wchar_t) * tag.size();
        }
    }
    auto& bytes = plaintext.bytes;
    bytes.reserve(size);
    WriteUint16(bytes, accounts.size());
    for (const auto& account : accounts) {
        WriteName(bytes, account.issuer);
        WriteName(bytes, account.label);
        WriteUint16(bytes, account.tags.size());
        for (std::size_t i = 0; i < account.tags.size(); ++i) {
            WriteName(bytes, account.tags[i]);
            WriteUint16(bytes, account.tagColors[i] & 0xffff);
            WriteUint16(bytes, account.tagColors[i] >> 16);
        }
        bytes.push_back(static_cast<std::uint8_t>(account.algorithm));
        bytes.push_back(static_cast<std::uint8_t>(account.digits));
        WriteUint16(bytes, account.period);
        SecretBytes decoded;
        if (!account.secret.Read(decoded)) {
            return false;
        }
        const auto secret = decoded.View();
        WriteUint16(bytes, secret.size());
        bytes.insert(bytes.end(), secret.begin(), secret.end());
        if (includeUsage) {
            WriteUnsigned(bytes, account.useCount, sizeof(account.useCount));
            WriteUnsigned(bytes, account.lastUsed, sizeof(account.lastUsed));
        }
        if (includeFavorites) {
            bytes.push_back(static_cast<std::uint8_t>(account.favorite));
        }
    }
    return true;
}

inline bool DeriveKey(std::wstring_view password, std::span<const std::uint8_t, kSaltSize> salt,
                      SecretBytes& key) {
    key.Clear();
    if (password.empty() || password.size() > kMaxPasswordLength) {
        return false;
    }
    CngAlgorithm hmac;
    if (BCryptOpenAlgorithmProvider(&hmac.handle, BCRYPT_SHA256_ALGORITHM, nullptr,
                                    BCRYPT_ALG_HANDLE_HMAC_FLAG) < 0) {
        return false;
    }
    auto* passwordBytes = reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(password.data()));
    const auto length = static_cast<ULONG>(password.size() * sizeof(wchar_t));
    key.size = 32;
    return BCryptDeriveKeyPBKDF2(hmac.handle, passwordBytes, length, const_cast<PUCHAR>(salt.data()),
                                kSaltSize, kIterations, key.data.data(), static_cast<ULONG>(key.size), 0) >= 0;
}

inline bool EncryptOrDecrypt(bool encrypt, SecretBytes& derived,
                             std::span<std::uint8_t> file, Plaintext& plaintext) {
    const auto header = file.first<kHeaderSize>();
    const auto nonce = header.last<kNonceSize>();
    const auto tag = file.last<kTagSize>();
    const auto ciphertext = file.subspan(kHeaderSize, file.size() - kHeaderSize - kTagSize);
    CngAlgorithm aes;
    CngKey key;
    wchar_t mode[] = BCRYPT_CHAIN_MODE_GCM;
    if (derived.size != 32 || BCryptOpenAlgorithmProvider(&aes.handle, BCRYPT_AES_ALGORITHM, nullptr, 0) < 0 ||
        BCryptSetProperty(aes.handle, BCRYPT_CHAINING_MODE, reinterpret_cast<PUCHAR>(mode), sizeof(mode), 0) < 0 ||
        BCryptGenerateSymmetricKey(aes.handle, &key.handle, nullptr, 0,
                                   derived.data.data(), static_cast<ULONG>(derived.size), 0) < 0) {
        return false;
    }
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = nonce.data();
    info.cbNonce = kNonceSize;
    info.pbAuthData = header.data();
    info.cbAuthData = kHeaderSize;
    info.pbTag = tag.data();
    info.cbTag = kTagSize;
    const auto plain = std::span(plaintext.bytes);
    const auto input = encrypt ? plain : ciphertext;
    const auto output = encrypt ? ciphertext : plain;
    const auto cipher = encrypt ? BCryptEncrypt : BCryptDecrypt;
    ULONG written;
    const auto status = cipher(key.handle, input.data(), static_cast<ULONG>(input.size()), &info,
                               nullptr, 0, output.data(), static_cast<ULONG>(output.size()), &written, 0);
    return status >= 0 && written == output.size();
}

inline bool EncryptOrDecrypt(bool encrypt, std::wstring_view password,
                             std::span<std::uint8_t> file, Plaintext& plaintext) {
    SecretBytes derived;
    const auto salt = file.subspan<kMagic.size(), kSaltSize>();
    return DeriveKey(password, salt, derived) && EncryptOrDecrypt(encrypt, derived, file, plaintext);
}

}  // namespace backup

inline std::optional<std::vector<std::uint8_t>> CreateBackup(std::span<const Account> accounts,
                                                             std::wstring_view password) {
    if (password.size() < kMinExportPasswordLength || password.size() > kMaxPasswordLength) {
        return std::nullopt;
    }
    backup::Plaintext plaintext;
    const bool favorites = std::ranges::any_of(accounts, &Account::favorite);
    if (!backup::Serialize(accounts, plaintext, false, false, favorites)) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(backup::kHeaderSize + plaintext.bytes.size() + backup::kTagSize);
    std::ranges::copy(favorites ? backup::kFavoritesMagic : backup::kMagic, bytes.begin());
    if (BCryptGenRandom(nullptr, bytes.data() + backup::kMagic.size(),
                         backup::kSaltSize + backup::kNonceSize, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
        !backup::EncryptOrDecrypt(true, password, bytes, plaintext)) {
        return std::nullopt;
    }
    return bytes;
}

inline std::optional<std::vector<Account>> LoadBackup(std::span<std::uint8_t> bytes,
                                                     std::wstring_view password) {
    if (bytes.size() <= backup::kHeaderSize + backup::kTagSize || bytes.size() > backup::kMaxFileSize) {
        return std::nullopt;
    }
    const auto magic = bytes.first<backup::kMagic.size()>();
    const bool favorites = std::ranges::equal(magic, backup::kFavoritesMagic);
    const unsigned version = favorites || std::ranges::equal(magic, backup::kMagic) ? 3 :
        std::ranges::equal(magic, backup::kTagsMagic) ? 2 :
        std::ranges::equal(magic, backup::kLegacyMagic) ? 1 : 0;
    const auto payloadSize = bytes.size() - backup::kHeaderSize - backup::kTagSize;
    if (!version || (version == 1 && payloadSize > backup::kMaxLegacyPayloadSize)) {
        return std::nullopt;
    }
    backup::Plaintext plaintext;
    plaintext.bytes.resize(payloadSize);
    if (!backup::EncryptOrDecrypt(false, password, bytes, plaintext)) {
        return std::nullopt;
    }
    return backup::Deserialize(plaintext.bytes, version, false, favorites);
}

enum class ExportFormat { Aegis, AndOtp };

namespace backup {

inline void JsonAdd(std::vector<std::uint8_t>& out, std::string_view text) {
    out.insert(out.end(), text.begin(), text.end());
}

inline void JsonName(std::vector<std::uint8_t>& out, std::wstring_view name) {
    // ValidName has already checked UTF-16 and ruled out control characters.
    std::array<char, 3 * kMaxNameLength> utf8;
    const auto count = name.empty() ? 0 : WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        name.data(), static_cast<int>(name.size()), utf8.data(), utf8.size(), nullptr, nullptr);
    out.push_back('"');
    for (int i = 0; i < count; ++i) {
        if (utf8[i] == '"' || utf8[i] == '\\') {
            out.push_back('\\');
        }
        out.push_back(static_cast<std::uint8_t>(utf8[i]));
    }
    out.push_back('"');
}

inline bool JsonSecret(std::vector<std::uint8_t>& out, const AccountSecret& secret, bool padding) {
    SecretBytes decoded;
    if (!secret.Read(decoded)) {
        return false;
    }
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    unsigned pending = 0;
    unsigned bits = 0;
    out.push_back('"');
    const auto start = out.size();
    for (const auto byte : decoded.View()) {
        pending = (pending << 8) | byte;
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            out.push_back(alphabet[(pending >> bits) & 31]);
        }
    }
    if (bits) {
        out.push_back(alphabet[(pending << (5 - bits)) & 31]);
    }
    if (padding) {
        out.insert(out.end(), (8 - (out.size() - start) % 8) % 8, '=');
    }
    out.push_back('"');
    return true;
}

inline std::optional<std::array<char, 36>> RandomUuid() {
    std::array<std::uint8_t, 16> random;
    if (BCryptGenRandom(nullptr, random.data(), random.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return std::nullopt;
    }
    random[6] = (random[6] & 0x0f) | 0x40;
    random[8] = (random[8] & 0x3f) | 0x80;
    constexpr std::string_view hex = "0123456789abcdef";
    std::array<char, 36> uuid;
    auto out = uuid.begin();
    for (std::size_t i = 0; i < random.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            *out++ = '-';
        }
        *out++ = hex[random[i] >> 4];
        *out++ = hex[random[i] & 0x0f];
    }
    return uuid;
}

inline void JsonUuid(std::vector<std::uint8_t>& out, const std::array<char, 36>& uuid) {
    out.push_back('"');
    JsonAdd(out, {uuid.data(), uuid.size()});
    out.push_back('"');
}

inline bool JsonOtp(std::vector<std::uint8_t>& out, const Account& account, ExportFormat format) {
    constexpr std::string_view algorithms[]{"SHA1", "SHA256", "SHA512"};
    JsonAdd(out, "\"secret\":");
    if (!JsonSecret(out, account.secret, format == ExportFormat::AndOtp)) {
        return false;
    }
    JsonAdd(out, format == ExportFormat::Aegis ? ",\"algo\":\"" : ",\"algorithm\":\"");
    JsonAdd(out, algorithms[static_cast<unsigned>(account.algorithm)]);
    JsonAdd(out, "\",\"digits\":");
    JsonAdd(out, std::to_string(account.digits));
    JsonAdd(out, ",\"period\":");
    JsonAdd(out, std::to_string(account.period));
    return true;
}

}  // namespace backup

inline std::optional<backup::Plaintext> CreatePortableExport(std::span<const Account> accounts,
                                                            ExportFormat format) {
    if (accounts.empty() || accounts.size() > kMaxAccounts || !std::ranges::all_of(accounts, ValidAccount)) {
        return std::nullopt;
    }
    const bool aegis = format == ExportFormat::Aegis;
    const auto tags = aegis ? AccountTags(accounts) : std::vector<std::wstring_view>{};
    std::vector<std::array<char, 36>> groupIds;
    groupIds.reserve(tags.size());
    for (std::size_t i = 0; i < tags.size(); ++i) {
        const auto uuid = backup::RandomUuid();
        if (!uuid) {
            return std::nullopt;
        }
        groupIds.push_back(*uuid);
    }

    // Reserve before emitting seeds so reallocation cannot leave an unwiped copy.
    constexpr std::size_t maxBase32 = ((kMaxSecretLength + 4) / 5) * 8;
    std::size_t capacity = 128;
    for (const auto& account : accounts) {
        capacity += 256 + maxBase32 + 3 * (account.label.size() + account.issuer.size());
        for (const auto& tag : account.tags) {
            capacity += 128 + 3 * tag.size();
        }
    }
    backup::Plaintext result;
    auto& out = result.bytes;
    out.reserve(capacity);
    backup::JsonAdd(out, aegis
        ? "{\"version\":1,\"header\":{\"slots\":null,\"params\":null},\"db\":{\"version\":3,\"entries\":["
        : "[");
    for (const auto& account : accounts) {
        if (&account != accounts.data()) {
            out.push_back(',');
        }
        if (aegis) {
            const auto uuid = backup::RandomUuid();
            if (!uuid) {
                return std::nullopt;
            }
            backup::JsonAdd(out, "{\"type\":\"totp\",\"uuid\":");
            backup::JsonUuid(out, *uuid);
            backup::JsonAdd(out, ",\"name\":");
        } else {
            backup::JsonAdd(out, "{\"type\":\"TOTP\",\"label\":");
        }
        backup::JsonName(out, account.label);
        backup::JsonAdd(out, ",\"issuer\":");
        backup::JsonName(out, account.issuer);
        backup::JsonAdd(out, aegis ? ",\"info\":{" : ",");
        if (!backup::JsonOtp(out, account, format)) {
            return std::nullopt;
        }
        backup::JsonAdd(out, aegis ? "},\"groups\":[" : ",\"tags\":[");
        for (const auto& tag : account.tags) {
            if (&tag != account.tags.data()) {
                out.push_back(',');
            }
            if (aegis) {
                const auto index = std::ranges::lower_bound(tags, tag, TagNameLess) - tags.begin();
                backup::JsonUuid(out, groupIds[index]);
            } else {
                backup::JsonName(out, tag);
            }
        }
        backup::JsonAdd(out, "]}");
    }
    if (aegis) {
        backup::JsonAdd(out, "],\"groups\":[");
        for (std::size_t i = 0; i < tags.size(); ++i) {
            if (i) {
                out.push_back(',');
            }
            backup::JsonAdd(out, "{\"uuid\":");
            backup::JsonUuid(out, groupIds[i]);
            backup::JsonAdd(out, ",\"name\":");
            backup::JsonName(out, tags[i]);
            out.push_back('}');
        }
        backup::JsonAdd(out, "]}}");
    } else {
        out.push_back(']');
    }
    out.push_back('\n');
    return result;
}

inline std::optional<std::vector<std::uint8_t>> ReadBackup(const std::wstring& path, bool local = false) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size;
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &size) ||
        size.QuadPart <= static_cast<LONGLONG>(backup::kHeaderSize + backup::kTagSize) ||
        size.QuadPart > static_cast<LONGLONG>(local ? backup::kMaxLocalFileSize : backup::kMaxFileSize)) {
        CloseHandle(file);
        return std::nullopt;
    }
    SensitiveBuffer<std::uint8_t, backup::kMagic.size()> magic;
    DWORD read;
    if (!ReadFile(file, magic.data.data(), static_cast<DWORD>(magic.data.size()), &read, nullptr) ||
        read != magic.data.size() || (magic.data != backup::kMagic &&
        (local ? magic.data != backup::kLocalMagic && magic.data != backup::kLocalFavoritesMagic &&
                 magic.data != backup::kWindowsVaultMagic && magic.data != backup::kWindowsFavoritesMagic :
                 magic.data != backup::kFavoritesMagic &&
                 magic.data != backup::kTagsMagic && magic.data != backup::kLegacyMagic))) {
        CloseHandle(file);
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
    std::ranges::copy(magic.data, bytes.begin());
    const auto remaining = std::span(bytes).subspan(magic.data.size());
    const bool ok = ReadFile(file, remaining.data(), static_cast<DWORD>(remaining.size()), &read, nullptr) &&
                    read == remaining.size();
    CloseHandle(file);
    return ok ? std::optional(std::move(bytes)) : std::nullopt;
}

inline constexpr std::size_t kMaxLinkFileSize = 1024 * 1024;

inline std::optional<backup::Plaintext> ReadLinkFile(const std::wstring& path) {
    const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    backup::Plaintext contents;
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > static_cast<LONGLONG>(kMaxLinkFileSize)) {
        CloseHandle(file);
        return std::nullopt;
    }
    contents.bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(file, contents.bytes.data(), static_cast<DWORD>(contents.bytes.size()), &read,
                             nullptr) && read == contents.bytes.size();
    CloseHandle(file);
    return ok ? std::optional(std::move(contents)) : std::nullopt;
}

inline std::optional<std::vector<Account>> ParseLinkFile(std::span<const std::uint8_t> data, bool html) {
    if (data.empty() || data.size() > kMaxLinkFileSize) {
        return std::nullopt;
    }
    const std::string_view text(reinterpret_cast<const char*>(data.data()), data.size());
    constexpr std::string_view scheme = "otpauth://totp/";
    std::vector<Account> accounts;
    std::size_t start = 0;
    for (;;) {
        const auto match = std::ranges::search(text.substr(start), scheme, [](char value, char expected) {
            return (value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value) == expected;
        });
        if (match.empty()) {
            break;
        }
        start = match.begin() - text.begin();
        const auto end = text.find_first_of(" \t\r\n<>\"'", start);
        const auto link = text.substr(start, end - start);
        start += link.size();
        if (link.size() > 6 * kMaxUriLength) {
            return std::nullopt;
        }
        SensitiveBuffer<char, kMaxUriLength + 1> utf8;
        for (std::size_t i = 0; i < link.size() && utf8.size < utf8.data.size(); ++i) {
            if (html && link[i] == '&') {
                for (const std::string_view entity : {"&amp;", "&#38;", "&#x26;", "&#X26;"}) {
                    if (link.substr(i).starts_with(entity)) {
                        i += entity.size() - 1;
                        break;
                    }
                }
                utf8.data[utf8.size++] = '&';
            } else {
                utf8.data[utf8.size++] = link[i];
            }
        }
        SensitiveBuffer<wchar_t, kMaxUriLength + 1> uri;
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data.data(),
            static_cast<int>(utf8.size), uri.data.data(), static_cast<int>(uri.data.size() - 1));
        if (!length || utf8.size > kMaxUriLength) {
            return std::nullopt;
        }
        auto account = ParseOtpAuth({uri.data.data(), static_cast<std::size_t>(length)});
        if (!account) {
            return std::nullopt;
        }
        if (std::ranges::none_of(accounts, [&](const auto& prior) {
                return SameName(prior.issuer, account->issuer) && SameName(prior.label, account->label);
            })) {
            if (accounts.size() == kMaxAccounts) {
                return std::nullopt;
            }
            accounts.push_back(std::move(*account));
        }
    }
    return accounts.empty() ? std::nullopt : std::optional(std::move(accounts));
}

inline bool WriteEncryptedFile(const std::wstring& path, std::span<const std::uint8_t> bytes,
                               bool replaceExisting) {
    std::uint64_t random;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&random), sizeof(random),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) {
        return false;
    }
    const auto temporary = path + L".tmp-" + std::to_wstring(random);
    const auto file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written;
    const bool ready = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                       written == bytes.size() && FlushFileBuffers(file);
    const bool closed = CloseHandle(file);
    const DWORD flags = MOVEFILE_WRITE_THROUGH | (replaceExisting ? MOVEFILE_REPLACE_EXISTING : 0);
    if (ready && closed && MoveFileExW(temporary.c_str(), path.c_str(), flags)) {
        return true;
    }
    DeleteFileW(temporary.c_str());
    return false;
}

inline bool WriteNewBackup(const std::wstring& path, std::span<const std::uint8_t> bytes) {
    return WriteEncryptedFile(path, bytes, false);
}

inline std::wstring LocalVaultPath(bool standalone) {
    wchar_t local[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, local))) {
        return {};
    }
    const auto filename = standalone ? L"\\standalone.2fa-vault" : L"\\windhawk.2fa-vault";
    const auto path = std::wstring(local) + L"\\TOTP-flyout" + filename;
    const auto legacy = std::wstring(local) + L"\\2fa-flyout" + filename;
    const auto mayExist = [](const std::wstring& candidate) {
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return true;
        }
        const auto error = GetLastError();
        return error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND;
    };
    return !mayExist(path) && mayExist(legacy) ? legacy : path;
}

class LocalVault {
public:
    bool Unlocked() const { return static_cast<bool>(lock_); }
    bool HasMasterPassword() const { return key_.Size() != 0; }

    static bool IsWindowsProtected(const std::wstring& path) {
        const auto file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        std::array<std::uint8_t, backup::kWindowsVaultMagic.size()> magic{};
        DWORD bytesRead{};
        const bool windows = ReadFile(file, magic.data(), static_cast<DWORD>(magic.size()), &bytesRead, nullptr) &&
                             bytesRead == magic.size() &&
                             (magic == backup::kWindowsVaultMagic || magic == backup::kWindowsFavoritesMagic);
        CloseHandle(file);
        return windows;
    }
    void Lock() {
        key_ = {};
        lock_.reset();
        path_.clear();
    }

    bool Create(const std::wstring& path, std::span<const Account> accounts, std::wstring_view password) {
        if (Unlocked() || (!password.empty() && password.size() < kMinLocalPasswordLength)) {
            return false;
        }
        const auto folder = path.substr(0, path.find_last_of(L"\\/"));
        if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return false;
        }
        auto lock = AcquireLock(path);
        if (!lock) {
            return false;
        }
        if (password.empty()) {
            if (!WriteWindowsVault(path, accounts, false)) {
                return false;
            }
            lock_ = std::move(lock);
            path_ = path;
            return true;
        }
        SecretBytes derived;
        if (BCryptGenRandom(nullptr, salt_.data(), salt_.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
            !backup::DeriveKey(password, salt_, derived)) {
            return false;
        }
        const auto file = Encrypt(accounts, derived);
        AccountSecret protectedKey;
        if (!file || !protectedKey.Store(std::move(derived)) || !WriteEncryptedFile(path, *file, false)) {
            return false;
        }
        key_ = std::move(protectedKey);
        lock_ = std::move(lock);
        path_ = path;
        return true;
    }

    std::optional<std::vector<Account>> Open(const std::wstring& path, std::wstring_view password) {
        if (Unlocked()) {
            return std::nullopt;
        }
        auto lock = AcquireLock(path);
        if (!lock) {
            return std::nullopt;
        }
        auto file = ReadBackup(path, true);
        if (!file) {
            return std::nullopt;
        }
        const auto magic = std::span(*file).first<backup::kMagic.size()>();
        const bool windowsFavorites = std::ranges::equal(magic, backup::kWindowsFavoritesMagic);
        if (windowsFavorites || std::ranges::equal(magic, backup::kWindowsVaultMagic)) {
            if (!password.empty()) {
                return std::nullopt;
            }
            auto accounts = ReadWindowsVault(std::span(*file).subspan(magic.size()), windowsFavorites);
            if (accounts) {
                lock_ = std::move(lock);
                path_ = path;
            }
            return accounts;
        }
        const bool legacy = std::ranges::equal(magic, backup::kMagic);
        if (file->size() > backup::kHeaderSize + backup::kMaxLocalPayloadSize + backup::kTagSize) {
            return std::nullopt;
        }
        auto salt = std::span(*file).subspan<backup::kMagic.size(), backup::kSaltSize>();
        SecretBytes derived;
        backup::Plaintext payload;
        payload.bytes.resize(file->size() - backup::kHeaderSize - backup::kTagSize);
        if (!backup::DeriveKey(password, salt, derived) || !backup::EncryptOrDecrypt(false, derived, *file, payload)) {
            return std::nullopt;
        }
        auto accounts = backup::Deserialize(payload.bytes, legacy ? 3 : 4, true,
                                            std::ranges::equal(magic, backup::kLocalFavoritesMagic));
        if (!accounts || !key_.Store(std::move(derived))) {
            return std::nullopt;
        }
        std::ranges::copy(salt, salt_.begin());
        lock_ = std::move(lock);
        path_ = path;
        return accounts;
    }

    bool Save(std::span<const Account> accounts) {
        if (!lock_) {
            return false;
        }
        if (!HasMasterPassword()) {
            return WriteWindowsVault(path_, accounts, true);
        }
        SecretBytes derived;
        if (!key_.Read(derived)) {
            return false;
        }
        const auto file = Encrypt(accounts, derived);
        derived.Clear();
        return file && WriteEncryptedFile(path_, *file, true);
    }

    bool UseWindowsProtection(std::span<const Account> accounts) {
        if (!Unlocked() || !HasMasterPassword() || !WriteWindowsVault(path_, accounts, true)) {
            return false;
        }
        key_ = {};
        return true;
    }

private:
    struct DpapiBlob {
        DATA_BLOB value{};
        ~DpapiBlob() {
            if (value.pbData) {
                SecureZeroMemory(value.pbData, value.cbData);
                LocalFree(value.pbData);
            }
        }
    };

    static bool WriteWindowsVault(const std::wstring& path, std::span<const Account> accounts, bool replace) {
        backup::Plaintext payload;
        const bool favorites = std::ranges::any_of(accounts, &Account::favorite);
        if (!backup::Serialize(accounts, payload, true, true, favorites)) {
            return false;
        }
        DATA_BLOB input{static_cast<DWORD>(payload.bytes.size()), payload.bytes.data()};
        DpapiBlob output;
        if (!CryptProtectData(&input, L"TOTP flyout local vault", nullptr, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &output.value)) {
            return false;
        }
        SecureZeroMemory(payload.bytes.data(), payload.bytes.size());
        auto& protectedData = output.value;
        const auto& magic = favorites ? backup::kWindowsFavoritesMagic : backup::kWindowsVaultMagic;
        std::vector<std::uint8_t> file(magic.begin(), magic.end());
        file.insert(file.end(), protectedData.pbData, protectedData.pbData + protectedData.cbData);
        return WriteEncryptedFile(path, file, replace);
    }

    static std::optional<std::vector<Account>> ReadWindowsVault(std::span<std::uint8_t> bytes, bool favorites) {
        DATA_BLOB input{static_cast<DWORD>(bytes.size()), bytes.data()};
        DpapiBlob output;
        if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &output.value) ||
            output.value.cbData > backup::kMaxLocalPayloadSize) {
            return std::nullopt;
        }
        return backup::Deserialize({output.value.pbData, output.value.cbData}, 4, true, favorites);
    }

    using FileLock = std::unique_ptr<void, decltype(&CloseHandle)>;

    static FileLock AcquireLock(const std::wstring& path) {
        const auto file = CreateFileW((path + L".lock").c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return FileLock(file == INVALID_HANDLE_VALUE ? nullptr : file, CloseHandle);
    }

    std::optional<std::vector<std::uint8_t>> Encrypt(std::span<const Account> accounts, SecretBytes& derived) const {
        backup::Plaintext payload;
        const bool favorites = std::ranges::any_of(accounts, &Account::favorite);
        if (!backup::Serialize(accounts, payload, true, true, favorites)) {
            return std::nullopt;
        }
        std::vector<std::uint8_t> file(backup::kHeaderSize + payload.bytes.size() + backup::kTagSize);
        std::ranges::copy(favorites ? backup::kLocalFavoritesMagic : backup::kLocalMagic, file.begin());
        std::ranges::copy(salt_, file.begin() + backup::kMagic.size());
        const auto nonce = std::span(file).first<backup::kHeaderSize>().last<backup::kNonceSize>();
        if (BCryptGenRandom(nullptr, nonce.data(), nonce.size(), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 ||
            !backup::EncryptOrDecrypt(true, derived, file, payload)) {
            return std::nullopt;
        }
        return file;
    }

    AccountSecret key_;
    std::array<std::uint8_t, backup::kSaltSize> salt_{};
    FileLock lock_{nullptr, CloseHandle};
    std::wstring path_;
};

inline bool WriteNewPortableExport(const std::wstring& path, std::span<const std::uint8_t> bytes) {
    // Write plaintext only to the path the user chose. An interrupted process can
    // leave that file incomplete, but cannot leave a second file with setup keys.
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD written = 0;
    const bool ready = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                       written == bytes.size() && FlushFileBuffers(file);
    if (!ready) {
        FILE_DISPOSITION_INFO disposition{TRUE};
        SetFileInformationByHandle(file, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    const bool closed = CloseHandle(file);
    return ready && closed;
}

}  // namespace twofa
