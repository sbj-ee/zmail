# Changelog

All notable changes to zmail. The version is the single
`project(zmail VERSION x.y.z)` in `CMakeLists.txt`; every pull request bumps it
([semantic versioning](https://semver.org/): patch for fixes and documentation,
minor for features) and adds an entry here. Releases are tagged `vX.Y.Z`,
published by `.github/workflows/release.yml` and signed with minisign (see
[docs/RELEASING.md](docs/RELEASING.md)). Entries before 0.9.0 are in the README's
status paragraph and on the
[GitHub Releases](https://github.com/sbj-ee/zmail/releases) page.

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
