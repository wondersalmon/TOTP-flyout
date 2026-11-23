#pragma once

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace twofa {

enum class Algorithm { Sha1, Sha256, Sha512 };

struct TotpCode {
    std::array<wchar_t, 9> text{};
    unsigned secondsRemaining;

    ~TotpCode() { SecureZeroMemory(text.data(), sizeof(text)); }
};

inline std::optional<TotpCode> CalculateTotp(std::span<const std::uint8_t> secret,
                                            std::uint64_t unixSeconds,
                                            Algorithm algorithm = Algorithm::Sha1,
                                            unsigned digits = 6,
                                            unsigned period = 30) {
    if (secret.empty() || secret.size() > (std::numeric_limits<ULONG>::max)() ||
        digits < 6 || digits > 8 || period == 0) {
        return std::nullopt;
    }

    BCRYPT_ALG_HANDLE provider;
    ULONG digestLength;
    switch (algorithm) {
        case Algorithm::Sha1:
            provider = BCRYPT_HMAC_SHA1_ALG_HANDLE;
            digestLength = 20;
            break;
        case Algorithm::Sha256:
            provider = BCRYPT_HMAC_SHA256_ALG_HANDLE;
            digestLength = 32;
            break;
        case Algorithm::Sha512:
            provider = BCRYPT_HMAC_SHA512_ALG_HANDLE;
            digestLength = 64;
            break;
        default:
            return std::nullopt;
    }

    std::array<std::uint8_t, 8> message{};
    auto counter = unixSeconds / period;
    for (auto byte = message.rbegin(); byte != message.rend(); ++byte) {
        *byte = static_cast<std::uint8_t>(counter);
        counter >>= 8;
    }

    std::array<std::uint8_t, 64> digest{};
    // BCryptHash treats the secret as input despite the non-const API type.
    const auto status = BCryptHash(provider, const_cast<PUCHAR>(secret.data()),
                                   static_cast<ULONG>(secret.size()), message.data(),
                                   static_cast<ULONG>(message.size()), digest.data(),
                                   digestLength);
    if (status < 0) {
        SecureZeroMemory(digest.data(), digest.size());
        return std::nullopt;
    }

    const auto offset = digest[digestLength - 1] & 0x0Fu;
    auto value = (static_cast<std::uint32_t>(digest[offset] & 0x7F) << 24) |
                 (static_cast<std::uint32_t>(digest[offset + 1]) << 16) |
                 (static_cast<std::uint32_t>(digest[offset + 2]) << 8) |
                 static_cast<std::uint32_t>(digest[offset + 3]);
    SecureZeroMemory(digest.data(), digest.size());

    TotpCode result{.secondsRemaining = period - static_cast<unsigned>(unixSeconds % period)};
    for (auto position = digits; position > 0; --position) {
        result.text[position - 1] = static_cast<wchar_t>(L'0' + value % 10);
        value /= 10;
    }
    return result;
}

}  // namespace twofa
