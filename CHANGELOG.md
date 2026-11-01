# Changelog

All notable changes to zmail. The version is the single
`project(zmail VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps it
([semantic versioning](https://semver.org/): patch for fixes and documentation,
minor for features) and adds an entry here. Releases are tagged `vX.Y.Z`,
published by `.github/workflows/release.yml` and signed with minisign (see
[docs/RELEASING.md](docs/RELEASING.md)). Entries before 0.9.0 are in the README's
status paragraph and on the
[GitHub Releases](https://github.com/sbj-ee/zmail/releases) page.

## 1.1.2 - 2026-10-31

- Accessibility: screen readers now announce names, and descriptions where
  useful, for the search box, mailbox tree, message list, message pane, status
  labels, the compose fields (To, From, Subject, Cc, Bcc, body, format,
  signature, priority, font, size meter), and the message view's banners and
  buttons.
- Test: tst_accessibility checks the names through QAccessible, and fails if
  any named text field, combo box or list in the main or compose window has
  no accessible name.

## 1.1.1

- docs/PLAN.md rewritten as the current plan: what shipped (IMAP and OAuth,
  in-house MIME parser, sanitizer + QTextBrowser), how that differs from the
  original plan, quality gates, and what's next.
- README: a "Local data and encryption" note. The cache is plaintext SQLite
  (folder 0700, database 0600); full-disk encryption is recommended.

## 1.1.0

- Messages of 256 KB or more are now parsed (sync engines), sanitized and
  prepared for layout (viewer) on a worker thread, so the window stays
  responsive on huge mail. SQLite access stays on the main thread. The viewer
  shows "Preparing message…" meanwhile, and a generation guard drops results
  for a message that is no longer selected. The final HtmlFit pass, which
  edits the live document, still runs on the UI thread.
- Tests: tst_offthread, plus tst_imap and tst_sync re-run with every message
  treated as large.

## 1.0.5

- **Searches with punctuation or a leading minus work again.** The search box
  could build a full-text query that SQLite rejects (`-spam invoice`, an
  address such as `ada@example.com`, `Q4)` or a stray quote),
  and the search then found nothing. Every word that isn't plain letters and
  digits is now quoted, and excluded words go after the wanted ones. Found by
  the new search-query fuzzer.
- **Fuzzing.** libFuzzer harnesses in `fuzz/` for the parsers that read
  untrusted input (raw MIME, Gmail API messages, the HTML sanitizer, calendar
  invitations and the search box), with small synthetic seed corpora. A new
  CI job builds them with clang, ASan and UBSan, runs the test suite and each
  seed corpus under that build, then runs every fuzzer for 60 seconds. See
  `fuzz/README.md`.
- **At most 1000 parts per multipart.** A hostile message with thousands of
  MIME parts is now read only that far.

## 1.0.4

- **A warning when Gmail couldn't confirm who sent a message.** zmail now reads
  the topmost `Authentication-Results` header, the one Gmail's own servers add
  (authserv-id `mx.google.com`), for its SPF, DKIM and DMARC results. Any
  other `Authentication-Results` header is ignored, because a sender can write
  one. When DMARC fails, or none of the three passes, the viewer shows a banner
  above the message. Mail with no such header (your own Sent mail, say) gets
  no banner.
- **A warning when the sender's name poses as another address.** If the
  display name looks like an email address or a domain (`"security@example.com"
  <x@example.net>`, `example.com <x@example.net>`) that isn't the real sender's,
  the same banner says so.
- Bodies cached by an older zmail are fetched again once, when next opened, so
  their header can be read.

## 1.0.3

- **Release builds are hardened explicitly.** The CMake build now asks for PIE,
  stack protector and stack-clash protection, `-fcf-protection` (x86), fortified
  libc calls, full RELRO and a non-executable stack (`ZMAIL_HARDEN`, on by
  default; off for sanitizer builds). Ubuntu's GCC already defaults to most of
  these; CI and the release build now check the finished binary with
  `tools/check-hardening.sh`, so a toolchain change can't drop one unnoticed.

## 1.0.2

- **No line breaks in outgoing addresses, whatever their source.** Address
  headers, the Message-ID, and the SMTP `MAIL FROM` / `RCPT TO` commands now drop
  control characters. The compose fields and `mailto:` links already did, but an
  address copied from a received message (a Reply-To, say) could have carried a
  CR/LF into a header or an SMTP command. Now it can't, wherever it came from.

## 1.0.1

- **Self-update only fetches from GitHub's release addresses.** The asset URLs in
  the release API reply are untrusted, so the downloader now refuses anything but
  `https://github.com/sbj-ee/zmail/releases/download/...` and follows a redirect
  only to https on `github.com` or `*.githubusercontent.com`. Downloads are also
  capped (64 KiB for `SHA256SUMS` and its signature, 512 MiB for the package) and
  cut off past that. The minisign and SHA-256 checks are unchanged and still
  decide what is installed.

## 1.0.0

- **First stable release.** No change to how zmail works from 0.9.3: the same
  app, with the version moved to 1.0.0 (window title, About, update checks,
  `zmail_1.0.0_amd64.deb`).
- **README for a public 1.0:** a feature overview, an Install section (download
  from Releases, check `SHA256SUMS.minisig` with key `DBC46B23C3AF0EF3` and the
  SHA-256, `apt install` the `.deb`), an app-password quick start, and the
  long per-version status paragraph replaced by a pointer to this changelog.
  The README no longer says Gmail is OAuth-only.
- **No personal or real-domain addresses in the docs or tests.** `docs/PLAN.md`
  no longer names a personal address; test fixtures and the screenshots made
  from them use `example.com` / `.example` addresses only (offscreen, fake
  servers, made-up mail).

## 0.9.3

- **Junk / Suspicious count matched by its list.** On an app-password (IMAP)
  account the sidebar could say `2 / 2` for Junk / Suspicious while the list
  said "No junk mail." and the status bar "Junk: 0 messages". The badge is the
  server's count for `[Gmail]/Spam` (IMAP `STATUS`), but the folder's mail was
  only fetched when the row was clicked: opening Junk another way (View ›
  Show Spam, a rebuild of the sidebar) showed the count over an empty list,
  and a full resync set Spam's mail aside and never read it again. Opening a
  mailbox in any way now loads its folder, and a full resync or reconnect
  reads again every folder zmail had opened, so the badge and the list come
  from the same folder. Tests: `tst_imapspam` `junkBadgeMatchesListFromShowSpam`,
  `junkListSurvivesFullResync`.

## 0.9.2

- **Release workflow fix.** The v0.9.1 tag built and tested fine but its
  Publish job failed (`cp: cannot stat 'dist/zmail-release-linux/...'`): with a
  single artifact, `actions/download-artifact` extracts it straight into `dist/`
  with no per-artifact subfolder. Publish now downloads the artifact by name
  into `dist/linux` and finds the `.deb` and `SHA256SUMS` wherever they landed,
  failing with a listing of `dist/` if either is missing. Publish also checks
  that the signed `SHA256SUMS` lists exactly the `.deb` it ships. No change to
  the app. Version 0.9.1 was tagged but never published; 0.9.2 is the first
  release signed with `DBC46B23C3AF0EF3`.

## 0.9.1 — tagged, not published (see 0.9.2)

- **zmail's own release key.** The updater now trusts minisign key
  `DBC46B23C3AF0EF3` (zmail's own, replacing the zterminal key `32960BBA77B36F1F`
  that 0.9.0 embedded), the key that `release.yml` signs releases with. 0.9.0 was
  never released; releases are checked against this key only. Docs
  (`docs/RELEASING.md`, README "Updates") name the new key. The real-signature
  test (a published zterminal `SHA256SUMS`, checked with a real minisign
  signature) now pins zterminal's key as a constant in the test instead of
  reading the embedded key, so it still proves real-minisign interoperability;
  new tests pin the embedded key's ID and public key bytes. No behaviour change
  otherwise.

## 0.9.0 — unreleased

- **Self-update**, the same as zterminal's. Help → Check for Updates… shows a
  newer release's version and notes with Install / Later / Skip This Version;
  Help → Check for Updates at Startup (on by default) checks quietly about once a
  day. Install downloads `SHA256SUMS` and `SHA256SUMS.minisig`, verifies the
  signature against the minisign key built into zmail, downloads
  `zmail_<version>_amd64.deb`, checks its SHA-256, runs
  `pkexec apt install` on a private re-verified copy (the system password prompt;
  no terminal needed), then offers Restart Now / Later (Restart asks "Save
  draft?" for any message still open). A release without a valid signature is
  never installed. Linux amd64 only.
- Release workflow (`.github/workflows/release.yml`): a `v*` tag builds, tests,
  packages, signs `SHA256SUMS` (`SHA256SUMS.minisig`) and publishes the release.
  Needs the repository secrets `MINISIGN_SECRET_KEY` and `MINISIGN_PASSWORD`
  (docs/RELEASING.md); without them it refuses to publish.
- Build/CI: libsodium (Ed25519 for the signature check). The `.deb` now depends
  on `libsodium23`.
- The old release-page prompt (`UpdateChecker`) is gone; its release-page URL
  check is now `isTrustedReleasePageUrl` (also rejects `.`/`..` path segments and
  anything outside `/sbj-ee/zmail/releases/`).
- Tests: `tst_update` (release JSON, versions, checksums, signatures, the whole
  check → download → verify → install → restart flow against a local fake
  GitHub), `tst_minisign` (real minisign output, a real signature published by
  the release key, tampering), the Help menu setting.
