# TOTP flyout

[Русский](README.ru.md)

TOTP flyout is an offline authenticator for the Windows tray. It **encrypts setup keys on disk**, generates codes locally, and sends neither keys nor codes to a server. The app has no telemetry.

## Key features

- Quick access from the tray or a global shortcut, with masked codes and one-click copying.
- Local vault encrypted with a master password or protected by your Windows account.
- Account tags, sorting by usage, and encrypted portable backups.
- English and Russian interface, with optional flyout dismissal after copying.

## Install

| Edition | File | How to start |
| --- | --- | --- |
| Standalone for 64-bit Windows | `TOTP-flyout-setup.exe` | Run Setup, then open **TOTP flyout** from Start. It installs for your Windows user. |
| Windhawk | `TOTP-flyout.wh.cpp` | Create a local mod in Windhawk 1.7.3 and compile the file in the editor. |

Open the tray icon, then follow [Move your authenticator keys](IMPORT.md). New installations start with an empty list.

## Use the flyout

- Click an account or press **Enter** to copy its code. Codes are masked by default.
- The pencil beside Settings reveals tag filters and sorting. In the **Tags** menu, **Edit tags** opens the selected account's tags. **Most used** and **Recently used** rank successful copies.
- To enable **Alt+O**, open **Settings > Shortcut**. You can choose another key with **Ctrl** or **Alt**, with **Shift** optional. Windows reports a conflict if the shortcut is taken.
- Use **Settings > Backup** to export or restore an encrypted `.2fa-vault` backup.
- Enable **Hide the flyout after copying** or **Русский интерфейс (Russian)** in **Settings**. When the vault locks, the flyout closes. For the Windhawk edition, set the interface language in the mod settings.

## How keys are protected

Choose a master password or Windows account protection when you create a vault. Master-password vaults use AES-256-GCM. The Windows option uses user-scoped DPAPI. Apps running as the same Windows user can also unlock a DPAPI vault. Portable backups use a separate password and move between editions.

The app clears unlocked keys from memory when the vault locks. Screen capture protection is on by default where Windows supports it. **Allow screenshots of the flyout** lets capture tools see the panel and any revealed codes. Copied codes can be read by other apps in your session until the clipboard is cleared.

Read [Vault and shortcut settings](ACCESS.md) or [Storage format](DESIGN.md) for details.

## Build and update

Run `pwsh -NoProfile -File ./build.ps1 -Velopack` in the staging checkout on 64-bit Windows with PowerShell 7.3+, Windhawk 1.7.3, and the .NET 9 runtime for packaging. The build tests both editions and writes the installer, mod source, update feed and checksums to `build/release/`. Omit `-Velopack` to build the manual-update installer with NSIS.

For the Velopack edition, use **Check for updates** in the tray icon's context menu. It contacts GitHub to check the latest stable release and asks before downloading. The installer is unsigned. [Update or roll back](UPDATES.md)

The source is available under the [MIT License](LICENSE).
