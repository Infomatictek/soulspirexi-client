#include "overlay.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <vector>

#include "abtest.hpp"
#include "zoneload.hpp"
#include "addons.hpp"
#include "alliancelist.hpp"
#include "castbar.hpp"
#include "hudwin.hpp"
#include "tpbar.hpp"
#include "partylist.hpp"
#include "itemtips.hpp"
#include "tipicons.hpp"
#include "winimage.hpp"
#include "noblink.hpp"
#include "fishing.hpp"
#include "autologin.hpp"
#include "chatpages.hpp"
#include "macrobar.hpp"
#include "labeltext.hpp"
#include "listtext.hpp"
#include "macrotext.hpp"
#include "menuhover.hpp"
#include "binds.hpp"
#include "camera.hpp"
#include "chat.hpp"
#include "chatfont.hpp"
#include "chattabs.hpp"
#include "commands.hpp"
#include "clients.hpp"
#include "configmenu.hpp"
#include "gamesettings.hpp"
#include "singlemenu.hpp"
#include "sliders.hpp"
#include "soundslots.hpp"
#include "equipfirst.hpp"
#include "namechars.hpp"
#include "menulog.hpp"
#include "partymenu.hpp"
#include "d3d8.hpp"
#include "damagetext.hpp"
#include "discord.hpp"
#include "entityhp.hpp"
#include "maxhp_ask.hpp"
#include "framerate.hpp"
#include "frametime.hpp"
#include "game.hpp"
#include "framepacing.hpp"
#include "game_config.hpp"
#include "gamewin.hpp"
#include "graphics.hpp"
#include "gearload.hpp"
#include "backdrop.hpp"
#include "gui.hpp"
#include "hideui.hpp"
#include "imports.hpp"
#include "itemcount.hpp"
#include "itemlist.hpp"
#include "spelllist.hpp"
#include "itemrarity.hpp"
#include "itemslot.hpp"
#include "log.hpp"
#include "mcp.hpp"
#include "helpbar.hpp"
#include "targetwin.hpp"
#include "mapwin.hpp"
#include "buffwin.hpp"
#include "buttons.hpp"
#include "netstat.hpp"
#include "statuswindow.hpp"
#include "profilewin.hpp"
#include "equipstats.hpp"
#include "packetflow.hpp"
#include "packetwatch.hpp"
#include "packets.hpp"
#include "perf.hpp"
#include "phraselist.hpp"
#include "raise.hpp"
#include "hwmon.hpp"
#include "stats.hpp"
#include "plugins.hpp"
#include "screenshot.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "soulspire/soulspire.hpp"
#include "text.hpp"
#include "ui.hpp"
#include "widgets.hpp"
#include "uiscale.hpp"
#include "uispace.hpp"
#include "window.hpp"

namespace vanacore::overlay {
namespace {

using d3d8::call;
namespace slot = d3d8::slot;

constexpr int kMargin = 8; // distance from the screen's edges, in back buffer pixels
constexpr int kGap = 4;    // between the label and an FPS counter under it
// The label shows when the game starts and then goes: it slides in from the left as it fades in,
// stays, and slides back out as it fades (seconds).
constexpr double kLabelIn = 0.4;
constexpr double kLabelStay = 5.0;
constexpr double kLabelOut = 0.6;
constexpr int kLogoInset = 2;         // between the logo and its box's edges (the label's box, widened)
constexpr ULONGLONG kTitleCheck = 1000; // how often the title looks for the character (ms)

// [client] font.family, font.size, font.color, font.outline, font.background: how the label and
// FPS counter look. Their defaults (a key not set, or taken out): Consolas, 20 pixels, gold (#FFD700),
// no box.
text::Style g_style;
constexpr wchar_t kDefaultFamily[] = L"Consolas";
constexpr int kDefaultSize = 20;

std::wstring g_title;
std::wstring g_logo_path;
std::wstring g_icon_file; // VanaCore.exe: its icon is the window's
bool g_show_fps = true; // [client] fps.display
int g_fps_corner = 1;    // [client] fps.position: 0 top-left, 1 top-right (the default), 2 bottom-right, 3 bottom-left
int g_fps_padding = 0;   // [client] fps.padding: from the counter's numbers to the corner; -1 (auto): as the label's box, kMargin in

// The FPS counter's look: the label's; with fps.padding, its box cut to the numbers with that padding
// round them (and no margin from the corner, fps_position).
text::Style fps_style()
{
    text::Style style = g_style;
    style.padding = g_fps_padding;
    style.trim = g_fps_padding >= 0;
    return style;
}

// vanacore.ini, looked at twice a second (on the render thread): the settings above, and fps.max,
// apply again when it changes.
std::wstring g_ini;
bool g_exclusive = false; // a full-screen device on Windows' own Direct3D 8: the window settings leave it alone
FILETIME g_ini_time{};
std::atomic<ULONGLONG> g_next_check{0};
std::atomic<ULONGLONG> g_reloaded{0}; // when check_settings last read them again (the file had changed)
constexpr ULONGLONG kChangedEvery = 100; // settings changed in game are read again at most this often (ms)

using Direct3DCreate8 = void*(WINAPI*)(UINT sdk_version);
using CreateDevice = HRESULT(__stdcall*)(void* d3d, UINT adapter, DWORD type, HWND focus, DWORD flags,
                                         d3d8::PresentParameters* params, void** device);
using Present = HRESULT(__stdcall*)(void* device, const RECT* source, const RECT* dest, HWND window,
                                    const RGNDATA* dirty);
using Reset = HRESULT(__stdcall*)(void* device, d3d8::PresentParameters* params);

using Scene = HRESULT(__stdcall*)(void* device);

Direct3DCreate8 g_direct3d_create = nullptr;
CreateDevice g_create_device = nullptr;
Present g_present = nullptr;
Reset g_reset = nullptr;
Scene g_begin_scene = nullptr;
Scene g_end_scene = nullptr;
// True inside Present's hook: the scenes VanaCore and the plugins begin there aren't the game's, and aren't told to the plugins
bool g_in_present = false;

d3d8::SurfaceDesc back_buffer_size(void* device);
void begin_gui_frame(void* device, const d3d8::SurfaceDesc& desc);

// Whether the scene being drawn is on the back buffer (the game's own picture), not a shadow map's or the like
bool on_back_buffer(void* device)
{
    void* target = nullptr;
    void* back_buffer = nullptr;
    bool same = false;
    if (SUCCEEDED(d3d8::call(device, slot::GetRenderTarget, &target)) && target != nullptr) {
        if (SUCCEEDED(d3d8::call(device, slot::GetBackBuffer, 0u, d3d8::kBackBufferMono, &back_buffer)) && back_buffer != nullptr) {
            same = target == back_buffer;
            d3d8::call<ULONG>(back_buffer, slot::Release);
        }
        d3d8::call<ULONG>(target, slot::Release);
    }
    return same;
}

// The game's scenes for plugins (Direct3DBeginScene, Direct3DEndScene) and addons (Ashita's d3d_beginscene and
// d3d_endscene) as they happen. Addons draw in them as Ashita's do: ImGui (its frame is opened at the first scene on
// the back buffer, ahead of Present, where it's drawn), and at the scene's end their sprites and fonts in the game's
// pixels, set up as in Present (the whole back buffer, VanaCore's own drawing for [ui] aspect) and the game's render
// state put back after.
HRESULT __stdcall begin_scene(void* device)
{
    const HRESULT result = g_begin_scene(device);
    if (SUCCEEDED(result) && !g_in_present) {
        plugins::on_scene(device, true);
        if (addons::wants_scenes()) {
            const bool back = on_back_buffer(device);
            if (back)
                begin_gui_frame(device, back_buffer_size(device));
            addons::on_scene(true, back);
        }
    }
    return result;
}

HRESULT __stdcall end_scene(void* device)
{
    if (!g_in_present) {
        // the game's windows that go on top (the auto-translate list, those over fancychat's chat) as it drew them,
        // before plugins and addons draw over them (laid back in Present)
        if (raise::pending() && on_back_buffer(device))
            raise::take(device);
        plugins::on_scene(device, false);
        if (addons::wants_scenes()) {
            const bool back = on_back_buffer(device);
            DWORD state = 0;
            const d3d8::SurfaceDesc desc = back_buffer_size(device);
            const bool prepared = back && text::prepare(device, desc.width, desc.height, state);
            if (prepared)
                graphics::own_drawing(true);
            addons::on_scene(false, back);
            if (prepared) {
                graphics::own_drawing(false);
                text::restore(device, state);
            }
        }
    }
    return g_end_scene(device);
}

// The game's draw calls, for addons' d3d_dp, d3d_dip, d3d_dpup and d3d_dipup (only while one listens): a draw one blocks
// isn't made. What VanaCore and the plugins draw in Present isn't the game's, and isn't told.
using DrawPrimitive = HRESULT(__stdcall*)(void* device, DWORD type, UINT start_vertex, UINT count);
using DrawIndexedPrimitive = HRESULT(__stdcall*)(void* device, DWORD type, UINT min_index, UINT num_vertices, UINT start_index,
                                                 UINT count);
using DrawPrimitiveUP = HRESULT(__stdcall*)(void* device, DWORD type, UINT count, const void* vertices, UINT stride);
using DrawIndexedPrimitiveUP = HRESULT(__stdcall*)(void* device, DWORD type, UINT min_index, UINT num_vertices, UINT count,
                                                   const void* indices, DWORD index_format, const void* vertices, UINT stride);
DrawPrimitive g_draw_primitive = nullptr;
DrawIndexedPrimitive g_draw_indexed_primitive = nullptr;
DrawPrimitiveUP g_draw_primitive_up = nullptr;
DrawIndexedPrimitiveUP g_draw_indexed_primitive_up = nullptr;

HRESULT __stdcall draw_primitive(void* device, DWORD type, UINT start_vertex, UINT count)
{
    if (!g_in_present && addons::wants_draws() && addons::on_draw_primitive(type, start_vertex, count))
        return S_OK;
    return g_draw_primitive(device, type, start_vertex, count);
}

HRESULT __stdcall draw_indexed_primitive(void* device, DWORD type, UINT min_index, UINT num_vertices, UINT start_index, UINT count)
{
    if (!g_in_present && addons::wants_draws() &&
        addons::on_draw_indexed_primitive(type, min_index, num_vertices, start_index, count))
        return S_OK;
    return g_draw_indexed_primitive(device, type, min_index, num_vertices, start_index, count);
}

HRESULT __stdcall draw_primitive_up(void* device, DWORD type, UINT count, const void* vertices, UINT stride)
{
    if (!g_in_present && addons::wants_draws() && addons::on_draw_primitive_up(type, count, vertices, stride))
        return S_OK;
    return g_draw_primitive_up(device, type, count, vertices, stride);
}

HRESULT __stdcall draw_indexed_primitive_up(void* device, DWORD type, UINT min_index, UINT num_vertices, UINT count,
                                            const void* indices, DWORD index_format, const void* vertices, UINT stride)
{
    if (!g_in_present && addons::wants_draws() &&
        addons::on_draw_indexed_primitive_up(type, min_index, num_vertices, count, indices, index_format, vertices, stride))
        return S_OK;
    return g_draw_indexed_primitive_up(device, type, min_index, num_vertices, count, indices, index_format, vertices, stride);
}

struct PresentScope {
    PresentScope() { g_in_present = true; }
    ~PresentScope() { g_in_present = false; }
};

// Only touched from the game's render thread (Present).
text::Sprite g_label;
text::Sprite g_logo;  // left of the label, in its box; empty without a logo
text::Sprite g_fps;
ULONGLONG g_next_title = 0;
void* g_device = nullptr; // device the textures belong to
bool g_failed = false;
bool g_drawn = false;
LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_window_start{}; // FPS is counted over 1s windows
int g_frames = 0;
LARGE_INTEGER g_label_start{}; // the label's first frame (0 before it)
double g_label_shown = 1.0;    // how far in the label is: 1 at rest, 0 gone (an FPS counter under it follows)

// The toast at the top of the screen, in the middle, while the picture waits on something being made (graphics::waiting):
// a dark rounded panel that slides down from the top edge with a title ("Compiling shaders", "Starting DLSS"), under it
// what it's at now ("Ambient occlusion": the shader compiling), the count on the right (12 of 43) and a progress bar along
// the bottom, which sweeps while there's no count. Sized to the screen's height, so it's read at 4K. It comes once a wait
// has lasted kNoticeDelay (a quick one isn't worth it), stays at least kNoticeMin (DLSS 5's start is under half a second),
// slides in and out over kNoticeFade, and gives up after kNoticeMost (DLSS gives up at 30).
constexpr double kNoticeDelay = 0.15;
constexpr double kNoticeMin = 1.2;
constexpr double kNoticeFade = 0.3;
constexpr double kNoticeMost = 30.0;
graphics::Waiting g_notice_want;  // what's waited on now (title "" nothing) ...
LARGE_INTEGER g_notice_since{};   // ... since when (a wait that follows another straight away goes on from the first's)
graphics::Waiting g_notice_words; // the toast's words: the last waited on, kept while it slides out
LARGE_INTEGER g_notice_on{};     // when the toast showing came on (0: none is)
LARGE_INTEGER g_notice_last{};   // the last frame's time
double g_notice_shown = 0.0;     // how far in it is, 0-1
bool g_notice_said = false;      // this wait's toast shown (logged as it ends)
// The toast's parts, drawn in place over the panel: its text set only when it changes (a set re-renders)
struct Toast {
    text::Sprite panel;  // the rounded panel, with the bar's track and the accent strip, rendered for a size
    text::Sprite title;
    text::Sprite detail;
    text::Sprite count;
    text::Sprite bar;    // the bar's fill, rendered for a width
    int width = 0, height = 0; // the panel's size (0: not rendered)
    int pad = 0;               // from its edge to its text
    int gap = 0;               // between its lines
    int strip = 0;             // the accent strip's width, left
    int bar_x = 0, bar_y = 0, bar_w = 0, bar_h = 0; // the bar's track in the panel
    int bar_fill = -1;         // the fill's width now (-1 none rendered)
    unsigned screen = 0;       // the screen height it was laid out for
    bool alert = false;        // laid out for an alert (graphics::Waiting::alert): its strip and bar red
    int least_width = 0;       // the widest it's been this wait: it doesn't jump narrower as the words change
    void release()
    {
        panel.release();
        title.release();
        detail.release();
        count.release();
        bar.release();
    }
} g_toast;

// "DEBUG: God rays" in the top-left corner while the /vana window's Render visual shows one part of the effects alone in
// place of the picture (graphics::render_visual), so a screenshot or video says so: God rays alone is near black wherever
// the sky isn't bright, and only while the rays are on and the sun is in view, so it looks like the rays darkening it.
text::Sprite g_visual;
int g_visual_said = 0; // the Render visual last logged

// "DirectX 12 can't run here: switched to DirectX 11" under the label when renderer=d3d12 couldn't draw this session (its
// shaders don't compile: Wine, Proton; or binding tier 1: a Mac under CrossOver) and Direct3D 11 took its place,
// vanacore.ini switched with it (graphics::switched_to_d3d11). It comes and goes as the label does, and stays long enough to be read.
constexpr double kSwitchStay = 20.0;
text::Sprite g_switch;
bool g_switch_said = false;

// How far in the label is at this frame, 0-1: easing out as it comes in, easing in as it goes. `stay`: how long it stays
// (the DirectX 11 notice, which starts with it, stays longer).
double label_shown(double stay = kLabelStay)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_label_start.QuadPart == 0)
        g_label_start = now;
    const double t = static_cast<double>(now.QuadPart - g_label_start.QuadPart) / static_cast<double>(g_frequency.QuadPart);
    if (t < kLabelIn) {
        const double k = 1.0 - t / kLabelIn;
        return 1.0 - k * k * k;
    }
    if (t < kLabelIn + stay)
        return 1.0;
    const double k = (t - kLabelIn - stay) / kLabelOut;
    return k >= 1.0 ? 0.0 : 1.0 - k * k * k;
}

// The toast's text on a screen `height` tall: the label's font, with no box of its own (the panel is it). The title bold
// in the label's colour, a 60th of the screen's height (36 pixels at 2160, 18 at 1080; never under the label's size); the
// rest smaller, in white, the count dimmer.
text::Style toast_style(unsigned height, int kind) // 0 title, 1 detail, 2 count
{
    text::Style style = g_style;
    const int title = std::max(g_style.size, static_cast<int>(height / 60));
    style.size = kind == 0 ? title : std::max(10, title * 4 / 5);
    style.weight = kind == 0 ? FW_BOLD : FW_NORMAL;
    style.background[3] = 0.0f;
    style.stroke = 0;
    style.padding = 0;
    style.trim = false;
    if (kind != 0) {
        style.color[0] = style.color[1] = style.color[2] = 255.0f;
        style.color[3] = kind == 1 ? 0.85f : 0.6f;
    }
    return style;
}

// An alert's accent (DLSS crashed), in place of the label's colour
constexpr std::uint32_t kAlertArgb = 0xFFE5484Du;

// The label's colour (font.color) as 0xAARRGGBB, `opacity` times as opaque: the toast's accent (its strip and bar).
std::uint32_t accent_argb(float opacity)
{
    const auto channel = [](float value) { return static_cast<std::uint32_t>(std::lround(value < 0 ? 0 : value > 255 ? 255 : value)); };
    return channel(255.0f * opacity) << 24 | channel(g_style.color[0]) << 16 | channel(g_style.color[1]) << 8 | channel(g_style.color[2]);
}

// A rounded rectangle `width` x `height` with corners of `radius`, as straight-alpha ARGB pixels: filled with `fill`
// (0xAARRGGBB), a one-pixel `border` round it, and its left `strip` pixels in `accent` (0 for none), the edges antialiased
// (each pixel's coverage from its distance to the rounded edge).
std::vector<std::uint32_t> rounded_image(int width, int height, float radius, std::uint32_t fill, std::uint32_t border,
                                         int strip, std::uint32_t accent)
{
    std::vector<std::uint32_t> pixels(static_cast<size_t>(width) * height);
    radius = std::min(radius, std::min(width, height) / 2.0f);
    const float hw = width / 2.0f, hh = height / 2.0f;
    // the signed distance from (x, y) to the rounded rectangle inset by `inset`, negative inside
    const auto distance = [&](float x, float y, float inset) {
        const float r = std::max(radius - inset, 0.0f);
        const float dx = std::max(std::fabs(x - hw) - (hw - inset - r), 0.0f);
        const float dy = std::max(std::fabs(y - hh) - (hh - inset - r), 0.0f);
        return std::sqrt(dx * dx + dy * dy) - r;
    };
    const auto coverage = [](float d) { return std::clamp(0.5f - d, 0.0f, 1.0f); };
    const auto blend = [](std::uint32_t argb, float k) { // argb's alpha times k, straight alpha
        const float a = static_cast<float>(argb >> 24) * k;
        return (static_cast<std::uint32_t>(std::lround(std::clamp(a, 0.0f, 255.0f))) << 24) | (argb & 0xFFFFFF);
    };
    const auto over = [](std::uint32_t top, std::uint32_t under) { // top over under, both straight alpha
        const float ta = (top >> 24) / 255.0f, ua = (under >> 24) / 255.0f;
        const float a = ta + ua * (1.0f - ta);
        if (a <= 0.0f)
            return std::uint32_t{0};
        std::uint32_t out = static_cast<std::uint32_t>(std::lround(a * 255.0f)) << 24;
        for (int shift = 0; shift < 24; shift += 8) {
            const float t = ((top >> shift) & 0xFF) * ta, u = ((under >> shift) & 0xFF) * ua * (1.0f - ta);
            out |= static_cast<std::uint32_t>(std::lround(std::clamp((t + u) / a, 0.0f, 255.0f))) << shift;
        }
        return out;
    };
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = x + 0.5f, py = y + 0.5f;
            const float outer = coverage(distance(px, py, 0.0f)), inner = coverage(distance(px, py, 1.0f));
            std::uint32_t pixel = blend(fill, outer);
            if (strip > 0 && x < strip) { // the accent strip, inside the edge, fading over its last pixel
                const float k = std::min(outer, std::clamp(static_cast<float>(strip) - px + 0.5f, 0.0f, 1.0f));
                pixel = over(blend(accent, k), pixel);
            }
            pixel = over(blend(border, std::max(outer - inner, 0.0f)), pixel);
            pixels[static_cast<size_t>(y) * width + x] = pixel;
        }
    }
    return pixels;
}

// The toast laid out for its words on a screen `height` tall: its text set (re-rendered when it changed), the panel
// rendered again when its size changed (the words wider than it was, or the screen another height). The panel: dark,
// a thin lighter edge, the accent strip down its left, and the bar's track (a lighter line) along the bottom. Returns
// false when it has nothing to draw.
bool toast_layout(unsigned height)
{
    const graphics::Waiting& want = g_notice_words;
    if (want.title.empty())
        return false;
    const bool counted = want.total != 0 && !want.alert; // (an alert's done/total is its time left: no count shown)
    g_toast.title.set(want.title, toast_style(height, 0));
    g_toast.detail.set(want.detail.empty() ? L" " : want.detail, toast_style(height, 1));
    if (counted) {
        wchar_t count[48];
        std::swprintf(count, std::size(count), L"%u of %u", want.done, want.total);
        g_toast.count.set(count, toast_style(height, 2));
    }
    const int title = toast_style(height, 0).size;
    const int pad = std::max(6, title * 3 / 5);
    const int gap = std::max(2, title / 4);
    const int strip = std::max(3, title / 6);
    const int bar_h = std::max(3, title / 5);
    const int count_w = counted ? g_toast.count.width() + pad : 0;
    // the words' width, never under about 18 of the title's letters, nor narrower than it was this wait
    int width = std::max({title * 11, g_toast.title.width() + count_w, g_toast.detail.width()}) + 2 * pad + strip;
    if (g_toast.screen != height)
        g_toast.least_width = 0;
    width = std::max(width, g_toast.least_width);
    g_toast.least_width = width;
    const int panel_h = pad + g_toast.title.height() + gap + g_toast.detail.height() + gap + bar_h + pad;
    if (width != g_toast.width || panel_h != g_toast.height || g_toast.screen != height || want.alert != g_toast.alert) {
        g_toast.width = width, g_toast.height = panel_h, g_toast.screen = height, g_toast.alert = want.alert;
        g_toast.pad = pad, g_toast.gap = gap, g_toast.strip = strip;
        g_toast.bar_x = strip + pad, g_toast.bar_w = width - strip - 2 * pad;
        g_toast.bar_y = panel_h - pad - bar_h, g_toast.bar_h = bar_h;
        const float radius = std::max(4.0f, title * 0.45f);
        std::vector<std::uint32_t> pixels = rounded_image(width, panel_h, radius, 0xD2101216u, 0x30FFFFFFu, strip,
                                                           want.alert ? kAlertArgb : accent_argb(1.0f));
        // the bar's track: a rounded line, lighter than the panel, drawn into it
        const std::vector<std::uint32_t> track = rounded_image(g_toast.bar_w, bar_h, bar_h / 2.0f, 0x38FFFFFFu, 0u, 0, 0u);
        for (int y = 0; y < bar_h; ++y)
            for (int x = 0; x < g_toast.bar_w; ++x) {
                const std::uint32_t t = track[static_cast<size_t>(y) * g_toast.bar_w + x];
                std::uint32_t& p = pixels[static_cast<size_t>(g_toast.bar_y + y) * width + g_toast.bar_x + x];
                const float ta = (t >> 24) / 255.0f;
                // the track's grey over the panel's dark: lighten by its alpha (both near-opaque in the end)
                std::uint32_t out = p & 0xFF000000u;
                for (int shift = 0; shift < 24; shift += 8) {
                    const float under = static_cast<float>((p >> shift) & 0xFF), top = static_cast<float>((t >> shift) & 0xFF);
                    out |= static_cast<std::uint32_t>(std::lround(under + (top - under) * ta)) << shift;
                }
                p = out;
            }
        g_toast.panel.set_image(std::move(pixels), width, panel_h);
        g_toast.bar_fill = -1;
    }
    return true;
}

// The toast this frame: how far in it is (0: not drawn; 1 in place), for a screen `height` tall; its words what's waited
// on, laid out (toast_layout).
float toast_shown(unsigned height)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double frequency = static_cast<double>(g_frequency.QuadPart);
    const double step = g_notice_last.QuadPart != 0 ? std::min(static_cast<double>(now.QuadPart - g_notice_last.QuadPart) / frequency, 0.1) : 0.0;
    g_notice_last = now;
    graphics::Waiting want = graphics::waiting();
    if (want.title != g_notice_want.title) {
        if (want.title.empty() && g_notice_said)
            log::write(L"Overlay: \"%ls\" shown, %.1f s", g_notice_want.title.c_str(),
                       static_cast<double>(now.QuadPart - g_notice_since.QuadPart) / frequency);
        if (want.title.empty() || g_notice_want.title.empty())
            g_notice_since = now, g_notice_said = false;
    }
    g_notice_want = std::move(want);
    const double held = static_cast<double>(now.QuadPart - g_notice_since.QuadPart) / frequency;
    const bool waiting = !g_notice_want.title.empty() && held >= kNoticeDelay && held < kNoticeMost;
    if (waiting) {
        g_notice_said = true;
        if (g_notice_on.QuadPart == 0)
            g_notice_on = now;
    }
    // once on, at least kNoticeMin (the last words kept)
    const bool on = waiting || (g_notice_on.QuadPart != 0 && static_cast<double>(now.QuadPart - g_notice_on.QuadPart) / frequency < kNoticeMin);
    if (!on)
        g_notice_on.QuadPart = 0;
    g_notice_shown = on ? std::min(1.0, g_notice_shown + step / kNoticeFade) : std::max(0.0, g_notice_shown - step / kNoticeFade);
    if (g_notice_shown <= 0.0) {
        g_notice_words = {}; // gone: the next wait lays out afresh
        g_toast.least_width = 0;
        return 0.0f;
    }
    if (!g_notice_want.title.empty())
        g_notice_words = g_notice_want; // the last words kept while it slides out
    if (!toast_layout(height))
        return 0.0f;
    return static_cast<float>(g_notice_shown);
}

// Draws the toast `shown` (0-1) of the way in on a `width` x `height` screen: in the middle at the top, slid down from
// above the top edge (easing out as it comes, in as it goes), faded with it. The bar's fill is the count's share of the
// track, with rounded ends; without a count a third of the track sweeps along it every 1.4 s.
void toast_draw(void* device, unsigned width, unsigned height, float shown)
{
    if (!g_toast.panel.upload(device))
        return;
    const graphics::Waiting& words = g_notice_words;
    const double k = 1.0 - shown;
    const double eased = 1.0 - k * k * k;
    const int rest = std::max(kMargin * 2, static_cast<int>(height / 72)); // where it stops, from the top edge
    const int x = (static_cast<int>(width) - g_toast.width) / 2;
    const int y = static_cast<int>(std::lround(-g_toast.height + (g_toast.height + rest) * eased));
    const float opacity = static_cast<float>(std::min(1.0, eased * 1.25));
    g_toast.panel.draw(device, x, y, opacity);
    const int text_x = x + g_toast.strip + g_toast.pad;
    int text_y = y + g_toast.pad;
    if (g_toast.title.upload(device))
        g_toast.title.draw(device, text_x, text_y, opacity);
    if (words.total != 0 && !words.alert && g_toast.count.upload(device))
        g_toast.count.draw(device, x + g_toast.width - g_toast.pad - g_toast.count.width(),
                           text_y + (g_toast.title.height() - g_toast.count.height()) / 2, opacity);
    text_y += g_toast.title.height() + g_toast.gap;
    if (!words.detail.empty() && g_toast.detail.upload(device))
        g_toast.detail.draw(device, text_x, text_y, opacity);
    // the bar: its fill's image rendered for the width it has now
    int fill_x = 0, fill_w = 0;
    if (words.total != 0) {
        fill_w = static_cast<int>(std::lround(static_cast<double>(g_toast.bar_w) * std::min(words.done, words.total) / words.total));
    } else {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const double t = std::fmod(static_cast<double>(now.QuadPart) / static_cast<double>(g_frequency.QuadPart), 1.4) / 1.4;
        const double e = t < 0.5 ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3) / 2; // eased in and out
        fill_w = g_toast.bar_w / 3;
        fill_x = static_cast<int>(std::lround((g_toast.bar_w - fill_w) * e));
    }
    fill_w = std::clamp(fill_w, 0, g_toast.bar_w);
    if (fill_w < g_toast.bar_h) // too short for its rounded ends: none
        return;
    if (fill_w != g_toast.bar_fill) {
        g_toast.bar.set_image(rounded_image(fill_w, g_toast.bar_h, g_toast.bar_h / 2.0f, words.alert ? kAlertArgb : accent_argb(1.0f),
                                            0u, 0, 0u), fill_w, g_toast.bar_h);
        g_toast.bar_fill = fill_w;
    }
    if (g_toast.bar.upload(device))
        g_toast.bar.draw(device, x + g_toast.bar_x + fill_x, y + g_toast.bar_y, opacity);
}

// The Render visual's tag, by the /vana window's names for it (vana.lua's Render visual choice); empty for none
std::wstring visual_words(int visual)
{
    // (12-14, the reflections' parts, have no name: no tag)
    static const wchar_t* const kNames[] = {nullptr, L"Occlusion", L"Fog", L"Bloom", L"God rays", L"Shadows", L"Ink lines",
                                            L"Depth of field", L"Bounce light", L"Glow effects lighting", L"Zone lines", L"Water",
                                            nullptr, nullptr, nullptr, L"Shadow rings", L"AO rings", L"Reflections",
                                            L"Reflection ground", L"Character mask"};
    if (visual <= 0 || visual >= static_cast<int>(std::size(kNames)) || kNames[visual] == nullptr)
        return {};
    return std::wstring(L"DEBUG: ") + kNames[visual];
}

// The tag's look: the label's font, bold, in red, in a dark box when the label has none (it's read over a picture that
// may be near black)
text::Style visual_style()
{
    text::Style style = g_style;
    style.weight = FW_BOLD;
    style.color[0] = 255.0f, style.color[1] = 80.0f, style.color[2] = 64.0f;
    if (style.background[3] < 0.6f) {
        style.background[0] = style.background[1] = style.background[2] = 0.0f;
        style.background[3] = 0.7f;
    }
    style.padding = -1;
    style.trim = false;
    return style;
}

// The DirectX 11 notice's look: the Render visual tag's, in amber
text::Style switch_style()
{
    text::Style style = visual_style();
    style.color[0] = 255.0f, style.color[1] = 176.0f, style.color[2] = 64.0f;
    return style;
}

// The logo, scaled to fit the label's box (renders it again for a new font size).
void set_logo()
{
    if (g_logo_path.empty())
        return;
    int width = 0, height = g_label.height() - 2 * kLogoInset;
    std::vector<std::uint32_t> pixels;
    if (height > 0 && text::load_image(g_logo_path, width, height, pixels)) {
        g_logo.set_image(std::move(pixels), width, height);
        return;
    }
    log::write(L"Overlay: couldn't read the logo %ls; the label goes without it", g_logo_path.c_str());
    g_logo_path.clear();
}

// The label's box colour (font.background), `opacity` times as opaque, as text::fill takes it.
std::uint32_t background_argb(double opacity)
{
    const auto channel = [](float value) { return static_cast<std::uint32_t>(std::lround(value < 0 ? 0 : value > 255 ? 255 : value)); };
    return channel(g_style.background[3] * 255.0f * static_cast<float>(opacity)) << 24 | channel(g_style.background[0]) << 16 |
           channel(g_style.background[1]) << 8 | channel(g_style.background[2]);
}

// The window's icons (the title bar's and Alt+Tab's, and the taskbar's) as VanaCore.exe's, in place of
// the game's own.
HICON g_big_icon = nullptr;   // what set_icon made, kept so update_title can put them back
HICON g_small_icon = nullptr;

void set_icon(HWND window)
{
    if (g_big_icon != nullptr || g_small_icon != nullptr) {
        if (g_big_icon != nullptr)
            SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_big_icon));
        if (g_small_icon != nullptr)
            SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_small_icon));
        return;
    }
    const HMODULE file = LoadLibraryExW(g_icon_file.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (file == nullptr) {
        log::write(L"Overlay: couldn't read the icon from %ls (error %lu)", g_icon_file.c_str(), GetLastError());
        return;
    }
    // Kept for the window's life (the process's): WM_SETICON doesn't copy them
    const auto load = [&](int size) {
        return static_cast<HICON>(LoadImageW(file, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
    };
    const HICON big_icon = load(GetSystemMetrics(SM_CXICON));
    const HICON small_icon = load(GetSystemMetrics(SM_CXSMICON));
    FreeLibrary(file);
    g_big_icon = big_icon;
    g_small_icon = small_icon;
    if (big_icon != nullptr)
        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big_icon));
    if (small_icon != nullptr)
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small_icon));
    if (big_icon == nullptr && small_icon == nullptr)
        log::write(L"Overlay: %ls has no icon; the window keeps the game's", g_icon_file.c_str());
}

// The window's title: the character's name while one is in the world (the player's party slot and
// entity agree), VanaCore's before and at the character select. Looked at once a second.
void update_title()
{
    const ULONGLONG now = GetTickCount64();
    const HWND window = window::handle();
    if (now < g_next_title || window == nullptr)
        return;
    g_next_title = now + kTitleCheck;
    // The game (or Windows) may have put its own icon back after the device was made
    static int icon_resets = 0; // a few tries: a window that won't take it isn't asked once a second for ever
    if (icon_resets < 5 &&
        ((g_big_icon != nullptr && SendMessageW(window, WM_GETICON, ICON_BIG, 0) != reinterpret_cast<LRESULT>(g_big_icon)) ||
         (g_small_icon != nullptr && SendMessageW(window, WM_GETICON, ICON_SMALL, 0) != reinterpret_cast<LRESULT>(g_small_icon)))) {
        ++icon_resets;
        set_icon(window);
        log::write(L"Overlay: window icon put back");
    }
    std::wstring wanted = g_title;
    if (const std::string character = game::local_player_name(); !character.empty()) {
        const int length = MultiByteToWideChar(932, 0, character.c_str(), -1, nullptr, 0); // Shift-JIS
        std::wstring name(length > 0 ? length - 1 : 0, L'\0');
        if (length > 1)
            MultiByteToWideChar(932, 0, character.c_str(), -1, name.data(), length);
        if (!name.empty())
            wanted = name;
    }
    wchar_t current[256] = {};
    GetWindowTextW(window, current, 256);
    if (wanted != current) {
        SetWindowTextW(window, wanted.c_str());
        log::write(L"Overlay: window title \"%ls\"", wanted.c_str());
    }
}

// Where the FPS counter goes in a `width` x `height` back buffer; under the label when both are top-left.
POINT fps_position(UINT width, UINT height)
{
    const int margin = g_fps_padding >= 0 ? 0 : kMargin; // fps.padding is all the space there is
    const int right = static_cast<int>(width) - margin - g_fps.width();
    const int bottom = static_cast<int>(height) - margin - g_fps.height();
    switch (g_fps_corner) {
    case 1: return {right, margin};
    case 2: return {right, bottom};
    case 3: return {margin, bottom};
    default: return {kMargin, kMargin + static_cast<int>(std::lround((g_label.height() + kGap) * g_label_shown))};
    }
}

// The back buffer's size (0 x 0 if it can't be had), which addons also see as the screen's.
d3d8::SurfaceDesc back_buffer_size(void* device)
{
    d3d8::SurfaceDesc desc{};
    void* back_buffer = nullptr;
    if (SUCCEEDED(call(device, slot::GetBackBuffer, 0u, d3d8::kBackBufferMono, &back_buffer))) {
        call(back_buffer, slot::SurfaceGetDesc, &desc);
        call<ULONG>(back_buffer, slot::Release);
    }
    return desc;
}

// ImGui's frame, in the game's pixels (the back buffer's) as Ashita's, drawn onto VanaCore's interface (the back
// buffer's size, or the drawn size if another): nothing if it's open.
void begin_gui_frame(void* device, const d3d8::SurfaceDesc& desc)
{
    unsigned width = desc.width, height = desc.height;
    graphics::ui_size(width, height);
    const stats::Scope timing(stats::kImGui);
    gui::begin_frame(device, window::handle(), width, height, desc.width, desc.height);
}

void draw(void* device, const d3d8::SurfaceDesc& desc, unsigned width, unsigned height)
{
    if (g_failed || desc.width == 0 || call(device, slot::TestCooperativeLevel) != S_OK) // lost device: nothing to draw on
        return;
    if (device != g_device) {
        g_label.release();
        g_logo.release();
        g_fps.release();
        g_toast.release();
        g_visual.release();
        g_switch.release();
        ui::release();
        g_device = device;
    }
    if (!g_label.upload(device)) {
        g_failed = true;
        log::write(L"Overlay: could not create the label texture; the overlay is off");
        return;
    }
    const bool fps = g_show_fps && g_fps.upload(device);
    const bool logo = g_logo.upload(device);
    const float toast = toast_shown(height);
    const int visual = graphics::render_visual();
    const std::wstring visual_text = visual_words(visual);
    if (visual != g_visual_said) {
        if (!visual_text.empty())
            log::write(L"Overlay: \"%ls\" shown: the Render visual ([graphics.effects] debug=%d) is in place of the picture",
                       visual_text.c_str(), visual);
        else if (g_visual_said != 0)
            log::write(L"Overlay: the picture is back (Render visual off)");
        g_visual_said = visual;
    }
    if (!visual_text.empty())
        g_visual.set(visual_text, visual_style());
    const bool visual_up = !visual_text.empty() && g_visual.upload(device);
    const bool switched = graphics::switched_to_d3d11();
    if (switched)
        g_switch.set(L"DirectX 12 can't run here: switched to DirectX 11", switch_style());
    const bool switch_ready = switched && g_switch.upload(device);

    // plugins' objects in the game's pixels, as they place them; then VanaCore's own interface over
    // them, in the drawn pixels (width x height), so it keeps its shape in a window of another shape
    DWORD state = 0;
    if (!text::begin(device, desc.width, desc.height, state))
        return;
    plugins::draw(device);
    text::end(device, state);
    graphics::own_pixels(true);
    if (text::begin(device, width, height, state)) {
        const bool hidden = hideui::hidden(); // with the game's interface ([ui] hide.key)
        if (!hidden) {
            text::record(true); // what's drawn over the game's windows, drawn again over those raise.cpp lays on top (redraw_raised)
            itemcount::draw(device, width, height); // [client] items.count.font=sharp: on the bag window's title bar
            netstat::draw(device, width, height);   // [client] network.icon=dot: in the Network window
            targetwin::draw(device, width, height); // [client] target.lock=glow: the Target window's glowing border
            chattabs::draw(device, width, height);  // chattabs.font: the chat log's tabs' names in True Font
            macrotext::draw(device, width, height); // menu.font, macrobar.font: the macro bar's names and keys' labels in True Font
            labeltext::draw(device, width, height); // buttons.font, titles.font: the Config windows' labels in True Font
            listtext::draw(device, width, height);  // items.font: the item lists' names in True Font
            statuswindow::draw(device, width, height); // status.name.font: the Status window's name in True Font
            text::record(false);
            damagetext::draw_preview(device, width, height); // [graphics] damage.preview: sample numbers
        }
        g_label_shown = label_shown();
        if (g_label_shown > 0.0 && !hidden) {
            const int slide = static_cast<int>(std::lround((1.0 - g_label_shown) * g_label.height()));
            const int x = kMargin - slide;
            // the logo first, in the label's box made wider (its padding is the text's)
            const int logo_box = logo ? kLogoInset * 2 + g_logo.width() : 0;
            if (logo) {
                text::fill(device, x, kMargin, logo_box, g_label.height(), background_argb(g_label_shown));
                g_logo.draw(device, x + kLogoInset * 2, kMargin + (g_label.height() - g_logo.height()) / 2,
                            static_cast<float>(g_label_shown));
            }
            g_label.draw(device, x + logo_box, kMargin, static_cast<float>(g_label_shown));
        }
        if (fps && !hidden) {
            const POINT at = fps_position(width, height);
            g_fps.draw(device, at.x, at.y);
        }
        // the tags under the label (as it slides out) and an FPS counter in the same corner: the DirectX 11 notice, then
        // the Render visual's
        int tag_y = kMargin + static_cast<int>(std::lround((g_label.height() + kGap) * g_label_shown));
        if (fps && g_fps_corner == 0)
            tag_y += g_fps.height() + kGap;
        const double switch_shown = switch_ready ? label_shown(kSwitchStay) : 0.0;
        if (switch_shown > 0.0 && !hidden) {
            if (!g_switch_said) {
                g_switch_said = true;
                log::write(L"Overlay: \"%ls\" shown", g_switch.text().c_str());
            }
            const int slide = static_cast<int>(std::lround((1.0 - switch_shown) * g_switch.height()));
            g_switch.draw(device, kMargin - slide, tag_y, static_cast<float>(switch_shown));
            tag_y += static_cast<int>(std::lround((g_switch.height() + kGap) * switch_shown));
        }
        if (visual_up && !hidden)
            g_visual.draw(device, kMargin, tag_y);
        if (toast > 0.0f && !hidden)
            toast_draw(device, width, height, toast);
        backdrop::flush(!hidden && gui::visible()); // frosted glass behind VanaCore's windows: the renderer blurs it at Present
        gui::draw(device, width, height); // ImGui: plugins' and addons' windows, under VanaCore's own
        ui::draw(device);
        widgets::draw_over_ui(device); // vana.world.place over_ui
        text::end(device, state);
    }
    graphics::own_pixels(false);

    if (!g_drawn) {
        g_drawn = true;
        log::write(L"Overlay: drawing \"%ls\"%ls on a %ux%u back buffer", g_label.text().c_str(),
                   g_show_fps ? L" and the FPS counter" : L"", desc.width, desc.height);
    }
}

// After raise::put: VanaCore's text over the game's windows (draw's recorded part: the macro bar's names and key labels, the
// item lists', the Config windows', the chat tabs'...) drawn again inside the windows it laid back over everything, whose
// copies were taken as the game drew them, without that text, and covered it where it was drawn (and, with the renderer,
// their places were cut out of VanaCore's layer). The macro bar meeting an addon's window under it (vana.ui.game_on_top,
// fancychat's menus.ontop) lost every name and label while its macros still worked (reported 2026-11-14 by Arestia, the
// bar bigger with the interface scaled up). Outside those windows the text stays as drawn once.
void redraw_raised(void* device, unsigned width, unsigned height)
{
    short rects[raise::kMost * 2][4] = {};
    const int count = raise::laid(rects, raise::kMost * 2);
    text::Crop boxes[raise::kMost * 2] = {};
    int n = 0;
    for (int i = 0; i < count; ++i) {
        float x0 = 0, y0 = 0, x1 = 0, y1 = 0, scale = 0;
        if (!uispace::to_pixels(rects[i][0], rects[i][1], width, height, x0, y0, scale) ||
            !uispace::to_pixels(rects[i][2], rects[i][3], width, height, x1, y1, scale))
            continue;
        boxes[n++] = {static_cast<int>(std::floor(x0)), static_cast<int>(std::floor(y0)), static_cast<int>(std::ceil(x1)),
                      static_cast<int>(std::ceil(y1))};
    }
    DWORD state = 0;
    if (n == 0) {
        text::replay(device, nullptr, 0); // (forgotten)
        return;
    }
    graphics::own_pixels(true); // (where draw drew it: VanaCore's layer, the drawn pixels)
    if (text::begin(device, width, height, state)) {
        text::replay(device, boxes, n);
        text::end(device, state);
    } else {
        text::replay(device, nullptr, 0);
    }
    graphics::own_pixels(false);
}

// Frames presented over the last second, as frames per second. With frame generation running, each of the game's
// frames is shown with the ones made between it and the last, and they count: 30 rendered, 60 on the screen. Called
// after VanaCore's limit: a frame it held is timed by its place in the limit's cadence (framerate::frame_slot), so one
// that came late and the one let straight through after it don't read as 61 or 62 for that second (121-124 shown at 2x).
void count_frame()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (const long long slot = framerate::frame_slot(); slot != 0)
        now.QuadPart = slot;
    if (g_window_start.QuadPart == 0) {
        g_window_start = now;
        return;
    }
    ++g_frames;
    const LONGLONG elapsed = now.QuadPart - g_window_start.QuadPart;
    if (elapsed >= g_frequency.QuadPart) {
        const double fps = g_frames * static_cast<double>(graphics::shown_per_frame()) *
                           static_cast<double>(g_frequency.QuadPart) / static_cast<double>(elapsed);
        g_fps.set(std::to_wstring(std::lround(fps)), fps_style());
        g_frames = 0;
        g_window_start = now;
    }
}

void check_settings();

HRESULT __stdcall present(void* device, const RECT* source, const RECT* dest, HWND window, const RGNDATA* dirty)
{
    graphics::close_if_lost(); // the frame after the graphics card stopped: nothing more to draw
    const PresentScope in_present;
    if (settings::tick()) // before anything below looks at the ini: a change taken up is read this frame
        settings_changed();
    clients::tick(); // who this game is, and the settings another game handed over
    frametime::frame_start();
    widgets::on_present(); // the next names' pass draws the widgets in the world (once a frame)
    stats::on_frame();
    perf::on_frame();
    hwmon::on_frame();
    {
        const stats::Scope timing(stats::kSettings);
        abtest::on_frame();
        check_settings();
    }
    const d3d8::SurfaceDesc desc = back_buffer_size(device);
    unsigned width = desc.width, height = desc.height; // VanaCore's interface: the drawn size, if another
    graphics::ui_size(width, height);
    ui::set_screen(width, height);
    ui::set_game_screen(desc.width, desc.height);
    {
        const stats::Scope timing(stats::kChatPackets);
        chat::on_frame();
        chatfont::on_frame(); // [client] chat.font: the glyphs made again for a new font or size on the screen
        phraselist::on_frame(desc.width, desc.height); // the auto-translate list over fancychat's input bar
        packets::on_frame();
    }
    entityhp::on_frame(); // the party's health sampled, for the addons' and plugins' maximum HP (before their frames)
    maxhp::on_frame();    // the server's answers to max HP requests reach their callbacks (maxhp_ask.hpp)
    mcp::on_frame(); // what the MCP server's tools asked of the game's thread
    {
        const stats::Scope timing(stats::kSettings);
        update_title();
    }
    {
        const stats::Scope timing(stats::kScreenshots);
        screenshot::on_frame();
    }
    {
        const stats::Scope timing(stats::kGameConfig);
        game_config::on_frame();
        camera::on_frame();
        framepacing::on_frame();
        packetflow::on_frame(); // [client] packetflow: the game sends sooner once the server has answered
        packetwatch::on_frame(); // [client] packets.watch: the setting read, the line of counts
        itemcount::on_frame();
        gearload::on_frame(); // gear parts that don't load, logged
        itemlist::on_frame();
        hudwin::raise_moved(); // [client] itemdetails.move: the item details drawn over every menu
        spelllist::on_frame();
        buttons::on_frame(); // [client] buttons.*: the menu buttons' style hooked, their nudge and flash written
        itemslot::on_frame();
        itemrarity::on_frame();
        itemtips::on_frame(); // [client] tips.extended: the item details' pages hooked, their 12-row limits widened
        tipicons::on_frame(); // [client] tips.icon.*, tips.badges.*: the item details' picture and badges moved
        netstat::on_frame();
        statuswindow::on_frame();
        equipstats::on_frame(g_ini.c_str()); // [client] equip.stats: the Equipment window's drawing hooked, /checkparam's answer awaited
        alliancelist::on_frame();
        tpbar::on_frame();
        partylist::on_frame(); // [client] party.names.upper, party.hp.color: the lists' row code patched
        noblink::on_frame(); // [client] blink.disable: the game's model rebuild step hooked
        fishing::on_frame(); // [input] fishing.dpad: the fish fight's direction read patched
        autologin::on_frame(); // [client] autologin: the title screen's windows answered
        chatpages::on_frame(); // the chat log pages named after this process (two games, one install)
        helpbar::on_frame();
        targetwin::on_frame(); // [client] target.move: the Target window placed
        chattabs::on_frame();  // chattabs.font, chat.tabs.spacing: the chat log's tabs' drawing hooked
        mapwin::on_frame();    // [client] map.move: the in-game map's limits and drawing hooked
        buffwin::on_frame();   // [client] buffs.move: the status icons placed
        macrobar::on_frame();
        menuhover::on_frame(); // [client] menu.hovermove: the mouse picks a menu row only when it moves
        gamewin::on_frame();   // vana.gameui.watch: watched windows that closed told (before the addons' frame below)
        configmenu::on_frame(); // [client] configmenu: VanaCore's item in the game's Config list
        gamesettings::on_frame(); // [client] gamesettings.extended: Config > Gameplay with VanaCore's rows
        singlemenu::on_frame(); // [client] singlemenu: the main menu's two pages as one list
        equipfirst::on_frame(); // [client] macrooptions.equipfirst: an equip set's options with Equip first
        namechars::on_frame(g_ini.c_str()); // [client] macronames.extended: book and equip set names take spaces and symbols
        menulog::on_frame();    // while Record Stats Report runs: the game's windows, menu keys and pad buttons logged
        partymenu::on_frame();  // [client] partyfinder.findmember: Party > Find Member opens Party Finder
        hideui::on_frame();
        soulspire::on_frame();
        discord::on_frame(); // [discord]: every 2 s while Discord is there, the character and zone for its thread
    }
    begin_gui_frame(device, desc); // addons and plugins draw with ImGui in their Present events (it's open already when
                                   // addons listening for scenes had one begin on the back buffer)
    graphics::own_drawing(true); // plugins', addons' and VanaCore's drawing stays where the mouse finds it ([ui] aspect)
    // Ashita's fonts and primitives, then what addons draw in their present events over them (an addon's
    // sprites, a font it draws by hand), as Ashita has it: in a scene of its own, in the game's pixels, the
    // game's render state put back after (a screenshot without overlays is taken first). Plugins' Present
    // after it, outside a scene as before (one may begin its own).
    screenshot::before_overlay(device);
    DWORD state = 0;
    const bool scene = text::begin(device, desc.width, desc.height, state);
    if (scene && !hideui::hidden()) { // with the game's interface ([ui] hide.key)
        const stats::Scope timing(stats::kPlugins);
        plugins::draw_objects(device);
    }
    addons::on_frame(); // each addon's frame is timed in it (stats::AddonScope)
    if (scene)
        text::end(device, state);
    {
        const stats::Scope timing(stats::kPlugins);
        plugins::on_present(device, source, dest, window, dirty);
    }
    {
        const stats::Scope timing(stats::kImGui);
        perf::draw();  // /vana graph's window: ImGui, in the frame the plugins and addons draw in
        stats::draw(); // and /vana stats'
        gui::end_frame();
    }
    {
        const stats::Scope timing(stats::kInterface);
        draw(device, desc, width, height);
        raise::put(device); // the game's windows that go on top, over everything drawn since the game drew them
        redraw_raised(device, width, height); // ... and VanaCore's text over them again on top
    }
    screenshot::after_overlay(device);
    graphics::own_drawing(false);
    {
        const stats::Scope timing(stats::kGraphicsFront);
        graphics::on_frame();
    }
    frametime::limit_start();
    framerate::before_present();
    frametime::limit_end();
    if (g_show_fps)
        count_frame(); // after the limit: its frames timed by their place in its cadence
    zoneload::on_present();
    frametime::present_start();
    const HRESULT result = g_present(device, source, dest, window, dirty);
    frametime::present_end();
    uiscale::after_present(); // [ffxi] resolution.ui changed: the menus made again at its size, between frames
    return result;
}

// The game's device reset (a lost full-screen device, a new size): what VanaCore keeps in the device's default pool (raise.cpp's
// copies) goes first, or the Reset fails
HRESULT __stdcall reset(void* device, d3d8::PresentParameters* params)
{
    raise::release();
    return g_reset(device, params);
}

std::atomic<bool> g_direct3d_reached{false};
bool g_startup_trace = false;

HRESULT __stdcall create_device(void* d3d, UINT adapter, DWORD type, HWND focus, DWORD flags,
                                d3d8::PresentParameters* params, void** device)
{
    const HRESULT result = g_create_device(d3d, adapter, type, focus, flags, params, device);
    if (g_startup_trace)
        log::write(L"Startup trace: IDirect3D8::CreateDevice returned 0x%08lX (tid=%lu, t=%llu ms)",
                   static_cast<unsigned long>(result), GetCurrentThreadId(), GetTickCount64());
    if (FAILED(result) || device == nullptr || *device == nullptr)
        return result;
    imports::hook_vtable(*device, slot::Reset, &reset, g_reset);
    imports::hook_vtable(*device, slot::Present, &present, g_present);
    imports::hook_vtable(*device, slot::BeginScene, &begin_scene, g_begin_scene);
    imports::hook_vtable(*device, slot::EndScene, &end_scene, g_end_scene);
    imports::hook_vtable(*device, slot::DrawPrimitive, &draw_primitive, g_draw_primitive);
    imports::hook_vtable(*device, slot::DrawIndexedPrimitive, &draw_indexed_primitive, g_draw_indexed_primitive);
    imports::hook_vtable(*device, slot::DrawPrimitiveUP, &draw_primitive_up, g_draw_primitive_up);
    imports::hook_vtable(*device, slot::DrawIndexedPrimitiveUP, &draw_indexed_primitive_up, g_draw_indexed_primitive_up);
    frametime::on_device(*device);
    const HWND window = params != nullptr && params->device_window != nullptr ? params->device_window : focus;
    if (window != nullptr) {
        SetWindowTextW(window, g_title.c_str());
        set_icon(window);
        window::install(window);
        g_exclusive = params != nullptr && !params->windowed && !graphics::active();
        window::apply(g_ini.c_str(), g_exclusive);
    }
    log::write(L"Direct3D 8 device created (%ux%u, %ls): window title set, overlay hooked into Present",
               params != nullptr ? params->back_buffer_width : 0, params != nullptr ? params->back_buffer_height : 0,
               params != nullptr && params->windowed ? L"windowed" : L"full screen");
    session::game_started();
    plugins::on_device(*device);
    return result;
}

void* WINAPI direct3d_create(UINT sdk_version)
{
    g_direct3d_reached.store(true, std::memory_order_release);
    if (g_startup_trace)
        log::write(L"Startup trace: Direct3DCreate8 entered (sdk=%u, tid=%lu, t=%llu ms)",
                   sdk_version, GetCurrentThreadId(), GetTickCount64());
    // [graphics] renderer=d3d12: VanaCore's own Direct3D 8, on Direct3D 12 (hooked below like the real one)
    void* d3d = graphics::direct3d_create();
    if (d3d == nullptr)
        d3d = g_direct3d_create(sdk_version);
    if (d3d != nullptr)
        imports::hook_vtable(d3d, slot::CreateDevice, &create_device, g_create_device);
    return d3d;
}

void read_colour(const wchar_t* ini, const wchar_t* key, float (&colour)[4])
{
    wchar_t text[32] = {};
    GetPrivateProfileStringW(L"client", key, L"", text, 32, ini);
    if (text[0] != L'\0' && !text::parse_colour(text, colour))
        log::write(L"Overlay: %ls=%ls isn't #RRGGBB or #RRGGBBAA; kept the default", key, text);
}

// [client] font.*, fps.display and fps.position; a key taken out goes back to its default.
void read_settings(const wchar_t* ini)
{
    g_style = text::Style{};
    g_style.family = kDefaultFamily;
    g_style.color[0] = 255.0f; // #FFD700
    g_style.color[1] = 215.0f;
    g_style.color[2] = 0.0f;
    g_style.background[3] = 0.0f; // no box
    wchar_t text[LF_FACESIZE] = {};
    GetPrivateProfileStringW(L"client", L"font.family", L"", text, LF_FACESIZE, ini);
    if (text[0] != L'\0')
        g_style.family = text;
    const UINT size = GetPrivateProfileIntW(L"client", L"font.size", kDefaultSize, ini);
    g_style.size = size < 6 ? 6 : size > 128 ? 128 : static_cast<int>(size);
    // font.bold: on (the default), the semibold it has always been drawn in; off, regular. font.italic: off by default
    g_style.weight = GetPrivateProfileIntW(L"client", L"font.bold", 1, ini) != 0 ? FW_SEMIBOLD : FW_NORMAL;
    g_style.italic = GetPrivateProfileIntW(L"client", L"font.italic", 0, ini) != 0;
    read_colour(ini, L"font.color", g_style.color);
    g_style.stroke = GetPrivateProfileIntW(L"client", L"font.outline", 1, ini) != 0 ? 1 : 0;
    read_colour(ini, L"font.background", g_style.background);
    g_show_fps = GetPrivateProfileIntW(L"client", L"fps.display", 1, ini) != 0;
    const UINT corner = GetPrivateProfileIntW(L"client", L"fps.position", 1, ini);
    g_fps_corner = corner <= 3 ? static_cast<int>(corner) : 1;
    // fps.padding: pixels, 0-64 (not set: 0); auto (or not a number) grows with the font as the label's does
    GetPrivateProfileStringW(L"client", L"fps.padding", L"0", text, LF_FACESIZE, ini);
    wchar_t* end = nullptr;
    const long padding = std::wcstol(text, &end, 10);
    g_fps_padding = text[0] != L'\0' && *end == L'\0' ? static_cast<int>(padding < 0 ? 0 : padding > 64 ? 64 : padding) : -1;
}

void log_style()
{
    // Windows quietly substitutes a font that isn't installed: say which one.
    const std::wstring used = text::installed_face(g_style);
    if (!used.empty() && _wcsicmp(used.c_str(), g_style.family.c_str()) != 0)
        log::write(L"Overlay: font \"%ls\" isn't installed; Windows uses \"%ls\"", g_style.family.c_str(), used.c_str());
    log::write(L"Overlay: font %ls %dpx, color %ls%ls, background %ls", g_style.family.c_str(), g_style.size,
               text::hex(g_style.color).c_str(), g_style.stroke > 0 ? L" with a black outline" : L"",
               text::hex(g_style.background).c_str());
}

FILETIME ini_time()
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    GetFileAttributesExW(g_ini.c_str(), GetFileExInfoStandard, &data);
    return data.ftLastWriteTime;
}

void check_settings()
{
    if (g_ini.empty())
        return;
    const ULONGLONG now = GetTickCount64();
    if (now < g_next_check)
        return;
    g_next_check = now + 500;
    window::poll(); // a character logging in: its own [window] position
    const FILETIME time = ini_time();
    if (CompareFileTime(&time, &g_ini_time) == 0)
        return;
    g_ini_time = time;
    g_reloaded = now;
    // [logging] enabled changed (the /vana window's switch, /vana logs, a hand edit): on or off now, not at the next start
    log::set_enabled(GetPrivateProfileIntW(L"logging", L"enabled", 1, g_ini.c_str()) != 0);
    const text::Style style = g_style;
    const bool show_fps = g_show_fps;
    const int corner = g_fps_corner;
    const int padding = g_fps_padding;
    read_settings(g_ini.c_str());
    framerate::reload(g_ini.c_str());
    window::apply(g_ini.c_str(), g_exclusive);
    uiscale::reload(g_ini.c_str());
    screenshot::reload(g_ini.c_str());
    hideui::reload(g_ini.c_str());
    itemcount::reload(g_ini.c_str());
    itemlist::reload(g_ini.c_str());
    spelllist::reload(g_ini.c_str());
    itemslot::reload(g_ini.c_str());
    buttons::reload(g_ini.c_str());
    sliders::reload(g_ini.c_str());
    itemrarity::reload(g_ini.c_str());
    itemtips::reload(g_ini.c_str());
    tipicons::reload(g_ini.c_str());
    winimage::reload(g_ini.c_str());
    netstat::reload(g_ini.c_str());
    chatfont::reload(g_ini.c_str());
    statuswindow::reload(g_ini.c_str());
    profilewin::reload(g_ini.c_str());
    equipstats::reload(g_ini.c_str());
    alliancelist::reload(g_ini.c_str());
    tpbar::reload(g_ini.c_str());
    partylist::reload(g_ini.c_str());
    noblink::reload(g_ini.c_str());
    fishing::reload(g_ini.c_str());
    autologin::reload(g_ini.c_str());
    helpbar::reload(g_ini.c_str());
    targetwin::reload(g_ini.c_str());
    chattabs::reload(g_ini.c_str());
    mapwin::reload(g_ini.c_str());
    castbar::reload(g_ini.c_str());
    buffwin::reload(g_ini.c_str());
    hudwin::reload(g_ini.c_str());
    macrobar::reload(g_ini.c_str());
    menuhover::reload(g_ini.c_str());
    configmenu::reload(g_ini.c_str());
    gamesettings::reload(g_ini.c_str());
    soundslots::reload(g_ini.c_str());
    singlemenu::reload(g_ini.c_str());
    equipfirst::reload(g_ini.c_str());
    namechars::reload(g_ini.c_str());
    partymenu::reload(g_ini.c_str());
    discord::reload(g_ini.c_str());
    binds::reload(g_ini.c_str());
    commands::reload(g_ini.c_str());
    addons::reload(g_ini.c_str());
    plugins::reload(g_ini.c_str());
    if (g_style == style && g_show_fps == show_fps && g_fps_corner == corner && g_fps_padding == padding)
        return;
    if (!(g_style == style)) {
        log_style();
        g_label.set(g_label.text(), g_style); // renders it again; the next draw uploads it
        set_logo();
    }
    if ((!(g_style == style) || g_fps_padding != padding) && !g_fps.text().empty())
        g_fps.set(g_fps.text(), fps_style());
    if (g_show_fps && !show_fps)
        g_window_start.QuadPart = 0, g_frames = 0; // counts afresh
    static const wchar_t* const kCorners[] = {L"top-left", L"top-right", L"bottom-right", L"bottom-left"};
    log::write(L"Overlay: settings reloaded from vanacore.ini: FPS counter %ls, padding %ls", g_show_fps ? kCorners[g_fps_corner] : L"off",
               g_fps_padding < 0 ? L"with the font" : std::to_wstring(g_fps_padding).c_str());
}

void on_game_loaded(HMODULE module)
{
    if (imports::hook(module, "d3d8.dll", "Direct3DCreate8", &direct3d_create, g_direct3d_create))
        log::write(L"FFXiMain.dll loaded: Direct3DCreate8 hooked for the title and overlay");
    else
        log::write(L"FFXiMain.dll loaded, but its Direct3DCreate8 import could not be hooked: no title or overlay");
}

} // namespace

void install(const wchar_t* ini, std::wstring title, std::wstring label, std::wstring logo, std::wstring icon_file)
{
    g_title = std::move(title);
    g_logo_path = std::move(logo);
    g_icon_file = std::move(icon_file);
    // Until a character is in the world the window is called what the game calls it, which window switchers
    // (XI Focus) take as "not logged in yet" and read again; a name of ours would be kept as the slot's name.
    // [client] title.vanacore=1 keeps VanaCore's own.
    if (ini == nullptr || ini[0] == L'\0' || GetPrivateProfileIntW(L"client", L"title.vanacore", 0, ini) == 0)
        g_title = L"FINAL FANTASY XI";
    if (ini != nullptr && ini[0] != L'\0') {
        g_ini = ini;
        g_ini_time = ini_time();
        g_startup_trace = GetPrivateProfileIntW(L"logging", L"startup.trace", 0, ini) != 0;
        read_settings(ini);
        log_style();
    }
    g_label.set(std::move(label), g_style); // after the style: this renders it
    set_logo();                             // after the label: it takes the label's height
    QueryPerformanceFrequency(&g_frequency);
    if (!imports::when_loaded(L"FFXiMain.dll", &on_game_loaded))
        log::write(L"Could not watch for FFXiMain.dll to load: no title or overlay");
}

void settings_changed()
{
    // read again soon, not at the next half-second look; but no sooner than kChangedEvery after the last read: a /vana
    // slider dragged changes a setting every frame, and each read is every module's settings again (some 20 ms)
    const ULONGLONG soon = g_reloaded + kChangedEvery;
    if (soon < g_next_check)
        g_next_check = soon;
}

bool in_present()
{
    return g_in_present;
}

bool direct3d_reached()
{
    return g_direct3d_reached.load(std::memory_order_acquire);
}

} // namespace vanacore::overlay
