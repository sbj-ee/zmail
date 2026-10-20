# Changelog

All notable changes to zmail. The version is the single
`project(zmail VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps it
([semantic versioning](https://semver.org/): patch for fixes and documentation,
minor for features) and adds an entry here. Releases are tagged `vX.Y.Z`,
published by `.github/workflows/release.yml` and signed with minisign (see
[docs/RELEASING.md](docs/RELEASING.md)). Entries before 0.9.0 are in the README's
status paragraph and on the
[GitHub Releases](https://github.com/sbj-ee/zmail/releases) page.

## 1.0.0

- **First stable release.** No change to how zmail works from 0.9.3: the same
  app, with the version moved to 1.0.0 (window title, About, update checks,
  `zmail_1.0.0_amd64.deb`).
- **README for a public 1.0:** a feature overview, an Install section (download
  from Releases, check `SHA256SUMS.minisig` with key `DBC46B23C3AF0EF3` and the
  SHA-256, `apt install` the `.deb`), an app-password quick start, and the
  long per-version status paragraph replaced by a pointer to this changelog.
  The README no longer says Gmail is OAuth-only.

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
