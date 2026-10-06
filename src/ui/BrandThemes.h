// BrandThemes.h: the shared brand colour themes of sbj-ee/zmail and
// sbj-ee/zterminal ("Boilermakers", "Badgers", "Packers").
//
// THIS FILE IS BYTE-IDENTICAL IN BOTH REPOS:
//   zmail:     src/ui/BrandThemes.h
//   zterminal: core/BrandThemes.h
// The table is documented in docs/THEMES.md of each repo. Change the file,
// docs/THEMES.md and the hex table in the unit tests in BOTH repos together.
//
// Plain C++17, no Qt: colours are 0xRRGGBB. Brand sources:
//   Purdue:  https://www.purdue.edu/brand-studio/brand/visual-identity/
//   UW:      https://brand.wisc.edu/visual-identity/colors/
//   Packers: NFL/Packers logo slick (PMS 5535 C #203731, PMS 1235 C #FFB612)
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace sbj::brand {

// An official brand colour, by the name the brand guide gives it.
struct Swatch {
    std::string_view name; // empty = unused slot
    std::uint32_t rgb;
};

// One theme. The UI roles are used by both apps; `ansi` is the terminal's
// 16-colour palette (zterminal; zmail ignores it). Terminal mapping:
// background/foreground as is, cursor = accent, selection = selection on
// selectionText.
struct Theme {
    std::string_view id;   // stable settings key, e.g. "boilermakers"
    std::string_view name; // shown in menus, e.g. "Boilermakers"
    std::array<Swatch, 6> swatches;

    std::uint32_t background;    // main content: terminal, mail list, mailbox tree
    std::uint32_t surface;       // window / panels / buttons behind the content
    std::uint32_t foreground;    // main text
    std::uint32_t muted;         // secondary text (>= 4.5:1 on background)
    std::uint32_t accent;        // cursor, focus, brand accent
    std::uint32_t link;          // links / accent text (>= 4.5:1 on background)
    std::uint32_t selection;     // selected rows / terminal selection background
    std::uint32_t selectionText; // text on `selection`
    std::uint32_t chrome;        // toolbar band
    std::uint32_t chromeText;    // text/icons on `chrome`
    std::uint32_t header;        // column headers
    std::uint32_t headerText;    // text on `header`

    std::array<std::uint32_t, 16> ansi; // 0-7 normal, 8-15 bright
};

inline constexpr std::array<Theme, 3> kThemes{{
    // Purdue: black and Boilermaker Gold; gold text on black.
    {"boilermakers", "Boilermakers",
     {{{"Boilermaker Gold", 0xCFB991}, {"Black", 0x000000}, {"Aged", 0x8E6F3E},
       {"Rush", 0xDAAA00}, {"Railway Gray", 0x9D9795}, {"White", 0xFFFFFF}}},
     /*background*/ 0x000000, /*surface*/ 0x141414, /*foreground*/ 0xCFB991, /*muted*/ 0x9D9795,
     /*accent*/ 0xDAAA00, /*link*/ 0xDAAA00, /*selection*/ 0xCFB991, /*selectionText*/ 0x000000,
     /*chrome*/ 0x000000, /*chromeText*/ 0xCFB991, /*header*/ 0xDAAA00, /*headerText*/ 0x000000,
     {0x262626, 0xE5534B, 0x8CC265, 0xDAAA00, 0x6CA0DC, 0xC792EA, 0x56B6C2, 0xC4BFC0,
      0x9D9795, 0xFF7B72, 0xB5E08A, 0xEBD99F, 0x9CC3F0, 0xE3B0F5, 0x8EDCE6, 0xFFFFFF}},

    // UW-Madison: Badger Red and white; white text on UW Black, red accents.
    {"badgers", "Badgers",
     {{{"Badger Red", 0xC5050C}, {"Dark Red", 0x9B0000}, {"White", 0xFFFFFF},
       {"Black", 0x121212}, {"Light Gray", 0xE1E5E7}, {"", 0}}},
     /*background*/ 0x121212, /*surface*/ 0x1E1E1E, /*foreground*/ 0xFFFFFF, /*muted*/ 0xADB1B4,
     /*accent*/ 0xC5050C, /*link*/ 0xFF7B80, /*selection*/ 0x9B0000, /*selectionText*/ 0xFFFFFF,
     /*chrome*/ 0xC5050C, /*chromeText*/ 0xFFFFFF, /*header*/ 0xC5050C, /*headerText*/ 0xFFFFFF,
     {0x2A2A2A, 0xF0474E, 0x7FC97F, 0xF2C14E, 0x6FA8EC, 0xD88AD8, 0x5CC8C8, 0xE1E5E7,
      0x8A8D91, 0xFF7B80, 0xA8E6A3, 0xFFDA7A, 0x9DC4F5, 0xF0B0F0, 0x90E3E3, 0xFFFFFF}},

    // Green Bay Packers: dark green and gold; white text on green, gold accents.
    {"packers", "Packers",
     {{{"Dark Green", 0x203731}, {"Gold", 0xFFB612}, {"White", 0xFFFFFF},
       {"", 0}, {"", 0}, {"", 0}}},
     /*background*/ 0x203731, /*surface*/ 0x1A2D28, /*foreground*/ 0xFFFFFF, /*muted*/ 0xB4C4BE,
     /*accent*/ 0xFFB612, /*link*/ 0xFFB612, /*selection*/ 0xFFB612, /*selectionText*/ 0x203731,
     /*chrome*/ 0x14241F, /*chromeText*/ 0xFFFFFF, /*header*/ 0xFFB612, /*headerText*/ 0x203731,
     {0x14241F, 0xFF8A80, 0x9EE07A, 0xFFB612, 0x8AB4F8, 0xE3A6F0, 0x7FE0D6, 0xE1E5E7,
      0x8FA89F, 0xFFB3AD, 0xC3F0A6, 0xFFD878, 0xB8D1FB, 0xF1C9F8, 0xB0F0E8, 0xFFFFFF}},
}};

// nullptr for an unknown id.
inline constexpr const Theme *findTheme(std::string_view id)
{
    for (const Theme &t : kThemes) {
        if (t.id == id) {
            return &t;
        }
    }
    return nullptr;
}

} // namespace sbj::brand
