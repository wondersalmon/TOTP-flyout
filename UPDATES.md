# Update or roll back

[Русский](UPDATES.ru.md) · [Back to README](README.md)

## Standalone with Velopack

1. Right-click the tray icon and choose **Check for updates**.
2. Confirm the download and installation. TOTP flyout closes and starts again automatically.
3. Unlock your vault as usual. The updater replaces app files and the vault stays in its existing user folder.

If you installed an earlier manual-update edition, export an encrypted backup, exit the app, and uninstall it from Windows Settings. Run the new Setup and open your existing vault. The old uninstaller preserves the vault.

## Manual update or rollback

1. In TOTP flyout, export an encrypted backup and keep its password.
2. Download the new installer or mod file and verify its SHA-256 against `SHA256SUMS.txt`.
3. For standalone, exit from the tray and run the new Setup EXE. When switching to the renamed Windhawk mod, disable the old mod and compile the new file as a local mod with ID `totp-flyout`. Later updates can replace the source of that mod in place.
4. Open the updated app and unlock your vault. Keep the earlier installer and portable backup until you verify your accounts.

For builds that cannot read the current local vault format, restore the portable backup in the earlier build. Local usage history is specific to the local vault. [Backup instructions](IMPORT.md)

## Release and local testing

Run `./build.ps1 -Velopack` to produce the installer, `releases.win.json` and the referenced `*-full.nupkg`. Upload all three with the mod source and checksums. A matching version tag builds and publishes through GitHub Actions. Tags with a preview suffix become prereleases, while the in-app check uses the latest stable release. A public GitHub repository needs no token on users' computers. [Velopack update sources](https://docs.velopack.io/integrating/update-sources)

To test locally, point the build at an existing directory with `-UpdateSource`. Build two increasing preview versions for the same app ID. Install the older build in an isolated Windows user account, put the newer feed and full package in that directory, then use the tray update command.
