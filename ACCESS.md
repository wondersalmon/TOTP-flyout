# Vault and shortcut settings

Open **Settings > Shortcut** to enable the global shortcut. The default is **Alt+O**. Use **Ctrl** or **Alt** with a key, optionally adding **Shift**. Press the shortcut again to close the panel. The standalone app and Windhawk mod store separate shortcuts.

| Vault protection | How it unlocks |
| --- | --- |
| Master password | Enter your password after startup or a vault lock. |
| Windows protection | Select **Unlock vault** in the signed-in Windows session. User-scoped DPAPI decrypts the vault. |

To switch an unlocked master-password vault, choose **Manage accounts > Use Windows protection without a password**. Keep a portable encrypted backup for recovery under a different user account or on another computer.

The app locks the vault when Windows locks, disconnects, or suspends and after the configured inactivity period. The default delay is five minutes. Each edition creates a separate vault under `%LOCALAPPDATA%\TOTP-flyout` and can open its existing vault from earlier versions.
