# Brand themes: Boilermakers, Badgers, Packers

These three colour themes are **shared by [zmail](https://github.com/sbj-ee/zmail)
and [zterminal](https://github.com/sbj-ee/zterminal)**: same names, same roles,
same hex values. The single source of truth is the byte-identical header
`BrandThemes.h` (zmail: `src/ui/BrandThemes.h`, zterminal: `core/BrandThemes.h`);
this page is byte-identical in both repos too (`docs/THEMES.md`). Change all of
them together, plus the hex tables in the unit tests (zmail `tests/tst_theme.cpp`,
zterminal `tests/tst_misc.cpp`).

## Selecting a theme

| App | Where | Stored as |
|---|---|---|
| zmail | **View → Theme → Boilermakers / Badgers / Packers** (also Light, Dark, Follow System), or `zmail --theme packers` | QSettings `ui/theme` (`light`, `dark`, `system`, `boilermakers`, `badgers`, `packers`); restored at start-up unless `--theme` is given |
| zterminal | **Settings → Preferences → Color scheme** (default for every tab), **View → Color Scheme** (applies now; saved to Preferences, or to the session’s override if it has one), or a saved session’s **Color scheme** dropdown in the session dialog (File → New / Open Session) (overrides "Default (Preferences)") | `appearance/colorScheme` = `boilermakers` / `badgers` / `packers` |

## Official brand colours (sources)

- **Boilermakers**: Boilermaker Gold `#CFB991`, Black `#000000`, Aged `#8E6F3E`, Rush `#DAAA00`, Railway Gray `#9D9795`, White `#FFFFFF`. Source: Purdue Brand Studio, [Fonts and Colors](https://www.purdue.edu/brand-studio/brand/visual-identity/) and the [Web Style Guide](https://www.purdue.edu/brand-studio/digital/design-copy/) (Gold and Black primary; Aged, Rush, Railway Gray supporting).
- **Badgers**: Badger Red `#C5050C`, Dark Red `#9B0000`, White `#FFFFFF`, Black `#121212`, Light Gray `#E1E5E7`. Source: UW–Madison Brand and Visual Identity, [Colors](https://brand.wisc.edu/visual-identity/colors/) (Badger Red and white primary; digital secondary Dark Red, Light Gray, Black).
- **Packers**: Dark Green `#203731`, Gold `#FFB612`, White `#FFFFFF`. Source: Green Bay Packers logo slick / identity sheet (PMS 5535 C = #203731, PMS 1235 C = #FFB612, white), as archived at [sportsarchive](https://nyc3.digitaloceanspaces.com/sportsarchive-documents/prod/643496b1c0f0e/GREENBAY_2012_logoslick.pdf).

All hex values match the official sources; none were adjusted.

## UI roles

Roles marked *derived* are not brand colours: they're tints chosen for
readability (panels, secondary text, a readable red for links on near-black).
Brand colours are used as is.

| Role | Used for | Boilermakers | Badgers | Packers |
|---|---|---|---|---|
| background | terminal / mail list, mailbox tree | `#000000` | `#121212` | `#203731` |
| surface | window, panels, buttons | `#141414` *derived* | `#1E1E1E` *derived* | `#1A2D28` *derived* |
| foreground | main text | `#CFB991` | `#FFFFFF` | `#FFFFFF` |
| muted | secondary text | `#9D9795` | `#ADB1B4` *derived* | `#B4C4BE` *derived* |
| accent | cursor, focus, brand accent | `#DAAA00` | `#C5050C` | `#FFB612` |
| link | links | `#DAAA00` | `#FF7B80` *derived* | `#FFB612` |
| selection | selected rows, terminal selection | `#CFB991` | `#9B0000` | `#FFB612` |
| selectionText | text on selection | `#000000` | `#FFFFFF` | `#203731` |
| chrome | zmail toolbar band | `#000000` | `#C5050C` | `#14241F` *derived* |
| chromeText | text on chrome | `#CFB991` | `#FFFFFF` | `#FFFFFF` |
| header | zmail column headers | `#DAAA00` | `#C5050C` | `#FFB612` |
| headerText | text on header | `#000000` | `#FFFFFF` | `#203731` |

## Terminal (zterminal) mapping

background → background, foreground → foreground, cursor → accent,
selection → selection on selectionText; ANSI 0–15:

| # | Name | Boilermakers | Badgers | Packers |
|---|---|---|---|---|
| 0 | black | `#262626` (1.4:1) | `#2A2A2A` (1.3:1) | `#14241F` (1.3:1) |
| 1 | red | `#E5534B` (5.7:1) | `#F0474E` (5.1:1) | `#FF8A80` (5.6:1) |
| 2 | green | `#8CC265` (10.0:1) | `#7FC97F` (9.4:1) | `#9EE07A` (8.1:1) |
| 3 | yellow | `#DAAA00` (9.7:1) | `#F2C14E` (11.2:1) | `#FFB612` (7.2:1) |
| 4 | blue | `#6CA0DC` (7.7:1) | `#6FA8EC` (7.6:1) | `#8AB4F8` (6.0:1) |
| 5 | magenta | `#C792EA` (8.7:1) | `#D88AD8` (7.6:1) | `#E3A6F0` (6.6:1) |
| 6 | cyan | `#56B6C2` (8.9:1) | `#5CC8C8` (9.4:1) | `#7FE0D6` (8.2:1) |
| 7 | white | `#C4BFC0` (11.6:1) | `#E1E5E7` (14.8:1) | `#E1E5E7` (10.0:1) |
| 8 | bright black | `#9D9795` (7.3:1) | `#8A8D91` (5.6:1) | `#8FA89F` (5.0:1) |
| 9 | bright red | `#FF7B72` (8.3:1) | `#FF7B80` (7.5:1) | `#FFB3AD` (7.4:1) |
| 10 | bright green | `#B5E08A` (14.0:1) | `#A8E6A3` (12.9:1) | `#C3F0A6` (9.9:1) |
| 11 | bright yellow | `#EBD99F` (15.0:1) | `#FFDA7A` (13.9:1) | `#FFD878` (9.3:1) |
| 12 | bright blue | `#9CC3F0` (11.5:1) | `#9DC4F5` (10.4:1) | `#B8D1FB` (8.2:1) |
| 13 | bright magenta | `#E3B0F5` (11.8:1) | `#F0B0F0` (10.9:1) | `#F1C9F8` (8.7:1) |
| 14 | bright cyan | `#8EDCE6` (13.5:1) | `#90E3E3` (12.7:1) | `#B0F0E8` (10.0:1) |
| 15 | bright white | `#FFFFFF` (21.0:1) | `#FFFFFF` (18.7:1) | `#FFFFFF` (12.7:1) |

Ratios are WCAG 2.x contrast against the theme background. Colour 0 is the
"black" a program draws backgrounds with, so it's deliberately close to the
background; every other colour is >= 4.5:1, and the brand golds are used for
yellow (Rush / Dust for Boilermakers, Packers Gold) and white (Steam, Light Gray).

## Contrast (WCAG 2.x)

| Pair | Boilermakers | Badgers | Packers |
|---|---|---|---|
| foreground on background | 11.00:1 | 18.73:1 | 12.71:1 |
| foreground on surface | 9.65:1 | 16.67:1 | 14.48:1 |
| muted on background | 7.29:1 | 8.68:1 | 7.01:1 |
| link on background | 9.73:1 | 7.49:1 | 7.23:1 |
| selectionText on selection | 11.00:1 | 8.77:1 | 7.23:1 |
| chromeText on chrome | 11.00:1 | 6.17:1 | 16.13:1 |
| headerText on header | 9.73:1 | 6.17:1 | 7.23:1 |
| cursor (accent) vs background | 9.73:1 | 3.03:1 | 7.23:1 |

Text pairs are >= 7:1 (WCAG AAA) except Badgers' white on Badger Red
(toolbar and headers, 6.17:1, AA); the cursor only needs 3:1 (non-text UI).
In zmail the message body keeps its own white page (or View → Dark Background
for Messages), so mail stays readable in every theme.

## Screenshots

One per theme per app (offline sample data). The files live in each repo's
`docs/screenshots/` (zmail-*.png in zmail, zterminal-*.png in zterminal); the
links point at each repo's main branch so this page stays identical in
both repos.

### Boilermakers

![zmail in the Boilermakers theme](https://raw.githubusercontent.com/sbj-ee/zmail/main/docs/screenshots/zmail-boilermakers.png)

![zterminal in the Boilermakers theme](https://raw.githubusercontent.com/sbj-ee/zterminal/main/docs/screenshots/zterminal-boilermakers.png)

### Badgers

![zmail in the Badgers theme](https://raw.githubusercontent.com/sbj-ee/zmail/main/docs/screenshots/zmail-badgers.png)

![zterminal in the Badgers theme](https://raw.githubusercontent.com/sbj-ee/zterminal/main/docs/screenshots/zterminal-badgers.png)

### Packers

![zmail in the Packers theme](https://raw.githubusercontent.com/sbj-ee/zmail/main/docs/screenshots/zmail-packers.png)

![zterminal in the Packers theme](https://raw.githubusercontent.com/sbj-ee/zterminal/main/docs/screenshots/zterminal-packers.png)
