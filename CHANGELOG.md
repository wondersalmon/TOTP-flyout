# Changelog

## [v0.3.3]

- Fixed Velopack update checks returning HTTP 404 by selecting the HTTP asset source explicitly.
- Added version numbers to Windows installer filenames.

If an earlier version shows an update error, install `TOTP-flyout-0.3.3-setup.exe` once from this release. Your local vault is kept during the upgrade.

## [v0.3.2]

- Redesigned the flyout with a search field, larger account codes, and settings controls at the bottom. Search matches services, account names, and tags.
- Added favorites to the account menu and edit mode. The footer star filters the list to favorites and remembers its state between launches.
- Kept the shared countdown visible under notifications and moved copy confirmation onto the account card.
- Added one-click account removal in edit mode and improved the tray icon's open and hide behavior.

Existing vaults open in this version. Vaults and backups containing favorites require v0.3.2 or later.

## [v0.3.1]

- First public release of TOTP flyout for Windows and Windhawk.
- Added a per-user Windows installer with Velopack. Automatic update checks are on by default, and manual checks are available from Settings and the tray menu.
- Included encrypted local vaults, portable backups, account tags, clipboard clearing, and English and Russian interfaces.
