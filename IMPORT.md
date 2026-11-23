# Move your authenticator keys to TOTP flyout

[Русский](IMPORT.ru.md) · [Back to README](README.md)

Keep your old authenticator active until you confirm the new codes work. Move the **setup key** or TOTP setup link for each account.

## Choose what you have

| What you have | What to do in TOTP flyout |
| --- | --- |
| A Base32 setup key | Open **Settings > Manage accounts > Add account**. Enter a name, provider, and key. Use the code length, interval, and algorithm supplied by the service. |
| An `otpauth://totp/…` setup link | Choose **Manage accounts > Import accounts > Enter TOTP link** and paste it. |
| Several TOTP links | Copy the links and choose **From clipboard**, or save them as a UTF-8 TXT file with one link per line and choose **From TXT file**. HTML with TOTP links is also supported. |
| A TOTP flyout `.2fa-vault` backup | Choose **Settings > Backup > Open encrypted backup** and enter the backup password. |

If your old app only shows a QR transfer screen, look for a setup key or standard TOTP link in the service's security settings. TOTP flyout currently imports text links and its own encrypted backup format. If needed, keep the old app available and reconfigure TOTP at the service.

## Finish the transfer

1. On first import, choose a master password of at least six characters or **Windows protection**, which uses your Windows account and needs no separate password.
2. Compare the codes for the same account within the same time window. Confirm you can sign in before removing the old authenticator.
3. Use **Settings > Backup > Export encrypted backup** to create a recovery copy. Choose a backup password of at least 12 characters and store the backup separately.

Setup keys, TOTP links, and plain-text exports can be used to generate your codes. Keep them out of chats and shared files. Clear temporary transfer files and the clipboard once the transfer is complete.
