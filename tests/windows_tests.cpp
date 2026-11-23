#include "../src/tray.hpp"

#include <cstdio>

namespace {

// A private desktop isolates the windows. Clipboard tests also use a private
// window station so they cannot read or overwrite the interactive clipboard.
class Desktop {
public:
    explicit Desktop(bool isolateClipboard) {
        originalStation_ = GetProcessWindowStation();
        originalDesktop_ = GetThreadDesktop(GetCurrentThreadId());
        const auto name = L"TOTP-flyout-test-" + std::to_wstring(GetCurrentProcessId());
        if (isolateClipboard) {
            station_ = CreateWindowStationW(name.c_str(), 0, WINSTA_ALL_ACCESS, nullptr);
            privateClipboard = station_ && SetProcessWindowStation(station_);
        }
        desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        ready = desktop && SetThreadDesktop(desktop);
    }
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;
    ~Desktop() {
        SetProcessWindowStation(originalStation_);
        if (desktop) {
            SetThreadDesktop(originalDesktop_);
            CloseDesktop(desktop);
        }
        if (station_) {
            CloseWindowStation(station_);
        }
    }

    HDESK desktop = nullptr;
    bool ready = false;
    bool privateClipboard = false;

private:
    HWINSTA originalStation_ = nullptr;
    HDESK originalDesktop_ = nullptr;
    HWINSTA station_ = nullptr;
};

bool TestClipboardImportText() {
    using namespace twofa;
    constexpr wchar_t sample[] =
        L"otpauth://totp/Example:first?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ\r\n"
        L"otpauth://totp/Example:second?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
    const auto clipboard = EncodeClipboardSetupText(sample);
    const auto imported = clipboard ? ParseLinkFile(clipboard->bytes, false) : std::nullopt;
    const std::array<wchar_t, 2> invalid{static_cast<wchar_t>(0xd800), L'\0'};
    return imported && imported->size() == 2 && imported->at(1).label == L"second" &&
           !EncodeClipboardSetupText(std::span(sample).first(ARRAYSIZE(sample) - 1)) &&
           !EncodeClipboardSetupText(invalid);
}

bool ReadCode(std::wstring_view expected) {
    if (!OpenClipboard(nullptr)) {
        return false;
    }
    const auto data = GetClipboardData(CF_UNICODETEXT);
    const auto value = data ? static_cast<PCWSTR>(GlobalLock(data)) : nullptr;
    const bool ok = value && value == expected;
    if (value) {
        GlobalUnlock(data);
    }
    CloseClipboard();
    return ok;
}

bool TestClipboard(HWND owner, HWND other) {
    using namespace twofa;
    constexpr std::array<std::uint8_t, 20> key{
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0',
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    const auto code = CalculateTotp(key, 59);
    CodeClipboard clipboard;
    const auto exclusion = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    if (!code || !clipboard.Copy(owner, *code, 10000)) {
        std::fprintf(stderr, "Could not publish test code to the isolated clipboard: %lu\n", GetLastError());
        return false;
    }
    if (!ReadCode(L"287082") || !IsClipboardFormatAvailable(exclusion)) {
        std::fputs("The test code or the history exclusion format was missing.\n", stderr);
        return false;
    }
    clipboard.Clear(owner);
    if (!ReadCode(L"287082")) {
        std::fputs("The clipboard code was removed before its deadline.\n", stderr);
        return false;
    }
    clipboard.Clear(owner, true);
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        std::fputs("The clipboard code was still available after clearing it.\n", stderr);
        return false;
    }
    if (!clipboard.Copy(owner, *code, 10000) || !OpenClipboard(other)) {
        std::fprintf(stderr, "Could not replace the clipboard code: %lu\n", GetLastError());
        return false;
    }
    constexpr wchar_t replacement[] = L"User copied something else";
    ClipboardBlock text(replacement, sizeof(replacement));
    const bool replaced = text && EmptyClipboard() && text.Publish(CF_UNICODETEXT);
    CloseClipboard();
    clipboard.Clear(owner, true);
    if (!replaced || !ReadCode(replacement)) {
        std::fputs("A newer clipboard value could not be retained.\n", stderr);
        return false;
    }
    return true;
}

struct TrayTest {
    HANDLE stop;
    bool verifyCopy;
    bool standalone;
    bool passed = false;
};

TrayTest* activeTrayTest = nullptr;

HWND FindTestFlyout() {
    return FindWindowW(twofa::TrayWindowClass(activeTrayTest->standalone), nullptr);
}

std::optional<std::array<COLORREF, 32>> SampleCardPaint(HWND list, const RECT& row) {
    RECT client{};
    GetClientRect(list, &client);
    const auto screen = GetDC(list);
    if (!screen) {
        return std::nullopt;
    }
    const auto target = CreateCompatibleDC(screen);
    const auto bitmap = CreateCompatibleBitmap(screen, client.right, client.bottom);
    std::optional<std::array<COLORREF, 32>> colors;
    if (target && bitmap) {
        const auto original = SelectObject(target, bitmap);
        if (original && PrintWindow(list, target, PW_CLIENTONLY)) {
            colors.emplace();
            for (unsigned i = 0; i < colors->size(); ++i) {
                const auto x = row.left + (row.right - row.left) * (1 + i % 8) / 16;
                const auto y = row.top + (row.bottom - row.top) * (1 + i / 8) / 6;
                (*colors)[i] = GetPixel(target, x, y);
            }
        }
        if (original) {
            SelectObject(target, original);
        }
    }
    DeleteObject(bitmap);
    DeleteDC(target);
    ReleaseDC(list, screen);
    return colors;
}

bool ExerciseProviderRow(bool verifyCopy) {
    const auto flyout = FindTestFlyout();
    const auto list = flyout ? FindWindowExW(flyout, nullptr, L"LISTBOX", nullptr) : nullptr;
    RECT first{}, second{}, client{};
    if (!list || !IsWindowVisible(list) || !GetClientRect(list, &client) ||
        SendMessageW(list, LB_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&first)) == LB_ERR ||
        SendMessageW(list, LB_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&second)) == LB_ERR ||
        first.bottom != second.top || second.bottom > client.bottom) {
        std::fputs("The isolated flyout did not lay out the first two account rows.\n", stderr);
        return false;
    }
    const auto close = GetDlgItem(flyout, 104);
    const auto gear = GetDlgItem(flyout, 105);
    ValidateRect(close, nullptr);
    SendMessageW(close, WM_MOUSEMOVE, 0, MAKELPARAM(8, 8));
    ValidateRect(close, nullptr);
    SendMessageW(gear, WM_MOUSEMOVE, 0, MAKELPARAM(8, 8));
    const bool closeCleared = GetUpdateRect(close, nullptr, FALSE);
    ValidateRect(gear, nullptr);
    SendMessageW(gear, WM_MOUSELEAVE, 0, 0);
    if (!closeCleared || !GetUpdateRect(gear, nullptr, FALSE)) {
        std::fputs("Moving across the flyout buttons did not clear hover.\n", stderr);
        return false;
    }
    wchar_t sort[32]{};
    GetWindowTextW(GetDlgItem(flyout, 107), sort, ARRAYSIZE(sort));
    if (std::wstring_view(sort) == L"Sort: Manual") {
        const auto rowText = [list](WPARAM row) {
            const auto length = SendMessageW(list, LB_GETTEXTLEN, row, 0);
            std::wstring text(length >= 0 ? length + 1 : 1, L'\0');
            if (length >= 0) {
                SendMessageW(list, LB_GETTEXT, row, reinterpret_cast<LPARAM>(text.data()));
                text.resize(length);
            }
            return text;
        };
        const auto original = rowText(0);
        if (original.find(L"Tags:") == std::wstring::npos) {
            std::fputs("The listbox does not expose the account's tag names.\n", stderr);
            return false;
        }
        const auto grip = MulDiv(19, GetDpiForWindow(list), 96);
        const auto from = MAKELPARAM(grip, (first.top + first.bottom) / 2);
        const auto to = MAKELPARAM(grip, (second.top + second.bottom) / 2);
        const auto beforeDrag = SampleCardPaint(list, second);
        SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, from);
        SendMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, to);
        UpdateWindow(list);
        const auto dragging = SampleCardPaint(list, second);
        SendMessageW(list, WM_CANCELMODE, 0, 0);
        SendMessageW(list, WM_LBUTTONUP, 0, to);
        const auto afterDrag = SampleCardPaint(list, second);
        if (beforeDrag && dragging && afterDrag && (*beforeDrag == *dragging || *beforeDrag != *afterDrag)) {
            std::fputs("The card preview was missing during drag or remained after cancellation.\n", stderr);
            return false;
        }
        if (rowText(0) != original || GetCapture() == list) {
            std::fputs("Cancelling card drag changed the order or retained capture.\n", stderr);
            return false;
        }
        SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, from);
        SendMessageW(list, WM_MOUSEMOVE, MK_LBUTTON, to);
        UpdateWindow(list);
        SendMessageW(list, WM_LBUTTONUP, 0, to);
        if (rowText(1) != original || SendMessageW(list, LB_GETCURSEL, 0, 0) != 1 || GetCapture() == list) {
            std::fputs("Dragging a manual grip did not keep the moved account selected.\n", stderr);
            return false;
        }
    }
    const auto nameX = MulDiv(70, GetDpiForWindow(list), 96);
    const auto providerName = MAKELPARAM(nameX, (second.top + second.bottom) / 2);
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, providerName);
    SendMessageW(list, WM_LBUTTONUP, 0, MAKELPARAM(nameX, (first.top + first.bottom) / 2));
    if (SendMessageW(list, LB_GETCURSEL, 0, 0) != 1 ||
        (verifyCopy && IsClipboardFormatAvailable(CF_UNICODETEXT))) {
        std::fputs("The account row was not selected or the drag was not cancelled.\n", stderr);
        return false;
    }
    if (!verifyCopy) {
        return true;
    }
    SendMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, providerName);
    SendMessageW(list, WM_LBUTTONUP, 0, providerName);
    for (HWND label = nullptr; (label = FindWindowExW(flyout, label, L"STATIC", nullptr));) {
        wchar_t text[64]{};
        GetWindowTextW(label, text, ARRAYSIZE(text));
        if (std::wstring_view(text) == L"Code copied." && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            return true;
        }
    }
    std::fputs("Clicking the account name did not copy a code and show confirmation.\n", stderr);
    return false;
}

bool TestFlyoutEntryPoints() {
    const auto flyout = FindTestFlyout();
    const auto gear = flyout ? GetDlgItem(flyout, 105) : nullptr;
    const auto tags = flyout ? GetDlgItem(flyout, 101) : nullptr;
    const auto sort = flyout ? GetDlgItem(flyout, 107) : nullptr;
    const auto edit = flyout ? GetDlgItem(flyout, 109) : nullptr;
    const auto list = flyout ? GetDlgItem(flyout, 102) : nullptr;
    const auto scrollbar = flyout ? GetDlgItem(flyout, 110) : nullptr;
    const auto progress = flyout ? FindWindowExW(flyout, nullptr, PROGRESS_CLASSW, nullptr) : nullptr;
    RECT tagBounds{}, sortBounds{};
    if (!gear || !edit || !list || !scrollbar || IsWindowVisible(tags) || IsWindowVisible(sort) ||
        !IsWindowVisible(scrollbar) || (GetWindowLongW(list, GWL_STYLE) & WS_VSCROLL)) {
        std::fputs("The filters must initially be hidden and overflow must use the flyout's scrollbar.\n", stderr);
        return false;
    }
    HWND toast = nullptr;
    for (HWND child = nullptr; (child = FindWindowExW(flyout, child, L"STATIC", nullptr));) {
        wchar_t text[64]{};
        GetWindowTextW(child, text, ARRAYSIZE(text));
        if (std::wstring_view(text) == L"Local vault unlocked.") {
            toast = child;
            break;
        }
    }
    RECT beforeToast{}, afterToast{};
    if (!toast || !IsWindowVisible(toast) || !progress || IsWindowVisible(progress) ||
        !GetWindowRect(flyout, &beforeToast)) {
        std::fputs("The unlock notification did not replace the countdown footer.\n", stderr);
        return false;
    }
    SendMessageW(flyout, WM_TIMER, 6, 0);
    if (IsWindowVisible(toast) || !IsWindowVisible(progress) || !GetWindowRect(flyout, &afterToast) ||
        !EqualRect(&beforeToast, &afterToast)) {
        std::fputs("Dismissing a toast must leave the flyout size unchanged.\n", stderr);
        return false;
    }
    RECT rows{};
    MONITORINFO layoutMonitor{sizeof(layoutMonitor), {}, {}, 0};
    GetClientRect(list, &rows);
    const auto rowHeight = SendMessageW(list, LB_GETITEMHEIGHT, 0, 0);
    if (GetMonitorInfoW(MonitorFromWindow(flyout, MONITOR_DEFAULTTONEAREST), &layoutMonitor) &&
        layoutMonitor.rcWork.bottom - layoutMonitor.rcWork.top >= rowHeight * 6 + 240 &&
        rows.bottom != rowHeight * 6) {
        std::fputs("The flyout must show six complete rows when the work area fits them.\n", stderr);
        return false;
    }
    RECT scrollBounds{};
    GetClientRect(scrollbar, &scrollBounds);
    SendMessageW(scrollbar, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(2, scrollBounds.bottom - 1));
    SendMessageW(scrollbar, WM_LBUTTONUP, 0, 0);
    if (SendMessageW(list, LB_GETTOPINDEX, 0, 0) <= 0) {
        std::fputs("The flyout scrollbar did not scroll to the last accounts.\n", stderr);
        return false;
    }
    SendMessageW(list, LB_SETTOPINDEX, 0, 0);
    SendMessageW(flyout, WM_COMMAND, MAKEWPARAM(109, BN_CLICKED), reinterpret_cast<LPARAM>(edit));
    if (!IsWindowVisible(tags) || !IsWindowVisible(sort) ||
        !GetWindowRect(tags, &tagBounds) || !GetWindowRect(sort, &sortBounds) ||
        tagBounds.top != sortBounds.top || tagBounds.right >= sortBounds.left || !IsWindowVisible(progress)) {
        std::fputs("The flyout should show Settings, aligned filters and a timer without account buttons.\n", stderr);
        return false;
    }

    // Exercise the runtime position without saving or modifying anyone's preferences.
    const auto standalone = activeTrayTest->standalone;
    if (twofa::ReadPreference(standalone ? L"WindowPlacement" : L"ModWindowPlacement", 0) != 0) {
        std::puts("Skipped the runtime default placement check because saved placement is configured.");
        return true;
    }
    MONITORINFO monitor{sizeof(monitor), {}, {}, 0};
    RECT before{};
    if (!GetWindowRect(flyout, &before) ||
        !GetMonitorInfoW(MonitorFromWindow(flyout, MONITOR_DEFAULTTONEAREST), &monitor)) {
        std::fputs("Could not determine the work area of the test monitor.\n", stderr);
        return false;
    }
    const SIZE size{before.right - before.left, before.bottom - before.top};
    const auto point = twofa::CornerFlyoutPosition(twofa::WindowPlacementMode::TopLeft, size, monitor.rcWork, 12);
    SendMessageW(flyout, WM_ENTERSIZEMOVE, 0, 0);
    SetWindowPos(flyout, nullptr, point.x, point.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(flyout, WM_EXITSIZEMOVE, 0, 0);
    RECT moved{};
    GetWindowRect(flyout, &moved);
    ShowWindow(flyout, SW_HIDE);
    SendMessageW(flyout, WM_APP + 1, 0, NIN_SELECT);
    // Simulate Explorer's broadcast and verify that it doesn't discard a manual placement.
    SendMessageW(flyout, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
    RECT reopened{};
    GetWindowRect(flyout, &reopened);
    const auto tolerance = (std::max)(1, MulDiv(1, GetDpiForWindow(flyout), 96));
    if (std::abs(moved.left - point.x) > tolerance || std::abs(moved.top - point.y) > tolerance ||
        !EqualRect(&moved, &reopened)) {
        std::fprintf(stderr, "Manual placement did not survive reopening. Expected (%ld,%ld), moved (%ld,%ld), reopened (%ld,%ld).\n",
                     point.x, point.y, moved.left, moved.top, reopened.left, reopened.top);
        return false;
    }
    return true;
}

bool RunTestTray(bool standalone, bool verifyCopy, const std::wstring& vaultPath, TIMERPROC testCallback) {
    const auto stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) {
        return false;
    }
    TrayTest test{stop, verifyCopy, standalone};
    activeTrayTest = &test;
    // Dispatch the regression only after RunTray has initialized its controls.
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, testCallback);
    const auto result = timer ? twofa::RunTray(standalone, stop, true, nullptr, vaultPath) : 1;
    KillTimer(nullptr, timer);
    MSG quit{};
    PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
    activeTrayTest = nullptr;
    CloseHandle(stop);
    const bool closed = !FindWindowW(twofa::TrayWindowClass(standalone), nullptr);
    const bool clipboardClear = !verifyCopy || !IsClipboardFormatAvailable(CF_UNICODETEXT);
    if (result != 0 || !test.passed || !closed || !clipboardClear) {
        std::fprintf(stderr, "%s flyout with %s vault: exit=%d, checks=%d, closed=%d, clipboard cleared=%d.\n",
                     standalone ? "Standalone" : "Windhawk", vaultPath.empty() ? "no" : "a fixture",
                     result, test.passed, closed, clipboardClear);
        return false;
    }
    return true;
}

constexpr auto kTestVaultPassword = L"p6test";

void CALLBACK EnterVaultPassword(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    if (const auto dialog = FindWindowW(L"#32770", L"Unlock local vault")) {
        SetDlgItemTextW(dialog, 101, kTestVaultPassword);
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    }
}

bool UnlockTestVault(HWND window, HWND button, int expectedAccounts) {
    const auto enter = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, EnterVaultPassword);
    if (!enter) {
        return false;
    }
    SendMessageW(window, WM_COMMAND, MAKEWPARAM(108, BN_CLICKED), reinterpret_cast<LPARAM>(button));
    KillTimer(nullptr, enter);
    return SendDlgItemMessageW(window, 102, LB_GETCOUNT, 0, 0) == expectedAccounts;
}

void CALLBACK TestEmptyFlyout(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto window = FindTestFlyout();
    const auto button = window ? GetDlgItem(window, 108) : nullptr;
    wchar_t title[32]{};
    activeTrayTest->passed = button && GetWindowTextW(button, title, ARRAYSIZE(title)) &&
        std::wstring_view(title) == L"Import accounts" && SendDlgItemMessageW(window, 102, LB_GETCOUNT, 0, 0) == 0;
    if (!activeTrayTest->passed) {
        std::fputs("The empty flyout did not expose its import button and empty account list.\n", stderr);
    }
    if (activeTrayTest->passed && activeTrayTest->standalone) {
        SendMessageW(window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        ShowWindow(window, SW_HIDE);
        SendMessageW(window, twofa::kShowStandaloneFlyout, 0, 0);
        activeTrayTest->passed = IsWindowVisible(window);
        if (!activeTrayTest->passed) {
            std::fputs("The standalone launch message did not reopen the hidden flyout.\n", stderr);
        }
    }
    if (activeTrayTest->passed) {
        SendMessageW(window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        ShowWindow(window, SW_HIDE);
        SendMessageW(window, WM_APP + 1, 0, WM_LBUTTONUP);
        const auto opened = IsWindowVisible(window);
        SendMessageW(window, WM_APP + 1, 0, WM_LBUTTONUP);
        const auto hidden = !IsWindowVisible(window);
        SendMessageW(window, WM_APP + 1, 0, WM_LBUTTONUP);
        activeTrayTest->passed = opened && hidden && IsWindowVisible(window);
        if (!activeTrayTest->passed) {
            std::fputs("Successive tray clicks did not open, hide, and reopen the flyout.\n", stderr);
        }
    }
    SetEvent(activeTrayTest->stop);
}

void CALLBACK TestRowCopy(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto window = FindTestFlyout();
    if (window) {
        SendMessageW(window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        if (!IsWindowVisible(window)) {
            SendMessageW(window, WM_APP + 1, 0, NIN_KEYSELECT);
        }
    }
    const auto button = window ? GetDlgItem(window, 108) : nullptr;
    if (button && !IsWindowEnabled(button)) {
        std::puts("Skipped provider-row UI checks because session notifications are unavailable.");
        activeTrayTest->passed = true;
    } else if (!button || !UnlockTestVault(window, button, 7)) {
        std::fputs("The isolated flyout could not unlock the fixture vault.\n", stderr);
    } else {
        activeTrayTest->passed = TestFlyoutEntryPoints() && ExerciseProviderRow(activeTrayTest->verifyCopy);
    }
    SetEvent(activeTrayTest->stop);
}

void CALLBACK ExerciseVaultLock(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto window = FindTestFlyout();
    if (window) {
        SendMessageW(window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        if (!IsWindowVisible(window)) {
            SendMessageW(window, twofa::kShowStandaloneFlyout, 0, 0);
        }
    }
    const auto button = window ? GetDlgItem(window, 108) : nullptr;
    const auto accounts = window ? GetDlgItem(window, 102) : nullptr;
    if (button && accounts && !IsWindowEnabled(button)) {
        std::puts("Skipped local vault UI check because session notifications are unavailable.");
        activeTrayTest->passed = true;
    } else if (button && accounts && SendMessageW(accounts, LB_GETCOUNT, 0, 0) == 0 &&
               UnlockTestVault(window, button, 7)) {
        SendMessageW(window, WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, 0);
        const bool cleared = !IsWindowVisible(window) &&
            SendMessageW(accounts, LB_GETCOUNT, 0, 0) == 0 && !IsWindowEnabled(button);
        SendMessageW(window, WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, 0);
        SendMessageW(window, twofa::kShowStandaloneFlyout, 0, 0);
        activeTrayTest->passed = cleared && IsWindowVisible(window) && IsWindowEnabled(button) &&
            UnlockTestVault(window, button, 7);
        if (activeTrayTest->passed) {
            std::puts("The isolated flyout hid on session lock, cleared its account list, and reopened the vault.");
        }
    }
    SetEvent(activeTrayTest->stop);
}

bool TestWithVault(bool standalone, bool verifyCopy, TIMERPROC callback) {
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
        auto first = ParseOtpAuth(L"otpauth://totp/GitHub:Fixture?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
        auto second = ParseOtpAuth(L"otpauth://totp/GitLab:Fixture?secret=GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ");
        if (!first || !second) {
            return false;
        }
        first->tags = {L"Fixture tag"};
        second->label = L"Second fixture";
        const auto additional = *first;
        std::vector<Account> accounts{std::move(*first), std::move(*second)};
        for (int i = 0; i < 5; ++i) {
            auto account = additional;
            account.label = L"Test fixture " + std::to_wstring(i);
            accounts.push_back(std::move(account));
        }
        LocalVault vault;
        if (!vault.Create(path, accounts, kTestVaultPassword)) {
            return false;
        }
        vault.Lock();
        return RunTestTray(standalone, verifyCopy, path, callback);
    };
    const auto passed = verify();
    DeleteFileW(path.c_str());
    DeleteFileW((path + L".lock").c_str());
    return passed;
}

bool TestPalettes() {
    using namespace twofa;
    const WindowsTextSize settings;
    for (const auto mode : {ThemeMode::System, ThemeMode::Light, ThemeMode::Dark}) {
        const auto palette = WindowsPalette(mode, settings);
        if (palette.highContrast) {
            if (palette.surface != GetSysColor(COLOR_WINDOW) || palette.text != GetSysColor(COLOR_WINDOWTEXT) ||
                palette.buttonText != GetSysColor(COLOR_BTNTEXT) ||
                TagMarkerColor(0, palette, true) != GetSysColor(COLOR_HIGHLIGHTTEXT)) {
                return false;
            }
            continue;
        }
        if (ColorContrast(palette.surface, palette.text) < 4.5 ||
            ColorContrast(palette.button, palette.buttonText) < 4.5 ||
            ColorContrast(palette.selected, palette.selectionText) < 4.5 ||
            ColorContrast(palette.focusOuter, palette.focusInner) < 3.0) {
            return false;
        }
        for (const auto background : {palette.card, palette.hover, palette.pressed, palette.selected}) {
            for (std::size_t i = 0; i < 8; ++i) {
                if (ColorContrast(TagMarkerColor(i, palette), background) < 3.0) {
                    return false;
                }
            }
        }
        for (std::size_t i = 0; i < 8; ++i) {
            const auto accent = TagMarkerColor(i, palette);
            const auto gradientEnd = AccountTint(palette.card, accent, palette.muted, palette.dark ? 24 : 19);
            const auto badge = AccountTint(palette.surface, accent, palette.text, 32);
            if (ColorContrast(gradientEnd, palette.muted) < 4.5 || ColorContrast(badge, palette.text) < 4.5) {
                return false;
            }
        }
    }
    return true;
}

bool TestCardGradient() {
    const twofa::PaintResources painting;
    const auto dc = CreateCompatibleDC(nullptr);
    if (!dc) {
        return false;
    }
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = 128;
    bitmap.bmiHeader.biHeight = -32;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    const auto surface = CreateDIBSection(dc, &bitmap, DIB_RGB_COLORS, &pixels, nullptr, 0);
    bool painted = false;
    if (surface) {
        const auto previous = SelectObject(dc, surface);
        const auto start = RGB(20, 30, 40);
        twofa::PaintRounded(dc, {0, 0, 128, 32}, 6, start, start, RGB(100, 70, 50));
        const auto left = GetPixel(dc, 20, 16);
        const auto right = GetPixel(dc, 108, 16);
        painted = left != CLR_INVALID && right != CLR_INVALID && GetRValue(right) - GetRValue(left) > 40;
        SelectObject(dc, previous);
        DeleteObject(surface);
    }
    DeleteDC(dc);
    return painted;
}

bool TestTrayKeyIcon() {
    const auto icon = twofa::CreateTrayKeyIcon();
    ICONINFO info{};
    if (!icon || !GetIconInfo(icon, &info)) {
        if (icon) {
            DestroyIcon(icon);
        }
        return false;
    }
    constexpr int size = 32;
    std::array<DWORD, size * size> pixels{};
    BITMAPINFO bitmap{};
    bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmap.bmiHeader.biWidth = size;
    bitmap.bmiHeader.biHeight = -size;
    bitmap.bmiHeader.biPlanes = 1;
    bitmap.bmiHeader.biBitCount = 32;
    bitmap.bmiHeader.biCompression = BI_RGB;
    const auto dc = CreateCompatibleDC(nullptr);
    const bool read = dc && GetDIBits(dc, info.hbmColor, 0, size, pixels.data(), &bitmap, DIB_RGB_COLORS) == size;
    DeleteDC(dc);
    DeleteObject(info.hbmColor);
    DeleteObject(info.hbmMask);
    DestroyIcon(icon);
    const auto opaque = std::ranges::count_if(pixels, [](DWORD pixel) { return (pixel >> 24) == 255; });
    const auto transparent = std::ranges::count_if(pixels, [](DWORD pixel) { return (pixel >> 24) == 0; });
    return read && opaque > 20 && transparent > 500;
}

struct SettingsTest {
    bool accept;
    bool russian = false;
    bool initialized = false;
};

SettingsTest* activeSettingsTest = nullptr;

void CALLBACK ChangeSettings(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", activeSettingsTest->russian ?
        L"Настройки TOTP flyout" : L"TOTP flyout settings");
    if (!dialog) {
        // The test runs on its own desktop and this is its only modal window.
        PostQuitMessage(1);
        return;
    }
    using twofa::kShowCodesChoice;
    using twofa::kThemeChoice;
    using twofa::kCountdownChoice;
    using twofa::kMotionChoice;
    using twofa::kLockChoice;
    using twofa::kClipboardChoice;
    using twofa::kScreenshotChoice;
    using twofa::kHideAfterCopyChoice;
    using twofa::kRussianChoice;
    wchar_t hideCaption[80]{};
    GetDlgItemTextW(dialog, kHideAfterCopyChoice, hideCaption, ARRAYSIZE(hideCaption));
    const auto theme = SendDlgItemMessageW(dialog, kThemeChoice, CB_GETCURSEL, 0, 0);
    activeSettingsTest->initialized = theme == 0 &&
        SendDlgItemMessageW(dialog, kThemeChoice, CB_GETCOUNT, 0, 0) == 3 &&
        IsDlgButtonChecked(dialog, kShowCodesChoice) == BST_UNCHECKED &&
        IsDlgButtonChecked(dialog, kCountdownChoice) == BST_CHECKED &&
        IsDlgButtonChecked(dialog, kMotionChoice) == BST_CHECKED &&
        IsDlgButtonChecked(dialog, kScreenshotChoice) == BST_UNCHECKED &&
        IsDlgButtonChecked(dialog, kHideAfterCopyChoice) == BST_UNCHECKED &&
        IsDlgButtonChecked(dialog, kRussianChoice) == (activeSettingsTest->russian ? BST_CHECKED : BST_UNCHECKED) &&
        std::wstring_view(hideCaption) == (activeSettingsTest->russian ?
            L"Скрывать панель после копирования" : L"Hide the flyout after copying") &&
        SendDlgItemMessageW(dialog, kLockChoice, CB_GETCURSEL, 0, 0) == 2 &&
        SendDlgItemMessageW(dialog, kClipboardChoice, CB_GETCURSEL, 0, 0) == 2;
    SendDlgItemMessageW(dialog, kThemeChoice, CB_SETCURSEL, static_cast<WPARAM>(twofa::ThemeMode::Dark), 0);
    CheckDlgButton(dialog, kShowCodesChoice, BST_CHECKED);
    CheckDlgButton(dialog, kCountdownChoice, BST_UNCHECKED);
    CheckDlgButton(dialog, kMotionChoice, BST_UNCHECKED);
    CheckDlgButton(dialog, kScreenshotChoice, BST_CHECKED);
    CheckDlgButton(dialog, kHideAfterCopyChoice, BST_CHECKED);
    CheckDlgButton(dialog, kRussianChoice, BST_CHECKED);
    SendDlgItemMessageW(dialog, kLockChoice, CB_SETCURSEL, 1, 0);
    SendDlgItemMessageW(dialog, kClipboardChoice, CB_SETCURSEL, 0, 0);
    SendMessageW(dialog, WM_COMMAND, activeSettingsTest->accept ? IDOK : IDCANCEL, 0);
}

bool TestSettings(bool accept, bool russian = false) {
    SettingsTest test{accept, russian};
    activeSettingsTest = &test;
    twofa::FlyoutPreferences initial{};
    initial.russian = russian;
    twofa::russianUi = russian;
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, ChangeSettings);
    const auto preferences = timer ? twofa::EditPreferences(nullptr, initial) : std::nullopt;
    KillTimer(nullptr, timer);
    twofa::russianUi = false;
    activeSettingsTest = nullptr;
    return test.initialized && (accept ? preferences ==
        twofa::FlyoutPreferences{twofa::ThemeMode::Dark, true, false, false, 3, 5, true, true, true}
                                       : !preferences);
}

void CALLBACK SubmitManualAccount(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", L"Add account");
    if (!dialog) {
        PostQuitMessage(1);
        return;
    }
    using namespace twofa;
    const auto key = GetDlgItem(dialog, kManualKey);
    SetDlgItemTextW(dialog, kManualLabel, L"Test account");
    SetDlgItemTextW(dialog, kManualKey, L"not a setup key!");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    wchar_t error[100]{};
    activeSettingsTest->initialized = IsWindow(dialog) &&
        (GetWindowLongW(key, GWL_STYLE) & ES_PASSWORD) &&
        GetDlgItemTextW(dialog, kManualError, error, ARRAYSIZE(error)) > 0 &&
        IsWindowEnabled(GetDlgItem(dialog, kManualTags));
    if (!IsWindow(dialog)) {
        return;
    }
    SetDlgItemTextW(dialog, kManualKey, L"GEZDGNBV-GY3TQOJQ GEZDGNBV-GY3TQOJQ");
    SendDlgItemMessageW(dialog, kManualAlgorithm, CB_SETCURSEL, 1, 0);
    SendDlgItemMessageW(dialog, kManualDigits, CB_SETCURSEL, 1, 0);
    SetDlgItemInt(dialog, kManualPeriod, 60, FALSE);
    SendMessageW(dialog, WM_COMMAND, activeSettingsTest->accept ? IDOK : IDCANCEL, 0);
}

bool TestManualAccount(bool accept) {
    using namespace twofa;
    SettingsTest test{accept};
    activeSettingsTest = &test;
    const std::vector<std::wstring> tags{L"Personal", L"Work"};
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, SubmitManualAccount);
    const auto account = timer ? PromptManualAccount(nullptr, ThemeMode::Light, tags[1], tags) : std::nullopt;
    KillTimer(nullptr, timer);
    activeSettingsTest = nullptr;
    if (!test.initialized || account.has_value() != accept) {
        return false;
    }
    if (!account) {
        return true;
    }
    const auto code = CalculateAccountCode(*account, 59);
    return ValidAccount(*account) && account->label == L"Test account" && account->issuer.empty() &&
           account->tags == std::vector<std::wstring>{L"Work"} && account->algorithm == Algorithm::Sha256 &&
           account->digits == 8 && account->period == 60 &&
           code && std::wstring_view(code->text.data()) == L"74875740";
}

int TagRow(HWND list, std::wstring_view name) {
    for (int index = 0; index < ListView_GetItemCount(list); ++index) {
        wchar_t text[twofa::kMaxNameLength + 1]{};
        ListView_GetItemText(list, index, 0, text, ARRAYSIZE(text));
        if (name == text) {
            return index;
        }
    }
    return -1;
}

void CALLBACK EnterNewTag(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", L"New tag");
    if (!dialog) {
        PostQuitMessage(1);
        return;
    }
    SetDlgItemTextW(dialog, 101, L"New, shared & team");
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
}

void CALLBACK CheckTags(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", L"Edit tags");
    if (!dialog) {
        PostQuitMessage(1);
        return;
    }
    using namespace twofa;
    const auto list = GetDlgItem(dialog, kTagList);
    const int work = TagRow(list, L"Work & Ops");
    const int personal = TagRow(list, L"Personal");
    const int legacy = TagRow(list, L"Legacy; team");
    auto& passed = activeSettingsTest->initialized;
    passed = list && (ListView_GetExtendedListViewStyle(list) & LVS_EX_CHECKBOXES) &&
        ListView_GetItemCount(list) == 3 && work >= 0 && personal >= 0 && legacy >= 0 &&
        ListView_GetCheckState(list, work) && ListView_GetCheckState(list, legacy) &&
        !ListView_GetCheckState(list, personal);
    if (passed) {
        ListView_SetItemState(list, work, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        passed = IsWindowEnabled(GetDlgItem(dialog, kTagColor));
        const auto editor = reinterpret_cast<TagEditor*>(GetWindowLongPtrW(dialog, DWLP_USER));
        if (editor && !editor->palette.highContrast) {
            RECT row{};
            ListView_GetItemRect(list, work, &row, LVIR_BOUNDS);
            const auto swatch = editor->ColorSquare(row);
            NMITEMACTIVATE click{};
            click.hdr = {list, kTagList, NM_CLICK};
            click.ptAction = {(swatch.left + swatch.right) / 2, (swatch.top + swatch.bottom) / 2};
            SendMessageW(dialog, WM_NOTIFY, kTagList, reinterpret_cast<LPARAM>(&click));
            MSG posted{};
            passed = passed && PeekMessageW(&posted, dialog, WM_COMMAND, WM_COMMAND, PM_REMOVE) &&
                LOWORD(posted.wParam) == kTagColor && ListView_GetCheckState(list, work);
        }
        SendMessageW(dialog, WM_COMMAND, kAutoTagColor, 0);
        ListView_SetCheckState(list, work, FALSE);
        ListView_SetItemState(list, personal, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SetFocus(list);
        SendMessageW(list, WM_KEYDOWN, VK_SPACE, 0);
        SendMessageW(list, WM_KEYUP, VK_SPACE, 0);
        const auto inputTimer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, EnterNewTag);
        if (inputTimer) {
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kNewTag, BN_CLICKED), 0);
            KillTimer(nullptr, inputTimer);
        }
        const auto created = TagRow(list, L"New, shared & team");
        passed = passed && inputTimer && created >= 0 && ListView_GetCheckState(list, created) &&
            ListView_GetCheckState(list, personal) && !ListView_GetCheckState(list, work);
    }
    SendMessageW(dialog, WM_COMMAND, activeSettingsTest->accept ? IDOK : IDCANCEL, 0);
}

bool TestTagChecklist(bool accept) {
    using namespace twofa;
    SettingsTest test{accept};
    activeSettingsTest = &test;
    const std::vector<std::wstring> initial{L"Work & Ops", L"Legacy; team"};
    const std::array<std::wstring, 2> known{L"Personal", L"work & ops"};
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, CheckTags);
    const auto initialColor = RGB(30, 100, 180) + 1;
    TagColors colors{{L"Work & Ops", initialColor}};
    const auto result = timer ? PromptAccountTags(nullptr, ThemeMode::Dark, initial, known, &colors) : std::nullopt;
    KillTimer(nullptr, timer);
    activeSettingsTest = nullptr;
    return test.initialized && (accept ?
        result == std::vector<std::wstring>{L"Legacy; team", L"Personal", L"New, shared & team"} &&
            FindTagColor(colors, L"Work & Ops") == 0 : !result && FindTagColor(colors, L"Work & Ops") == initialColor);
}

void CALLBACK ChangeShortcut(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", L"Shortcut");
    if (!dialog) {
        PostQuitMessage(1);
        return;
    }
    using namespace twofa;
    const auto keys = GetDlgItem(dialog, kShortcutKeys);
    activeSettingsTest->initialized = !IsWindowEnabled(keys) && !GetDlgItem(dialog, kBackupExport) &&
        SendMessageW(keys, HKM_GETHOTKEY, 0, 0) == MAKEWORD('O', HOTKEYF_ALT) &&
        !ValidHotkey(MAKEWORD('B', HOTKEYF_SHIFT));
    CheckDlgButton(dialog, kShortcutEnabled, BST_CHECKED);
    SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(kShortcutEnabled, BN_CLICKED), 0);
    activeSettingsTest->initialized = activeSettingsTest->initialized && IsWindowEnabled(keys);
    SendMessageW(keys, HKM_SETHOTKEY, MAKEWORD('B', HOTKEYF_ALT), 0);
    SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    wchar_t status[90]{};
    GetDlgItemTextW(dialog, kShortcutStatus, status, ARRAYSIZE(status));
    activeSettingsTest->initialized = activeSettingsTest->initialized &&
        std::wstring_view(status).starts_with(L"Shortcut unavailable");
    if (IsWindow(dialog)) {
        SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    }
}

bool TestShortcutSettings() {
    SettingsTest test{false};
    activeSettingsTest = &test;
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, ChangeShortcut);
    bool tried = false;
    if (timer) {
        twofa::EditShortcut(nullptr, twofa::ThemeMode::Dark, {}, false, [&](twofa::HotkeyPreferences shortcut) {
            tried = shortcut.enabled && LOBYTE(shortcut.chord) == 'B' &&
                    twofa::HotkeyModifiers(shortcut.chord) == MOD_ALT;
            return false;
        });
        KillTimer(nullptr, timer);
    }
    activeSettingsTest = nullptr;
    return test.initialized && tried;
}

void CALLBACK ChooseBackup(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", L"Backup");
    if (!dialog) {
        PostQuitMessage(1);
        return;
    }
    using namespace twofa;
    const bool canExport = activeSettingsTest->accept;
    activeSettingsTest->initialized = IsWindowEnabled(GetDlgItem(dialog, kBackupExport)) == canExport &&
        IsWindowEnabled(GetDlgItem(dialog, kBackupExportAegis)) == canExport &&
        IsWindowEnabled(GetDlgItem(dialog, kBackupExportAndOtp)) == canExport &&
        IsWindowEnabled(GetDlgItem(dialog, kBackupRestore));
    SendMessageW(dialog, WM_COMMAND, canExport ? kBackupExportAegis : kBackupRestore, 0);
}

bool TestBackupSettings(bool exportAllowed) {
    using namespace twofa;
    SettingsTest test{exportAllowed};
    activeSettingsTest = &test;
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, ChooseBackup);
    const auto action = timer ? ChooseBackupAction(nullptr, ThemeMode::Light, exportAllowed, true) :
                                BackupAction::None;
    KillTimer(nullptr, timer);
    activeSettingsTest = nullptr;
    return test.initialized && action == (exportAllowed ? BackupAction::ExportAegis : BackupAction::Restore);
}

void CALLBACK ChangePlacement(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", L"Window position");
    if (!dialog) {
        PostQuitMessage(1);
        return;
    }
    using namespace twofa;
    const auto choice = GetDlgItem(dialog, kPlacementChoice);
    activeSettingsTest->initialized = SendMessageW(choice, CB_GETCOUNT, 0, 0) == 5 &&
        SendMessageW(choice, CB_GETCURSEL, 0, 0) == 0;
    CheckDlgButton(dialog, kLockDragChoice, BST_CHECKED);
    CheckDlgButton(dialog, kRememberPositionChoice, BST_CHECKED);
    SendMessageW(dialog, WM_COMMAND, activeSettingsTest->accept ? IDOK : IDCANCEL, 0);
}

bool TestPlacementSettings(bool accept) {
    using namespace twofa;
    SettingsTest test{accept};
    activeSettingsTest = &test;
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, ChangePlacement);
    const auto updated = timer ? EditWindowPlacement(nullptr, ThemeMode::Dark, {}) : std::nullopt;
    KillTimer(nullptr, timer);
    activeSettingsTest = nullptr;
    return test.initialized && (accept ? updated == WindowPlacementPreferences{WindowPlacementMode::Manual, true, true}
                                      : !updated);
}

bool TestPlacementGeometry() {
    using namespace twofa;
    const RECT work{-1440, 100, 0, 980};
    const SIZE window{558, 450};
    const auto tl = CornerFlyoutPosition(WindowPlacementMode::TopLeft, window, work, 12);
    const auto tr = CornerFlyoutPosition(WindowPlacementMode::TopRight, window, work, 12);
    const auto bl = CornerFlyoutPosition(WindowPlacementMode::BottomLeft, window, work, 12);
    const auto br = CornerFlyoutPosition(WindowPlacementMode::BottomRight, window, work, 12);
    const auto manual = ClampFlyoutPosition({400, 85}, window, work);
    const auto small = ClampFlyoutPosition({400, 85}, {1600, 1000}, work);
    const auto dpi96 = RestoreManualFlyoutPosition({100, 120}, {372, 300}, work, 96);
    const auto dpi144 = RestoreManualFlyoutPosition({100, 120}, window, work, 144);
    const auto smallerWork = RestoreManualFlyoutPosition({200, 300}, window, {10, 20, 610, 540}, 192);
    return tl.x == -1428 && tr.x == -570 && bl.x == tl.x && br.x == tr.x &&
           tl.y == 112 && tr.y == tl.y && bl.y == 518 && br.y == bl.y &&
           manual.x == -558 && manual.y == 100 && small.x == work.left && small.y == work.top &&
           dpi96.x == -1340 && dpi96.y == 220 && dpi144.x == -1290 && dpi144.y == 280 &&
           smallerWork.x == 52 && smallerWork.y == 90;
}

void CALLBACK RespondToRemoval(HWND, UINT, UINT_PTR timer, DWORD) {
    KillTimer(nullptr, timer);
    const auto dialog = FindWindowW(L"#32770", L"Remove account?");
    if (!dialog) {
        PostQuitMessage(1);
        return;
    }
    const auto button = SendMessageW(dialog, DM_GETDEFID, 0, 0);
    activeSettingsTest->initialized = HIWORD(button) == DC_HASDEFID && LOWORD(button) == IDCANCEL;
    SendMessageW(dialog, WM_COMMAND, activeSettingsTest->accept ? IDOK : IDCANCEL, 0);
}

bool TestRemovalConfirmation(bool accept) {
    SettingsTest test{accept};
    activeSettingsTest = &test;
    const auto timer = SetTimer(nullptr, 0, USER_TIMER_MINIMUM, RespondToRemoval);
    const auto confirmed = timer && twofa::ConfirmAccountRemoval(nullptr, L"Fixture account", twofa::ThemeMode::Dark);
    KillTimer(nullptr, timer);
    activeSettingsTest = nullptr;
    return test.initialized && confirmed == accept;
}
}  // namespace

int main() {
    if (!TestClipboardImportText()) {
        std::fputs("Clipboard TOTP text import failed.\n", stderr);
        return 1;
    }
    using twofa::ClipboardLifetime;
    if (ClipboardLifetime(1, 999) != 1 || ClipboardLifetime(5, 837) != 4163 ||
        ClipboardLifetime(30, 123) != 29877 || ClipboardLifetime(60, 0) != 30000 ||
        ClipboardLifetime(0, 500) != 0 || ClipboardLifetime(28, 123, 5) != 5000 ||
        ClipboardLifetime(5, 900, 15) != 4100 || ClipboardLifetime(60, 0, 200) != 30000 ||
        twofa::LockMinutes(0) != 5 || twofa::LockMinutes(3) != 3 ||
        twofa::ClipboardSeconds(99) != 30 || twofa::ClipboardSeconds(15) != 15) {
        std::fputs("Clipboard deadline tests failed.\n", stderr);
        return 1;
    }
    std::puts("Clipboard deadline tests passed.");
    if (!TestPlacementGeometry()) {
        std::fputs("Flyout placement geometry failed.\n", stderr);
        return 1;
    }
    if (!TestPalettes() || !TestCardGradient()) {
        std::fputs("Flyout theme contrast checks failed.\n", stderr);
        return 1;
    }
    if (!TestTrayKeyIcon()) {
        std::fputs("Tray key icon rendering failed.\n", stderr);
        return 1;
    }
    std::puts("Tray key icon rendering passed.");
    {
        Desktop isolatedClipboard(true);
        if (isolatedClipboard.ready && isolatedClipboard.privateClipboard) {
            const auto owner = CreateWindowExW(0, L"STATIC", nullptr, WS_POPUP, 0, 0, 1, 1,
                                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            const auto other = CreateWindowExW(0, L"STATIC", nullptr, WS_POPUP, 0, 0, 1, 1,
                                               nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            const bool passed = owner && other && TestClipboard(owner, other);
            if (other) {
                DestroyWindow(other);
            }
            if (owner) {
                DestroyWindow(owner);
            }
            if (!passed) {
                std::fputs("Isolated clipboard test failed.\n", stderr);
                return 1;
            }
            std::puts("Isolated clipboard integration passed.");
        } else {
            std::puts("Skipped clipboard integration because a private window station is unavailable.");
        }
    }
    // A separate noninteractive window station accepts clipboard operations but cannot reliably show a flyout.
    Desktop isolation(false);
    if (!isolation.ready) {
        std::fprintf(stderr, "Skipped Windows desktop integration because desktop creation is restricted: %lu\n",
                     GetLastError());
        return 0;
    }
    if (!RunTestTray(true, false, {}, TestEmptyFlyout) || !RunTestTray(false, false, {}, TestEmptyFlyout) ||
        !TestWithVault(true, false, TestRowCopy) ||
        !TestWithVault(false, false, TestRowCopy)) {
        std::fputs("Isolated flyout test failed.\n", stderr);
        return 1;
    }
    if (!TestWithVault(true, false, ExerciseVaultLock)) {
        std::fputs("Isolated local vault lock and reopen test failed.\n", stderr);
        return 1;
    }
    if (!TestSettings(false) || !TestSettings(true) || !TestSettings(true, true)) {
        std::fputs("Isolated standalone settings test failed.\n", stderr);
        return 1;
    }
    if (!TestManualAccount(false) || !TestManualAccount(true)) {
        std::fputs("Manual account form validation or confirmation failed.\n", stderr);
        return 1;
    }
    if (!TestTagChecklist(false) || !TestTagChecklist(true)) {
        std::fputs("Tag checkboxes, new tag creation, or Save and Cancel failed.\n", stderr);
        return 1;
    }
    if (!TestShortcutSettings() || !TestBackupSettings(false) || !TestBackupSettings(true)) {
        std::fputs("Shortcut conflict feedback or backup action routing failed.\n", stderr);
        return 1;
    }
    if (!TestPlacementSettings(false) || !TestPlacementSettings(true)) {
        std::fputs("Window placement settings failed.\n", stderr);
        return 1;
    }
    if (!TestRemovalConfirmation(false) || !TestRemovalConfirmation(true)) {
        std::fputs("The account removal confirmation failed.\n", stderr);
        return 1;
    }
    std::puts("Account removal defaults to Cancel and requires explicit confirmation.");
    std::puts("Standalone settings opened with defaults and handled Cancel and OK.");
    std::puts("Manual account form validated the masked setup key and handled Cancel and Add.");
    std::puts("Tag checkboxes, new tag creation, Save and Cancel passed.");
    std::puts("Shortcut, backup and window placement dialogs passed.");
    std::puts("Both isolated flyout modes passed.");
}
