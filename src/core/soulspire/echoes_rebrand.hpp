#pragma once
// SoulspireXI — Soul Echoes Native UI & System Text Rebranding (QA-SOUL-COMPANIONS-001 / WS8-INFRA-020)
//
// Canonical player-facing terminology:
//   - Official system name: "Soul Echoes" (singular: "Soul Echo")
//   - Short UI label:       "Echoes"
//
// Zero new FFXiMain.dll hooks: reuses VanaCore's existing buttons.cpp (YmMenuShape::Draw),
// labeltext.cpp (Config/Filter labels), chat.cpp (add_line system messages), and
// soulspire/hooks.cpp (magic row/desc/target/visibility) pipelines.
//
// Preserves all internal identifiers, spell IDs (896..961), packet protocols, and
// native keyboard/mouse/gamepad navigation.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace vanacore::soulspire {

inline constexpr const char kSoulEchoesSystemName[] = "Soul Echoes";
inline constexpr const char kSoulEchoesSingular[]   = "Soul Echo";
inline constexpr const char kSoulEchoesShortLabel[] = "Echoes";

// Exact "font    font    " 256x256 sheet glyph table (mirrors vanacore::chatfont::kLabelSprites).
struct EchoesFontGlyph {
    char c;
    std::uint8_t u, v, w, h, row;
};

inline constexpr EchoesFontGlyph kEchoesFontGlyphs[] = {
    {'!', 12, 0, 6, 11, 0},    {'"', 21, 0, 7, 5, 0},     {'#', 30, 0, 10, 11, 0},   {'$', 40, 0, 10, 11, 0},
    {'%', 50, 0, 10, 11, 0},   {'&', 60, 0, 10, 11, 0},   {'\'', 73, 1, 4, 6, 0},    {'(', 81, 0, 5, 12, 0},
    {')', 94, 0, 5, 12, 0},    {'*', 100, 0, 10, 11, 0},  {'+', 111, 1, 8, 10, 0},   {',', 1, 11, 6, 13, 13},
    {'-', 11, 13, 8, 8, 13},   {'.', 23, 20, 4, 4, 13},   {'/', 30, 13, 9, 11, 13},  {'0', 40, 13, 9, 11, 13},
    {'1', 51, 13, 8, 11, 13},  {'2', 60, 13, 9, 11, 13},  {'3', 70, 13, 9, 11, 13},  {'4', 80, 13, 9, 11, 13},
    {'5', 90, 13, 9, 11, 13},  {'6', 101, 13, 8, 11, 13}, {'7', 111, 13, 9, 11, 13}, {'8', 0, 26, 9, 11, 26},
    {'9', 9, 26, 10, 11, 26},  {':', 19, 28, 4, 8, 26},   {';', 24, 28, 5, 9, 26},   {'<', 30, 28, 8, 9, 26},
    {'=', 39, 29, 8, 7, 26},   {'>', 48, 28, 8, 9, 26},   {'?', 56, 26, 10, 11, 26}, {'@', 66, 26, 9, 11, 26},
    {'A', 77, 26, 9, 11, 26},  {'B', 87, 26, 9, 11, 26},  {'C', 97, 26, 9, 12, 26},  {'D', 107, 26, 9, 11, 26},
    {'E', 0, 39, 9, 11, 39},   {'F', 10, 39, 9, 11, 39},  {'G', 20, 39, 9, 12, 39},  {'H', 31, 39, 9, 11, 39},
    {'I', 42, 39, 6, 11, 39},  {'J', 48, 39, 9, 12, 39},  {'K', 58, 39, 9, 11, 39},  {'L', 69, 39, 9, 11, 39},
    {'M', 80, 39, 9, 11, 39},  {'N', 91, 39, 9, 11, 39},  {'O', 102, 39, 9, 12, 39}, {'P', 113, 39, 9, 11, 39},
    {'Q', 0, 52, 9, 12, 52},   {'R', 11, 52, 10, 11, 52}, {'S', 22, 52, 9, 12, 52},  {'T', 31, 52, 9, 11, 52},
    {'U', 41, 52, 9, 12, 52},  {'V', 52, 52, 10, 11, 52}, {'W', 63, 52, 9, 11, 52},  {'X', 76, 52, 9, 11, 52},
    {'Y', 88, 52, 9, 11, 52},  {'Z', 98, 52, 11, 11, 52}, {'[', 114, 52, 6, 11, 52}, {']', 13, 65, 6, 11, 65},
    {'^', 21, 65, 8, 7, 65},   {'_', 31, 72, 8, 4, 65},   {'`', 42, 65, 5, 7, 65},   {'a', 49, 67, 8, 9, 65},
    {'b', 58, 65, 9, 11, 65},  {'c', 68, 67, 9, 9, 65},   {'d', 78, 65, 9, 11, 65},  {'e', 89, 67, 9, 9, 65},
    {'f', 99, 65, 7, 11, 65},  {'g', 107, 67, 9, 12, 65}, {'h', 0, 78, 9, 11, 78},   {'i', 10, 78, 5, 11, 78},
    {'j', 16, 78, 8, 14, 78},  {'k', 25, 78, 9, 11, 78},  {'l', 35, 78, 6, 11, 78},  {'m', 42, 80, 9, 9, 78},
    {'n', 56, 80, 9, 9, 78},   {'o', 66, 80, 9, 9, 78},   {'p', 76, 80, 9, 12, 78},  {'q', 87, 80, 9, 12, 78},
    {'r', 98, 80, 7, 9, 78},   {'s', 106, 80, 9, 9, 78},  {'t', 0, 92, 6, 10, 91},   {'u', 7, 93, 9, 9, 91},
    {'v', 17, 93, 9, 9, 91},   {'w', 27, 93, 9, 9, 91},   {'x', 39, 93, 9, 9, 91},   {'y', 50, 93, 9, 12, 91},
    {'z', 60, 93, 8, 9, 91},
};

inline constexpr int kEchoesSpaceAdvancePx = 4;

inline const EchoesFontGlyph* find_echoes_glyph_by_char(char c) noexcept
{
    for (const auto& g : kEchoesFontGlyphs) {
        if (g.c == c)
            return &g;
    }
    return nullptr;
}

inline const EchoesFontGlyph* find_echoes_glyph_by_uv(int u, int v) noexcept
{
    for (int off = 0; off <= 1; ++off) {
        for (const auto& g : kEchoesFontGlyphs) {
            const int du = static_cast<int>(g.u) - u;
            const int dv = static_cast<int>(g.v) - v;
            if (du >= -off && du <= off && dv >= -off && dv <= off)
                return &g;
        }
    }
    return nullptr;
}

inline int measure_font_text_px(std::string_view text) noexcept
{
    int width = 0;
    for (char c : text) {
        if (c == ' ') {
            width += kEchoesSpaceAdvancePx;
        } else if (const EchoesFontGlyph* g = find_echoes_glyph_by_char(c)) {
            width += static_cast<int>(g->w);
        } else {
            return -1;
        }
    }
    return width;
}

// ============================================================================
// Deterministic, Word-Boundary-Safe Player-Facing Text Rebranding
// ============================================================================

inline bool echoes_is_word_char(unsigned char c) noexcept
{
    return std::isalnum(c) != 0 || c == '_';
}

inline bool echoes_iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

inline bool echoes_matches_word_at(std::string_view s, std::size_t pos, std::string_view needle) noexcept
{
    if (pos + needle.size() > s.size())
        return false;
    if (pos > 0 && echoes_is_word_char(static_cast<unsigned char>(s[pos - 1])))
        return false;
    const std::size_t end = pos + needle.size();
    if (end < s.size() && echoes_is_word_char(static_cast<unsigned char>(s[end])))
        return false;
    return echoes_iequals(s.substr(pos, needle.size()), needle);
}

inline std::string echoes_trim(std::string_view s)
{
    std::size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first])) != 0)
        ++first;
    std::size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1])) != 0)
        --last;
    return std::string(s.substr(first, last - first));
}

// Returns true for non-conversational FFXI chat log modes (system/game/battle notices).
// Strictly excludes player chat channels so player messages are never modified.
inline bool is_system_rebrand_chat_mode(std::uint8_t mode) noexcept
{
    switch (mode) {
        case 1:   // Say (outgoing)
        case 2:   // Shout (outgoing)
        case 4:   // Tell (outgoing)
        case 5:   // Party (outgoing)
        case 6:   // Linkshell (outgoing)
        case 7:   // Emote (outgoing)
        case 9:   // Say (incoming)
        case 10:  // Shout (incoming)
        case 11:  // Yell (outgoing)
        case 12:  // Tell (incoming)
        case 13:  // Party (incoming)
        case 14:  // Linkshell (incoming)
        case 15:  // Emote (incoming)
        case 26:  // Yell (incoming)
        case 27:  // Linkshell 2 (outgoing)
        case 28:  // Linkshell 2 (incoming)
        case 212: // Unity chat
        case 213: // Linkshell 2 (alt)
        case 214: // Linkshell 2 (alt)
            return false;
        default:
            return true;
    }
}

// Rewrites player-facing FFXI Trust / Alter Ego / Soul Companion text to Soul Echoes terminology.
// Preserves FFXI color/control byte sequences (0x1E, 0x1F, 0x7F, 0xFD) and leaves unrelated words
// ("entrust", "distrust", "trusted", "trustworthy") untouched.
inline std::string rebrand_echoes_text(std::string_view input)
{
    if (input.empty())
        return {};

    // Fast pre-check: avoid allocations on the 99.9% of strings without Trust/Alter/Companion.
    bool candidate = false;
    for (std::size_t i = 0; i + 4 < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(input[i])));
        if (c == 't' || c == 'a' || c == 'c') {
            if ((i + 5 <= input.size() && echoes_iequals(input.substr(i, 5), "trust")) ||
                (i + 5 <= input.size() && echoes_iequals(input.substr(i, 5), "alter")) ||
                (i + 9 <= input.size() && echoes_iequals(input.substr(i, 9), "companion"))) {
                candidate = true;
                break;
            }
        }
    }
    if (!candidate)
        return std::string(input);

    const std::string trimmed = echoes_trim(input);
    if (echoes_iequals(trimmed, "Trust") || echoes_iequals(trimmed, "Trusts"))
        return std::string(kSoulEchoesShortLabel);

    struct Rule {
        std::string_view from;
        std::string_view to;
    };
    static constexpr Rule kRules[] = {
        {"Soul Companions", "Soul Echoes"},
        {"Soul Companion",  "Soul Echo"},
        {"Trust magic",     "Soul Echoes"},
        {"Trust spells",    "Soul Echoes spells"},
        {"Trust spell",     "Soul Echo spell"},
        {"an alter ego",    "a Soul Echo"},
        {"An alter ego",    "A Soul Echo"},
        {"alter egos",      "Soul Echoes"},
        {"alter ego",       "Soul Echo"},
        {"Release Trust",   "Dismiss Echo"},
        {"Return Trust",    "Dismiss Echo"},
        {"Call Trust",      "Summon Echo"},
        {"Trusts",          "Soul Echoes"},
    };

    std::string out;
    out.reserve(input.size() + 16);
    std::size_t i = 0;
    while (i < input.size()) {
        const unsigned char b = static_cast<unsigned char>(input[i]);
        // Preserve FFXI control / color / auto-translate sequences verbatim.
        if (b == 0x1E || b == 0x1F || b == 0x7F) {
            const std::size_t take = (i + 2 <= input.size()) ? 2 : 1;
            out.append(input.substr(i, take));
            i += take;
            continue;
        }
        if (b == 0xFD) {
            const std::size_t take = (i + 6 <= input.size()) ? 6 : (input.size() - i);
            out.append(input.substr(i, take));
            i += take;
            continue;
        }

        bool matched = false;
        for (const auto& r : kRules) {
            if (echoes_matches_word_at(input, i, r.from)) {
                out.append(r.to);
                i += r.from.size();
                matched = true;
                break;
            }
        }
        if (matched)
            continue;

        // Standalone "Trust" in prose (only when capitalized as the FFXI mechanic name,
        // avoiding generic lowercase English verb "trust" such as "trust where it came from").
        if (input[i] == 'T' && echoes_matches_word_at(input, i, "Trust")) {
            out.append(kSoulEchoesSystemName);
            i += 5;
            continue;
        }

        out.push_back(input[i]);
        ++i;
    }
    return out;
}

// ============================================================================
// Native FFXI CYySprite Button Label Decoder & Anti-Clipping Synthesizer
// ============================================================================

inline constexpr std::size_t kEchoesMaxSpriteTiles = 48u;
inline constexpr std::size_t kEchoesMaxSpriteBanks = 16u;

struct EchoesMenuSpriteBank {
    bool active = false;
    const std::uint8_t* orig_shape = nullptr;
    std::uint8_t orig_hdr[16]{};
    std::uint8_t orig_tile0_hdr[32]{};
    std::uint8_t sprite[0x30]{};
    std::uint8_t nodes[kEchoesMaxSpriteTiles][0x18]{};
    std::uint8_t tiles[kEchoesMaxSpriteTiles][0xE0]{};
};

struct DecodedButtonSprite {
    bool is_button = false;
    char label[64]{};
    std::uint8_t label_len = 0;
    int pill_left = 0;
    int pill_right = 0;
    int fg_left = 5;
    int base_y = 12;
    const std::uint8_t* pill_nodes[4]{};
    const std::uint8_t* pill_tiles[4]{};
    std::size_t pill_count = 0;
    const std::uint8_t* tpl_fg_node = nullptr;
    const std::uint8_t* tpl_fg_tile = nullptr;
    const std::uint8_t* tpl_sh_node = nullptr;
    const std::uint8_t* tpl_sh_tile = nullptr;
    int shadow_dx = 2;
    int shadow_dy = 2;

    std::string_view label_view() const noexcept { return std::string_view(label, label_len); }
};

inline bool decode_button_sprite_label(const std::uint8_t* shape, DecodedButtonSprite& out) noexcept
{
    out = {};
    if (shape == nullptr)
        return false;

    std::uint32_t count = 0;
    std::memcpy(&count, shape + 0x14, sizeof(count));
    if (count == 0 || count > 64u)
        return false;

    const std::uint8_t* node = nullptr;
    std::memcpy(&node, shape + 0x04, sizeof(node));

    struct FgLetter {
        int left;
        int right;
        int base;
        char c;
        const std::uint8_t* node;
        const std::uint8_t* tile;
    };
    FgLetter fgs[48]{};
    std::size_t fg_count = 0;
    bool have_pill = false;
    int first_sh_left = 0;
    int first_sh_top = 0;
    bool have_sh = false;
    int first_fg_top = 0;
    bool have_fg_top = false;

    std::uint32_t guard = 0;
    while (node != nullptr && guard < count && guard < 64u) {
        ++guard;
        if (node[0x14] == 0) {
            const std::uint8_t* tile = nullptr;
            std::memcpy(&tile, node + 0x10, sizeof(tile));
            if (tile != nullptr) {
                if (std::memcmp(tile + 0x32, "menu    button", 14) == 0) {
                    std::int16_t xs[4]{};
                    std::memcpy(xs, tile + 0xC0, sizeof(xs));
                    const int mn = std::min({xs[0], xs[1], xs[2], xs[3]});
                    const int mx = std::max({xs[0], xs[1], xs[2], xs[3]});
                    out.pill_left  = have_pill ? std::min(out.pill_left, mn) : mn;
                    out.pill_right = have_pill ? std::max(out.pill_right, mx) : mx;
                    have_pill = true;
                    if (out.pill_count < 4u) {
                        out.pill_nodes[out.pill_count] = node;
                        out.pill_tiles[out.pill_count] = tile;
                        ++out.pill_count;
                    }
                } else if (std::memcmp(tile + 0x32, "font    font    ", 16) == 0) {
                    const std::uint8_t blend_a = tile[0x2E];
                    std::uint16_t u = 0, v = 0;
                    std::memcpy(&u, tile + 0x0C, sizeof(u));
                    std::memcpy(&v, tile + 0x0E, sizeof(v));
                    std::int16_t xs[4]{}, ys[4]{};
                    std::memcpy(xs, tile + 0xC0, sizeof(xs));
                    std::memcpy(ys, tile + 0xC8, sizeof(ys));
                    const int left = std::min({xs[0], xs[1], xs[2], xs[3]});
                    const int right = std::max({xs[0], xs[1], xs[2], xs[3]});
                    const int top = std::min({ys[0], ys[1], ys[2], ys[3]});
                    if (blend_a == 2u) {
                        if (!have_sh) {
                            have_sh = true;
                            first_sh_left = left;
                            first_sh_top = top;
                            out.tpl_sh_node = node;
                            out.tpl_sh_tile = tile;
                        }
                    } else if (fg_count < 48u) {
                        const EchoesFontGlyph* g = find_echoes_glyph_by_uv(u, v);
                        if (g != nullptr) {
                            const int base = top + (static_cast<int>(g->row) + 10 - static_cast<int>(v));
                            fgs[fg_count++] = {left, right, base, g->c, node, tile};
                            if (!have_fg_top) {
                                have_fg_top = true;
                                first_fg_top = top;
                                out.tpl_fg_node = node;
                                out.tpl_fg_tile = tile;
                            }
                        }
                    }
                }
            }
        }
        const std::uint8_t* next = nullptr;
        std::memcpy(&next, node + 0x00, sizeof(next));
        node = next;
    }

    if (!have_pill || fg_count == 0 || out.tpl_fg_tile == nullptr)
        return false;

    std::sort(fgs, fgs + fg_count, [](const FgLetter& a, const FgLetter& b) {
        return a.left < b.left;
    });

    out.is_button = true;
    out.fg_left = fgs[0].left;
    out.base_y = fgs[0].base;
    if (have_sh && have_fg_top) {
        const int dx = first_sh_left - fgs[0].left;
        const int dy = first_sh_top - first_fg_top;
        if (dx >= 0 && dx <= 4) out.shadow_dx = dx;
        if (dy >= 0 && dy <= 4) out.shadow_dy = dy;
    }

    out.label_len = 0;
    for (std::size_t i = 0; i < fg_count; ++i) {
        if (i > 0 && (fgs[i].left - fgs[i - 1].right) >= 3 && out.label_len + 1u < sizeof(out.label))
            out.label[out.label_len++] = ' ';
        if (out.label_len + 1u < sizeof(out.label))
            out.label[out.label_len++] = fgs[i].c;
    }
    out.label[out.label_len] = '\0';
    return out.label_len > 0;
}

// Chooses a rebranded button label that mathematically fits within `pill_width` with >= 4px margin
// on each side (zero clipping at any resolution or UI scale). Returns false if no rename applies.
inline bool choose_fitted_echoes_button_label(
    std::string_view decoded_label,
    int pill_width,
    std::string& out_label)
{
    const std::string trimmed = echoes_trim(decoded_label);
    if (trimmed.empty())
        return false;

    const int max_text_px = std::max(0, pill_width - 8);
    auto pick = [&](std::string_view preferred, std::string_view fallback) -> bool {
        const int pref_w = measure_font_text_px(preferred);
        if (pref_w > 0 && pref_w <= max_text_px) {
            out_label.assign(preferred);
            return true;
        }
        const int fb_w = measure_font_text_px(fallback);
        if (fb_w > 0 && fb_w <= max_text_px) {
            out_label.assign(fallback);
            return true;
        }
        return false;
    };

    if (echoes_iequals(trimmed, "Trust") || echoes_iequals(trimmed, "Trusts"))
        return pick(kSoulEchoesShortLabel, kSoulEchoesShortLabel);
    if (echoes_iequals(trimmed, "Trust Magic") || echoes_iequals(trimmed, "Alter Egos") ||
        echoes_iequals(trimmed, "Soul Companions"))
        return pick(kSoulEchoesSystemName, kSoulEchoesShortLabel);
    if (echoes_iequals(trimmed, "Alter Ego") || echoes_iequals(trimmed, "Soul Companion"))
        return pick(kSoulEchoesSingular, kSoulEchoesShortLabel);
    if (echoes_iequals(trimmed, "Release Trust") || echoes_iequals(trimmed, "Return Trust") ||
        echoes_iequals(trimmed, "Dismiss Trust"))
        return pick("Dismiss Echo", "Dismiss");
    if (echoes_iequals(trimmed, "Call Trust") || echoes_iequals(trimmed, "Summon Trust"))
        return pick("Summon Echo", kSoulEchoesShortLabel);

    const std::string rewritten = rebrand_echoes_text(trimmed);
    if (rewritten == trimmed)
        return false;
    return pick(rewritten, kSoulEchoesShortLabel);
}

inline void patch_echoes_letter_quad(
    std::uint8_t* dst_tile,
    const std::uint8_t* tpl_tile,
    const EchoesFontGlyph& glyph,
    int x0,
    int y0,
    float inv_sheet_w,
    float inv_sheet_h) noexcept
{
    if (dst_tile != tpl_tile)
        std::memcpy(dst_tile, tpl_tile, 0xE0u);

    const std::uint16_t w = glyph.w;
    const std::uint16_t h = glyph.h;
    const std::uint16_t u = glyph.u;
    const std::uint16_t v = glyph.v;
    std::memcpy(dst_tile + 0x08, &w, sizeof(w));
    std::memcpy(dst_tile + 0x0A, &h, sizeof(h));
    std::memcpy(dst_tile + 0x0C, &u, sizeof(u));
    std::memcpy(dst_tile + 0x0E, &v, sizeof(v));

    const float u0 = static_cast<float>(u) * inv_sheet_w;
    const float v0 = static_cast<float>(v) * inv_sheet_h;
    const float u1 = static_cast<float>(u + w) * inv_sheet_w;
    const float v1 = static_cast<float>(v + h) * inv_sheet_h;
    const float corner_u[4] = {u0, u1, u0, u1};
    const float corner_v[4] = {v0, v0, v1, v1};
    for (int c = 0; c < 4; ++c) {
        std::memcpy(dst_tile + 0x50 + c * 0x1C + 0x14, &corner_u[c], sizeof(float));
        std::memcpy(dst_tile + 0x50 + c * 0x1C + 0x18, &corner_v[c], sizeof(float));
    }

    const std::int16_t x1 = static_cast<std::int16_t>(x0 + w);
    const std::int16_t y1 = static_cast<std::int16_t>(y0 + h);
    const std::int16_t xs[4] = {static_cast<std::int16_t>(x0), x1, static_cast<std::int16_t>(x0), x1};
    const std::int16_t ys[4] = {static_cast<std::int16_t>(y0), static_cast<std::int16_t>(y0), y1, y1};
    std::memcpy(dst_tile + 0xC0, xs, sizeof(xs));
    std::memcpy(dst_tile + 0xC8, ys, sizeof(ys));
}

// Synthesizes a native FFXI CYySprite button displaying `new_label` using the original button's
// own pill bars and font sheet template quads (works identically on 256x256 vanilla & HD sheets).
inline bool synthesize_echoes_button_sprite(
    EchoesMenuSpriteBank& bank,
    const std::uint8_t* orig_shape,
    const DecodedButtonSprite& decoded,
    std::string_view new_label) noexcept
{
    if (orig_shape == nullptr || !decoded.is_button || decoded.pill_count == 0 || decoded.tpl_fg_tile == nullptr)
        return false;

    const int text_w = measure_font_text_px(new_label);
    const int pill_w = decoded.pill_right - decoded.pill_left;
    if (text_w <= 0 || pill_w < text_w + 6)
        return false;

    std::size_t non_space = 0;
    for (char c : new_label) {
        if (c != ' ')
            ++non_space;
    }
    const bool has_shadow = (decoded.tpl_sh_tile != nullptr && decoded.tpl_sh_node != nullptr);
    const std::size_t total_tiles = decoded.pill_count + non_space * (has_shadow ? 2u : 1u);
    if (total_tiles > kEchoesMaxSpriteTiles)
        return false;

    // Infer exact font sheet UV scale from the template foreground quad (supports vanilla 256x256 and HD sheets).
    float inv_w = 1.0f / 256.0f;
    float inv_h = 1.0f / 256.0f;
    {
        std::uint16_t tw = 0, th = 0;
        std::memcpy(&tw, decoded.tpl_fg_tile + 0x08, sizeof(tw));
        std::memcpy(&th, decoded.tpl_fg_tile + 0x0A, sizeof(th));
        float tu0 = 0.0f, tu1 = 0.0f, tv0 = 0.0f, tv1 = 0.0f;
        std::memcpy(&tu0, decoded.tpl_fg_tile + 0x50 + 0 * 0x1C + 0x14, sizeof(float));
        std::memcpy(&tu1, decoded.tpl_fg_tile + 0x50 + 1 * 0x1C + 0x14, sizeof(float));
        std::memcpy(&tv0, decoded.tpl_fg_tile + 0x50 + 0 * 0x1C + 0x18, sizeof(float));
        std::memcpy(&tv1, decoded.tpl_fg_tile + 0x50 + 2 * 0x1C + 0x18, sizeof(float));
        if (tw > 0 && tu1 > tu0)
            inv_w = (tu1 - tu0) / static_cast<float>(tw);
        if (th > 0 && tv1 > tv0)
            inv_h = (tv1 - tv0) / static_cast<float>(th);
    }

    std::memset(&bank, 0, sizeof(bank));
    std::memcpy(bank.sprite, orig_shape, sizeof(bank.sprite));

    // Preserve button pill tiles verbatim.
    std::size_t out_idx = 0;
    for (std::size_t p = 0; p < decoded.pill_count; ++p) {
        std::memcpy(bank.nodes[out_idx], decoded.pill_nodes[p], sizeof(bank.nodes[out_idx]));
        std::memcpy(bank.tiles[out_idx], decoded.pill_tiles[p], sizeof(bank.tiles[out_idx]));
        ++out_idx;
    }

    // Clamp horizontal start so text never clips past the pill's right edge (with shadow headroom).
    int start_x = decoded.fg_left;
    const int min_x = decoded.pill_left + 4;
    const int max_x = decoded.pill_right - 4 - text_w - (has_shadow ? decoded.shadow_dx : 0);
    if (start_x > max_x)
        start_x = std::max(min_x, max_x);
    if (start_x < min_x)
        start_x = min_x;

    // Emit shadow tiles first (FFXi draws shadow pass before foreground pass).
    if (has_shadow) {
        int cur_x = start_x;
        for (char c : new_label) {
            if (c == ' ') {
                cur_x += kEchoesSpaceAdvancePx;
                continue;
            }
            const EchoesFontGlyph* g = find_echoes_glyph_by_char(c);
            if (g == nullptr)
                return false;
            const int fg_y0 = decoded.base_y - (static_cast<int>(g->row) + 10 - static_cast<int>(g->v));
            std::memcpy(bank.nodes[out_idx], decoded.tpl_sh_node, sizeof(bank.nodes[out_idx]));
            patch_echoes_letter_quad(
                bank.tiles[out_idx], decoded.tpl_sh_tile, *g,
                cur_x + decoded.shadow_dx, fg_y0 + decoded.shadow_dy, inv_w, inv_h);
            ++out_idx;
            cur_x += static_cast<int>(g->w);
        }
    }

    // Emit foreground letter tiles.
    {
        int cur_x = start_x;
        for (char c : new_label) {
            if (c == ' ') {
                cur_x += kEchoesSpaceAdvancePx;
                continue;
            }
            const EchoesFontGlyph* g = find_echoes_glyph_by_char(c);
            if (g == nullptr)
                return false;
            const int fg_y0 = decoded.base_y - (static_cast<int>(g->row) + 10 - static_cast<int>(g->v));
            std::memcpy(bank.nodes[out_idx], decoded.tpl_fg_node, sizeof(bank.nodes[out_idx]));
            patch_echoes_letter_quad(
                bank.tiles[out_idx], decoded.tpl_fg_tile, *g, cur_x, fg_y0, inv_w, inv_h);
            ++out_idx;
            cur_x += static_cast<int>(g->w);
        }
    }

    // Link CYyListNode forward/backward pointers cleanly.
    for (std::size_t i = 0; i < out_idx; ++i) {
        std::uint8_t* next_ptr = (i + 1u < out_idx) ? bank.nodes[i + 1u] : nullptr;
        std::uint8_t* prev_ptr = (i > 0u) ? bank.nodes[i - 1u] : nullptr;
        std::uint8_t* tile_ptr = bank.tiles[i];
        std::memcpy(bank.nodes[i] + 0x00, &next_ptr, sizeof(next_ptr));
        std::memcpy(bank.nodes[i] + 0x04, &prev_ptr, sizeof(prev_ptr));
        std::memcpy(bank.nodes[i] + 0x10, &tile_ptr, sizeof(tile_ptr));
        bank.nodes[i][0x14] = 0;
    }

    std::uint8_t* head_ptr = bank.nodes[0];
    std::uint8_t* tail_ptr = bank.nodes[out_idx - 1u];
    std::uint8_t* null_ptr = nullptr;
    const std::uint32_t tile_count = static_cast<std::uint32_t>(out_idx);
    std::memcpy(bank.sprite + 0x04, &head_ptr, sizeof(head_ptr));
    std::memcpy(bank.sprite + 0x08, &tail_ptr, sizeof(tail_ptr));
    std::memcpy(bank.sprite + 0x0C, &null_ptr, sizeof(null_ptr));
    std::memcpy(bank.sprite + 0x10, &null_ptr, sizeof(null_ptr));
    std::memcpy(bank.sprite + 0x14, &tile_count, sizeof(tile_count));

    bank.orig_shape = orig_shape;
    std::memcpy(bank.orig_hdr, orig_shape, sizeof(bank.orig_hdr));
    std::memcpy(bank.orig_tile0_hdr, decoded.pill_tiles[0], sizeof(bank.orig_tile0_hdr));
    bank.active = true;
    return true;
}

// Runtime cached rebrander invoked from buttons.cpp (YmMenuShape::Draw) with zero new hooks.
void* rebrand_menu_shape(void* shape) noexcept;

} // namespace vanacore::soulspire
