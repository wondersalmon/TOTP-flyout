#pragma once

#include "totp.hpp"

#include <dpapi.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace twofa {

inline constexpr std::size_t kMaxAccounts = 256;
inline constexpr std::size_t kMaxTags = 16;
inline constexpr std::size_t kMaxNameLength = 128;
inline constexpr std::size_t kMaxUriLength = 4096;
inline constexpr std::size_t kMaxSecretLength = 128;

// Fixed storage keeps secrets out of reallocating strings and wipes moved-from values.
template <typename T, std::size_t Capacity>
struct SensitiveBuffer {
    std::array<T, Capacity> data{};
    std::size_t size = 0;

    SensitiveBuffer() = default;
    SensitiveBuffer(const SensitiveBuffer&) = delete;
    SensitiveBuffer& operator=(const SensitiveBuffer&) = delete;
    SensitiveBuffer(SensitiveBuffer&& other) noexcept { *this = std::move(other); }
    SensitiveBuffer& operator=(SensitiveBuffer&& other) noexcept {
        if (this != &other) {
            Clear();
            data = other.data;
            size = other.size;
            other.Clear();
        }
        return *this;
    }
    ~SensitiveBuffer() { Clear(); }

    void Clear() {
        SecureZeroMemory(data.data(), sizeof(data));
        size = 0;
    }

    std::span<const T> View() const { return {data.data(), size}; }
};

using SecretBytes = SensitiveBuffer<std::uint8_t, kMaxSecretLength>;

class AccountSecret {
public:
    AccountSecret() = default;
    AccountSecret(const AccountSecret& other) { *this = other; }
    AccountSecret& operator=(const AccountSecret& other) {
        if (this != &other) {
            // DPAPI ciphertext stays usable in another buffer in the same process.
            protected_.data = other.protected_.data;
            protected_.size = other.protected_.size;
        }
        return *this;
    }
    AccountSecret(AccountSecret&&) noexcept = default;
    AccountSecret& operator=(AccountSecret&&) noexcept = default;

    bool Store(SecretBytes value) {
        if (!value.size || value.size > value.data.size() ||
            !CryptProtectMemory(value.data.data(), value.data.size(), CRYPTPROTECTMEMORY_SAME_PROCESS)) {
            return false;
        }
        protected_ = std::move(value);
        return true;
    }

    bool Read(SecretBytes& plaintext) const {
        plaintext.Clear();
        if (!protected_.size) {
            return false;
        }
        plaintext.data = protected_.data;
        if (!CryptUnprotectMemory(plaintext.data.data(), plaintext.data.size(), CRYPTPROTECTMEMORY_SAME_PROCESS)) {
            plaintext.Clear();
            return false;
        }
        plaintext.size = protected_.size;
        return true;
    }

    std::size_t Size() const { return protected_.size; }

private:
    static_assert(kMaxSecretLength % CRYPTPROTECTMEMORY_BLOCK_SIZE == 0);
    SecretBytes protected_;
};

struct Account {
    std::wstring issuer;
    std::wstring label;
    std::vector<std::wstring> tags;
    // A zero entry uses the automatic marker color. Other entries are COLORREF + 1.
    std::array<std::uint32_t, kMaxTags> tagColors{};
    AccountSecret secret;
    Algorithm algorithm = Algorithm::Sha1;
    unsigned digits = 6;
    unsigned period = 30;
    std::uint32_t useCount = 0;
    std::uint64_t lastUsed = 0;
};

inline std::optional<TotpCode> CalculateAccountCode(const Account& account, std::uint64_t unixSeconds) {
    SecretBytes plaintext;
    if (!account.secret.Read(plaintext)) {
        return std::nullopt;
    }
    return CalculateTotp(plaintext.View(), unixSeconds, account.algorithm, account.digits, account.period);
}

inline std::wstring_view TrimName(std::wstring_view value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos) {
        return {};
    }
    return value.substr(first, value.find_last_not_of(L" \t\r\n") - first + 1);
}

inline bool SameName(std::wstring_view a, std::wstring_view b) {
    if (a.empty() || b.empty()) {
        return a.empty() && b.empty();
    }
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

inline bool ValidName(std::wstring_view value) {
    return value.size() <= kMaxNameLength && TrimName(value) == value &&
           (value.empty() || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                                 static_cast<int>(value.size()), nullptr, 0,
                                                 nullptr, nullptr) > 0) &&
           std::ranges::none_of(value, [](wchar_t ch) {
               return ch < 32 || (ch >= 0x7f && ch <= 0x9f) ||
                      (ch >= 0x202a && ch <= 0x202e) || (ch >= 0x2066 && ch <= 0x2069);
           });
}

inline bool ValidTags(std::span<const std::wstring> tags) {
    if (tags.size() > kMaxTags) {
        return false;
    }
    for (std::size_t i = 0; i < tags.size(); ++i) {
        const auto& tag = tags[i];
        if (tag.empty() || !ValidName(tag) ||
            std::ranges::any_of(tags.first(i), [&](auto& prior) { return SameName(prior, tag); })) {
            return false;
        }
    }
    return true;
}

inline bool ValidAccount(const Account& account) {
    return !account.label.empty() && ValidName(account.issuer) && ValidName(account.label) &&
           ValidTags(account.tags) &&
           std::ranges::all_of(account.tagColors, [](auto color) { return color <= 0x1000000; }) &&
           account.secret.Size() >= 10 &&
           (account.algorithm == Algorithm::Sha1 || account.algorithm == Algorithm::Sha256 ||
            account.algorithm == Algorithm::Sha512) &&
           (account.digits == 6 || account.digits == 8) &&
           account.period >= 1 && account.period <= 3600;
}

inline std::uint32_t NameColorHash(std::wstring_view name) {
    wchar_t upper[kMaxNameLength];
    const auto length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, name.data(),
                                      static_cast<int>(name.size()), upper, ARRAYSIZE(upper),
                                      nullptr, nullptr, 0);
    if (length) {
        name = {upper, static_cast<std::size_t>(length)};
    }
    std::uint32_t hash = 2166136261u;
    for (const auto ch : name) {
        hash = (hash ^ ch) * 16777619u;
    }
    return hash;
}

inline bool TagNameLess(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
                                b.data(), static_cast<int>(b.size()), TRUE) == CSTR_LESS_THAN;
}

inline std::vector<std::wstring_view> AccountTags(std::span<const Account> accounts) {
    std::vector<std::wstring_view> tags;
    for (const auto& account : accounts) {
        tags.insert(tags.end(), account.tags.begin(), account.tags.end());
    }
    std::ranges::stable_sort(tags, TagNameLess);
    const auto duplicates = std::ranges::unique(tags, SameName);
    tags.erase(duplicates.begin(), duplicates.end());
    return tags;
}

inline bool HasTag(const Account& account, std::wstring_view name) {
    return std::ranges::any_of(account.tags, [&](const auto& tag) { return SameName(tag, name); });
}

// A missing filter shows all accounts. An empty filter shows accounts without tags.
inline bool MatchesTag(const Account& account, std::optional<std::wstring_view> filter) {
    return !filter || (filter->empty() ? account.tags.empty() : HasTag(account, *filter));
}

enum class AccountSort : unsigned char { Manual, NameAscending, NameDescending, Provider, MostUsed, RecentlyUsed };

inline std::wstring_view AccountDisplayName(std::wstring_view issuer, std::wstring_view label) {
    if (issuer.empty()) {
        return label;
    }
    while (label.size() > issuer.size() && SameName(label.substr(0, issuer.size()), issuer)) {
        const auto suffix = TrimName(label.substr(issuer.size()));
        if (suffix.empty() || (suffix.front() != L':' && suffix.front() != L'_')) {
            break;
        }
        const auto rest = TrimName(suffix.substr(1));
        if (rest.empty()) {
            break;
        }
        label = rest;
    }
    return label;
}

inline int CompareAccountNames(std::wstring_view left, std::wstring_view right) {
    if (left.empty() || right.empty()) {
        return left.empty() == right.empty() ? CSTR_EQUAL : left.empty() ? CSTR_LESS_THAN : CSTR_GREATER_THAN;
    }
    const auto order = CompareStringEx(LOCALE_NAME_USER_DEFAULT, NORM_IGNORECASE | SORT_DIGITSASNUMBERS,
                                       left.data(), static_cast<int>(left.size()),
                                       right.data(), static_cast<int>(right.size()), nullptr, nullptr, 0);
    return order ? order : CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                                right.data(), static_cast<int>(right.size()), TRUE);
}

inline std::vector<std::size_t> AccountRows(std::span<const Account> accounts,
                                            std::optional<std::wstring_view> filter, AccountSort sort) {
    std::vector<std::size_t> rows;
    rows.reserve(accounts.size());
    for (std::size_t i = 0; i < accounts.size(); ++i) {
        if (MatchesTag(accounts[i], filter)) {
            rows.push_back(i);
        }
    }
    if (sort != AccountSort::Manual) {
        std::stable_sort(rows.begin(), rows.end(), [&](std::size_t i, std::size_t j) {
            const auto& left = accounts[i];
            const auto& right = accounts[j];
            if (sort == AccountSort::Provider) {
                if (const auto order = CompareAccountNames(left.issuer, right.issuer); order != CSTR_EQUAL) {
                    return order == CSTR_LESS_THAN;
                }
            }
            if (sort == AccountSort::MostUsed && left.useCount != right.useCount) {
                return left.useCount > right.useCount;
            }
            if ((sort == AccountSort::MostUsed || sort == AccountSort::RecentlyUsed) && left.lastUsed != right.lastUsed) {
                return left.lastUsed > right.lastUsed;
            }
            const auto order = CompareAccountNames(AccountDisplayName(left.issuer, left.label),
                                                   AccountDisplayName(right.issuer, right.label));
            return order == (sort == AccountSort::NameDescending ? CSTR_GREATER_THAN : CSTR_LESS_THAN);
        });
    }
    return rows;
}

inline bool MoveAccount(std::span<Account> accounts, std::size_t source, std::size_t target,
                        std::optional<std::wstring_view> filter) {
    if (source == target || source >= accounts.size() || target >= accounts.size() ||
        !MatchesTag(accounts[source], filter) || !MatchesTag(accounts[target], filter)) {
        return false;
    }
    const int step = source < target ? 1 : -1;
    for (auto next = static_cast<int>(source) + step; ; next += step) {
        if (MatchesTag(accounts[next], filter)) {
            std::swap(accounts[source], accounts[next]);
            source = next;
        }
        if (next == static_cast<int>(target)) {
            return true;
        }
    }
}

struct RefreshTime {
    unsigned seconds;
    unsigned period;
};

inline std::optional<RefreshTime> NextRefresh(std::span<const Account> accounts,
                                              std::optional<std::wstring_view> tag,
                                              std::uint64_t unixSeconds) {
    std::optional<RefreshTime> next;
    for (const auto& account : accounts) {
        if (!MatchesTag(account, tag) || !account.period) {
            continue;
        }
        const auto seconds = account.period - static_cast<unsigned>(unixSeconds % account.period);
        if (!next || seconds < next->seconds || (seconds == next->seconds && account.period < next->period)) {
            next = RefreshTime{seconds, account.period};
        }
    }
    return next;
}

inline std::optional<std::size_t> MergeAccounts(std::vector<Account>& current,
                                                std::vector<Account> incoming) {
    std::vector<Account> additions;
    for (auto& candidate : incoming) {
        const auto sameAccount = [&](const Account& account) {
            return SameName(account.issuer, candidate.issuer) &&
                SameName(AccountDisplayName(account.issuer, account.label), AccountDisplayName(candidate.issuer, candidate.label));
        };
        if (!std::ranges::any_of(current, sameAccount) && !std::ranges::any_of(additions, sameAccount)) {
            additions.push_back(std::move(candidate));
        }
    }
    if (current.size() > kMaxAccounts || additions.size() > kMaxAccounts - current.size()) {
        return std::nullopt;
    }
    for (auto& account : additions) {
        current.push_back(std::move(account));
    }
    return additions.size();
}

inline int HexDigit(wchar_t value) {
    if (value >= L'0' && value <= L'9') {
        return value - L'0';
    }
    value |= 0x20;
    return value >= L'a' && value <= L'f' ? value - L'a' + 10 : -1;
}

inline std::optional<std::wstring> DecodeName(std::wstring_view source) {
    std::wstring result;
    for (std::size_t pos = 0; pos < source.size();) {
        if (source[pos] != L'%') {
            result.push_back(source[pos++]);
            continue;
        }
        std::string bytes;
        do {
            if (source.size() - pos < 3) {
                return std::nullopt;
            }
            const int high = HexDigit(source[pos + 1]);
            const int low = HexDigit(source[pos + 2]);
            if (high < 0 || low < 0) {
                return std::nullopt;
            }
            bytes.push_back(static_cast<char>(high * 16 + low));
            pos += 3;
        } while (pos < source.size() && source[pos] == L'%');

        const auto length = static_cast<int>(bytes.size());
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                               bytes.data(), length, nullptr, 0);
        if (!count) {
            return std::nullopt;
        }
        const auto initialSize = result.size();
        result.resize(initialSize + count);
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), length,
                                 result.data() + initialSize, count)) {
            return std::nullopt;
        }
    }
    return std::wstring(TrimName(result));
}

enum class SetupKeyFormat { Uri, Manual };

inline bool ParseBase32(std::wstring_view input, SecretBytes& secret, SetupKeyFormat format = SetupKeyFormat::Uri) {
    unsigned pending = 0;
    unsigned bits = 0;
    unsigned symbols = 0;
    unsigned padding = 0;
    for (std::size_t pos = 0; pos < input.size(); ++pos) {
        wchar_t ch = input[pos];
        if (format == SetupKeyFormat::Manual && std::wstring_view(L" -\t\r\n").find(ch) != std::wstring_view::npos) {
            continue;
        }
        if (format == SetupKeyFormat::Uri && ch == L'%' && input.size() - pos >= 3) {
            const int high = HexDigit(input[pos + 1]);
            const int low = HexDigit(input[pos + 2]);
            if (high < 0 || low < 0) {
                return false;
            }
            ch = static_cast<wchar_t>(high * 16 + low);
            pos += 2;
        }
        if (ch == L'=') {
            ++padding;
            continue;
        }
        if (ch >= L'a' && ch <= L'z') {
            ch -= L'a' - L'A';
        }
        int value = -1;
        if (ch >= L'A' && ch <= L'Z') {
            value = ch - L'A';
        } else if (ch >= L'2' && ch <= L'7') {
            value = ch - L'2' + 26;
        }
        if (value < 0 || padding) {
            return false;
        }
        ++symbols;
        pending = (pending << 5) | static_cast<unsigned>(value);
        bits += 5;
        if (bits >= 8) {
            if (secret.size == secret.data.size()) {
                return false;
            }
            bits -= 8;
            secret.data[secret.size++] = static_cast<std::uint8_t>(pending >> bits);
            pending &= (1u << bits) - 1;
        }
    }
    const unsigned remainder = symbols % 8;
    return pending == 0 && bits < 5 && (!padding || (remainder && remainder + padding == 8));
}

inline std::optional<unsigned> ParseNumber(std::wstring_view text) {
    if (text.empty() || text.size() > 4) {
        return std::nullopt;
    }
    unsigned result = 0;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') {
            return std::nullopt;
        }
        result = result * 10 + ch - L'0';
    }
    return result;
}

inline std::optional<Account> ParseOtpAuth(std::wstring_view uri) {
    constexpr std::wstring_view prefix = L"otpauth://totp/";
    uri = TrimName(uri);
    if (uri.size() > kMaxUriLength || uri.size() < prefix.size() ||
        !SameName(uri.substr(0, prefix.size()), prefix) || uri.contains(L'#')) {
        return std::nullopt;
    }
    uri.remove_prefix(prefix.size());
    const auto queryStart = uri.find(L'?');
    if (queryStart == std::wstring_view::npos) {
        return std::nullopt;
    }
    const auto label = DecodeName(uri.substr(0, queryStart));
    if (!label) {
        return std::nullopt;
    }
    Account account;
    const auto separator = label->find(L':');
    if (separator == std::wstring::npos) {
        account.label = *label;
    } else {
        account.issuer = TrimName(std::wstring_view(*label).substr(0, separator));
        account.label = TrimName(std::wstring_view(*label).substr(separator + 1));
    }

    constexpr std::wstring_view parameters[] = {L"secret", L"issuer", L"algorithm", L"digits", L"period"};
    std::array<std::optional<std::wstring_view>, std::size(parameters)> values;
    auto query = uri.substr(queryStart + 1);
    while (!query.empty()) {
        const auto end = query.find(L'&');
        const auto pair = query.substr(0, end);
        const auto equals = pair.find(L'=');
        if (equals == std::wstring_view::npos) {
            return std::nullopt;
        }
        const auto name = pair.substr(0, equals);
        const auto param = std::ranges::find_if(parameters, [&](auto spelling) { return SameName(spelling, name); });
        if (param != std::end(parameters)) {
            auto& value = values[param - parameters];
            if (value) {
                return std::nullopt;
            }
            value = pair.substr(equals + 1);
        }
        query = end == std::wstring_view::npos ? std::wstring_view{} : query.substr(end + 1);
    }
    const auto& [secret, issuer, algorithm, digits, period] = values;
    if (!secret) {
        return std::nullopt;
    }
    if (issuer) {
        const auto decoded = DecodeName(*issuer);
        if (!decoded || decoded->empty() || (!account.issuer.empty() && !SameName(account.issuer, *decoded))) {
            return std::nullopt;
        }
        account.issuer = *decoded;
    }
    account.label = std::wstring(AccountDisplayName(account.issuer, account.label));
    if (algorithm) {
        if (SameName(*algorithm, L"SHA256")) {
            account.algorithm = Algorithm::Sha256;
        } else if (SameName(*algorithm, L"SHA512")) {
            account.algorithm = Algorithm::Sha512;
        } else if (!SameName(*algorithm, L"SHA1")) {
            return std::nullopt;
        }
    }
    account.digits = digits ? ParseNumber(*digits).value_or(0) : account.digits;
    account.period = period ? ParseNumber(*period).value_or(0) : account.period;
    SecretBytes decodedSecret;
    if (!ParseBase32(*secret, decodedSecret) || !account.secret.Store(std::move(decodedSecret))) {
        return std::nullopt;
    }
    return ValidAccount(account) ? std::optional(std::move(account)) : std::nullopt;
}

}  // namespace twofa
