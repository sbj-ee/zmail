# Releasing zmail

1. Make sure `project(zmail VERSION x.y.z)` in `CMakeLists.txt` has the version
   to release (every PR already bumps it and adds its `CHANGELOG.md` entry; the
   README's status line and the three window-title/user-agent test strings follow
   it). Replace "unreleased" in the CHANGELOG heading with the release date.
2. Merge to `main`, then tag and push: `git tag vX.Y.Z && git push origin vX.Y.Z`.
3. `.github/workflows/release.yml` builds and tests on Ubuntu 24.04, packages the
   `.deb`, writes `SHA256SUMS`, **signs it** with minisign (`SHA256SUMS.minisig`),
   checks that signature against the public key in `src/core/UpdateSigningKey.cpp`
   at the tagged commit, and creates the GitHub Release (`zmail_<version>_amd64.deb`,
   `SHA256SUMS`, `SHA256SUMS.minisig`). Without the signing secrets, or with no
   public key in the source, the publish job stops and nothing is released.
   Do not also create the release by hand: the workflow does it, and a release
   without `SHA256SUMS.minisig` can never be installed by the in-app updater.

## Signing key

The in-app updater (Help → Check for Updates → Install) installs only releases
whose `SHA256SUMS` verifies against the minisign public key compiled into zmail,
so a compromised GitHub release alone can't push malware to users.

zmail has its **own release key**: key ID `DBC46B23C3AF0EF3` (public key below),
generated with `minisign -G` in October 2026. Its public half is in
`src/core/UpdateSigningKey.cpp`. The secret key lives only on the maintainer's
machine (`~/.minisign/minisign.key`) and, with its password, in the two
repository secrets `MINISIGN_SECRET_KEY` and `MINISIGN_PASSWORD` that
`release.yml` uses. It must never be committed or pasted into a chat. To set or
replace the secrets (the owner only):

```sh
gh secret set MINISIGN_SECRET_KEY -R sbj-ee/zmail < ~/.minisign/minisign.key
gh secret set MINISIGN_PASSWORD   -R sbj-ee/zmail    # paste the password when asked
```

To change the key: `minisign -G -p zmail-release.pub -s zmail-release.key`, set
the two secrets above from the new key, copy the second line of the `.pub` file
(it starts with `RW`) into `kUpdateSigningPublicKey` in
`src/core/UpdateSigningKey.cpp`, and update the key ID in this file, in the
README's "Updates" section and in `tests/tst_minisign.cpp`
(`compiledReleaseKeyIsValid`). `tests/data/minisign/release-key/` holds a real
signature made by a *different* key (zterminal's, with its public key as a
constant in the test): it only proves the verifier reads real minisign output
and does not need to change. Keep the secret key and its password in a password
manager plus an offline backup: if it is lost, users can't update in the app
(they install the next release by hand); if it leaks, rotate it (below).

Check a release by hand:

```sh
minisign -Vm SHA256SUMS -P RWTzDq/DI2vE2xMeF01QWTxP2lwwlxGL9M1UxQE4EFV5kVmTF84wwyi2
```

## Releases before the first signed one

0.8.0 and earlier have no `SHA256SUMS.minisig`, and 0.8.0 has no updater: install
the first signed release (0.9.1 or later) by hand,
`sudo apt install ./zmail_<version>_amd64.deb`, after verifying it with
`minisign -Vm` and `sha256sum -c`. From then on updates are signature-checked.
A release published without a signature is offered by the app only as a link
to its release page.

## Rotating the key

Generate a new key pair, update the secrets and `src/core/UpdateSigningKey.cpp`,
and release. Clients that still have the old key can't verify releases signed
with the new key, so tell users to install that release by hand.

## Current release key

Key ID `DBC46B23C3AF0EF3`:

```
RWTzDq/DI2vE2xMeF01QWTxP2lwwlxGL9M1UxQE4EFV5kVmTF84wwyi2
```
