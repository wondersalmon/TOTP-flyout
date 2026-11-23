#include "../src/totp.hpp"

#include <cstdio>
#include <cstring>
#include <string_view>

int main() {
    using twofa::Algorithm;
    using twofa::CalculateTotp;

    // Public test material from RFC 6238, Appendix B, and its reference errata.
    constexpr char testSecret[] =
        "1234567890123456789012345678901234567890123456789012345678901234";
    struct Vector {
        std::uint64_t time;
        std::wstring_view sha1;
        std::wstring_view sha256;
        std::wstring_view sha512;
    };
    constexpr Vector vectors[] = {
        {59, L"94287082", L"46119246", L"90693936"},
        {1111111109, L"07081804", L"68084774", L"25091201"},
        {1111111111, L"14050471", L"67062674", L"99943326"},
        {1234567890, L"89005924", L"91819424", L"93441116"},
        {2000000000, L"69279037", L"90698825", L"38618901"},
        {20000000000, L"65353130", L"77737706", L"47863826"},
    };
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(testSecret);

    for (const auto& vector : vectors) {
        const auto sha1 = CalculateTotp({bytes, 20}, vector.time, Algorithm::Sha1, 8);
        const auto sha256 = CalculateTotp({bytes, 32}, vector.time, Algorithm::Sha256, 8);
        const auto sha512 = CalculateTotp({bytes, 64}, vector.time, Algorithm::Sha512, 8);
        if (!sha1 || !sha256 || !sha512 || sha1->text.data() != vector.sha1 ||
            sha256->text.data() != vector.sha256 || sha512->text.data() != vector.sha512) {
            std::fprintf(stderr, "RFC 6238 mismatch at timestamp %llu\n",
                         static_cast<unsigned long long>(vector.time));
            return 1;
        }
    }

    const auto sixDigits = CalculateTotp({bytes, 20}, 59);
    const auto nextPeriod = CalculateTotp({bytes, 20}, 60);
    if (!sixDigits || !nextPeriod || sixDigits->text.data() != std::wstring_view(L"287082") ||
        sixDigits->secondsRemaining != 1 || nextPeriod->secondsRemaining != 30 ||
        sixDigits->text == nextPeriod->text || CalculateTotp({}, 59) ||
        CalculateTotp({bytes, 20}, 59, Algorithm::Sha1, 9) ||
        CalculateTotp({bytes, 20}, 59, Algorithm::Sha1, 6, 0)) {
        std::fputs("TOTP parameter or period boundary check failed\n", stderr);
        return 1;
    }

    std::puts("18 RFC 6238 vectors and TOTP parameter and period checks passed.");
}
