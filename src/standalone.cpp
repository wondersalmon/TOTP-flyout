#include "tray.hpp"

#ifdef TWOFA_UPDATE_SOURCE
#include <Velopack.hpp>

namespace {

void ShowUpdateError(HWND owner) {
    MessageBoxW(owner, twofa::Ui(L"Could not complete the update. Check your connection and try again."),
                L"TOTP flyout", MB_OK | MB_ICONERROR);
}

void StartUpdateCheck(HWND owner, bool manual) {
    wchar_t executable[32768];
    const auto length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    const auto argument = manual ? L"--check-updates" : L"--check-updates-background";
    const bool started = length && length < ARRAYSIZE(executable) &&
        reinterpret_cast<INT_PTR>(ShellExecuteW(owner, L"open", executable, argument, nullptr, SW_SHOWNORMAL)) > 32;
    if (!started && manual) {
        ShowUpdateError(owner);
    }
}

bool CloseRunningApp() {
    const auto window = FindWindowW(twofa::TrayWindowClass(true), nullptr);
    if (!window) {
        return true;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    const auto process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (!process) {
        return !IsWindow(window);
    }
    PostMessageW(window, WM_CLOSE, 0, 0);
    const bool exited = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}

void CheckForUpdates(bool manual) {
    twofa::russianUi = twofa::LoadPreferences().russian;
    try {
        const std::string source = TWOFA_UPDATE_SOURCE;
        std::unique_ptr<Velopack::IUpdateSourcePointer> updateSource;
        if (source.find("://") != std::string::npos) {
            // The configured URL points to the asset directory, not a GitHub repository.
            updateSource = std::make_unique<Velopack::HttpSource>(source);
        } else {
            updateSource = std::make_unique<Velopack::FileSource>(source);
        }
        Velopack::UpdateManager updates(std::move(updateSource));
        const auto pending = updates.UpdatePendingRestart();
        const auto available = pending ? std::nullopt : updates.CheckForUpdates();
        if (!pending && !available) {
            if (manual) {
                MessageBoxW(nullptr, twofa::Ui(L"TOTP flyout is up to date."), L"TOTP flyout",
                            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            }
            return;
        }
        if (MessageBoxW(nullptr, twofa::Ui(L"Download and install the available update? TOTP flyout will restart."),
                        L"TOTP flyout", MB_YESNO | MB_DEFBUTTON2 | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) {
            return;
        }
        if (available) {
            updates.DownloadUpdates(*available);
        }
        if (!CloseRunningApp()) {
            ShowUpdateError(nullptr);
            return;
        }
        updates.WaitExitThenApplyUpdates(pending ? *pending : available->TargetFullRelease, false, true, {"--show"});
    } catch (const std::exception&) {
        if (manual) {
            ShowUpdateError(nullptr);
        }
    }
}

}  // namespace
#endif

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    const auto command = twofa::TrimName(arguments);
    twofa::UpdateCheck checkUpdates = nullptr;
#ifdef TWOFA_UPDATE_SOURCE
    Velopack::VelopackApp::Build().SetAutoApplyOnStartup(false).Run();
    const bool manual = command == L"--check-updates";
    if (manual || command == L"--check-updates-background") {
        const auto updateCheck = CreateMutexW(nullptr, FALSE, L"Local\\TOTP-flyout-update-check");
        if (!updateCheck) {
            return 1;
        }
        if (GetLastError() != ERROR_ALREADY_EXISTS) {
            CheckForUpdates(manual);
        } else if (manual) {
            twofa::russianUi = twofa::LoadPreferences().russian;
            MessageBoxW(nullptr, twofa::Ui(L"An update check is already running."), L"TOTP flyout",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        }
        CloseHandle(updateCheck);
        return 0;
    }
    checkUpdates = StartUpdateCheck;
#endif
    constexpr wchar_t mutexName[] = L"Local\\TOTP-flyout-standalone";
    const HANDLE instance = CreateMutexW(nullptr, FALSE, mutexName);
    if (!instance) {
        return 1;
    }

    const bool alreadyRunning = GetLastError() == ERROR_ALREADY_EXISTS;
    const bool show = checkUpdates || command == L"--show";
    if (alreadyRunning && show) {
        if (const auto window = FindWindowW(twofa::TrayWindowClass(true), nullptr)) {
            PostMessageW(window, twofa::kShowStandaloneFlyout, 0, 0);
        }
    }
    const int result = alreadyRunning ? 0 : twofa::RunTray(true, nullptr, show, nullptr, std::nullopt, checkUpdates);
    CloseHandle(instance);
    return result;
}
