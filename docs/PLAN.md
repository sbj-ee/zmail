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
  v1 polls `history.list` every 30 s (configurable, minimum 15 s) and again on
  window focus. That's close enough to instant for a desk client. An IMAP
  IDLE "doorbell" on INBOX remains a later option (§9).
- **Raw messages.** `messages.get?format=raw` returns RFC 822 bytes for the
  offline cache. `messages.send` takes RFC 822 bytes that we build with GMime.
- **Scopes.** We request `gmail.modify` (read, label, archive, send, trash, no
  permanent delete) [3]. Google classes `https://mail.google.com/`, `gmail.readonly`,
  `gmail.compose`, `gmail.insert`, `gmail.modify` and `gmail.metadata` as
  **restricted** [3], so any useful mail-reading scope makes zmail a
  restricted-scope app. We don't need permanent delete, so we skip
  `https://mail.google.com/`. People API scopes are optional (§4.11).

### 2.1 OAuth token lifetime (the key finding)

Google's OAuth documentation says: *"A Google Cloud Platform project with an
OAuth consent screen configured for an external user type and a publishing
status of 'Testing' is issued a refresh token expiring in 7 days, unless the
only OAuth scopes requested are a subset of name, email address, and user
profile."* [1] Gmail scopes aren't in that subset. **In Testing mode, zmail
would make Stephen sign in again every week.**

The fix is to set the app's publishing status to **In production** and leave
it **unverified**:

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

1. Go to <https://console.cloud.google.com/> and **create a project** named `zmail-personal`.
2. **APIs & Services → Library**: enable the **Gmail API**. If you want contacts sync, also enable the **People API**.
3. **Google Auth Platform → Branding / Audience** (the OAuth consent screen):
   app name `zmail`, support email stevebj.ee@gmail.com, **User type: External**.
4. **Audience → Test users**: add stevebj.ee@gmail.com. This is needed while in Testing.
5. **Data Access → Add scopes**:
   - `https://www.googleapis.com/auth/gmail.modify` (restricted)
   - `openid`, `https://www.googleapis.com/auth/userinfo.email`
   - optional: `https://www.googleapis.com/auth/contacts.readonly` and
     `https://www.googleapis.com/auth/contacts.other.readonly` [10] (sensitive)
6. **Clients → Create client → Application type: Desktop app**, name `zmail
   desktop`. Download the JSON.
7. Save it as `~/.config/zmail/oauth-client.json`, then `chmod 600` it. zmail
   reads only this path, or `$ZMAIL_OAUTH_CLIENT` if set. It refuses to start
   sign-in if the file is group- or world-readable, and offers to fix the
   mode. The repo's `.gitignore` excludes `client_secret*.json` and
   `oauth-client*.json`. **Never commit this file.**
8. **Audience → Publish app** (move to **In production**). Don't submit for
   verification. This avoids the 7-day refresh-token expiry (§2.1). On first
   sign-in, accept the "Google hasn't verified this app" screen via
   **Advanced → Go to zmail (unsafe)**.

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

### 4.5 Compose and send
- **Plain text or HTML**, with a toggle per message. The default is set in
  Settings. HTML compose uses QTextEdit's rich text (bold, italic, lists,
  links, inline images). Plain text uses the same widget with
  `setAcceptRichText(false)`.
- **HTML mail** goes out as `multipart/alternative`: a `text/plain` part
  generated from the document (lists become `-`/`1.`, links become
  `text <url>`), plus `text/html`. Inline images make it
  `multipart/related`, and attachments `multipart/mixed`. GMime builds the
  message, and `messages.send` sends it (with `threadId` for replies,
  plus `In-Reply-To`/`References`).
- **Signatures:** several named signatures, each with a plain and an HTML
  version. One is the default and is inserted into new messages and replies.
  The plain version gets the standard `"-- \n"` delimiter. In replies, the
  signature goes **above** the quoted text. A signature combo box in the
  compose window switches it per message, replacing only the signature
  block, which a document marker tracks. The version matching the message
  format is used. Signatures are stored in the `signatures` table and edited
  in Settings.
- **Attachments (sending):** add them with the file picker or by dropping
  files onto the compose window. A chip list shows each file's size and the
  running total. **Gmail's limit is 25 MB** for personal accounts [11], and
  that limit applies to the encoded message, so zmail measures the
  **base64-encoded** size: `4·⌈n/3⌉` plus CRLF every 76 characters, about
  1.37× in total. The indicator turns amber at 80% (20 MB encoded) and red
  above 25 MB, where Send asks "remove attachments or share a Drive link
  instead (later)".

### 4.6 Spell check (reused from zwriter)
zwriter's `SpellChecker` is a thin Hunspell wrapper: it searches
`/usr/share/hunspell` and `/usr/share/myspell/dicts` for `en_US.aff/.dic`,
supports session-only `ignoreWord`, and keeps a persistent
`addToUserDictionary` at `AppDataLocation/user-dictionary.txt`. Its
`WritingHighlighter` is a `QSyntaxHighlighter` with a pluggable
"is misspelled" function that draws a red wavy underline. zmail copies both
(MIT, same author), along with zwriter's CMake Hunspell detection (an
optional `ZMAIL_HAS_HUNSPELL`, a no-op when absent). The highlighter skips
quoted lines (`> `), URLs, email addresses and the signature block. The
compose context menu shows up to 8 suggestions, then **Add to Dictionary**
and **Ignore**. A `QSyntaxHighlighter` works on the QTextDocument, so spell
check is the same in plain and HTML mode. The `.deb` depends on
`libhunspell-1.7-0, hunspell-en-us`.

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
  contacts, frequent correspondents and a bundled brand list. Punycode
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
**User controls:** Mark Safe / Mark Suspicious (`user_verdicts` table,
optionally applying the Gmail label `zmail/Suspicious`), and a trusted-sender
allowlist (`trusted_senders`, also fed by the contacts trusted flag). Suspicious
mail can trigger its own sound through a built-in rule condition
`scan:suspicious`.

**Limits:** these are heuristics. False positives and false negatives will
happen. The scanner never deletes, moves or hides mail. It only labels and
warns, and Gmail's own spam filtering is still the first line of defense.

### 4.9 Rules: sound, color, notification, priority
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
**Bundled sounds are original (generated by our own script, as zwriter's
`gen_typewriter_sounds.py` does) or CC0 only.** Each bundled file has an
entry in `assets/sounds/LICENSES` (file name, source/author, license,
URL). A CI step (`tools/check-sound-licenses.sh`, in this PR) fails the
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
- **Optional `zmail-sender` systemd --user service** (same binary,
  `zmail --sender`) reads the refresh token from the keyring and sends due
  items. To make sure nothing is sent twice, a sender claims a row in
  `BEGIN IMMEDIATE` (`state=sending`, `lease_until`). The Message-ID is
  generated when the item is queued. Before retrying an expired lease, the
  sender searches Sent for `rfc822msgid:` to see whether the message
  already went out.
- **The tradeoff:** without the service, mail only goes out while zmail is
  open. With it, mail still only goes out while the machine is awake, logged
  in (the keyring is unlocked) and online. **Nothing is sent while the
  machine is asleep or off.** An optional setting **mirrors queued items as
  Gmail drafts** (`drafts.create`), so Stephen can send one from his phone if
  needed. zmail deletes the mirror draft after sending.

### 4.13 Snooze
`snoozes(message_id, wake_utc, tz_name, label_applied, created_utc)`. Presets
are later today (+3 h), tomorrow 8 AM, next week (Monday 8 AM), or a custom
time. Snoozed messages are hidden from INBOX in the local view. When one
wakes, it returns to the top of the inbox with a notification and rule
sound. **Optionally** zmail applies the label `zmail/Snoozed` and removes
`INBOX` on Gmail so other devices see it, then restores INBOX and removes
the label on wake. The same caveat as schedule send applies: messages only
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

### 4.15 UI and update checker
The main window has a three-pane layout: labels, then the message list
(sender, subject, date, ⚠, 📎, color), then the preview. The menu bar is
File/Edit/View/Message/Settings/Help, with `setNativeMenuBar(false)` because
Fildem hid zterminal's menus. The title is `zmail <PROJECT_VERSION>`.
**Help → About** and **Help → Check for Updates** use zwriter's
`UpdateChecker`, ported in this PR: GitHub `releases/latest`, a 5 s timeout,
soft failure, semver comparison, and a rate-limit message.

## 5. v1 features vs later

**v1**
- OAuth sign-in (loopback + PKCE), refresh token in the keyring, one account
- Gmail API sync: full plus History-API incremental, 30 s polling, an offline
  cache and an offline action queue
- FTS5 search with operators, under 100 ms
- Safe HTML viewer, Load images, per-sender always-allow, View as plain text
- Compose in plain text or HTML, reply/forward, signatures, spell check,
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
- Themes (light/dark/system, accent)
- About, Check for Updates, the version in the title, a `.deb`

**Later**
- Several accounts. An IMAP IDLE doorbell for instant push. HTML signature
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
  - encoded-size math against real base64 output
  - AttachmentPolicy (extension × sniffed-type matrix, no exec bit)
  - scanner fixtures (forged lower `Authentication-Results`, homoglyph
    domains, xn--, mismatched links, docm/OLE VBA, encrypted zip, clamd with
    a mock socket and the EICAR string)
  - sound import (copy to `~/.local/share/zmail/sounds/`, de-dup names,
    missing-file fallback) and WAV/OGG/MP3 playback through `QMediaPlayer`
  - bundled-sound license check (`tools/check-sound-licenses.sh`)
  - rule engine (first match wins, quiet hours across midnight, contact
    implicit rules, suspicious override)
  - contrast adjustment
  - vCard round-trips (3.0 and 4.0)
  - autocomplete ranking
  - outbox scheduling across both 2026/2027 America/Chicago DST transitions
  - lease and claim with two processes (no double send)
  - missed-send prompt
  - snooze wake and label restore
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

- **M0 (this PR):** PLAN.md and scaffold (window, menus, About, update
  check, CI, `.deb`)
- **M1:** auth (loopback/PKCE, keyring), Google setup doc, `oauth-client.json`
  handling
- **M2:** sync engine, cache, blob store, FTS5 search, message list
- **M3:** MIME parse, safe viewer, attachments (receiving), security scanner
- **M4:** rules (sound, color, notification), notifications, DND, quiet hours,
  themes
- **M5:** compose (plain/HTML), send, signatures, spell check, attachments
  (sending)
- **M6:** contacts (address book, autocomplete, vCard, groups, People sync)
- **M7:** schedule send, snooze, `zmail-sender` user service
- **M8:** polish, a performance pass, the v1.0.0 release `.deb`

## 8. Open questions for Stephen

1. Are you OK publishing the Cloud project as **In production but unverified**
   (one-time "unverified app" click-through) to avoid weekly re-sign-in?
2. `gmail.modify` only, with no permanent delete (Trash only)? Or do you want
   `https://mail.google.com/` for permanent delete or a future IMAP IDLE?
3. Default poll interval: is 30 s fine?
4. Should zmail share zwriter's personal dictionary file, or keep its own?
5. Is QtWebEngine's size (about 150 MB of deps) acceptable for the HTML
   viewer?
6. Do you want the `zmail-sender` user service installed (disabled) by the
   `.deb`, or opt-in from Settings?
7. Which personal clips should be your defaults (for example "Message for
   you, sir" as the default new-mail sound)? They stay local and are never
   bundled. Do you want any original sounds generated for the repo
   (chime, thunk, klaxon)?
8. Should snooze and Suspicious mirror to Gmail labels by default, or be
   local-only by default?
9. Should the brand list for lookalike checks start with banks, PayPal,
   Amazon, Microsoft, Google and Apple, plus your employer?

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
