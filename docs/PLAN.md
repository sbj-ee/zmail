# zmail: plan

This is the current plan of record, rewritten for 1.1.1. The original
pre-build plan (written for PR #1, before any code existed) is in git history.
Read it there for the original Gmail API vs IMAP reasoning and the Google setup
research. Several of its choices changed during the build; they are listed below.

## Where zmail is now

zmail is a Linux desktop mail client for **one Gmail account**, with a
per-sender and per-rule new-mail sound in the style of Eudora. It ships as a
signed `.deb` for amd64 with a verified self-updater.

- **Two ways to connect:** an app password over IMAP + SMTP
  (`ImapSyncEngine`, `ImapClient`, `SmtpSender`), or Google sign-in with OAuth
  2.0 and the Gmail REST API (`GmailSyncEngine`, `GmailClient`, `AuthManager`,
  a loopback + PKCE flow). `MailSession` hides which one is in use. Secrets
  live only in the OS keyring (QtKeychain).
- **Cache:** SQLite with FTS5 through Qt SQL (`MailCache`). Search uses Gmail
  syntax (`SearchQuery`). See "Local data" below.
- **Reading:** an in-house MIME parser (`RawMime`, `MessageParser`) with caps
  on size, depth and part count (`Limits.h`). A strict allow-list HTML
  sanitizer (`HtmlSanitizer`) feeds a `QTextBrowser`-based `SafeHtmlView`
  (with no web engine and no JavaScript). Remote images are blocked or asked
  for (HTTPS only), and trackers are removed. Also handled: calendar invites,
  one-click unsubscribe, and an Authentication-Results banner with a
  display-name spoof warning (1.0.4).
- **Composing:** HTML, plain text or Markdown (md4c), attachments with a
  25 MB check and optional zip (libzip), drafts, signatures, spell check
  (Hunspell), Send Later and Undo Send.
- **Eudora parts:** mailboxes, filters/rules, flags, nicknames, stationery,
  stripes, themes (shared format with zterminal), and the new-mail sound.
- **Updates:** GitHub Releases, verified with minisign and SHA-256 (libsodium).

### Changes from the original plan

| Original plan | What shipped |
|---|---|
| Gmail REST API only, no IMAP in v1 | Both. The app-password IMAP path came first in practice, because it needs no Google Cloud project. |
| GMime 3.2 for MIME | An in-house parser and builder (`RawMime`, `MessageParser`, `MimeBuilder`), fuzzed since 1.0.5. |
| QtWebEngine for HTML | A sanitizer plus `QTextBrowser`, so there is no Chromium and no JS surface. |
| Everything on the UI thread | Parsing, sanitizing and HtmlFit preparation for messages of 256 KB or more run on a worker since 1.1.0 (`OffThread`). |

## Quality gates

- CI on every PR: a GCC build with `-Werror`, a full ctest under ASan + UBSan,
  and (since 1.0.5) libFuzzer harnesses in `fuzz/` for the raw MIME and
  message parsers, the HTML sanitizer, search queries and calendar invites.
- Every change bumps the version (CMake, CHANGELOG, README, the version strings
  in the tests). Releases follow `docs/RELEASING.md`.
- Test data uses `example.com` addresses and made-up mail only.

## Local data

The cache (`~/.local/share/zmail/`) is **plaintext SQLite**. The directory is
created 0700 and the database files 0600 (`MailCache.cpp`), so other users on
the machine can't read it. Anyone with your login, or with the disk, can.
Full-disk encryption (LUKS) is the recommended protection. Encrypting the cache
itself (for example with SQLCipher and a key in the keyring) is a possible
future item; it is not planned yet.

## Next

Roughly in order. Nothing here is promised.

1. **Accessibility:** accessible names and descriptions on the main widgets
   (1.1.2), then a screen-reader pass with Orca.
2. **Split the giant files:** `MainWindow.cpp` (about 3k lines, plus its
   split-out parts), `ImapSyncEngine.cpp`, `ComposeWindow.cpp` and
   `GmailSyncEngine.cpp` should be broken into units with tests.
3. **Ownership cleanup:** replace raw `new` with parent ownership or smart
   pointers where the owner isn't obvious.
4. **More off-thread work:** large attachment handling and send-side MIME
   building, if profiling shows they need it.
5. **Packaging:** the `.deb` stays primary. Other formats (Flatpak, AppImage)
   only if someone asks.
6. **Multiple accounts:** out of scope for now. The design assumes one account
   throughout (session, cache path, keyring entries).
