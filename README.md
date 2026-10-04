# zmail

A local Linux desktop email client for Gmail that plays a custom sound for
new mail, per sender or per rule, the way Eudora did.

**Status:** v0.2.1: Gmail sign-in and read-only sync (Inbox, labels, read +
mark-as-read, move to Trash, new-mail chime) with a reworked message viewer. Sending comes next. See [docs/PLAN.md](docs/PLAN.md).

- Linux amd64 first, packaged as a `.deb`
- Gmail over OAuth 2.0 only; no passwords are ever stored, and tokens live in
  the OS keyring (Secret Service / libsecret)
- Local offline cache with full-text search
- C++20, Qt 6 Widgets, CMake/CPack (same stack as
  [zterminal](https://github.com/sbj-ee/zterminal) and
  [zwriter](https://github.com/sbj-ee/zwriter))

## Connect your Gmail

zmail uses **your own** Google OAuth client, so nothing goes through a
third-party server. One-time setup (about 5 minutes):

1. Open [Google Cloud Console](https://console.cloud.google.com/) and create
   (or pick) a project, e.g. `zmail`.
2. **APIs & Services → Library**: search for **Gmail API** and click **Enable**.
3. Open [Google Auth Platform](https://console.cloud.google.com/auth/overview)
   and click **Get started**: app name `zmail`, your email as the support and
   contact address.
4. **Audience**: user type **External**. Click **Publish app** so the status
   is **In production**. It stays *unverified*; that's fine for your own
   account. (Google shows a warning at sign-in, and refresh tokens don't
   expire after 7 days the way they do in Testing.)
5. **Clients → Create client**: application type **Desktop app**, name
   `zmail`, **Create**, then **Download JSON** in the "OAuth client created"
   box right away (Google may not show the secret again). It saves
   `client_secret_….apps.googleusercontent.com.json` to `~/Downloads`.
6. Start `zmail`. The **Connect your Gmail** dialog opens. Click
   **Choose client file…** and pick that file. zmail copies it to
   `~/.config/zmail/oauth-client.json` with mode 0600. (Or do it by hand:
   `install -D -m 600 ~/Downloads/client_secret_*.json ~/.config/zmail/oauth-client.json`.)
   zmail won't sign in while that file is readable by other users; the
   dialog offers **Fix (chmod 600)**.
7. Click **Sign in with Google**. Your browser opens Google's sign-in page.
   Pick your account. On **"Google hasn't verified this app"** click
   **Advanced → Go to zmail (unsafe)**. Tick the box that lets zmail
   **read, compose, and send email**, then **Continue**. The browser tab says
   *zmail is signed in*; switch back to zmail.

zmail asks only for `gmail.modify` plus `openid email` (to show which account
signed in). The refresh token goes to your system keyring (GNOME Keyring /
KWallet via QtKeychain), never to a file; the access token stays in memory.
The mail cache lives in `~/.local/share/zmail/<account>/zmail.db` (mode 0600,
directory 0700). **File → Sign Out** forgets the token and revokes it at Google.
If Google ever rejects the saved sign-in (revoked, password changed), zmail
shows the sign-in dialog again.

`zmail --offline` shows the built-in sample mailbox without connecting.

| First run | Sign in | Synced mailbox (mock data) |
|---|---|---|
| ![](docs/screenshots/gmail-setup.png) | ![](docs/screenshots/gmail-signin.png) | ![](docs/screenshots/gmail-synced.png) |

## Build

```sh
sudo apt install qt6-base-dev qt6-svg-dev qt6-multimedia-dev qtkeychain-qt6-dev \
  libqt6sql6-sqlite cmake ninja-build g++
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cd build && cpack -G DEB   # zmail_0.2.1_amd64.deb
```

The version lives only in `project(zmail VERSION ...)` in `CMakeLists.txt`.
The tests never talk to Google: they run against an in-process mock of the
OAuth and Gmail endpoints (`tests/mock/MockGoogle`).

## Reading mail

- HTML mail is shown on a white page, laid out to the width of the pane, with
  remote images blocked until you press **Load images** for that message.
  **View → Dark Background for Messages** gives a dark version.
- Ctrl+= and Ctrl+- (or Ctrl+wheel) zoom the message; Ctrl+0 resets it.
- **View → Preview Pane** shows the preview below the list or to its right.
  Drag the divider to resize it; zmail remembers the layout.
- Double-click a message (or press Ctrl+O) to open it in its own window.

## License

MIT. See [LICENSE](LICENSE).
