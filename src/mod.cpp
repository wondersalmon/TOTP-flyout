// ==WindhawkMod==
// @id              totp-flyout
// @name            TOTP flyout
// @description     TOTP tray flyout with account tags and encrypted backups
// @version         0.3.3
// @author          wondersalmon
// @github          https://github.com/wondersalmon
// @include         windhawk.exe
// @architecture    x86
// @architecture    x86-64
// @compilerOptions -lbcrypt -lshell32 -luser32 -lgdi32 -ladvapi32 -lcrypt32 -lcomdlg32 -lwtsapi32 -ldwmapi -luxtheme -lcomctl32 -lgdiplus -lruntimeobject
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
Click the TOTP flyout tray icon to see your accounts and TOTP codes.
Open Settings > Manage accounts to add an account or unlock the local vault. Choose a master password or Windows protection when adding the first account. Account changes are saved to the encrypted local vault.
Open Settings > Backup to export an encrypted backup or restore one.
Sort by name or provider, or drag the row grips in manual order.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- ShowCodes: false
  $name: Show one-time codes
  $description: Display digits next to each account. Masked codes can still be copied.
- AllowScreenshots: false
  $name: Allow screenshots of the flyout
  $description: Allow Windows capture of the panel, including any visible codes.
- HideAfterCopy: false
  $name: Hide the flyout after copying
  $description: Close the panel shortly after a successful copy.
- Russian: false
  $name: Русский интерфейс (Russian)
  $description: Show the tray flyout in Russian.
- Theme: system
  $name: Theme
  $options:
    - system: Follow Windows
    - light: Light
    - dark: Dark
- HideCountdown: false
  $name: Hide timer seconds
  $description: Keep the progress bar and hide the numeric countdown.
- ReduceCopyMotion: false
  $name: Reduce copy animation
  $description: Show the copied confirmation with no color animation.
- AutoLockMinutes: 5
  $name: Auto-lock delay in minutes
  $description: Choose 1, 3, 5, or 10 minutes. Windows session lock always locks the vault.
- ClipboardSeconds: 30
  $name: Clipboard timeout in seconds
  $description: Choose 5, 15, or 30 seconds. Codes clear sooner if they expire.
*/
// ==/WindhawkModSettings==

#include "tray.hpp"

#include <windhawk_api.h>

#include <string>
#include <string_view>

// Dedicated tool host for Windhawk 1.7.3, following the Windhawk wiki's
// "Mods as tools" lifecycle and -tool-mod command line protocol.
namespace {

bool isLauncher;
HANDLE toolInstance;
HANDLE stopEvent;
HANDLE trayThread;
std::atomic<twofa::FlyoutPreferences> preferences;

void UpdatePreferences() {
    twofa::FlyoutPreferences updated;
    updated.showCodes = Wh_GetIntSetting(L"ShowCodes") != 0;
    updated.allowScreenshots = Wh_GetIntSetting(L"AllowScreenshots") != 0;
    updated.hideAfterCopy = Wh_GetIntSetting(L"HideAfterCopy") != 0;
    updated.russian = Wh_GetIntSetting(L"Russian") != 0;
    updated.showCountdown = Wh_GetIntSetting(L"HideCountdown") == 0;
    updated.animateCopy = Wh_GetIntSetting(L"ReduceCopyMotion") == 0;
    updated.lockMinutes = twofa::LockMinutes(Wh_GetIntSetting(L"AutoLockMinutes"));
    updated.clipboardSeconds = twofa::ClipboardSeconds(Wh_GetIntSetting(L"ClipboardSeconds"));
    const auto value = Wh_GetStringSetting(L"Theme");
    if (value) {
        const std::wstring_view theme(value);
        if (theme == L"light") {
            updated.theme = twofa::ThemeMode::Light;
        } else if (theme == L"dark") {
            updated.theme = twofa::ThemeMode::Dark;
        }
        Wh_FreeStringSetting(value);
    }
    preferences.store(updated);
}

bool IsDesktopUser() {
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session == 0) {
        return false;
    }

    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size;
    const auto ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && !elevation.TokenIsElevated;
}

DWORD WINAPI TrayThread(void*) {
    return twofa::RunTray(false, stopEvent, false, &preferences);
}

void WINAPI StopOriginalEntryPoint() {
    ExitThread(0);
}

bool StartTool() {
    toolInstance = CreateMutexW(nullptr, FALSE, L"Local\\TOTP-flyout-mod-instance");
    if (!toolInstance || GetLastError() == ERROR_ALREADY_EXISTS) {
        return false;
    }

    const auto base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto image = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto entryPoint = base + image->OptionalHeader.AddressOfEntryPoint;
    if (!Wh_SetFunctionHook(entryPoint, reinterpret_cast<void*>(StopOriginalEntryPoint), nullptr)) {
        return false;
    }

    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stopEvent) {
        return false;
    }
    trayThread = CreateThread(nullptr, 0, TrayThread, nullptr, 0, nullptr);
    return trayThread != nullptr;
}

}  // namespace

BOOL Wh_ModInit() {
    int count = 0;
    const auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) {
        return FALSE;
    }
    bool isService = false;
    bool isTool = false;
    bool isThisTool = false;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view arg(args[index]);
        if (arg == L"-service" || arg == L"-service-start" || arg == L"-service-stop") {
            isService = true;
        }
        if (!isTool && arg == L"-tool-mod" && index + 1 < count) {
            isTool = true;
            isThisTool = std::wstring_view(args[index + 1]) == WH_MOD_ID;
        }
    }
    LocalFree(args);
    if (isService || !IsDesktopUser()) {
        return FALSE;
    }

    if (isThisTool) {
        UpdatePreferences();
        if (!StartTool()) {
            ExitProcess(1);
        }
        return TRUE;
    }
    isLauncher = !isTool;
    return isLauncher;
}

void Wh_ModSettingsChanged() {
    if (trayThread) {
        UpdatePreferences();
    }
}

void Wh_ModAfterInit() {
    if (!isLauncher) {
        return;
    }

    WCHAR executable[MAX_PATH];
    const auto length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length == ARRAYSIZE(executable)) {
        Wh_Log(L"Could not resolve the Windhawk executable path");
        return;
    }
    std::wstring command = L"\"" + std::wstring(executable, length) + L"\" -tool-mod \"" WH_MOD_ID L"\"";

    // Use the tool-host launch path recommended by Windhawk 1.7.3.
    using CreateToolProcess = BOOL(WINAPI*)(HANDLE, LPCWSTR, LPWSTR, LPSECURITY_ATTRIBUTES,
                                            LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCWSTR,
                                            LPSTARTUPINFOW, LPPROCESS_INFORMATION, PHANDLE);
    const auto kernel = GetModuleHandleW(L"kernelbase.dll");
    const auto launch = kernel ? reinterpret_cast<CreateToolProcess>(
                                     GetProcAddress(kernel, "CreateProcessInternalW"))
                               : nullptr;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_FORCEOFFFEEDBACK;
    PROCESS_INFORMATION process{};
    if (!launch || !launch(nullptr, executable, command.data(), nullptr, nullptr, FALSE,
                           NORMAL_PRIORITY_CLASS, nullptr, nullptr, &startup, &process, nullptr)) {
        Wh_Log(L"Could not start the dedicated TOTP flyout process");
        return;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

void Wh_ModUninit() {
    if (!trayThread) {
        return;
    }
    SetEvent(stopEvent);
    if (WaitForSingleObject(trayThread, 5000) != WAIT_OBJECT_0) {
        // This callback runs in the dedicated mod host. A blocked file dialog or disk
        // must not leave its tray thread running after Windhawk unloads the module.
        Wh_Log(L"The TOTP flyout worker did not stop in time");
        TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
        return;
    }
    CloseHandle(trayThread);
    CloseHandle(stopEvent);
    CloseHandle(toolInstance);
    ExitProcess(0);
}
