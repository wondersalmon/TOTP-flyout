#include "../src/backup.hpp"
#include "../src/providers.hpp"

#include <cstdio>

namespace {

constexpr std::wstring_view kExample =
    L"otpauth://totp/GitHub:Demo%20%C3%84?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
    L"&issuer=github&digits=8&period=60&algorithm=SHA256";
constexpr std::wstring_view kPassword = L"Public fixture password";
constexpr std::array<std::uint8_t, 20> kPublicSecret{
    '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};

bool ImportAndTags() {
    using namespace twofa;
    auto account = ParseOtpAuth(kExample);
    const auto labelOnly = ParseOtpAuth(
        L"otpauth://totp/GitLab:Demo?secret=gez dgnbvgy3tqojqgez dgnbvgy3tqojq");
    if (!account || labelOnly || account->label != L"Demo \u00c4" || account->secret.Size() != 20 ||
        account->algorithm != Algorithm::Sha256 ||
        account->digits != 8 || account->period != 60 ||
        account->issuer != L"github") {
        return false;
    }

    constexpr std::wstring_view invalid[] = {
        L"otpauth://hotp/GitHub:Demo?secret=GEZDGNBVGY3TQOJQ&counter=0",
        L"otpauth://totp/GitHub:Demo?secret=GEZDGNBVGY3TQOJQ&issuer=GitLab",
        L"otpauth://totp/Account?secret=GEZDGNBVGY3TQOJQ&secret=GEZDGNBVGY3TQOJQ",
        L"otpauth://totp/%00?secret=GEZDGNBVGY3TQOJQ",
        L"otpauth://totp/%FF?secret=GEZDGNBVGY3TQOJQ",
        L"otpauth://totp/Account?secret=GEZDGNBVGY3TQOJQ2",
        L"otpauth://totp/Account?secret=GEZDGNBVGY3TQOJQ&period=0",
        L"otpauth://totp/Account?secret=GEZDGNBVGY3TQOJQ&digits=7",
    };
    for (const auto link : invalid) {
        if (ParseOtpAuth(link)) {
            return false;
        }
    }

    account->tags = {L"Work", L"Shared"};
    auto another = ParseOtpAuth(
        L"otpauth://totp/GitLab:Demo?secret=GEZDGNBVGY3TQOJQ");
    if (!another) {
        return false;
    }
    another->tags = {L"work"};
    std::vector<Account> current;
    current.push_back(std::move(*account));
    current.push_back(std::move(*another));

    auto duplicate = ParseOtpAuth(kExample);
    duplicate->tags = {L"Personal"};
    std::vector<Account> imported;
    imported.push_back(std::move(*duplicate));
    const auto merged = MergeAccounts(current, std::move(imported));
    const auto tags = AccountTags(current);
    const auto next = NextRefresh(current, L"WORK", 65);
    const auto boundary = NextRefresh(current, std::nullopt, 60);
    return merged == 0 && current.size() == 2 && current[0].tags == std::vector<std::wstring>{L"Work", L"Shared"} &&
           tags == std::vector<std::wstring_view>{L"Shared", L"Work"} &&
           MatchesTag(current[0], L"WORK") && MatchesTag(current[0], L"shared") &&
           !MatchesTag(current[1], L"Personal") && !MatchesTag(current[0], L"") &&
           MatchesTag(current[0], std::nullopt) && MatchesTag(Account{}, L"") &&
           next && next->seconds == 25 && next->period == 30 &&
           boundary && boundary->seconds == 30 && boundary->period == 30 &&
           !NextRefresh(current, L"Personal", 65);
}

bool ProtectedAccountSecret() {
    using namespace twofa;
    auto account = ParseOtpAuth(kExample);
    if (!account) {
        return false;
    }
    Account moved = std::move(*account);
    const auto representation = std::as_bytes(std::span(&moved.secret, 1));
    const auto publicBytes = std::as_bytes(std::span(kPublicSecret));
    SecretBytes plaintext;
    const auto code = CalculateAccountCode(moved, 59);
    return !account->secret.Size() && !account->secret.Read(plaintext) &&
           std::ranges::search(representation, publicBytes).empty() &&
           moved.secret.Read(plaintext) && std::ranges::equal(plaintext.View(), kPublicSecret) &&
           code && std::wstring_view(code->text.data()) == L"74875740";
}

bool ManualOrderAndSetupKey() {
    using namespace twofa;
    SecretBytes key;
    if (!ParseBase32(L"gezd-gnbv gy3t-qojq gezd gnbv-gy3t qojq", key, SetupKeyFormat::Manual) ||
        !std::ranges::equal(key.View(), kPublicSecret)) {
        return false;
    }
    SecretBytes invalid;
    if (ParseBase32(L"%47EZDGNBVGY3TQOJQ", invalid, SetupKeyFormat::Manual)) {
        return false;
    }
    std::vector<Account> accounts;
    const auto add = [&](PCWSTR issuer, PCWSTR name, PCWSTR tag) {
        auto account = ParseOtpAuth(kExample);
        account->issuer = issuer;
        account->label = name;
        account->tags = {tag};
        accounts.push_back(std::move(*account));
    };
    add(L"Example", L"Account 10", L"Work");
    add(L"Another", L"Account 4", L"Personal");
    add(L"Example", L"Account 2", L"WORK");
    accounts.back().tags.push_back(L"Personal");
    add(L"Example", L"Account 12", L"Work");
    accounts[0].useCount = 6;
    accounts[0].lastUsed = 100;
    accounts[2].useCount = 1;
    accounts[2].lastUsed = 200;
    if (AccountRows(accounts, L"work", AccountSort::NameAscending) != std::vector<std::size_t>{2, 0, 3} ||
        AccountRows(accounts, L"work", AccountSort::NameDescending) != std::vector<std::size_t>{3, 0, 2} ||
        AccountRows(accounts, L"work", AccountSort::MostUsed) != std::vector<std::size_t>{0, 2, 3} ||
        AccountRows(accounts, L"work", AccountSort::RecentlyUsed) != std::vector<std::size_t>{2, 0, 3} ||
        AccountRows(accounts, std::nullopt, AccountSort::Provider) != std::vector<std::size_t>{1, 2, 0, 3} ||
        !MoveAccount(accounts, 0, 3, L"work") || MoveAccount(accounts, 0, 1, L"Work") ||
        accounts[0].label != L"Account 2" || accounts[1].label != L"Account 4" ||
        accounts[2].label != L"Account 12" || accounts[3].label != L"Account 10") {
        return false;
    }
    backup::Plaintext payload;
    if (!backup::Serialize(accounts, payload)) {
        return false;
    }
    const auto restored = backup::Deserialize(payload.bytes);
    return restored && std::ranges::equal(accounts, *restored, {}, &Account::label, &Account::label) &&
           AccountRows(*restored, L"work", AccountSort::Manual) == std::vector<std::size_t>{0, 2, 3};
}

bool ProviderMonograms() {
    using namespace twofa;
    return ProviderMonogram(L"  github.COM ") == L"GH" && ProviderMonogram(L"gitlab.COM") == L"GL" &&
           ProviderMonogram(L"Discord") == L"DC" && ProviderMonogram(L"github.com.invalid") == L"GC" &&
           ProviderMonogram(L"Example Service") == L"ES" && ProviderMonogram(L"Example") == L"EX" &&
           ProviderMonogram(L"1Password") == L"1P" && ProviderMonogram(L"\u044f\u043d\u0434\u0435\u043a\u0441") == L"\u042f\u041d" &&
           ProviderMonogram(L"\u5fae\u8f6f") == L"\u5fae\u8f6f" && ProviderMonogram(L"\U0001f680a") == L"A" &&
           ProviderMonogram(L"---").empty() && ProviderMonogram(L"").empty();
}

bool LinkFileImports() {
    using namespace twofa;
    constexpr std::string_view txt =
        "otpauth://totp/Example:first?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\n"
        "otpauth://totp/Example:second?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    constexpr std::string_view html =
        "<a href=\"otpauth://totp/Example:third?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
        "&amp;issuer=Example&#38;digits=8&#x26;period=60\">Account</a>";
    const auto parse = [](std::string_view text, bool isHtml) {
        return ParseLinkFile({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, isHtml);
    };
    const auto textAccounts = parse(txt, false);
    const auto htmlAccounts = parse(html, true);
    const auto mixed = std::string(txt) + "\nOTPAUTH://TOTP/Example:invalid?secret=1";
    const auto repeated = ParseOtpAuth(L"otpauth://totp/Discord%3Adiscord%3Afixture%40example.invalid"
                                       L"?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=Discord");
    const auto gitlab = ParseOtpAuth(L"otpauth://totp/gitlab.com%3Agitlab.com%3Agitlab.com_fixture%40example.invalid"
                                    L"?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ&issuer=gitlab.com");
    return textAccounts && textAccounts->size() == 2 && textAccounts->at(1).label == L"second" &&
           htmlAccounts && htmlAccounts->size() == 1 && htmlAccounts->front().digits == 8 &&
           htmlAccounts->front().period == 60 && htmlAccounts->front().issuer == L"Example" &&
           repeated && repeated->label == L"fixture@example.invalid" && repeated->issuer == L"Discord" &&
           gitlab && gitlab->label == L"fixture@example.invalid" && gitlab->issuer == L"gitlab.com" &&
           AccountDisplayName(L"gitlab.com", L"GITLAB.COM_fixture") == L"fixture" &&
           AccountDisplayName(L"gitlab.com", L"gitlab.com.my_fixture") == L"gitlab.com.my_fixture" &&
           AccountDisplayName(L"team_name", L"team_name_fixture") == L"fixture" &&
           AccountDisplayName(L"Discord", L"Discord:previous") == L"previous" &&
           AccountDisplayName(L"Discord", L"Different:name") == L"Different:name" &&
           !parse(mixed, false) && !parse("<p>No setup links</p>", true);
}

bool Backups() {
    using namespace twofa;
    auto account = ParseOtpAuth(kExample);
    account->tags = {L"Team \u043D\u0430\u0437\u0432\u0430\u043D\u0438\u0435", L"Other"};
    std::vector<Account> original;
    account->tagColors[0] = RGB(12, 140, 180) + 1;
    account->favorite = true;
    original.push_back(std::move(*account));
    auto encrypted = CreateBackup(original, kPassword);
    auto encryptedAgain = CreateBackup(original, kPassword);
    if (!encrypted || !encryptedAgain || encrypted == encryptedAgain || CreateBackup(original, L"short")) {
        return false;
    }
    const auto& secret = kPublicSecret;
    if (std::search(encrypted->begin(), encrypted->end(), secret.begin(), secret.end()) != encrypted->end()) {
        return false;
    }
    auto restored = LoadBackup(*encrypted, kPassword);
    SecretBytes restoredSecret;
    if (!restored || restored->size() != 1 || !restored->front().favorite ||
        (*restored)[0].tags != original[0].tags ||
        (*restored)[0].tagColors != original[0].tagColors ||
        (*restored)[0].issuer != original[0].issuer || (*restored)[0].label != original[0].label ||
        (*restored)[0].digits != 8 || (*restored)[0].period != 60 ||
        (*restored)[0].algorithm != Algorithm::Sha256 ||
        !(*restored)[0].secret.Read(restoredSecret) || !std::ranges::equal(restoredSecret.View(), secret) ||
        LoadBackup(*encrypted, L"Wrong fixture password")) {
        return false;
    }
    auto changedHeader = *encrypted;
    changedHeader[backup::kMagic.size()] ^= 1;
    auto changedVersion = *encrypted;
    changedVersion[backup::kMagic.size() - 1] = backup::kLegacyMagic.back();
    auto changedTag = *encrypted;
    changedTag.back() ^= 1;
    auto truncated = std::span(*encrypted).first(encrypted->size() - 1);
    if (LoadBackup(changedHeader, kPassword) || LoadBackup(changedVersion, kPassword) ||
        LoadBackup(changedTag, kPassword) ||
        LoadBackup(truncated, kPassword)) {
        return false;
    }

    wchar_t tempFolder[MAX_PATH];
    wchar_t tempFile[MAX_PATH];
    const auto size = GetTempPathW(ARRAYSIZE(tempFolder), tempFolder);
    if (!size || size >= ARRAYSIZE(tempFolder) || !GetTempFileNameW(tempFolder, L"2fa", 0, tempFile)) {
        return false;
    }
    const std::wstring path = std::wstring(tempFile) + L".2fa-vault";
    DeleteFileW(tempFile);
    const bool written = WriteNewBackup(path, *encrypted);
    const bool overwritten = WriteNewBackup(path, *encryptedAgain);
    const auto disk = ReadBackup(path);
    const auto portablePath = std::wstring(tempFile) + L".json";
    const auto portable = CreatePortableExport(original, ExportFormat::AndOtp);
    const bool portableWritten = portable && WriteNewPortableExport(portablePath, portable->bytes);
    const bool portableOverwritten = WriteNewPortableExport(portablePath, *encrypted);
    const auto portableDisk = ReadLinkFile(portablePath);
    DeleteFileW(portablePath.c_str());
    DeleteFileW(path.c_str());
    return written && !overwritten && disk == encrypted && portableWritten && !portableOverwritten &&
           portableDisk && portableDisk->bytes == portable->bytes;
}

bool LocalVaultPersistence() {
    using namespace twofa;
    wchar_t tempFolder[MAX_PATH];
    wchar_t tempFile[MAX_PATH];
    const auto size = GetTempPathW(ARRAYSIZE(tempFolder), tempFolder);
    if (!size || size >= ARRAYSIZE(tempFolder) || !GetTempFileNameW(tempFolder, L"2fa", 0, tempFile)) {
        return false;
    }
    DeleteFileW(tempFile);
    const auto path = std::wstring(tempFile) + L".2fa-vault";
    const auto verify = [&] {
        auto sample = ParseOtpAuth(kExample);
        if (!sample) {
            return false;
        }
        const std::vector<Account> original{*sample};
        LocalVault vault;
        LocalVault second;
        const auto oldFormat = CreateBackup(original, kPassword);
        if (!oldFormat || !WriteNewBackup(path, *oldFormat) || !vault.Open(path, kPassword) ||
            !vault.Unlocked() || second.Open(path, kPassword)) {
            return false;
        }
        const auto initial = ReadBackup(path, true);
        auto updated = original;
        updated.front().label = L"Changed synthetic account";
        updated.front().tags = {L"Fixture tag"};
        updated.front().useCount = 6;
        updated.front().lastUsed = 2200000000000ull;
        updated.front().favorite = true;
        const auto held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (held == INVALID_HANDLE_VALUE) {
            return false;
        }
        const auto replaced = vault.Save(updated);
        CloseHandle(held);
        if (!initial || replaced || ReadBackup(path, true) != initial || !vault.Save(updated)) {
            return false;
        }
        const auto revised = ReadBackup(path, true);
        vault.Lock();
        if (vault.Unlocked() || vault.Save(original) || !revised || initial == revised ||
            second.Open(path, L"Wrong fixture password") || second.Create(path, original, kPassword)) {
            return false;
        }
        auto reopened = second.Open(path, kPassword);
        SecretBytes decrypted;
        if (!reopened || reopened->size() != 1 || !reopened->front().favorite ||
            reopened->front().label != updated.front().label ||
            reopened->front().tags != updated.front().tags || !reopened->front().secret.Read(decrypted) ||
            !std::ranges::equal(decrypted.View(), kPublicSecret) || reopened->front().useCount != updated.front().useCount ||
            reopened->front().lastUsed != updated.front().lastUsed || !second.UseWindowsProtection(*reopened) ||
            !LocalVault::IsWindowsProtected(path)) {
            return false;
        }
        second.Lock();
        reopened = vault.Open(path, {});
        if (!reopened || reopened->size() != 1 || !reopened->front().favorite ||
            reopened->front().lastUsed != updated.front().lastUsed ||
            !vault.Save({})) {
            return false;
        }
        vault.Lock();
        reopened = second.Open(path, {});
        const bool empty = reopened && reopened->empty();
        second.Lock();
        return empty && DeleteFileW(path.c_str()) && vault.Create(path, original, {}) &&
            LocalVault::IsWindowsProtected(path);
    };
    const bool passed = verify();
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".lock").c_str());
    return passed;
}

bool LegacyBackupMigration() {
    using namespace twofa;
    constexpr auto legacyGroup = L"Legacy, \u2603";
    backup::Plaintext payload;
    auto& plain = payload.bytes;
    plain.reserve(512);
    backup::WriteUint16(plain, 2);
    for (const auto name : {L"Tagged", L"Untagged"}) {
        backup::WriteName(plain, L"Fixture");
        backup::WriteName(plain, name);
        backup::WriteName(plain, SameName(name, L"Tagged") ? legacyGroup : L"");
        plain.insert(plain.end(), {1, 8});
        backup::WriteUint16(plain, 60);
        backup::WriteUint16(plain, kPublicSecret.size());
        plain.insert(plain.end(), kPublicSecret.begin(), kPublicSecret.end());
    }
    std::vector<std::uint8_t> file(backup::kHeaderSize + plain.size() + backup::kTagSize);
    std::ranges::copy(backup::kLegacyMagic, file.begin());
    // A fixed salt and nonce are sufficient for this one synthetic fixture only.
    if (!backup::EncryptOrDecrypt(true, kPassword, file, payload)) {
        return false;
    }
    const auto migrated = LoadBackup(file, kPassword);
    SecretBytes seed;
    return migrated && migrated->size() == 2 &&
           migrated->front().tags == std::vector<std::wstring>{legacyGroup} &&
           migrated->back().tags.empty() && migrated->front().label == L"Tagged" &&
           migrated->front().secret.Read(seed) && std::ranges::equal(seed.View(), kPublicSecret) &&
           migrated->front().period == 60 && migrated->front().digits == 8 &&
           migrated->front().algorithm == Algorithm::Sha256 &&
           CreateBackup(*migrated, kPassword).has_value();
}

bool VersionTwoBackupMigration() {
    using namespace twofa;
    backup::Plaintext payload;
    auto& bytes = payload.bytes;
    bytes.reserve(128);
    backup::WriteUint16(bytes, 1);
    backup::WriteName(bytes, L"Fixture");
    backup::WriteName(bytes, L"Version two");
    backup::WriteUint16(bytes, 1);
    backup::WriteName(bytes, L"Work");
    bytes.insert(bytes.end(), {0, 6});
    backup::WriteUint16(bytes, 30);
    backup::WriteUint16(bytes, kPublicSecret.size());
    bytes.insert(bytes.end(), kPublicSecret.begin(), kPublicSecret.end());
    std::vector<std::uint8_t> file(backup::kHeaderSize + bytes.size() + backup::kTagSize);
    std::ranges::copy(backup::kTagsMagic, file.begin());
    if (!backup::EncryptOrDecrypt(true, kPassword, file, payload)) {
        return false;
    }
    const auto restored = LoadBackup(file, kPassword);
    return restored && restored->size() == 1 && restored->front().tags == std::vector<std::wstring>{L"Work"} &&
           restored->front().tagColors[0] == 0;
}

bool PortableExports() {
    using namespace twofa;
    auto account = ParseOtpAuth(kExample);
    if (!account) {
        return false;
    }
    account->issuer = L"Example \\ \"\u00c4\"";
    account->tags = {L"Work", L"Other"};
    // An 11-byte seed exercises the distinct Base32 padding rules.
    SecretBytes key;
    std::copy_n(kPublicSecret.begin(), 11, key.data.begin());
    key.size = 11;
    if (!account->secret.Store(std::move(key))) {
        return false;
    }
    const auto accounts = std::span(&*account, 1);
    auto andOtp = CreatePortableExport(accounts, ExportFormat::AndOtp);
    auto aegis = CreatePortableExport(accounts, ExportFormat::Aegis);
    if (!andOtp || !aegis) {
        return false;
    }
    const auto view = [](const backup::Plaintext& plain) {
        return std::string_view(reinterpret_cast<const char*>(plain.bytes.data()), plain.bytes.size());
    };
    const auto json = view(*aegis);
    constexpr std::string_view groupsKey = "\"groups\":[";
    const auto groups = json.find(groupsKey);
    const auto firstGroup = groups == json.npos ? std::string_view{} : json.substr(groups + groupsKey.size(), 38);
    return view(*andOtp) ==
        "[{\"type\":\"TOTP\",\"label\":\"Demo \xc3\x84\",\"issuer\":\"Example \\\\ \\\"\xc3\x84\\\"\","
        "\"secret\":\"GEZDGNBVGY3TQOJQGE======\",\"algorithm\":\"SHA256\",\"digits\":8,\"period\":60,"
        "\"tags\":[\"Work\",\"Other\"]}]\n" &&
        json.starts_with("{\"version\":1,\"header\":{\"slots\":null,\"params\":null},\"db\":{\"version\":3,") &&
        json.contains("\"info\":{\"secret\":\"GEZDGNBVGY3TQOJQGE\",\"algo\":\"SHA256\",\"digits\":8,\"period\":60}") &&
        firstGroup.size() == 38 && firstGroup[15] == '4' &&
        json.contains("{\"uuid\":" + std::string(firstGroup) + ",\"name\":\"Work\"}") &&
        json.contains(",\"name\":\"Other\"}");
}

bool TestPayloadBounds() {
    using namespace twofa;
    std::vector<Account> accounts;
    accounts.reserve(kMaxAccounts);
    for (std::size_t i = 0; i < kMaxAccounts; ++i) {
        auto parsed = ParseOtpAuth(kExample);
        if (!parsed) {
            return false;
        }
        parsed->label = L"Fixture " + std::to_wstring(i);
        accounts.push_back(std::move(*parsed));
    }
    backup::Plaintext payload;
    if (!backup::Serialize(accounts, payload)) {
        return false;
    }
    const auto decoded = backup::Deserialize(payload.bytes);
    if (!decoded || decoded->size() != kMaxAccounts) {
        return false;
    }
    auto newAccount = ParseOtpAuth(kExample);
    if (!newAccount) {
        return false;
    }
    std::vector<Account> additions;
    additions.push_back(std::move(*newAccount));
    if (MergeAccounts(accounts, std::move(additions)) || accounts.size() != kMaxAccounts) {
        return false;
    }
    for (std::size_t cut = 0; cut < 100; ++cut) {
        if (backup::Deserialize(std::span(payload.bytes).first(cut))) {
            return false;
        }
    }
    auto& bytes = payload.bytes;
    if (backup::Deserialize(std::span(bytes).first(bytes.size() - 1))) {
        return false;
    }
    bytes[1] = 2;
    const std::vector<std::wstring> duplicateTags{L"Work", L"WORK"};
    const std::vector<std::wstring> excessTags(kMaxTags + 1, L"Type");
    return !backup::Deserialize(bytes) && !backup::Deserialize({}) &&
           !ValidTags(duplicateTags) && !ValidTags(excessTags) &&
           !ValidName(L"invalid \u202e name") && !ValidName(L" trailing ");
}

}  // namespace

int main() {
    if (!ImportAndTags() || !LinkFileImports()) {
        std::fputs("Account import, merging, or tag tests failed.\n", stderr);
        return 1;
    }
    if (!ProtectedAccountSecret()) {
        std::fputs("In-memory account protection or TOTP after moving an account failed.\n", stderr);
        return 1;
    }
    if (!ManualOrderAndSetupKey()) {
        std::fputs("Manual setup key, sorted accounts, or backup order test failed.\n", stderr);
        return 1;
    }
    if (!ProviderMonograms()) {
        std::fputs("Provider monogram tests failed.\n", stderr);
        return 1;
    }
    if (!Backups() || !TestPayloadBounds() || !LegacyBackupMigration() || !VersionTwoBackupMigration()) {
        std::fputs("Encrypted backup tests failed.\n", stderr);
        return 1;
    }
    if (!LocalVaultPersistence()) {
        std::fputs("Local vault persistence or write recovery tests failed.\n", stderr);
        return 1;
    }
    if (!PortableExports()) {
        std::fputs("Aegis or andOTP export format tests failed.\n", stderr);
        return 1;
    }
    std::puts("Account import, tags, provider monograms, memory protection, backup migration, and portable exports passed.");
    std::puts("Local vault lock, reopen, failed replacement, retry, and empty state passed.");
}
