# Vault formats

TOTP keys are encrypted at rest. Local vaults accept a master password or Windows user protection. Portable backups always use their own password.

## Encrypted backups

The `2FAVLT03` envelope uses a random 16-byte salt, a random 12-byte nonce, and an authenticated AES-256-GCM header with a 16-byte tag. PBKDF2-HMAC-SHA-256 derives the key in 600,000 iterations.

The payload starts with a little-endian 16-bit account count. Each account has length-prefixed UTF-16LE issuer and label fields, a 16-bit tag count, tag names and 32-bit colors, one algorithm byte, one digits byte, a 16-bit period, and a length-prefixed key. Zero color means automatic. Custom colors are Windows COLORREF plus one. Algorithm values 0, 1, and 2 mean SHA-1, SHA-256, and SHA-512.

The reader also accepts version 1 (one optional group) and version 2 (tags without colors). Backups always write version 3.

## Local vaults

The local password format `2FALVT04` extends each account with a little-endian 32-bit copy count and a 64-bit last-copy timestamp in Unix milliseconds. The Windows mode `2FADPA01` wraps the same payload with user-scoped DPAPI. Local vaults allow an empty account list and read the older version 3 envelope. Usage history stays in the local vault.

A per-file lock serializes access. Encrypted changes are flushed and moved into place before account edits take effect. Portable backups omit usage history.

## Transfer exports

Aegis and andOTP JSON exports contain plain-text setup keys. Aegis uses envelope version 1 and database version 3. andOTP uses an array of TOTP entries with separate issuer and label fields.
