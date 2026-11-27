#include "labeltext.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cwctype>
#include <string>

#include "../gfx/vanacore_gfx.h"
#include "chatfont.hpp"
#include "gameui.hpp"
#include "soulspire/echoes_rebrand.hpp"
#include "soulspire/state.hpp"
#include "text.hpp"
#include "textcache.hpp"
#include "uispace.hpp"

namespace vanacore::labeltext {
namespace {

// The windows taken, by their layout's own name's first letters (after "menu    "): what the Config list opens (its input,
// FUN_10194EC0, read live 2026-11-12: conf2win, conf3win, conf4, conf6win, conf7, conf12wi, conf13wi, conftxtc, cfilter,
// fxfilter) and what those open (conf5win, conf5w1, conf5w2, conf11l, conf11s, textcol1 to 3, confyn). The Config list
// ("configwi") and its Windows and Log lists ("conf5m", "conf11m") are the main menu's windows: left out below.
constexpr const char* kWindows[] = {"conf", "cfilter ", "fxfilter", "textcol"};
constexpr std::ptrdiff_t kLayoutName = 0x46; // a layout's name, 16 letters
constexpr std::ptrdiff_t kRect = 0x3A;       // a window's left, top, right and bottom (shorts, the interface's units)

// A shape (buttons.cpp's): its quads' list (+4: the first node, a node's next at +0, its quad at +10h) and their count (+14h).
// A quad (jplabels.cpp's): its picture's width and height (+8, +0Ah) and place (+0Ch, +0Eh) on its texture, texels; its
// blending's A (+2Eh: 2, taken away: a label's shadow copy); its texture's name (+32h, 16 letters); its first corner's colour
// (+60h: B G R A, 80h full, doubled as it's drawn); its corners in the shape's units, x shorts at +C0h and y at +C8h.
// Read live 2026-11-12 ("menu    conf12wi", Misc. 3): the frame's shape is 154 quads, its background ("menu    newtex"), the
// title's bar ("menu    gauge") and letters, then for each setting a bullet, its name's letters and its options' wells
// ("menu    buttonto"); a name's letters stand on one line (each sprite's top plus its own baseline), each starting where the
// last ends (within a unit either way), a space 4 units; no shadow copies. An option's shape (88 x 16): its two bars, its
// label's shadow (A 2, alpha 31h, 2 units right and down of the letters), then its letters (alpha C0h).
constexpr std::ptrdiff_t kList = 4, kCount = 0x14, kNodeQuad = 0x10;
constexpr std::ptrdiff_t kSize = 0x08, kPlace = 0x0C, kBlendA = 0x2E, kName = 0x32, kTint = 0x60, kOwnX = 0xC0, kOwnY = 0xC8;
constexpr char kLetterSheet[] = "font    font    ", kBar[] = "menu    button";
constexpr int kTakenAway = 2;
constexpr int kQuadsMost = 2048, kLettersMost = 768, kLabelsMost = 128, kDepthMost = 4;
constexpr std::size_t kTextSize = 48; // a label's letters kept ("Furniture Camera Collision" is 26)
constexpr int kSpace = 3;             // a gap this wide between a label's letters is a space (gamewin.cpp's)
constexpr int kJoin = 8;              // ... and one wider than this another label on the line ("Min" and "Max")
constexpr int kOverlap = 4;           // a letter may start this far left of the last's end (a "y" under the next letter)
constexpr int kUnderMost = 4;         // a label's shadow copy stands no further than this right and down of it
constexpr int kEdge = 2;              // a label may pass its window's edge by this and still be taken

struct Letter {
    const char* quad;
    short left, right, top, bottom; // its own corners' box, the shape's units
    short base;                     // what it stands on
    char c;                         // 0: not a label's letter, or not drawn a texel to a unit
    bool shadow;
    std::uint32_t tint; // A R G B
    short label;        // its label in the frame's, -1 none
};
enum State : std::uint8_t { kUndecided, kTaken, kLeft };
struct Label {
    char text[kTextSize];
    std::uint8_t n;                // its letters (and spaces)
    short left, right, top, bottom, base;
    std::uint32_t tint;
    bool shadow, ok;               // ok: every letter of it known, and room for them
    short of;                      // a shadow copy's label, -1 none found
    short under_x, under_y;        // a label's shadow copy: how far right and down of it,
    std::uint8_t under;            // ... and its alpha (0: it has none)
    State state;                   // this draw: taken (drawn whole) or left to the game
    bool kept;
};
// The shape being drawn's letters and labels (a shape drawn inside another's draw: its own)
struct Frame {
    const void* window;
    short rect[4];
    bool button; // bars in it: its labels are the buttons'
    Letter letters[kLettersMost];
    int count;
    Label labels[kLabelsMost];
    int label_count;
    int next; // the letter after the last one found (they're drawn in the shape's order)
};
Frame g_frames[kDepthMost];
int g_depth = 0;

// The window being drawn, if it's one taken
struct Now {
    const void* window;
    short rect[4];
};
Now g_now = {};

struct Text {
    const void* window;
    int area;             // VANA_FONT_BUTTONS or VANA_FONT_TITLES
    int x, base;          // its pen's start and its baseline, the interface's units
    std::uint32_t colour; // as drawn: A R G B, FFh full
    short under_x, under_y;
    std::uint8_t under; // its shadow copy's alpha as drawn, 0 none
    char text[kTextSize];
};
constexpr int kTextsMost = 256;
Text g_texts[kTextsMost] = {}; // this frame's, kept as the game drew its windows (draw takes them)
int g_count = 0;
DWORD g_when = 0;             // GetTickCount as a window taken was last drawn
constexpr DWORD kStale = 250; // texts older than this were never drawn (the interface hidden): dropped

text::SpriteCache<128> g_sprites; // the texts' pictures, each made once and kept while it's drawn (textcache.hpp)

template <typename T> T at(const void* base, std::ptrdiff_t offset)
{
    T value;
    std::memcpy(&value, static_cast<const char*>(base) + offset, sizeof value);
    return value;
}

bool read_rect(const void* window, short (&rect)[4])
{
    __try {
        std::memcpy(rect, static_cast<const char*>(window) + kRect, sizeof rect);
        return rect[2] > rect[0] && rect[3] > rect[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The texts kept for `window` at a draw before (it's drawn again), and another window's that start under it: let go
void forget(const void* window, const short (&rect)[4])
{
    int n = 0;
    for (int i = 0; i < g_count; ++i) {
        const Text& t = g_texts[i];
        const bool under = t.x >= rect[0] && t.x < rect[2] && t.base >= rect[1] && t.base < rect[3];
        if (t.window != window && !under)
            g_texts[n++] = t;
    }
    g_count = n;
}

void read_letter(const char* quad, Letter& l)
{
    short xs[4], ys[4];
    std::memcpy(xs, quad + kOwnX, sizeof xs);
    std::memcpy(ys, quad + kOwnY, sizeof ys);
    l.quad = quad;
    l.left = *std::min_element(xs, xs + 4), l.right = *std::max_element(xs, xs + 4);
    l.top = *std::min_element(ys, ys + 4), l.bottom = *std::max_element(ys, ys + 4);
    l.shadow = at<std::uint8_t>(quad, kBlendA) == kTakenAway;
    l.tint = at<std::uint32_t>(quad, kTint);
    l.label = -1;
    int base = 0;
    const bool known = chatfont::label_letter(at<std::uint16_t>(quad, kPlace), at<std::uint16_t>(quad, kPlace + 2), l.c, base) &&
                       l.right - l.left == at<std::uint16_t>(quad, kSize) && l.bottom - l.top == at<std::uint16_t>(quad, kSize + 2);
    if (!known)
        l.c = 0;
    l.base = static_cast<short>(l.top + base);
}

// `shape`'s letters (the label sheet's quads) and whether it has a button's bars; false: it couldn't be read, or it's past
// what's kept (nothing of it is taken then)
bool collect(Frame& f, const char* shape)
{
    __try {
        const int count = at<int>(shape, kCount);
        const char* node = at<const char*>(shape, kList);
        if (count <= 0 || count > kQuadsMost)
            return false;
        for (int i = 0; i < count && node != nullptr; ++i, node = at<const char*>(node, 0)) {
            const char* quad = at<const char*>(node, kNodeQuad);
            if (quad == nullptr)
                continue;
            if (std::memcmp(quad + kName, kBar, sizeof kBar - 1) == 0)
                f.button = true;
            if (std::memcmp(quad + kName, kLetterSheet, sizeof kLetterSheet - 1) != 0)
                continue;
            if (f.count == kLettersMost)
                return false;
            read_letter(quad, f.letters[f.count++]);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Letter b goes on after a: of its pass, on its line, in its colour, beside it
bool joined(const Letter& a, const Letter& b)
{
    const int gap = b.left - a.right;
    return a.shadow == b.shadow && a.base == b.base && a.tint == b.tint && gap >= -kOverlap && gap <= kJoin;
}

int begin_label(Frame& f, const Letter& first)
{
    Label& label = f.labels[f.label_count];
    label = {};
    label.left = label.right = first.left, label.top = first.top, label.bottom = first.bottom, label.base = first.base;
    label.tint = first.tint, label.shadow = first.shadow, label.ok = true, label.of = -1;
    return f.label_count++;
}

void add_letter(Label& label, const Letter& l)
{
    const auto push = [&label](char c) {
        if (c == 0 || label.n + 1u >= kTextSize)
            label.ok = false;
        else
            label.text[label.n++] = c;
    };
    if (label.n > 0 && l.left - label.right >= kSpace)
        push(' ');
    push(l.c);
    label.right = std::max(label.right, l.right);
    label.top = std::min(label.top, l.top), label.bottom = std::max(label.bottom, l.bottom);
}

// The frame's letters put together into labels: each pass's (the letters', then the shadow copies'), line by line, left to right
void group(Frame& f)
{
    static short order[kLettersMost];
    for (int i = 0; i < f.count; ++i)
        order[i] = static_cast<short>(i);
    std::sort(order, order + f.count, [&f](short a, short b) {
        const Letter &x = f.letters[a], &y = f.letters[b];
        if (x.shadow != y.shadow)
            return y.shadow;
        if (x.base != y.base)
            return x.base < y.base;
        return x.left != y.left ? x.left < y.left : a < b;
    });
    int current = -1;
    const Letter* last = nullptr;
    for (int i = 0; i < f.count; ++i) {
        Letter& l = f.letters[order[i]];
        if (last == nullptr || !joined(*last, l))
            current = f.label_count < kLabelsMost ? begin_label(f, l) : -1;
        if (current >= 0)
            add_letter(f.labels[current], l);
        l.label = static_cast<short>(current);
        last = &l;
    }
}

// Shadow copy `shadow` is label `main`'s: its letters, a little right and down of it
bool shadow_of(const Label& shadow, const Label& main)
{
    const int dx = shadow.left - main.left, dy = shadow.base - main.base;
    return !main.shadow && main.ok && main.under == 0 && dx >= 0 && dx <= kUnderMost && dy >= 0 && dy <= kUnderMost &&
           std::strcmp(shadow.text, main.text) == 0;
}

// Each shadow copy told which label it's under (it's left out with it, and the text gets one of its own)
void pair_shadows(Frame& f)
{
    for (int s = 0; s < f.label_count; ++s) {
        Label& shadow = f.labels[s];
        if (!shadow.shadow || !shadow.ok)
            continue;
        for (int m = 0; m < f.label_count && shadow.of < 0; ++m) {
            Label& main = f.labels[m];
            if (!shadow_of(shadow, main))
                continue;
            shadow.of = static_cast<short>(m);
            main.under_x = static_cast<short>(shadow.left - main.left), main.under_y = static_cast<short>(shadow.base - main.base);
            main.under = static_cast<std::uint8_t>(std::max<std::uint32_t>(1, shadow.tint >> 24));
        }
    }
}

Letter* find(Frame& f, const char* quad)
{
    for (int i = 0; i < f.count; ++i) {
        const int at = (f.next + i) % f.count;
        if (f.letters[at].quad == quad) {
            f.next = at + 1;
            return &f.letters[at];
        }
    }
    return nullptr;
}

int area_of(const Frame& f)
{
    return f.button ? VANA_FONT_BUTTONS : VANA_FONT_TITLES; // (as d3d8_chatfont.h takes a label's letter in a General window)
}

// Whether `label`, its shape drawn at x, y at a scale, is drawn whole: at the game's own size, its font True Font's, all of it
// inside its window (what's past the window the game cuts, and VanaCore's text isn't cut: the game's letters there)
bool takes(const Frame& f, const Label& label, int x, int y, float sx, float sy)
{
    if (std::fabs(sx - 1.0f) > 0.001f || std::fabs(sy - 1.0f) > 0.001f || g_count == kTextsMost || !chatfont::area_on(area_of(f)))
        return false;
    return x + label.left >= f.rect[0] - kEdge && x + label.right <= f.rect[2] + kEdge && y + label.top >= f.rect[1] - kEdge &&
           y + label.bottom <= f.rect[3] + kEdge;
}

// A quad's colour as it's drawn in `colour`: each channel the two multiplied, 80h full in both, doubled
std::uint32_t drawn_colour(std::uint32_t tint, std::uint32_t colour)
{
    std::uint32_t out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= std::min<std::uint32_t>(255, (tint >> shift & 0xFF) * (colour >> shift & 0xFF) / 64) << shift;
    return out;
}

void keep(const Frame& f, const Label& label, int x, int y, std::uint32_t colour)
{
    Text& t = g_texts[g_count++]; // (takes saw there's room)
    t.window = f.window, t.area = area_of(f);
    t.x = x + label.left, t.base = y + label.base;
    t.colour = drawn_colour(label.tint, colour);
    t.under_x = label.under_x, t.under_y = label.under_y;
    t.under = static_cast<std::uint8_t>(std::min<std::uint32_t>(255, label.under * (colour >> 24) / 64));
    if (soulspire::engine().enabled()) {
        const std::string rebranded = soulspire::rebrand_echoes_text(std::string_view(label.text, label.n));
        const std::size_t copy_n = std::min(rebranded.size(), sizeof(t.text) - 1u);
        std::memcpy(t.text, rebranded.data(), copy_n);
        t.text[copy_n] = '\0';
    } else {
        std::memcpy(t.text, label.text, sizeof t.text);
    }
}

// A text's look: its area's True Font as built with its outline in the same picture, in the colour the game drew its letters
// in. False while the area's letters are left out (its size 0)
bool style_of(const Text& t, text::Style& style, bool& upper)
{
    text::Style letters, stroked;
    float opacity = 0.0f;
    if (!chatfont::area_style(t.area, letters, stroked, upper, opacity))
        return false;
    style = letters;
    for (int i = 0; i < 3; ++i)
        style.color[i] = static_cast<float>(t.colour >> (16 - 8 * i) & 0xFF);
    style.color[3] = 1.0f;
    style.stroke = stroked.stroke, style.exact_stroke = stroked.exact_stroke;
    style.stroke_color[3] = stroked.stroke > 0 ? opacity : 0.0f;
    style.background[3] = 0.0f;
    return true;
}

void draw_sprite(void* device, const std::wstring& text, const text::Style& style, int x, int y, float opacity)
{
    text::Sprite& sprite = g_sprites.get(text, style);
    if (sprite.upload(device))
        sprite.draw(device, x - sprite.pen_left(), y - sprite.baseline(), opacity);
}

// One text from its pen's start on its baseline, as see-through as the game drew its letters; under a font with no outline
// the game's shadow, a black copy where the game's stood (an outline stands in for it, as for the letters kept one by one)
void draw_text(void* device, unsigned width, unsigned height, const Text& t)
{
    text::Style style;
    bool upper = false;
    float px = 0.0f, py = 0.0f, scale = 0.0f;
    if (!style_of(t, style, upper) ||
        !uispace::to_pixels(static_cast<float>(t.x), static_cast<float>(t.base), width, height, px, py, scale))
        return;
    std::wstring shown(t.text, t.text + std::strlen(t.text));
    if (upper)
        for (wchar_t& c : shown)
            c = static_cast<wchar_t>(std::towupper(c));
    const int x = static_cast<int>(std::lround(px)), y = static_cast<int>(std::lround(py));
    if (t.under != 0 && style.stroke == 0) {
        text::Style dark = style;
        dark.color[0] = dark.color[1] = dark.color[2] = 0.0f;
        draw_sprite(device, shown, dark, x + static_cast<int>(std::lround(static_cast<float>(t.under_x) * scale)),
                    y + static_cast<int>(std::lround(static_cast<float>(t.under_y) * scale)), static_cast<float>(t.under) / 255.0f);
    }
    draw_sprite(device, shown, style, x, y, static_cast<float>(t.colour >> 24) / 255.0f);
}

} // namespace

bool config_window(const char* layout)
{
    const char* name = layout + kLayoutName;
    if (std::memcmp(name, "menu    ", 8) != 0)
        return false;
    for (const char* window : kWindows)
        if (std::strncmp(name + 8, window, std::strlen(window)) == 0)
            return !chatfont::main_menu_window(layout) && !chatfont::action_menu_window(layout);
    return false;
}

Window::Window(const void* window, const char* layout) : was_(g_now.window)
{
    std::memcpy(was_rect_, g_now.rect, sizeof was_rect_);
    g_now = {};
    if (window == nullptr || layout == nullptr || !chatfont::labels_on() || !config_window(layout) || gameui::veiled(window) ||
        !read_rect(window, g_now.rect))
        return;
    g_now.window = window;
    forget(window, g_now.rect);
    g_when = GetTickCount();
}

Window::~Window()
{
    g_now.window = was_;
    std::memcpy(g_now.rect, was_rect_, sizeof was_rect_);
}

Shape::Shape(const void* shape) : pushed_(false)
{
    if (g_now.window == nullptr || shape == nullptr || g_depth == kDepthMost)
        return;
    Frame& f = g_frames[g_depth++];
    pushed_ = true;
    f.window = g_now.window, f.button = false, f.count = 0, f.label_count = 0, f.next = 0;
    std::memcpy(f.rect, g_now.rect, sizeof f.rect);
    if (!collect(f, static_cast<const char*>(shape))) {
        f.count = 0;
        return;
    }
    group(f);
    pair_shadows(f);
}

Shape::~Shape()
{
    if (pushed_ && g_depth > 0)
        --g_depth;
}

bool letter(const char* quad, int x, int y, float sx, float sy, std::uint32_t colour)
{
    if (g_depth == 0)
        return false;
    Frame& f = g_frames[g_depth - 1];
    const Letter* l = find(f, quad);
    if (l == nullptr || l->label < 0)
        return false;
    const Label& own = f.labels[l->label];
    if (!own.ok || (own.shadow && own.of < 0))
        return false; // (a sprite not known in it, or a shadow copy of no label taken: the game's)
    Label& label = own.shadow ? f.labels[own.of] : f.labels[l->label];
    if (label.state == kUndecided)
        label.state = takes(f, label, x, y, sx, sy) ? kTaken : kLeft;
    if (label.state != kTaken)
        return false;
    if (!own.shadow && !label.kept) { // (at a letter, not its shadow copy: buttons.cpp gives the letters their colour)
        label.kept = true;
        keep(f, label, x, y, colour);
    }
    return true;
}

bool wants_quads()
{
    return chatfont::labels_on();
}

void draw(void* device, unsigned width, unsigned height)
{
    const int count = g_count;
    g_count = 0;
    if (count == 0 || GetTickCount() - g_when > kStale)
        return;
    g_sprites.begin(device);
    for (int i = 0; i < count; ++i)
        draw_text(device, width, height, g_texts[i]);
}

} // namespace vanacore::labeltext
