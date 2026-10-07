# zmail: plan

zmail is a local Linux desktop mail client for one Gmail account
(stevebj.ee@gmail.com). Its signature feature comes from Eudora: a custom
sound, and a color, per sender or per rule when new mail arrives. This document
covers the stack, the architecture, the Google setup, the feature scope for v1,
tests and milestones. It is the plan of record for PR #1. Nothing here is built
yet beyond the scaffold.

Sources are cited inline as [n] and listed at the end. Google facts were
checked against Google's live documentation on 2026-10-03.

## 1. Stack decision

**C++20, Qt 6 Widgets, CMake + CPack, Linux amd64 `.deb` first, MIT.**

It matches zterminal and zwriter: the same CMake layout, the generated
`version.hpp` from `project(VERSION)`, the CPack `.deb`, the CI on
ubuntu-24.04, and the GitHub-releases update checker (ported from zwriter's
`UpdateChecker` in this PR). Qt also covers everything a mail client needs
from one vendor: networking with TLS (QNetworkAccessManager), SQLite (Qt SQL),
D-Bus (QtDBus) for notifications, audio (Qt Multimedia), a sandboxed HTML
engine (QtWebEngine) and rich-text editing (QTextEdit). There's nothing to
gain from a second language or toolkit. The floor is **Qt 6.4.2**, the version
Ubuntu 24.04 ships. APIs from later versions (for example `QStyleHints::colorScheme()`,
added in 6.5) go behind `QT_VERSION` checks with fallbacks.

Libraries (all available as Ubuntu 24.04 packages, all MIT-compatible):

| Need | Choice | License | Why |
|---|---|---|---|
| Mail transport | **Gmail REST API** over QNetworkAccessManager | n/a | see §2 |
| OAuth 2.0 | Small in-house loopback + PKCE flow (QTcpServer on 127.0.0.1) | MIT (ours) | QtNetworkAuth's PKCE support only arrived in Qt 6.8; Ubuntu 24.04 has 6.4. The flow is about 300 lines and easy to unit test. Switch to `QOAuth2AuthorizationCodeFlow` once the floor is 6.8 or later. |
| Token storage | **QtKeychain** (`qtkeychain-qt6-dev`) → Secret Service / libsecret | BSD-3 | The standard Qt keyring wrapper. Works with GNOME Keyring and KWallet. |
| MIME parse/build | **GMime 3.2** (`libgmime-3.0-dev`) | LGPL-2.1+ (dynamic link) | Mature (it's notmuch's parser). Handles both parsing and building `multipart/alternative`. vmime was rejected because it's GPL. KF6 Mime isn't packaged in 24.04. mailio is less proven on hostile input. |
| Cache/search | SQLite with **FTS5**, via Qt SQL | public domain | §4.3 |
| HTML view | **QtWebEngine**, sandboxed, JS off | LGPL-3 | §4.4 |
| Spell check | **Hunspell** + `hunspell-en-us`, reusing zwriter's wrapper | LGPL/MPL | §4.6 |
| Sound | Qt Multimedia (`QSoundEffect` for WAV, `QMediaPlayer` for OGG) | LGPL-3 | |
| Markdown compose | **md4c** + md4c-html (`libmd4c-dev`, `libmd4c-html0-dev`) | MIT | A fast, CommonMark-compliant C parser with GitHub tables and strikethrough. Renders Markdown to HTML for sending (§4.5). |
| Zip attachments | **libzip** (`libzip-dev`) | BSD-3-Clause | Standard deflate `.zip` with UTF-8 names, used only after the user agrees (§4.5.1). minizip-ng (zlib license) is the fallback if libzip is a problem. |
| Icons | **Lucide** SVG set, bundled, rendered with QtSvg (`qt6-svg-dev`) | ISC (a few icons MIT, from Feather) | Each file is listed in `assets/icons/LICENSES` and checked in CI (§4.15). |

## 2. Gmail API vs IMAP: the decision

**Use the Gmail REST API and its History API for sync. Don't use IMAP in v1.**

- **Labels are first-class.** Gmail labels map one-to-one onto API label IDs.
  Over IMAP they become fake folders, which brings duplicate copies and the
  `X-GM-LABELS` extensions.
- **Incremental sync is cheap and exact.** `history.list` from a stored
  `historyId` returns only what changed, at 2 quota units per call. If the
  `historyId` is too old the API returns 404, and the client does a full
  resync [5]. Per-user quota is 6,000 units a minute, and `messages.get` costs
  20 units [4], so a poll every 30 s is negligible.
- **Push.** Gmail API push goes through Cloud Pub/Sub, which is aimed at
  servers. For installed apps and user devices, Google still recommends
  polling with the sync guide [6]. IMAP IDLE would give instant push, but it
  would need the broad `https://mail.google.com/` scope and an IMAP parser.
  **Decided: v1 polls `history.list` every 30 s** (configurable in Settings,
  minimum 15 s) and again on window focus. That's close enough to instant for
  a desk client. An IMAP IDLE "doorbell" would need the full-mail scope, which
  was ruled out (§8), so it's off the table unless that decision is revisited.
- **Raw messages.** `messages.get?format=raw` returns RFC 822 bytes for the
  offline cache. `messages.send` takes RFC 822 bytes that we build with GMime.
- **Scopes. Decided: `gmail.modify` only** (read, label, archive, send, trash,
  no permanent delete) [3]. "Delete" in zmail always means move to Trash. Google classes `https://mail.google.com/`, `gmail.readonly`,
  `gmail.compose`, `gmail.insert`, `gmail.modify` and `gmail.metadata` as
  **restricted** [3], so any useful mail-reading scope makes zmail a
  restricted-scope app. zmail never requests full mail access
  (`https://mail.google.com/`). People API scopes are optional (§4.11).

### 2.1 OAuth token lifetime (the key finding)

Google's OAuth documentation says: *"A Google Cloud Platform project with an
OAuth consent screen configured for an external user type and a publishing
status of 'Testing' is issued a refresh token expiring in 7 days, unless the
only OAuth scopes requested are a subset of name, email address, and user
profile."* [1] Gmail scopes aren't in that subset. **In Testing mode, zmail
would make Stephen sign in again every week.**

**Decided:** set the app's publishing status to **In production** and leave
it **unverified** (personal use):

- Apps with restricted scopes normally need verification and, if they store or
  transmit restricted data on servers, a security assessment [3][8]. Google
  lists **personal use** as an exception: *"If the app is for your personal
  use (fewer than 100 users), you and your limited number of users can
  continue using the app without going through verification (users will be
  allowed to click through 'unverified app' warning screens during
  sign-in)"* [7][8]. zmail keeps data on Stephen's machine only, with no
  server, so the security-assessment trigger doesn't apply either.
- What that costs: an "unverified app" warning on each consent (Advanced →
  Go to zmail), a cap of 100 new users in total [2], and the app must comply
  with the User Data Policy regardless.
- Refresh tokens then last until revoked. Revocation still happens if the
  user revokes access, if the token goes unused for 6 months, if **the
  password changes (for Gmail scopes)**, or if the user exceeds the per-client
  token limit (currently 100) [1]. zmail handles `invalid_grant` by showing
  "Sign in again", never by asking for a password.

Each zmail user creates their own Cloud project (§3). That keeps every
install under its own 100-user cap and within the personal-use exception.

## 3. Google Cloud setup (Stephen does this once)

Matches the first-run dialog and the README's "Connect your Gmail" (PR #3):

1. <https://console.cloud.google.com/>: **create a project** (e.g. `zmail`).
2. **APIs & Services → Library**: enable the **Gmail API**. (People API later,
   for contacts sync.)
3. **Google Auth Platform → Get started** (Branding): app name `zmail`,
   support/contact email stevebj.ee@gmail.com.
4. **Audience**: **User type: External**, then **Publish app** → **In
   production** (unverified; don't submit for verification). No test-user
   list is needed in production, and refresh tokens don't hit the 7-day
   Testing expiry (§2.1).
5. **Data Access** (optional): add `https://www.googleapis.com/auth/gmail.modify`.
   zmail requests exactly `gmail.modify openid email` at sign-in either way.
6. **Clients → Create client → Application type: Desktop app**, name `zmail`.
   **Download JSON** in the "OAuth client created" box right away (Google
   may not show the secret again) → `~/Downloads/client_secret_<id>.apps.googleusercontent.com.json`.
7. In zmail's **Connect your Gmail** dialog, **Choose client file…** copies it
   to `~/.config/zmail/oauth-client.json` (or `$ZMAIL_OAUTH_CLIENT`) with mode
   0600, directory 0700, via an `O_EXCL` temp file + rename. A file that's
   group/other-readable gets a warning and a **Fix (chmod 600)** button, and
   sign-in stays disabled until it's 0600. Only the standard `installed`
   format is accepted; a "Web application" client or non-Google endpoints are
   refused. The repo's `.gitignore` excludes `client_secret*.json` and
   `oauth-client*.json`. **Never commit this file.**
8. **Sign in with Google** → browser → **Advanced → Go to zmail (unsafe)** →
   allow Gmail access.

**About the client secret:** for Desktop clients, Google says the secret is
embedded in the app and "is obviously not treated as a secret" [1]. Security
comes from PKCE and the loopback redirect (`http://127.0.0.1:<random port>`)
[9]. We still keep it out of git, so a public repo doesn't hand out a working
client ID that others could burn quota on.

**Incremental consent:** sign-in asks for Gmail only. Turning on contacts sync
later starts a second authorization with `include_granted_scopes=true`, adding
the People scopes to the same grant [9]. The keyring then holds one refresh
token covering both.

## 4. Architecture

```
ui/  (MainWindow, three panes, Compose, Settings, RulesEditor, Scheduled, Contacts)
 │
 ├─ auth/      OAuthLoopback (PKCE), TokenStore (QtKeychain), AccessTokenProvider
 ├─ gmail/     GmailClient (REST, retry/backoff, batch), PeopleClient
 ├─ sync/      SyncEngine (full + history), Poller, Outbox, SnoozeWaker
 ├─ cache/     Db (SQLite schema/migrations), BlobStore, SearchIndex (FTS5), QueryParser
 ├─ mime/      MimeParser (GMime → model), MessageBuilder, HtmlSanitizer, AttachmentPolicy
 ├─ view/      MessageView (QtWebEngine + interceptor), PlainView (QTextBrowser)
 ├─ compose/   ComposeWindow (QTextEdit), SpellChecker+Highlighter, Signatures
 ├─ scan/      SecurityScanner (headers, senders, links, attachments, wording, clamd)
 ├─ rules/     RuleEngine (match → sound/color/notify/priority), QuietHours
 ├─ notify/    Sounds (Qt Multimedia), Notifier (org.freedesktop.Notifications)
 ├─ contacts/  AddressBook, VCard, Autocomplete, GroupExpander
 ├─ theme/     ThemeManager (light/dark/system, accent, palette)
 └─ update/    UpdateChecker (GitHub releases/latest; ported from zwriter)
```

Everything except the UI is a Qt-free-at-the-edges library (`zmail_core`)
with unit tests. Network calls go through one `GmailClient` interface, which
has a fake implementation for tests.

### 4.1 Auth
`OAuthLoopback` opens a `QTcpServer` on 127.0.0.1 at a random port, generates
a PKCE verifier and S256 challenge plus a `state` value, and opens the
browser with `QDesktopServices::openUrl`. It accepts exactly one matching
redirect, then exchanges the code. The refresh token goes to the keyring
under service `zmail`, account `stevebj.ee@gmail.com`. Access tokens stay in
memory only. No password is ever requested or stored. If the keyring is
locked or missing, zmail refuses to persist tokens, and never falls back to a
plaintext file.

### 4.2 Sync engine
- **Initial sync:** `labels.list`. Then `messages.list` newest first (INBOX and
  the last 90 days first, the rest in the background), then
  `format=raw` fetches through batch requests, while respecting quota.
- **Incremental:** `history.list(startHistoryId)` for messagesAdded/Deleted
  and labelsAdded/Removed. A 404 triggers a full resync [5].
- **New-mail detection:** a message is "new" if it arrives through history
  with INBOX and UNREAD. Each new message goes through the scanner, then the
  rule engine, then notifications and sound.
- **Local actions** (archive, label, read, trash) apply optimistically and
  queue in `pending_ops`, then replay with exponential backoff (so they work
  offline).

### 4.3 Cache, data model and fast search
The data lives in `~/.local/share/zmail/<account>/` (mode 0700): `zmail.db`
(SQLite, WAL) and `raw/<aa>/<gmailId>.eml`, a maildir-like layout of raw
RFC 822 files that can be rebuilt from the server.

Core tables:
`messages(id PK, thread_id, history_id, internal_date_utc, from_addr, from_name, to_addrs, cc_addrs, subject, snippet, size, has_attachment, unread, starred, body_text, raw_path)`,
`labels(id PK, name, type, color)`, `message_labels(message_id, label_id)`,
`attachments(message_id, part_id, filename, mime_type, size, risky)`,
`sync_state(account, history_id, last_full_sync_utc)`, `pending_ops`.

**Search:** `messages_fts` is an FTS5 external-content table over
`subject, from_name, from_addr, to_addrs, body_text, attachment_names`, using
`tokenize='unicode61 remove_diacritics 2'` and `prefix='2 3'`. It's kept in
sync by triggers. `QueryParser` turns `from:`, `to:`, `subject:` into
column-scoped FTS terms (`from_addr:alice*`). `has:attachment`,
`before:`/`after:` (dates in local time, converted to UTC epochs) and `label:`
become SQL predicates on indexed columns (`idx_messages_date`,
`message_labels(label_id, message_id)`). Free text becomes an FTS `MATCH`.
Results are ordered by date descending with `LIMIT 200`, and bm25 ranking is
available as an option. **Target:** p95 under 100 ms on 100k messages, on a
warm cache, on Stephen's desktop. HTML bodies are indexed as extracted text,
with quoted replies trimmed to keep the index small. Index updates run in
the sync thread in 500-message transactions, with `optimize` after an
initial sync.

### 4.4 MIME rendering (renderer decision)
**QtWebEngine for HTML viewing, QTextBrowser for "View as plain text", and
QTextEdit for composing.**

QTextBrowser understands only an HTML 4 subset with basic CSS. It mangles
table- and CSS-heavy newsletters and can't do modern layouts. Its one
advantage, that there's no JS at all, can be matched in WebEngine by
configuration. The QtWebEngine setup:
- off-the-record `QWebEngineProfile`: no cookies, cache or storage on disk
- `JavascriptEnabled=false`, `JavascriptCanOpenWindows=false`,
  `PluginsEnabled=false`, `LocalContentCanAccessRemoteUrls=false`,
  `DnsPrefetchEnabled=false`, `AutoLoadIconsForPage=false`
- a `QWebEngineUrlRequestInterceptor` that blocks every request except the
  `zmail-cid:` custom scheme (inline `cid:` parts served from the cache) and
  `data:`. When the user clicks **Load images** for a message, or the sender
  is on the per-sender **always allow** list, http/https image requests for
  that page only are allowed
- an injected CSP `<meta>`: `default-src 'none'; img-src zmail-cid: data:
  [https: if allowed]; style-src 'unsafe-inline'`
- `acceptNavigationRequest` blocks all navigation. Link clicks open in the
  system browser after showing the real target, which feeds the scanner's
  mismatch warning
- Chromium's sandbox stays on. It's never disabled.

The cost is a larger dependency (`qt6-webengine`, about 150 MB installed).
For a mail client that has to show real HTML mail, that's worth it.
**Decided:** Stephen accepts the size.

### 4.4.1 Interim viewer, v0.2.1 (QTextBrowser)
Until the WebEngine view lands, `ui/MessageView` makes the QTextBrowser
viewer usable for real mail. 0.2.0 drew builder-made HTML (Unlayer,
Mailchimp, PetDesk...) as a ~90 px column with words broken mid-word, faint
theme-coloured text and a white card on the dark theme. The causes, and the
fixes in `ui/HtmlFit`:

- These templates keep their fixed widths (`<table style="width:500px">`,
  `<td width="250">`) inside Outlook-only `<!--[if mso]>` comments. What's
  left is an outer table with no width and nested `width=100%` tables, which
  QTextDocument shrinks to the narrowest word. `HtmlFit::fit()` runs after
  `setHtml()`: tables with no width become 100 %, fixed tables or columns
  wider than the pane become proportional percentages, narrower ones keep
  their size; images are capped to the pane, keeping aspect.
- QTextDocument ignores CSS `display:table`/`table-cell` and draws a
  `<div>`'s background only behind its own lines. `HtmlFit::prepare()` turns
  those divs into real tables, so columns stay side by side and coloured
  bands sit behind their content. A link inside `<span style="color:...">`
  gets that colour (Qt would paint it link-blue).
- Mail is drawn on a white page with dark text whatever the app theme, like
  other clients. **View → Dark Background for Messages** (`viewer/darkMail`)
  inverts the mail's own colours' lightness instead.
- Content that still can't fit (a long unbreakable line) is zoomed to fit
  rather than scrolled sideways. Plain-text mail is capped at 78 characters
  per line and links are made clickable.

Around the body: a header block (Subject, From, To, Cc, Date, label), an
attachments row, and a **Load images** bar. Remote images stay blocked
until it's pressed, per message; they're then fetched with no cookies, no
auth, a 15 s timeout and a 10 MB cap per image. `cid:` parts aren't fetched.
Ctrl+= / Ctrl++ / Ctrl+- / Ctrl+0 and Ctrl+wheel zoom (50-300 %, remembered
as `viewer/zoom`). The list/preview splitter remembers its sizes per layout,
and **View → Preview Pane** puts the preview below the list (Eudora) or to
its right (`ui/previewRight`). Double-click (or Message → Open in New
Window, Ctrl+O) opens a message in its own window with Reply / Reply All /
Forward / Delete. Delete is `users.messages.trash` with an optimistic cache
update that is rolled back on failure. Several windows can be open; they
cascade from the last saved geometry (`messageWindow/geometry`). Since 0.3.0
Reply, Reply All and Forward there open the composer for that message.

### 4.5 Compose and send
- **Plain text, HTML or Markdown**, with a mode switch per message (the
  compose toolbar's **Format** box). The default is set in Settings. HTML compose uses QTextEdit's rich text (bold, italic, lists,
  links, inline images). Plain text uses the same widget with
  `setAcceptRichText(false)`.
- **HTML mail** goes out as `multipart/alternative`: a `text/plain` part
  generated from the document (lists become `-`/`1.`, links become
  `text <url>`), plus `text/html`. Inline images make it
  `multipart/related`, and attachments `multipart/mixed`. GMime builds the
  message, and `messages.send` sends it (with `threadId` for replies,
  plus `In-Reply-To`/`References`).
- **Markdown mode:** you write Markdown in a plain-text editor, with
  monospace for code and lightly styled headings, emphasis and list
  markers. A **live preview** toggle shows a side-by-side rendered pane,
  updated about 150 ms after typing stops. On send, **md4c** renders the
  source to HTML (CommonMark plus tables, strikethrough and autolinks; raw
  HTML is disabled, so pasted `<script>` text stays text). The message goes
  out as `multipart/alternative`:
  - `text/html` is the rendering, with zmail's minimal inline CSS for code
    blocks and quotes
  - `text/plain` is the Markdown source, lightly cleaned: reference links
    are expanded to `text <url>`, image syntax becomes `[image: alt]`, and
    HTML entities are decoded

  **Recipients never see raw Markdown as markup.** HTML clients show the
  rendering, and text clients get source that's readable as is. Markdown
  messages use the **HTML version of the signature**, appended after
  rendering. **Spell check** skips code spans, fenced or indented code
  blocks, link URLs and the signature. The **size meter** counts the
  rendered output (HTML part, text part and attachments, encoded).
- **Signatures:** several named signatures, each with a plain and an HTML
  version. One is the default and is inserted into new messages and replies.
  The plain version gets the standard `"-- \n"` delimiter. In replies, the
  signature goes **above** the quoted text. A signature combo box in the
  compose window switches it per message, replacing only the signature
  block, which a document marker tracks. The version matching the message
  format is used. Signatures are stored in the `signatures` table and edited
  in Settings.
- **Attachments (sending):** add them with the file picker or by dropping
  files onto the compose window. A chip list shows each file's size, and a
  **size meter** in the compose window shows the running total of the
  **encoded** message. The limits and thresholds are in §4.5.1.

#### 4.5.1 Message size limits (mirroring Google's)
Every limit lives in a single constants header, **`src/core/Limits.h`**
(added in this PR, with unit tests). Each value is written next to its doc
link, so a change on Google's side is a one-line edit. Checked on
2026-10-03:

| Limit | Value used | Google's figure | Source |
|---|---|---|---|
| Send (personal Gmail) | `kSendLimitBytes` = 25,000,000 | "For personal Gmail accounts, the limit is 25 MB" | [11] |
| Gmail API upload for `messages.send` and `drafts.*` | `kApiSendUploadMaxBytes` = 36,700,160 (35 MiB) | `mediaUpload.maxSize: "36700160"` | [18][12] |
| Receive | `kReceiveLimitBytes` = 50,000,000 | "receive emails of up to 50 MB ... after encoding, which adds about a 37% increase" | [19] |

How these compare with what we expected:
- **Send:** 25 MB, as expected.
- **API upload:** 35 MB as expected. Precisely, it's 35 **MiB** (36,700,160
  bytes), and `messages.import`/`insert` allow 150 MiB.
- **Receive:** 50 MB as expected, but Google documents it on the
  **Workspace** receiving-limits page (Enterprise Plus can get 70 MB). The
  personal-Gmail help page states a send limit only. zmail uses 50 MB solely
  to plan downloads.
- **Encoding overhead:** Google quotes "about 37%", not 33%. Base64's 4/3
  plus a CRLF every 76 characters works out to about 1.37×.

The 25 MB send limit is lower than the API cap, so it's the one that
binds. Rules:
- **Measure the whole encoded MIME message**: headers, text and HTML parts,
  inline images, and every attachment at `base64MimeSize(n) = 4·⌈n/3⌉ +
  2·⌊(len−1)/76⌋`. That's not the sum of file sizes. An 18.5 MB file already
  goes over the limit.
- **Size meter:** green below 80% (`kSendWarnPercent`, 20 MB encoded),
  amber from 80%, and red above the limit.
- **Block sending** when the total is over `kSendLimitBytes`. Send is
  disabled with "Message is 26.1 MB after encoding; Gmail's limit is
  25 MB", and the zip offer below is shown.
- **Offer to zip, never zip silently.** When attachments push a message over
  the limit, zmail runs a quick **deflate probe** first. It compresses up to
  the first 1 MB of each file at level 6 on a worker thread, which takes
  well under a second, and extrapolates the ratio. A dialog then shows the
  estimated savings: "Zipping could bring this message from 31.4 MB to about
  22.9 MB (encoded)." The choices are **Zip attachments** and **Cancel**,
  plus a **Remember my choice** checkbox. Settings → Composing →
  "When attachments are too large" offers **Ask** (default), **Always zip**
  and **Never** (block only).
  - The archive is a standard **`.zip` (deflate, UTF-8 filenames, flag bit
    11)** built with **libzip**, which Windows Explorer and macOS Finder
    open natively. It's named `attachments.zip`, or `<subject>.zip` when
    the subject is short. The originals stay listed in the chip bar, and
    one action reverts the zip. There's no encryption or ZIP64 in v1.
  - After zipping, zmail **recomputes the encoded size with the same
    `Limits.h` math** (`base64MimeSize` of the real archive bytes). If it
    still doesn't fit, the message says so plainly: "Still 27.8 MB after
    zipping. PDFs, JPEG/PNG photos, video, audio and existing archives are
    already compressed and barely shrink. Share large files with a Google
    Drive link, or split them across several messages." The probe flags
    those types up front, so the estimate isn't overly hopeful. The same check runs
  again in `MessageBuilder` on the final bytes, before `messages.send`
  and before the outbox (§4.12) accepts a scheduled message.
- **Large received messages:** messages can't exceed the receive limit, but
  big ones still need care. Anything whose `sizeEstimate` is over
  `kLargeMessageBytes` (5 MB):
  - is fetched in stages: `format=full` structure, then text/HTML, then
    attachments through `messages.attachments.get` on demand or in idle time
  - streams to disk from `QNetworkReply::readyRead`, so it's never buffered
    whole in memory
  - shows progress in the preview pane, with a size-scaled timeout
  - restarts after a failure without blocking the sync queue
  - is marked "attachments not yet downloaded" when offline

### 4.6 Spell check (reused from zwriter)
zwriter's `SpellChecker` is a thin Hunspell wrapper: it searches
`/usr/share/hunspell` and `/usr/share/myspell/dicts` for `en_US.aff/.dic`,
supports session-only `ignoreWord`, and keeps a persistent
`addToUserDictionary` at `AppDataLocation/user-dictionary.txt`, which
resolves to `~/.local/share/sbj-ee/zwriter/user-dictionary.txt` because
zwriter sets organization `sbj-ee`. Its
`WritingHighlighter` is a `QSyntaxHighlighter` with a pluggable
"is misspelled" function that draws a red wavy underline. zmail copies both
(MIT, same author), along with zwriter's CMake Hunspell detection (an
optional `ZMAIL_HAS_HUNSPELL`, a no-op when absent). The highlighter skips
quoted lines (`> `), URLs, email addresses and the signature block. The
compose context menu shows up to 8 suggestions, then **Add to Dictionary**
and **Ignore**. A `QSyntaxHighlighter` works on the QTextDocument, so spell
check is the same in plain and HTML mode. The `.deb` depends on
`libhunspell-1.7-0, hunspell-en-us`.

**Decided: share zwriter's personal dictionary.** zmail reads and appends to
`~/.local/share/sbj-ee/zwriter/user-dictionary.txt`, the same one-word-per-line
format zwriter uses, instead of keeping its own. A word added in either app
is known to both:
- the file is created if it doesn't exist, and is never rewritten in place
- **Add to Dictionary** appends one line with `O_APPEND`, so writes from both
  apps don't clobber each other
- a `QFileSystemWatcher` reloads the file when zwriter changes it
- Settings → Spelling shows the path and has an override, for anyone who
  wants a separate file
- session-only **Ignore** stays per-app

### 4.7 Attachments (receiving)
The viewer's attachment bar lists name, size and MIME type, with **Open**,
**Save**, and **Save All** (to a chosen folder, renaming on collisions).
Images (png/jpeg/gif/webp) preview inline and are decoded only with
`QImageReader`, with size limits set; they never go to an external viewer
automatically. **Open** writes the file to a private temp dir
(`QTemporaryDir`, mode 0700, under `$XDG_RUNTIME_DIR/zmail`) and calls
`QDesktopServices::openUrl`. Saved files get mode 0600 or 0644 and **never**
the exec bit. `AttachmentPolicy` marks a file risky if **either** its
extension (`.exe .sh .desktop .AppImage .jar .bat .cmd .run .py .pl .msi .scr
.ps1 .vbs .js .deb .rpm`, double extensions) **or** its sniffed type
(`QMimeDatabase` by content: ELF, PE, shell shebang, `application/x-desktop`,
`x-executable`, java archive) says so. Risky files get a confirmation
explaining the risk before Open. Save is still allowed.

### 4.8 Security scanning (local only)
Nothing leaves the machine. `SecurityScanner` runs on each new message and
produces `scan_results(message_id, score, level, reasons_json, scanned_utc,
scanner_version)`. Each reason has a code, a weight and a human-readable
explanation.

Checks:
- **SPF/DKIM/DMARC:** parse only the **top-most** `Authentication-Results`
  header, and only if its authserv-id is `mx.google.com`. Headers lower in
  the chain may have been injected by the sender and are ignored. A
  `dmarc=fail`, or `spf`/`dkim` fail or none on a domain that claims a brand,
  adds weight.
- **Sender spoofing and lookalikes:** a display name that contains a
  different email address or domain ("PayPal <x@evil.tld>"). Confusable
  characters, detected by comparing the Unicode TR39 skeleton of the domain
  and display name (with a bundled confusables table) against known
  contacts, frequent correspondents and a bundled brand list. The brand list is editable in Settings → Security,
stored at `~/.config/zmail/brands.json`. The proposed default: major US banks
and card issuers, PayPal, Venmo, Amazon, Apple, Google, Microsoft,
DocuSign, Dropbox, FedEx, UPS, USPS, DHL, IRS/SSA, plus Stephen's
employer domain once he gives it. Punycode
  `xn--` domains. Mixed-script labels. Edit distance ≤ 2 from a known
  contact's domain. `Reply-To` that differs from `From`.
- **Links** (from the HTML DOM and from text): visible text that looks like a
  URL but whose domain differs from the href's. Known shorteners (a bundled
  list). IP-literal hosts. `@` userinfo tricks. Punycode hosts.
- **Attachments:** executables (from §4.7), macro Office files (`.docm .xlsm
  .pptm .dotm`, and OLE files containing a `VBA` storage), double
  extensions, encrypted zip/7z/rar (detected from the headers' encryption
  flags). An **optional ClamAV** scan streams parts to `clamd` over its local
  socket with `INSTREAM`. It only runs if a clamd socket exists, and it never
  sends anything off-machine.
- **Wording:** a weighted local keyword list (urgency plus credential
  phrases: "verify your account", "password expires", "wire transfer",
  "gift card"), with weights capped, so wording alone can't reach
  "suspicious".

The score maps to Clean, Caution or Suspicious. **The UI** shows a warning
banner above the message listing the reasons, a ⚠ column in the message
list with the fixed warning color, and "Suspicious:" in notifications.
**Gmail label mirror (decided: on by default):** a Suspicious result
(from the scanner or from the user) applies the Gmail label
`zmail/Suspicious`, so other devices see it. **Mark Safe** removes the label.
Settings → Security can turn the mirror off; zmail then keeps verdicts local
only. **User controls:** Mark Safe / Mark Suspicious (`user_verdicts` table), and a trusted-sender
allowlist (`trusted_senders`, also fed by the contacts trusted flag). Suspicious
mail can trigger its own sound through a built-in rule condition
`scan:suspicious`.

**Limits:** these are heuristics. False positives and false negatives will
happen. The scanner never deletes, moves or hides mail. It only labels and
warns, and Gmail's own spam filtering is still the first line of defense.

### 4.9 Rules: sound, color, notification, priority
> **Built in 0.6.0 as Filters** (`core/Rules`, `ui/RulesDialog`): ordered rules,
> first match wins; conditions on from / to / subject / any with contains,
> does not contain, is, starts with, ends with, regex, ANDed or ORed; actions
> colour, flag, move to folder, mark read, sound (a WAV, the usual one, or
> none). The file is `<config>/rules.json` with explicit condition objects
> rather than the glob map sketched below. Not built yet: label / contact /
> group / keyword / scan conditions, notify and priority actions, quiet
> hours, OGG/MP3 and the sound import folder.

One rule list, edited in **Settings → Rules**, stored as JSON at
`~/.config/zmail/rules.json`. **First match wins**, as in Eudora's filters.
That's predictable, and order is easy to reason about in the editor
(drag to reorder). One matching rule supplies all its actions. Conditions:
`from`, `to`, `subject`, `label`, `contact`, `group`, `keyword` (body text),
`scan` (clean/caution/suspicious). Each is glob or regex, case-insensitive,
and conditions are ANDed. Actions: `sound`, `color`, `notify`
(true/false/silent), `priority` (low/normal/high).

```json
{
  "quietHours": { "start": "22:00", "end": "07:00", "days": "all",
                  "allowPriority": "high" },
  "rules": [
    { "name": "Suspicious", "match": { "scan": "suspicious" },
      "sound": "sounds/uh-oh.ogg", "priority": "high" },
    { "name": "Family", "match": { "group": "Family" },
      "sound": "sounds/chime.wav", "color": "#007a7a", "priority": "high" },
    { "name": "Boss", "match": { "from": "*@neros.tech", "subject": "*urgent*" },
      "sound": "sounds/klaxon.wav", "color": "#c62828" },
    { "name": "Work label", "match": { "label": "Work" }, "color": "#6a1b9a" },
    { "name": "Newsletters", "match": { "label": "CATEGORY_PROMOTIONS" },
      "notify": false },
    { "name": "Default", "match": {}, "sound": "default" }
  ]
}
```

**Sound files.** Every rule, including the default new-mail sound, accepts a
**user-supplied WAV, OGG or MP3** (Stephen's own clips, such as the Holy Grail
"arrow thunk + Message for you, sir"). Playback uses **`QMediaPlayer` +
`QAudioOutput`** for every format. `QSoundEffect` only handles WAV, so it's
used only as a low-latency path for bundled WAVs. The rules editor and
Settings → Sounds both have **Choose file…** (WAV/OGG/MP3 filter, probed with
`QMediaPlayer` before acceptance) and a **▶ Preview** button. An imported
file is **copied** into `~/.local/share/zmail/sounds/` (name de-duplicated,
mode 0600), and the rule stores that copy's path. Moving or deleting the
original then can't break the rule. A missing file falls back to the
default sound and logs a warning.

**Copyright.** User clips stay on the user's disk and are **never** committed
or bundled. In particular, the Monty Python clip and any other copyrighted
audio must never enter the public repo. `.gitignore` excludes
`/sounds-local/`, and review rejects audio outside `assets/sounds/`.
**Decided: the default sounds are bundled, and are original (generated by
our own script, as zwriter's `gen_typewriter_sounds.py` does) or CC0 only.
Users can pick their own files for any rule, including the default sound.**
The proposed original set is chime (default), thunk and klaxon. Each bundled file has an
entry in `assets/sounds/LICENSES` (file name, source/author, license,
URL). A CI step (`tools/check-asset-licenses.sh`, which also checks icons) fails the
build if any `assets/sounds/*.{wav,ogg,mp3}` lacks an entry, or if an entry's
license isn't `CC0-1.0` or `original-MIT`.

**MP3 and packaging.** On Ubuntu 24.04, Qt 6.4's `libqt6multimedia6`
ships both the GStreamer and the FFmpeg media backends
(`plugins/multimedia/libffmpegmediaplugin.so`). zmail sets
`QT_MEDIA_BACKEND=ffmpeg` at startup unless the user has set it, so MP3, OGG
and WAV decode through FFmpeg (`libavcodec60`/`libavformat60`, which the
plugin package pulls in). The `.deb` declares `Depends: libqt6multimedia6`,
plus `Recommends: gstreamer1.0-plugins-good, gstreamer1.0-libav` as a
fallback for the GStreamer backend. A CI test plays (to a null audio sink)
a tiny generated WAV, OGG and MP3 and asserts `QMediaPlayer` reaches
`EndOfMedia` without errors.

When a sync batch brings several new messages, zmail plays **one** sound:
the highest-priority match, rate-limited to one every 3 s.
**Do-not-disturb** (a tray/menu toggle) and **quiet hours** mute sounds and
notification popups, except for priorities at or above `allowPriority`.

**Colors:** a rule's `color` sets the message-list foreground. Rows are
contrast-adjusted against the current palette's base color to at least a
4.5:1 WCAG ratio, by lightening in dark mode and darkening in light mode.
**Suspicious mail always uses the fixed warning foreground/background, and
rules can't override it.**

### 4.10 Notifications
`Notifier` uses QtDBus to call `org.freedesktop.Notifications.Notify`, with
sender and subject, a snippet, `app_name=zmail`, `desktop-entry=zmail` and
an urgency hint from the rule priority. It checks `GetCapabilities`, and
only if `actions` is advertised does it add **Reply** and **Archive**,
handled through `ActionInvoked`. `body-markup` is used only if advertised,
and text is escaped either way. More than 3 new messages are grouped into
one "N new messages" notification. Suspicious mail gets the title prefix
"⚠ Suspicious:" and the `critical` urgency is never used for it, so a
phisher can't make a sticky popup. Notifications follow the rules'
`notify` flag, DND and quiet hours.

### 4.11 Contacts
Tables: `contacts(id, display_name, phone, notes, trusted, sound, color,
source, google_resource, etag, updated_utc)`, `contact_emails(contact_id,
email, label, primary)`, `contact_groups(id, name)`,
`contact_group_members(group_id, contact_id)`, `address_stats(email,
sent_count, last_sent_utc, recv_count)`.

Features:
- **Add sender to contacts** from any message
- **Autocomplete** in To/Cc/Bcc, ranked by contacts first, then a frecency
  score from sent mail (`sent_count`, decayed by `last_sent_utc`)
- **vCard 3.0 and 4.0** import and export
- **groups** that expand to addresses on send (Bcc by default for groups of
  more than 10)
- **optional read-only Google Contacts sync** through the People API
  (`people.connections.list` with `syncToken`, plus `otherContacts.list`
  [10]), using the same sign-in through incremental consent (§3). Google
  contacts are read-only locally. Edits create a local overlay.

Tie-ins: a contact's `sound` and `color` become implicit rules evaluated
before the user's rule list. `trusted` feeds the security allowlist. All
contacts feed the lookalike check.

### 4.12 Schedule send
The Gmail API has **no public scheduled-send**: `messages.send` and
`drafts.send` take only the message or draft, with no time parameter
[12][13]. zmail implements a **local outbox**:
`outbox(id, raw_path, send_at_utc, tz_name, state[queued|sending|sent|failed|cancelled], attempts, lease_until_utc, message_id_hdr, gmail_draft_id)`.
- Users pick a date and time, or a preset: in 1 hour, tomorrow 8 AM, Monday
  8 AM, next week. Times are entered in local time, stored as **UTC plus the
  IANA zone** (America/Chicago), and recomputed through `QTimeZone` when
  edited. Across a DST shift the wall-clock time the user chose is kept.
  Nonexistent times (spring forward) move to the next valid minute, and for
  ambiguous ones the first occurrence is used.
- The **Scheduled** view offers edit, cancel and send now.
- **Missed sends** (whose time passed while zmail wasn't running) are listed
  at startup with a prompt: send now, reschedule or cancel. They're never
  sent silently.
- **`zmail-sender` systemd --user service. Decided: the `.deb` installs it
  disabled; turning it on is opt-in from Settings.** The `.deb` ships
  `/usr/lib/systemd/user/zmail-sender.service`, which the package scripts
  never enable. **Settings → Sending → "Send scheduled mail while zmail is
  closed"** runs `systemctl --user enable --now zmail-sender.service`, and
  turning it off runs `disable --now`. The service is the same binary,
  `zmail --sender`, which reads the refresh token from the keyring and sends due
  items. To make sure nothing is sent twice, a sender claims a row in
  `BEGIN IMMEDIATE` (`state=sending`, `lease_until`). The Message-ID is
  generated when the item is queued. Before retrying an expired lease, the
  sender searches Sent for `rfc822msgid:` to see whether the message
  already went out.
- **The tradeoff:** without the service, mail only goes out while zmail is
  open. With it, mail still only goes out while the machine is awake, logged
  in (the keyring is unlocked) and online. **Nothing is sent while the
  machine is asleep or off.** An optional setting **mirrors queued items as
  Gmail drafts** (`drafts.create`, covered by `gmail.modify`), so Stephen can send one from his phone if
  needed. zmail deletes the mirror draft after sending.

### 4.13 Snooze
`snoozes(message_id, wake_utc, tz_name, label_applied, created_utc)`. Presets
are later today (+3 h), tomorrow 8 AM, next week (Monday 8 AM), or a custom
time. Snoozed messages are hidden from INBOX in the local view. When one
wakes, it returns to the top of the inbox with a notification and rule
sound. **Decided: Gmail label mirror on by default.** zmail applies the label
`zmail/Snoozed` and removes `INBOX` on Gmail so other devices see it, then
restores INBOX and removes the label on wake. Settings can switch snooze to
local-only. The same caveat as schedule send applies: messages only
wake while zmail or the `zmail-sender` service is running. Overdue snoozes
wake at startup.

### 4.14 Themes
Light, dark and **follow system**. Following the system uses
`QStyleHints::colorScheme()` and `colorSchemeChanged` on Qt 6.5 and later.
On 6.4, zmail falls back to the XDG desktop portal
`org.freedesktop.appearance color-scheme`, then the GNOME
`org.gnome.desktop.interface color-scheme` setting. Settings also offers a
custom **accent color** and palette (window, base, text, highlight), applied
through a Fusion `QPalette`. Rule colors are re-contrasted when the palette
changes.

### 4.15 UI: Eudora-inspired layout (in this PR)
The look follows classic **Eudora 5–7**: dense, utilitarian and fast to
scan, rendered with Qt's **Fusion** style and tuned light and dark palettes,
so it looks the same on every desktop. Screenshots are in
`docs/screenshots/` (`eudora-main.png`, `eudora-main-dark.png`,
`eudora-compose.png`).

**Main window**
- **Menu bar:** File/Edit/View/Message/Settings/Help, kept in the window
  with `setNativeMenuBar(false)` because Fildem hid zterminal's menus. View →
  Theme offers Light, Dark and Follow System. The title is
  `zmail <PROJECT_VERSION>`.
- **Toolbar** (icons with text labels underneath): **Check Mail, New
  Message | Reply, Reply All, Forward | Delete, Attach**, then a
  right-aligned **search box** (`from:`, `subject:`, `has:attachment`… →
  §4.3).
- **Mailbox tree** on the left: **In, Out** (queued and sent), **Junk /
  Suspicious** (always in the warning color), **Trash**, then **Gmail
  Labels** as mailboxes, each with its label-color swatch. Unread counts
  are shown bold, right-aligned.
- **Message list** (top right): a dense, sortable `QTreeView` with
  movable columns, in Eudora order:
  1. status glyph: `•` unread, blank read, `R` replied, `F` forwarded,
     `Q` queued, `S` sent, plus a shield for suspicious mail
  2. priority (`!` high, `↓` low)
  3. attachment paperclip
  4. label-color swatch
  5. **Who**
  6. **Date** (`M/d/yy h:mm AP`)
  7. **K** (size, right-aligned)
  8. **Subject**

  Unread rows are bold. **Rule colors tint the row**: a subtle background
  mixed with the base color, and the foreground pushed to at least 4.5:1
  WCAG contrast. Suspicious rows always use the fixed warning colors, which
  rules can't override.
- **Preview pane** (below the list): a shaded Eudora-style header block
  (From/To/Subject/Date/Label/Attached), then the body. Suspicious mail
  shows the warning banner with its reasons above the headers (§4.8). The
  scaffold uses `QTextBrowser` for sample data. The real viewer is the
  sandboxed QtWebEngine view (§4.4).
- **Status bar:** sync state on the left (for example "● Synced 8:52 PM",
  or offline or error), and counts on the right ("In: 12 messages, 4 unread,
  3,840 K · 1 queued").

**Compose window**
- **Toolbar:** Send, Send Later…, Attach, Spelling (toggle), Signature box,
  Format box (**HTML / Plain text / Markdown**), Priority.
- **Formatting toolbar:** font, size, bold/italic/underline, text color,
  bulleted and numbered lists, alignment, link, quote. It's disabled in
  plain-text mode, and in Markdown mode it inserts Markdown syntax.
- **Eudora header block:** right-aligned bold labels **To / From / Subject
  / Cc / Bcc / Attached** over a shaded panel. Attached shows one chip per
  file with a paperclip and its size in K.
- **Status bar:** format, spell-check and signature state, plus the
  **size meter** ("2.2 MB of 25 MB (encoded)"), green, amber from 80%, red
  over the limit. Send is disabled when red (§4.5.1).

**Icons:** **Lucide** SVGs bundled under `assets/icons/lucide/` (ISC; six
are MIT from Feather) and compiled into the binary as Qt resources. They're
recolored from the palette at runtime (`stroke="currentColor"` becomes the
button-text color), so they stay crisp in light and dark themes and dim
when disabled. Every file has an entry in `assets/icons/LICENSES`.
`tools/check-asset-licenses.sh` (renamed from `check-sound-licenses.sh`)
checks sounds (CC0-1.0 or original-MIT) and icons (ISC, MIT, CC0-1.0 or
original-MIT) in CI.

**Sample data:** until sign-in lands, the scaffold shows fictional mail.
The people are made up, the addresses are `example.*`, and the brands are
Microsoft's fictional Contoso, Fabrikam, Northwind and Tailspin. No real
people appear.

**Update checker:** **Help → About** and **Help → Check for Updates** use
zwriter's `UpdateChecker`: GitHub `releases/latest`, a 5 s timeout, soft
failure, semver comparison, and a rate-limit message.

### 4.16 PR #3 (v0.2.0): what's implemented

- `core/ClientConfig`: parse/validate/install the Desktop client JSON (0600).
- `core/AuthManager` + `core/LoopbackServer` + `core/Pkce`: loopback redirect
  on `127.0.0.1:<random port>` (one-shot, 5-minute timeout), PKCE S256,
  `state` compared in constant time, `access_type=offline&prompt=consent`,
  system browser via `QDesktopServices`. Verifies the granted scope includes
  `gmail.modify` and reads the address from the `id_token`. Refresh tokens
  go to QtKeychain (`KeychainTokenStore`, service `zmail`, key
  `refresh-token:<email>`); there is no plaintext fallback. Concurrent callers
  share one refresh. `invalid_grant` deletes the stored token and raises a
  re-sign-in prompt. Sign out revokes at `oauth2.googleapis.com/revoke`.
- Logging: categories `zmail.auth`, `zmail.sync`, `zmail.gmail`. Secrets,
  codes and tokens are never logged (tests assert this).
- `core/GmailClient`: REST v1 with a quota token bucket (6,000 units/min,
  600 burst), exponential backoff + jitter on 429/5xx/rate-limit 403
  (honours `Retry-After`, up to 6 attempts), and a single refresh-and-retry on
  401.
- `core/SyncEngine`: profile `historyId` first, `labels.list`, newest 500
  INBOX messages via `messages.list` + `messages.get?format=metadata`
  (From/To/Subject/Date, snippet, labels, size), next pages on scroll and
  other labels' first page when selected. `history.list` (paged) every 30 s
  and on window focus; a 404 clears the cache and does a full resync. New
  INBOX+UNREAD arrivals emit `newMail` → bundled CC0 chime
  (`assets/sounds/new-mail.wav`, generated by `tools/make-new-mail-chime.py`).
- `core/MailCache`: SQLite (WAL) per account with `meta`, `labels`,
  `messages`, `message_labels` and an FTS5 external-content `messages_fts`
  kept in sync by triggers. (Raw `.eml` blob store, `pending_ops` and the
  query parser are still M2/M3 work.)
- Reading: `messages.get?format=full` on select, parsed from JSON (MIME tree
  walk, base64url, charset via `QStringDecoder`), cached. Opening a message
  calls `messages.modify` with `removeLabelIds: ["UNREAD"]` (optimistic).
  Live mail is never auto-opened, so nothing is marked read by accident.
- Viewer (interim): `ui/SafeHtmlView` on QTextBrowser, which has no JS engine.
  `loadResource` refuses everything but `data:` (remote, `file:`, `cid:`
  images blocked), the HTML is stripped of scripts/iframes/forms/event
  handlers/`javascript:` URLs, and link clicks show the real URL and ask. The
  QtWebEngine viewer from §4.4 is a follow-up PR, to keep its Chromium
  sandbox/AppArmor setup and CI cost out of this one.
- UI: `ui/ConnectDialog` (first run, sign-in, waiting-for-browser with a
  copyable URL, re-auth notice); File → Sign In / Sign Out; the mailbox tree
  maps INBOX→In, SENT→Out, SPAM→Junk/Suspicious, TRASH→Trash, with Starred,
  Important, Drafts and user labels (nested on `/`, Gmail colours, unread
  counts) under Gmail Labels. Without a session (`--offline`, tests,
  screenshots) the sample data is shown.
- Tests: `tests/mock/MockGoogle` is an in-process HTTP server for the auth,
  token, revoke and Gmail endpoints (PKCE verified server-side, fault
  injection, history expiry). `tst_auth`, `tst_sync` and `tst_connect` cover
  PKCE, state mismatch, refresh, `invalid_grant`, revoke, client-file
  handling, labels, initial sync and paging, history paging, 404 resync,
  429/503 backoff, 401 retry, the rate limiter, full fetch + mark-read, the
  new-mail chime, the sanitizer, and that no secret reaches the logs. CI
  never calls Google.
- Not yet: sending (next PR), attachments download, batch requests, the
  WebEngine viewer, the background fetch of older mail beyond scroll paging.

### 4.17 PR #4 (v0.3.0): sending, what's implemented

- **Compose from the Eudora UI.** New, Reply, Reply All and Forward (toolbar,
  Message menu, Ctrl+R / Ctrl+Shift+R / Ctrl+Shift+F) open `ComposeWindow`
  wired to the signed-in `MailSession`. Reply goes to Reply-To or From; Reply
  All adds the original To and Cc minus your own address, deduplicated.
  Forward re-fetches the original (`format=full`) and pulls each attachment
  with `users.messages.attachments.get`. From is the default send-as identity
  (`users.settings.sendAs`).
- **Formats.** HTML (rich editor; `RichText::toPlainText` makes the text part),
  Plain (text/plain only) and Markdown (md4c, raw HTML disabled, inline CSS;
  the source is the text part). HTML and Markdown go out as
  `multipart/alternative`; attachments wrap that in `multipart/mixed`.
- **MIME (`core/MimeBuilder`).** CRLF throughout, headers folded at 78
  columns, RFC 2047 encoded-words for non-ASCII header text, RFC 2231
  `filename*` for non-ASCII attachment names, bodies 7bit when they're
  short-line ASCII and base64 (76-column lines) otherwise. Bcc is in the raw
  message; Gmail delivers to it and strips the header.
- **Threading.** `In-Reply-To` = the parent's Message-ID, `References` = the
  parent's References + Message-ID (trimmed to 20, keeping the root), and the
  Gmail `threadId`. Gmail only honours threadId when the headers and subject
  match, and the mock enforces the same rule. The cache gained `cc_addr`,
  `reply_to`, `message_id_hdr` and `references_hdr` (added in place to 0.2.0
  caches); bodies cached by 0.2.0 are re-fetched once for the headers.
- **Upload (`core/Sender`).** Under `kSimpleUploadMaxBytes` (5 MB):
  `POST /messages/send` with JSON `{raw, threadId}`. JSON `raw` is used rather
  than `uploadType=media` because a media upload has nowhere to carry the
  threadId. At 5 MB and up: the resumable protocol
  (`/upload/gmail/v1/users/me/messages/send?uploadType=resumable`, metadata
  `{threadId}`, then a PUT of the bytes to the session URI). An interrupted PUT
  is resumed: `Content-Range: bytes */N` returns 308 with a Range header, and
  the rest is sent. Anything over `kSendLimitBytes` (25 MB, inside the 35 MiB
  API cap) is refused before any request is made.
- **Drafts.** Save Draft (Ctrl+S) uses `users.drafts.create`, then
  `users.drafts.update` for the same draft; sending deletes it. Closing a dirty
  window asks Save / Discard / Cancel.
- **Sent.** After a send, `MailSession::syncSoon()` runs a history poll, so
  the message appears under Out (SENT) on the next sync.
- **Attachments.** File picker, drag and drop, removable chips, and the size
  meter. Over the limit: a red banner, Send disabled, and the §4.5.1 zip offer.
  zmail probes with deflate (first 1 MB per file; already-compressed types
  counted at full size). If zipping would fit, it asks first (Zip / Cancel plus
  "Remember my choice", stored as the Ask/Always/Never policy in
  `compose/zipPolicy`). It builds `attachments.zip` with libzip and re-checks
  the size. "Undo zip" restores the originals.
- **Signatures.** Settings → Signatures… stores named rich and plain versions
  plus a default in QSettings. The signature sits above the quote behind a
  `-- ` delimiter, and switching it in the combo replaces it in place.
- **Spell check.** Optional at build time (Hunspell + `hunspell-en-us`). It's a
  port of zwriter's SpellChecker and shares its personal dictionary
  (`~/.local/share/sbj-ee/zwriter/user-dictionary.txt`). The context menu
  offers suggestions, Add to Dictionary and Ignore.
- **Not yet:** Send Later and the outbox service (M7), contacts autocomplete
  (M6).

## 5. v1 features vs later

**v1**
- OAuth sign-in (loopback + PKCE), refresh token in the keyring, one account
- Gmail API sync: full plus History-API incremental, 30 s polling, an offline
  cache and an offline action queue
- FTS5 search with operators, under 100 ms
- Safe HTML viewer, Load images, per-sender always-allow, View as plain text
- Compose in plain text, HTML or Markdown (md4c, live preview), reply/forward, signatures, spell check,
  attachments with a size meter
- Received attachments: list, open safely, save, inline image preview,
  risky-type warnings
- Local security scanning with a banner, column, verdicts, allowlist and
  optional clamd
- Rules for sound, color, notification and priority. DND and quiet hours.
  user WAV/OGG/MP3 sounds with Choose file… and Preview (copied locally),
  bundled sounds limited to original or CC0 with a CI license check
- Desktop notifications with Reply/Archive actions
- Contacts: address book, autocomplete, vCard, groups, optional read-only
  Google sync
- Schedule send (local outbox, optional user service, optional draft mirror)
  and snooze
- Themes (light/dark/system, accent); Eudora-style three-pane UI with Lucide icons
- Confirm-to-zip when attachments exceed the send limit (libzip, with a savings estimate)
- About, Check for Updates, the version in the title, a `.deb`

**Later**
- Several accounts. An IMAP IDLE doorbell for instant push (only if the `gmail.modify`-only decision is revisited). HTML signature
  editor polish (v1 edits HTML signatures in a QTextEdit). Drive links for
  large files. Send-later through the service while the machine is asleep
  (not possible locally). PGP/S/MIME. Gmail filters sync. Writing to Google
  Contacts. A Flatpak, arm64 and macOS builds. Threads view polish. Undo
  send.

## 6. Test plan

- **Unit (QtTest, offscreen, in CI):**
  - version/title/menus (in this PR)
  - semver comparison
  - PKCE and `state` validation
  - token store (with a fake keychain)
  - `history.list` merging, including the 404 → full resync path, against
    `FakeGmailClient` fixtures
  - schema migrations
  - QueryParser (every operator, quoting, dates)
  - FTS results
  - GMime parsing on a corpus (multipart, broken charsets, RFC 2047/2231
    names, nested message/rfc822)
  - building `multipart/alternative` and its generated text part
  - signature insertion and swapping, including the `-- ` delimiter and
    placement above quotes
  - spell checker (ignore, add to dictionary, skipping quotes and URLs)
  - the shared zwriter dictionary (concurrent appends from two processes
    without lost lines, reload after an external change, path override)
  - encoded-size math against real base64 output, and the 80%/limit
    thresholds (`tst_limits` in this PR), the compose meter and Send
    blocking, and staged or streamed download of a 40 MB fixture
  - AttachmentPolicy (extension × sniffed-type matrix, no exec bit)
  - scanner fixtures (forged lower `Authentication-Results`, homoglyph
    domains, xn--, mismatched links, docm/OLE VBA, encrypted zip, clamd with
    a mock socket and the EICAR string)
  - sound import (copy to `~/.local/share/zmail/sounds/`, de-dup names,
    missing-file fallback) and WAV/OGG/MP3 playback through `QMediaPlayer`
  - bundled-asset license check (`tools/check-asset-licenses.sh`: sounds and icons)
  - Eudora UI (in this PR: `tst_mainwindow`, `tst_compose`, `tst_theme`):
    - toolbar buttons and labels, mailbox tree, list above preview
    - column headers, and sorting by size
    - rule tints, and the suspicious override
    - status-bar counts
    - the compose header-block order, formatting actions and the
      HTML/Plain/Markdown modes
    - the size meter levels, with Send disabled when over the limit
    - dark-mode contrast of at least 4.5:1, Fusion style
  - Markdown mode:
    - md4c rendering fixtures (tables, code, autolinks; raw HTML is escaped)
    - the cleaned text/plain part
    - the HTML signature appended after rendering
    - spell check skipping code spans and blocks
    - the meter counting rendered bytes
  - zip offer:
    - the probe estimate stays within 15% of the real archive on mixed
      fixtures
    - the dialog never zips without consent, and Remember/Ask/Always/Never
      work
    - the archive opens with `unzip -l`, and Python `zipfile` sees UTF-8 names
    - the still-too-large message for JPEG/PDF fixtures
  - rule engine (first match wins, quiet hours across midnight, contact
    implicit rules, suspicious override)
  - contrast adjustment
  - vCard round-trips (3.0 and 4.0)
  - autocomplete ranking
  - outbox scheduling across both 2026/2027 America/Chicago DST transitions
  - lease and claim with two processes (no double send)
  - missed-send prompt
  - snooze wake and label restore (`zmail/Snoozed` applied, INBOX restored)
  - the Suspicious label mirror (applied by default, removed by Mark Safe,
    nothing sent to Gmail when the mirror is off)
  - the `.deb` ships `zmail-sender.service` but doesn't enable it (a CI
    `dpkg-deb -c` check, plus maintainer scripts free of `systemctl enable`)
- **Renderer:** a WebEngine test page with remote `<img>`, `<script>`, CSS
  `url()` and `<link rel=prefetch>`. The interceptor log must show zero
  allowed external requests until Load images is clicked.
- **Notifications:** a mock `org.freedesktop.Notifications` on a private
  session bus (`dbus-run-session`), with capabilities on and off.
- **Performance:** a generated 100k-message corpus. A benchmark asserts
  p95 under 100 ms for 20 representative queries (reported in CI; enforced
  locally).
- **Manual:** a sign-in smoke test against Stephen's account, a Fildem menu
  check on vertex, a `.deb` install and uninstall, and a dark-mode check.
- **CI:** ubuntu-24.04, build, ctest, `cpack -G DEB`, `.deb` artifact (in
  this PR).

## 7. Milestones

- **M0 (PR #1, done):** PLAN.md and scaffold (window, menus, About, update
  check, CI, `.deb`)
- **M1 (PR #3, v0.2.0, done):** auth (loopback/PKCE, keyring), Google setup
  doc, `oauth-client.json` handling
- **M2 (PR #3 in part):** sync engine, cache and FTS5 table, message list done;
  blob store and the search query parser remain
- **M3:** MIME parse, safe viewer, attachments (receiving), security scanner
- **M4:** rules (sound, color, notification), notifications, DND, quiet hours,
  themes
- **M5 (PR #4, v0.3.0, done):** compose (plain/HTML/Markdown), send, drafts,
  reply/forward threading, signatures, spell check, attachments (sending,
  size block, zip offer). Scheduled send and the outbox stay in M7.
- **M6:** contacts (address book, autocomplete, vCard, groups, People sync)
- **M7:** schedule send, snooze, `zmail-sender` user service
- **M8:** polish, a performance pass, the v1.0.0 release `.deb`

## 8. Decisions (2026-10-03)

Stephen settled these on 2026-10-03. The sections above reflect them.

1. **OAuth publishing:** **In production, unverified** (personal use). That
   avoids the 7-day Testing-mode refresh-token expiry, at the cost of the
   one-time "unverified app" click-through (§2.1, §3).
2. **Scope:** **`gmail.modify` only.** No full-mail scope
   (`https://mail.google.com/`), so no permanent delete (Trash only) and no
   IMAP IDLE (§2).
3. **Polling:** **every 30 s**, plus on window focus. It's configurable,
   with a minimum of 15 s (§2).
4. **Spell check:** **share the Hunspell personal dictionary with zwriter**
   (`~/.local/share/sbj-ee/zwriter/user-dictionary.txt`) (§4.6).
5. **QtWebEngine:** about **150 MB of dependencies is acceptable** for the
   sandboxed HTML viewer (§4.4).
6. **Sender service:** the `.deb` **installs `zmail-sender` (systemd --user)
   disabled**. It's **opt-in from Settings** (§4.12).
7. **Gmail label mirrors:** **on by default** for snooze (`zmail/Snoozed`,
   INBOX removed while snoozed) and Suspicious (`zmail/Suspicious`). Both
   can be switched off (§4.8, §4.13).
8. **Sounds:** **bundled defaults are original or CC0 only**, plus
   user-supplied WAV/OGG/MP3 for any rule. Copyrighted clips are never
   bundled (§4.9).

### Still open

1. **Lookalike brand list:** not answered yet. zmail ships the proposed
   default in §4.8 (major banks, PayPal, Venmo, Amazon, Apple, Google,
   Microsoft, DocuSign, Dropbox, the shipping carriers, IRS/SSA), editable
   in Settings → Security. Stephen can confirm or trim the list, and supply
   his employer's domain if he wants it added.

## 9. Sources

1. Google, *Using OAuth 2.0 to Access Google APIs*: refresh token expiration (7 days in Testing), the Gmail-scope password-change revocation, the 100-token limit, "client secret is obviously not treated as a secret". <https://developers.google.com/identity/protocols/oauth2>
2. Google Cloud Help, *Unverified apps*: the 100-new-user cap and the unverified-app screen. <https://support.google.com/cloud/answer/7454865>
3. Google, *Choose Gmail API scopes*: sensitive and restricted scopes, `mail.google.com`, the security assessment when data is stored on servers. <https://developers.google.com/workspace/gmail/api/auth/scopes>
4. Google, *Gmail API usage limits*: 6,000 units/min/user, `history.list` = 2, `messages.get` = 20. <https://developers.google.com/workspace/gmail/api/reference/quota>
5. Google, *Synchronize clients with Gmail*: history sync, the 404 when `startHistoryId` is out of range. <https://developers.google.com/workspace/gmail/api/guides/sync>
6. Google, *Push notifications*: Pub/Sub, and polling recommended for user-owned devices. <https://developers.google.com/workspace/gmail/api/guides/push>
7. Google Cloud Help, *When is verification not needed*: the personal-use exception (fewer than 100 users). <https://support.google.com/cloud/answer/13464323>
8. Google, *Restricted scope verification*: exceptions including personal use. <https://developers.google.com/identity/protocols/oauth2/production-readiness/restricted-scope-verification>
9. Google, *OAuth 2.0 for iOS & Desktop Apps*: loopback redirect, PKCE, optional secret, `include_granted_scopes`. <https://developers.google.com/identity/protocols/oauth2/native-app>
10. Google, *People API otherContacts.list*: the `contacts.other.readonly` scope. <https://developers.google.com/people/api/rest/v1/otherContacts/list>
11. Gmail Help, *Attachment size limits*: 25 MB for personal accounts. <https://support.google.com/mail/answer/6584>
12. Google, *users.messages.send* (no scheduling parameter). <https://developers.google.com/workspace/gmail/api/reference/rest/v1/users.messages/send>
13. Google, *users.drafts.send* (no scheduling parameter). <https://developers.google.com/workspace/gmail/api/reference/rest/v1/users.drafts/send>
14. Google Cloud Help, *Manage App Audience*: Testing vs In production, test users. <https://support.google.com/cloud/answer/15549945>
15. Unicode, *UTS #39 Unicode Security Mechanisms* (confusable skeletons). <https://www.unicode.org/reports/tr39/>
16. sbj-ee/zwriter, `src/UpdateChecker.*`, `src/SpellChecker.*`, `src/WritingHighlighter.*` (read-only reference). <https://github.com/sbj-ee/zwriter>
17. sbj-ee/zterminal, CMake/CPack/CI layout and the Fildem menu-bar note (read-only reference). <https://github.com/sbj-ee/zterminal>
18. Google, *Gmail API discovery document*: `users.messages.send`/`drafts.send`/`drafts.create`/`drafts.update` `mediaUpload.maxSize` = 36700160, and `messages.import`/`insert` = 157286400. <https://gmail.googleapis.com/$discovery/rest?version=v1>
19. Google Workspace Admin Help, *Gmail receiving limits in Google Workspace*: 50 MB (70 MB Enterprise Plus) after encoding, about 37% overhead. <https://support.google.com/a/answer/1366776>
