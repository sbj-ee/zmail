# zmail

A local Linux desktop email client for Gmail that plays a custom sound for
new mail, per sender or per rule, the way Eudora did.

**Status:** v0.6.23: Gmail sign-in, sync (Inbox, labels, read + mark-as-read,
move to Trash, new-mail chime), a reworked message viewer, and sending: New /
Reply / Reply All / Forward, HTML, plain or Markdown, attachments (with the
25 MB check and an optional zip), drafts, signatures and spell check, plus search operators, list right-click
menus with Undo Delete, a sign-in Retry banner, a new-mail sound mute
(Ctrl+Shift+M), and Settings → Sounds (custom WAV, Test, Default). New in
0.4.0: Mark as Junk / Not Junk with a Hide Spam filter, Snooze (presets and a
Snoozed mailbox), full-text search across the local cache, read-only Google
Contacts, the Boilermakers, Badgers and Packers themes, and a Theme Editor
for custom themes (`*.ztheme.json`, shared with zterminal). New in 0.4.1:
security hardening (no local file reads from mail or quoted replies, remote
images default to Ask over HTTPS only with private addresses refused, OAuth
state, header and `mailto:` fixes). New in 0.4.2: Delete works in every
mailbox, not just In: a message deleted from a Gmail label, Starred, Sent,
Snoozed, Junk or search results now leaves the list (it stays in Trash, and
Undo Delete still puts it back), and if Gmail refuses the Delete the message
and the selection come back with the error in the status bar. New in 0.5.0:
styled signatures (rich-text editor, sanitized HTML, generated plain text)
and Labels as folders (sidebar counts, New/Rename/Delete Folder, drag-to-move
that peels INBOX / the source user label). New in 0.5.1:
account-status circle in the status bar (green connected, grey offline,
red on sync error). New in 0.5.2:
message list multi-select (Shift/Ctrl range+toggle, bulk Delete/Junk/Snooze/
Mark Read). New in 0.5.3:
Gmail label-count fetch throttle (max in-flight + 429 cooldown) so startup
no longer livelocks the UI thread. New in 0.5.4:
sync fixes (snoozes survive a full resync, a failed fetch no longer skips
mail, folders can't get stuck loading), a faster message list and cache,
interactive Gmail calls ahead of background sync, a stricter HTML sanitizer,
and the theme editor's colour picker no longer freezes it on Wayland. New in 0.5.5:
a zmail app icon (a green Z on a light envelope, matching zterminal's style),
installed at all Linux hicolor sizes, plus a window icon. New in 0.5.6:
mail with deeply nested tables no longer freezes the window (images are
shared rather than copied per layout, table nesting is capped at 8 levels),
Settings → Message List (text size and row spacing), and one folder per
message (a move drops the other labels; drag onto In to move back). New in 0.5.7:
deleting a message takes it out of its folder, and right-click a folder for
Empty Folder. New in 0.5.8: a folder whose list is too short to scroll now
loads the rest of its mail (it could show 2 messages under a count of 212),
and the 0.5.7 startup pass that removed labels from mail in Trash is gone. New in 0.5.9:
Contacts can be organised (categories, your own fields, a comment per
contact) and hidden; all of it local, none of it written to Google. New in 0.5.10:
coloured flags, Empty Trash, dragging a message to a folder works again (a
list reload while the mouse button was down cancelled the drag), and range
selection always starts from the current message. New in 0.5.11:
the Contacts list separates one contact from the next, and contacts can be
exported to JSON. New in 0.5.12: the Snooze entry in the message
right-click menu has its label. New in 0.5.13: the folder under a dragged
message lights up, and a message's right-click menu can add its sender to
Contacts. New in 0.5.14: a message whose tables would stall the window is
shown simplified, with a button for the full layout. New in 0.6.0:
Filters, as in Eudora (Settings → Filters): ordered rules that colour,
flag, move, mark read and play a sound, first match wins. New in 0.6.1:
nicknames (a contact's short name, or a category's name for everyone in
it, typed in To, Cc or Bcc). New in 0.6.2: Save Attachments, Print, Copy
and View as Plain Text, dates as 01/01/2026, and counts that read "1
message" rather than "1 message(s)". New in 0.6.3: the queue (Send Later
puts a message in Out; Send Queued Messages delivers what is waiting). New
in 0.6.4: Eudora's Mailbox and Transfer menus, and its shortcut keys. New
in 0.6.5: stationery (New Message With, Reply With). New in 0.6.6: a
mailbox can open in a window of its own. New in 0.6.7: one Settings
window, Undo for moves and emptying, editable queued messages, new-mail
notifications, a shortcut list, flag search, and a round of polish. New in
0.6.8: contacts are edited in a window of their own, name and addresses
included. New in 0.6.9: Load All Messages for a mailbox, and large
deletes go to Gmail in batches. New in 0.6.10: live folder counts, shown
as unread / total. New in 0.6.11: an Archive mailbox, coloured folder
icons, filed mail leaves Gmail's Important, Send Later takes a date and
time, Out is called Sent, and the Trash and Drafts counts are right.
0.6.12 fixes Archive, whose folder Gmail refused to create. New in
0.6.13: an All Mail mailbox. New in 0.6.14: Ctrl+click opens a
folder's menu, for when there is no mouse. New in 0.6.15: dialogs
are tinted, to stand out from the main window. New in 0.6.16: a
Queue mailbox for mail that is waiting to be sent. 0.6.17 makes
dialogs easier to tell from the main window: a stronger tint, lists
included, and a coloured frame. New in 0.6.18: All Mail has a count,
and a message is marked read only after 10 seconds in the preview. New
in 0.6.19: one-click unsubscribe from mailing lists, the mark-as-read
delay as a setting, Gmail's vacation responder, calendar invitations
shown as a card, and a swoosh when mail is sent. New in 0.6.20: Send
waits two minutes in Queue first (a setting) and Undo Send takes it
back, and Send Later's time can be set to the current hour. New in
0.6.21: six more new-mail sounds to choose from. New in 0.6.22: an
image sized by its height alone keeps its shape, and quitting with a
message still being written no longer touches freed memory. New in
0.6.23: quitting asks about each message still being written (Save
draft?) instead of dropping it. See
[docs/PLAN.md](docs/PLAN.md).

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

![Replying in zmail (mock data)](docs/screenshots/compose-send.png)

## Build

```sh
sudo apt install qt6-base-dev qt6-svg-dev qt6-multimedia-dev qtkeychain-qt6-dev \
  libqt6sql6-sqlite libmd4c-dev libmd4c-html0-dev libzip-dev \
  libhunspell-dev hunspell-en-us cmake ninja-build g++
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cd build && cpack -G DEB   # zmail_0.6.23_amd64.deb
```

The version lives only in `project(zmail VERSION ...)` in `CMakeLists.txt`.
The tests never talk to Google: they run against an in-process mock of the
OAuth and Gmail endpoints (`tests/mock/MockGoogle`).

## Reading mail

- HTML mail is shown on a white page, laid out to the width of the pane.
  **View → Dark Background for Messages** gives a dark version. Content the
  sender hid (preheaders, dark-mode duplicates) stays hidden, and 600 px
  newsletter cards are centred as in Gmail.
- Remote images: **Settings → Privacy → Remote images** is *Ask* (default:
  a **Load images** bar per message, with **Always for this sender**; the
  allow list is edited or cleared on the same page), *Always load* or
  *Never*. Image requests never carry cookies or credentials, are
  size-capped, use HTTPS only and only go to public addresses (never
  localhost, the LAN or a tailnet), and known tracking pixels (1×1 images,
  read-receipt services) are dropped even in *Always*, unless you untick
  **Block tracking pixels**.
- Emoji in subjects and messages use Noto Color Emoji when it's installed
  (`fonts-noto-color-emoji`, recommended by the .deb).
- A message whose layout would stall the window (very wide or deeply nested
  tables) is shown simplified, with a **Show full layout** button for when
  you want it as sent and don't mind the wait.
- Ctrl+= and Ctrl+- (or Ctrl+wheel) zoom the message; Ctrl+0 resets it.
- **View → Preview Pane** shows the preview below the list or to its right;
  by default the message gets about two thirds of the space. Drag the divider to resize it; zmail remembers the layout.
- Double-click a message (or press Ctrl+O) to open it in its own window.
- Labels are folders, and a message is in one folder at a time: dragging it
  onto a label removes it from In and from any other label it had; dragging
  it onto **In** moves it back.
  Deleting a message takes it out of its folder too (Undo Delete puts it
  back). A folder loads more of its mail by itself when the list is too
  short to scroll.
  Right-click a folder for **Empty Folder** (all its mail to Trash) or
  **Delete Folder** (the label only; its mail is kept).
- **Filters** (Settings → Filters), as in Eudora: an ordered list of rules,
  and the first one that matches a message applies. A rule matches on From,
  To or Subject (contains, is, starts or ends with, a regular expression;
  all or any of its conditions) and can colour the message's row, flag it,
  move it to a folder, mark it read, and play its own sound or none when it
  arrives. Colours show on every matching message; the rest happens when
  mail arrives, or on **Message → Filter Messages** (Ctrl+Alt+J) for the
  selection. Right-click a message → **Make Filter** starts one from its
  sender. Rules are kept in `rules.json` in zmail's config folder.
- Eudora's **Mailbox** and **Transfer** menus list your mailboxes and
  folders: Mailbox goes to one (Ctrl+1 for In, Ctrl+2 for Sent), Transfer
  moves the selected messages to one, also from the right-click menu.
  Eudora's keys work: Ctrl+D delete, Ctrl+E send, Ctrl+T send queued,
  Ctrl+Alt+J filter, Ctrl+K add the sender to Contacts, Ctrl+L Contacts,
  Ctrl+H attach, Ctrl+M check mail.
- The sidebar's counts are live. Each mailbox shows **unread / total**
  (`3 / 212`, in bold while something is unread; just the total otherwise),
  and the numbers move as you delete, read or move mail, without waiting
  for Gmail. Mail and changes made elsewhere show within about 15 seconds.
- **Archive** has a row of its own above Trash. Right-click a message and
  choose Archive (or Message → Archive, or drag it there) to move it out of
  the Inbox. It is an ordinary folder, made the first time you archive
  something: a Gmail label called "Archived" (Gmail keeps the name
  "Archive" for itself).
- **All Mail**, below Archive, lists every message in Gmail except Trash
  and Spam, whether or not it is in the Inbox or a folder, so mail that is
  in neither can still be found. It loads as you scroll (or with Load All
  Messages). Its count is every message in the account outside Trash and Spam.
- Every folder in the sidebar is a folder icon in its own colour: the
  label's Gmail colour if it has one, otherwise one picked from its name.
- Moving a message into a folder also takes it out of Gmail's **Important**
  (which Gmail assigns by itself); Undo puts it back. **Mailbox → Clear
  Important from Filed Mail** does the same, once, for everything already
  in a folder.
- The mailbox for sent mail is called **Sent** (it was Out). Mail that
  hasn't gone yet is in **Queue**, just above it, not mixed in with what
  was sent.
- **Mailbox → Load All Messages** (or right-click a mailbox) fetches every
  message in it, not only the ones scrolled to so far, with progress in the
  status bar and Stop Loading to interrupt. Deleting a large selection goes
  to Gmail in batches of a thousand.
- A mailbox can open in **a window of its own**, as in Eudora (right-click
  it → Open in New Window, or Mailbox → Open in New Window, Ctrl+Shift+N):
  the same live list with its own preview pane, so two mailboxes can be seen
  side by side. Double-click opens a message, Delete deletes, right-click has
  the full set of commands, and messages can be dragged from it onto a folder
  in the main window.
- **Stationery**, as in Eudora: messages kept as templates (Settings →
  Stationery, or **Save This Message as Stationery** from the compose
  window's Stationery button). **File → New Message With** starts a message
  from one and **Message → Reply With** answers with one: its text goes
  above your signature and the quoted original, and its recipients and
  subject are used where the message has none.
- **Send** waits two minutes before the message goes: it sits in **Queue**
  until then, and **Edit → Undo Send** (Ctrl+Z, or Undo in the status bar)
  takes it back, open again and unsent. **Settings → Send Delay** sets how
  long (at once, 30 seconds, 1, 2 or 5 minutes). zmail has to be running
  for it to go; one still waiting when zmail quits goes the next time it
  starts.
- **Send Later** in the compose window (Ctrl+Shift+Enter) asks when: at a
  date and time you pick, or held, as in Eudora, until you choose **File →
  Send Queued Messages**. Either way the message waits in **Queue**, the
  mailbox above Sent, marked Q; its count is how many are waiting. A
  timed one goes at its time if zmail is running, and otherwise the next
  time zmail starts. **File → Send Queued Messages** (Ctrl+T) sends
  everything waiting, timed or not, oldest first; one that Gmail refuses stays queued with the reason. Delete
  takes a queued message out of the queue. The queue is kept on this
  computer until it is sent.
- **Flags** come in seven colours. Click a message's flag column to flag it
  (in the colour used last) or clear it; right-click, or **Message → Flag**,
  to pick a colour, for one message or several. Any flag also stars the
  message in Gmail; the colour is zmail's own. Mail starred elsewhere shows
  a yellow flag.
- Dragging a message over the sidebar lights the folder it would go to.
- Right-click a message to add its sender to Contacts (offered when that
  address isn't a contact yet).
- Right-click **Trash → Empty Trash** removes everything in it from zmail.
  Gmail does not let a mail client with zmail's permission erase mail, so
  Gmail keeps those messages in its own Trash until it purges them.
- Shift+click and Shift+arrows select a range from the current message, up
  or down.
- **Settings → Contacts** organises the contacts synced from Google. Select
  one to read it; **Edit** (or a double-click) opens it in a window of its
  own to change its name, addresses, nickname, categories (a contact can be
  in several), your own fields (Phone, Company, anything), comment, and
  whether it is hidden. Edits to a Google contact's name or addresses are
  kept across syncs, and **Use Google's** goes back to Google's version.
  **New Contact** adds one of your own. Hidden contacts leave the list and are not suggested when you write
  a message. All of this stays on your computer; nothing is written to Google.
  **Export** saves the contacts shown, or all of them, as a JSON file with
  their categories, fields and comments.
- **Nicknames**, as in Eudora: give a contact a nickname (Settings →
  Contacts) and type it in To, Cc or Bcc instead of the address. A
  category's name is a nickname for everyone in it. zmail suggests them as
  you type and writes the addresses out when you leave the field.
- **Settings → Settings…** (Ctrl+,) is one window with a section each for
  Filters, Signatures, Stationery, Message List, Row Stripes, Sounds and
  Privacy, and one OK and Cancel for all of them.
- **Undo** (Ctrl+Z, or the bar in the status bar) takes back a Delete, a
  move to a folder, Empty Folder and Empty Trash for a few seconds.
- A queued message opens in the compose window again when you double-click
  it, to change it before it goes.
- When mail arrives and zmail isn't in front you get a desktop notification
  (**View → Notify for New Mail**), and the window title shows the Inbox's
  unread count.
- Right-clicking a mailbox in the sidebar acts on that mailbox without
  leaving the one you are in; with no mouse attached, Ctrl+click opens the
  same menu. Empty mailboxes and an empty preview say so.
- Dialogs (Contacts, Settings, message boxes) are tinted with the theme's
  selection colour, lists and fields included, and have a frame in that
  colour, so one lying over the main window stands out from it.
- An unread message is marked read once it has been on show for 10
  seconds, in the preview or in a window of its own, so arrowing past new
  mail, or a glance at it, leaves it unread. **View → Mark Messages as
  Read** changes that: immediately, after 3, 10 or 30 seconds, or only when
  you mark them yourself.
- Mail from a mailing list that says how to leave it (a `List-Unsubscribe`
  header) gets an **Unsubscribe** bar above the message (also **Message →
  Unsubscribe from Mailing List**). zmail asks first, then does it the way
  the list offers: a one-click request to the list's server (RFC 8058), an
  unsubscribe e-mail sent from your address, or, when the list has neither,
  its unsubscribe page in your browser. Only `https` addresses are used,
  and never for mail in Spam. The bar then says when you unsubscribed.
- A calendar invitation (a `text/calendar` part or an `.ics` file) shows as
  a card above the message: what it is (invitation, cancellation, someone's
  reply), when in your own time zone, where, who organised it and who is
  invited. zmail only displays it; it does not answer invitations.
- **Settings → Vacation Responder** turns Gmail's automatic reply on or
  off: subject, message, optional first and last day, and whether only
  people in your Contacts get it. Google sends the replies, so it works
  with zmail closed, and the status bar shows while it is on. The first
  time you save, Google asks in your browser to let zmail change Gmail
  settings.
- **Help → Keyboard Shortcuts** (Ctrl+/) lists every key.
- Search understands `is:flagged` and `flag:red` (or any flag colour).
- Dates in the message list read `01/01/2026  9:05 AM`.
- **File → Save Attachments** saves the open message's attachments into a
  folder you choose (safe file names, never over an existing file), and
  **File → Print** (Ctrl+P) prints the message with its header.
- **Edit → Copy** (Ctrl+C) copies the text selected in the message, or the
  selected messages a line each. **View → View as Plain Text** shows a
  message's text part instead of its HTML.
- **Settings → Message List** sets the message list's text size and the
  space between its rows (Compact / Comfortable / Roomy presets, or any value).
- **Settings → Row Stripes** sets how strongly alternate rows in the message
  list are shaded (a slider with Off / Subtle / Normal / Strong presets).

## Themes

**View → Theme** offers Light (default), Dark, Follow System and three brand
themes: **Boilermakers** (Purdue gold on black), **Badgers** (white on UW black
with a Badger Red toolbar and headers) and **Packers** (white on Packers green
with gold headers and selection). zmail remembers the choice (`ui/theme`);
`zmail --theme packers` overrides it for one run. Messages keep their white
page in every theme. The brand palette is shared with
[zterminal](https://github.com/sbj-ee/zterminal); see
[docs/THEMES.md](docs/THEMES.md) for every role, hex value, source and
contrast ratio.

**View → Theme → Theme Editor…** makes custom themes: duplicate any theme
(the brand ones too), pick a colour for each palette role, optionally a UI font
and size and the list's row stripes, with a live preview; then save, rename,
delete, apply, import or export. Custom themes appear in View → Theme and are
`*.ztheme.json` files in `~/.config/zmail/themes/`, a format shared with
zterminal ([docs/THEMES.md](docs/THEMES.md#theme-files-custom-themes)); zterminal's themes in
`~/.config/zterminal/themes/` are listed read-only.

| Boilermakers | Badgers | Packers |
|---|---|---|
| ![zmail in the Boilermakers theme](docs/screenshots/zmail-boilermakers.png) | ![zmail in the Badgers theme](docs/screenshots/zmail-badgers.png) | ![zmail in the Packers theme](docs/screenshots/zmail-packers.png) |

![The Theme Editor with a custom theme (Lakeside Dusk) and its live preview](docs/screenshots/theme-editor.png)

### Shared with zterminal

The themes work the same way in both apps. Boilermakers, Badgers and Packers
are built into zmail and [zterminal](https://github.com/sbj-ee/zterminal) with
the same palette, and custom themes move between them as `*.ztheme.json`
files: **Export…** in one app's Theme Editor writes `<name>.ztheme.json`, and
**Import…** in the other app's editor copies it into that app's themes folder.
If both apps run under the same user, each one also lists the other's custom
themes read-only without importing. zmail keeps but ignores a theme's terminal
colours; zterminal derives terminal colours for a theme made in zmail. The
format, the file locations and how missing colours are filled in are in
[docs/THEMES.md](docs/THEMES.md).

![The Boilermakers, Badgers and Packers themes in zmail (top) and zterminal (bottom)](docs/screenshots/brand-themes.png)

## New-mail sound

When new INBOX mail arrives (while zmail is already running), zmail plays a
short built-in two-note chime once per sync batch. Mute it from **View → Play
Sound for New Mail**, the toolbar speaker, or **Ctrl+Shift+M**. **Settings →
Sounds** turns the chime on or off, offers six more sounds that come with
zmail (Bell, Marimba, Glass, Knock, Water Drop, Harp: all original and
CC0, made by `tools/make-new-mail-sounds.py`; a filter can use one too),
lets you **Browse…** for a custom `.wav`,
**Default** resets to the bundled sound, and **Test** previews the current
choice. A missing or unreadable custom file falls back to the built-in chime
(`assets/sounds/new-mail.wav`, also embedded in the binary via Qt resources).

A message that leaves (Send, or File → Send Queued Messages) plays a short
"swoosh" (`assets/sounds/sent.wav`). **Settings → Sounds → Sent mail** turns
it off, and has its own **Test**.

## License

MIT. See [LICENSE](LICENSE).
