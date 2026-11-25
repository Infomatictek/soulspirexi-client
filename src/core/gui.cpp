#include "gui.hpp"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "addonbox.hpp"
#include "backdrop.hpp"
#include "d3d8.hpp"
#include "hideui.hpp"
#include "imgui_legacy.hpp"
#include "layout.hpp"
#include "mapwin.hpp"
#include "log.hpp"
#include "soulspire/soulspire.hpp"

// ImGui's failed checks (imconfig.h): logged once each, then ImGui carries on as Ashita's does.
void vanacore_imgui_assert(const char* expression, const char* file, int line)
{
    static std::mutex lock;
    static std::set<std::string> seen;
    const char* name = std::strrchr(file, '\\');
    char where[512];
    std::snprintf(where, sizeof where, "%s:%d: %s", name != nullptr ? name + 1 : file, line, expression);
    {
        const std::lock_guard guard(lock);
        if (seen.size() >= 64 || !seen.insert(where).second)
            return;
    }
    vanacore::log::write(L"ImGui: check failed (a plugin or addon used it wrongly?): %hs", where);
}

namespace vanacore::gui {
namespace {

using d3d8::call;
namespace slot = d3d8::slot;

std::wstring g_install;     // VanaCore's folder, trailing backslash
std::wstring g_settings;    // the settings file the game reads ([client] imgui.font)
std::string g_ini_file;     // config\imgui.ini, UTF-8 (ImGui keeps the pointer)
ImGuiContext* g_context = nullptr;
bool g_visible = true;
bool g_in_frame = false;

// Drawn and taking the mouse and keyboard: visible (IGuiManager's), and the game's interface not hidden ([ui] hide.key)
bool shown()
{
    return g_visible && !hideui::hidden();
}

void* g_device = nullptr;
bool g_fonts_added = false;     // the atlas's fonts are in (Ashita's, or ImGui's own if those failed)
bool g_texture_failed = false;  // a texture couldn't be made on this device: logged once

LARGE_INTEGER g_frequency{};
LARGE_INTEGER g_last{};
int g_cursor = ImGuiMouseCursor_Arrow; // the cursor shape last set
std::atomic<bool> g_cursor_override{false}; // authoritative visible cursor for VanaCore-native UI

// The mouse's buttons and keys, from messages, through ImGui's input queue (a press and its release
// inside one frame still reads as a click).
constexpr int kButtons = 5;
unsigned g_taken = 0;           // presses ImGui took (over its windows), not yet let go
std::string g_taken_owner[kButtons]; // ... whose window each was over (owner_at)
std::string g_last_owner;       // whose window the last message on_mouse took was over (taken_by)
bool g_keys_taken[256] = {};    // key presses ImGui took: their releases are its too
BYTE g_lead = 0;                // the first byte of a double-byte character (an ANSI window's WM_CHAR)

// What the mouse hook and DirectInput see (any thread): where ImGui's windows take the mouse.
SRWLOCK g_rects_lock = SRWLOCK_INIT;
std::vector<RECT> g_rects;
std::vector<std::string> g_rects_owner; // whose each of g_rects is (owner_of_window)
bool g_capture_all = false; // a popup is open: a click anywhere is ImGui's (it closes the popup)
std::string g_capture_owner; // ... whose
std::vector<RECT> g_covered; // the game's windows drawn over ImGui's (set_covered): the mouse there is the game's
std::vector<RECT> g_taken_rects; // take_mouse's this frame (game thread), published with the windows'
std::atomic<bool> g_wants_keyboard{false};
std::atomic<int> g_gamepad_frames{0}; // frames left of vana.gamepad.nav(true)

// (x, y), in the game's pixels, over a game window drawn over ImGui's, with no popup open. Takes the lock.
bool covered(float x, float y)
{
    bool over = false;
    AcquireSRWLockShared(&g_rects_lock);
    for (size_t i = 0; !g_capture_all && !over && i < g_covered.size(); ++i) {
        const RECT& r = g_covered[i];
        over = x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }
    ReleaseSRWLockShared(&g_rects_lock);
    return over;
}

// The game's pixels to an interface pixel, each way (begin_frame): ImGui's space over the interface's, which the mouse
// messages and what's drawn are in. 1 when they're the same size. Any thread.
std::atomic<float> g_game_per_ui_x{1.0f};
std::atomic<float> g_game_per_ui_y{1.0f};

// ImGui's own IME placement (the composition window at the text box), given the box in the window's client pixels rather
// than ImGui's, which are the game's.
void (*g_default_ime)(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData*) = nullptr;

void set_ime_data(ImGuiContext* context, ImGuiViewport* viewport, ImGuiPlatformImeData* data)
{
    ImGuiPlatformImeData scaled = *data;
    RECT client{};
    const HWND window = static_cast<HWND>(viewport->PlatformHandleRaw);
    const ImVec2 size = context->IO.DisplaySize;
    if (window != nullptr && size.x > 0 && size.y > 0 && GetClientRect(window, &client)) {
        const float sx = static_cast<float>(client.right) / size.x, sy = static_cast<float>(client.bottom) / size.y;
        scaled.InputPos = ImVec2(data->InputPos.x * sx, data->InputPos.y * sy);
        scaled.InputLineHeight = data->InputLineHeight * sy;
    }
    g_default_ime(context, viewport, &scaled);
}

std::string utf8(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    out.pop_back();
    return out;
}

bool exists(const std::wstring& file)
{
    const DWORD attributes = GetFileAttributesW(file.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// [client] imgui.font as the settings file has it: the file the text draws in instead of Agave ("" is Agave).
std::wstring font_setting()
{
    wchar_t value[MAX_PATH] = {};
    if (!g_settings.empty())
        GetPrivateProfileStringW(L"client", L"imgui.font", L"", value, MAX_PATH, g_settings.c_str());
    return value;
}

// The font file a [client] imgui.font names: a full path as it is, else a file name looked for in fonts\imgui\, fonts\, the
// fonts Windows installs for one user and Windows' own. "" when it isn't there.
std::wstring find_font(const std::wstring& name)
{
    if (name.find(L':') != std::wstring::npos || name.compare(0, 2, L"\\\\") == 0)
        return exists(name) ? name : std::wstring();
    std::vector<std::wstring> folders = {g_install + L"fonts\\imgui\\", g_install + L"fonts\\"};
    wchar_t path[MAX_PATH] = {};
    const DWORD own = GetEnvironmentVariableW(L"LOCALAPPDATA", path, MAX_PATH);
    if (own > 0 && own < MAX_PATH)
        folders.push_back(std::wstring(path) + L"\\Microsoft\\Windows\\Fonts\\");
    const UINT windows = GetWindowsDirectoryW(path, MAX_PATH);
    if (windows > 0 && windows < MAX_PATH)
        folders.push_back(std::wstring(path) + L"\\Fonts\\");
    for (const std::wstring& folder : folders) {
        if (exists(folder + name))
            return folder + name;
    }
    return {};
}

// Ashita's fonts (GuiManager::PrepareFonts): Agave at 18 (the default), 24 and 32 px, each with Font
// Awesome 6's regular, brands and solid icons merged in, in that order (the first to have a
// character draws it), set 4 px apart from the text and 1 px lower. Not oversampled as Ashita's are:
// with every glyph on whole pixels it changes nothing but the texture's size. Each font draws at any
// size (ImGui 1.92 bakes glyphs as they're needed). [client] imgui.font puts another font in Agave's place (the icons
// stay), as a file name or a full path.
void add_fonts(ImGuiIO& io)
{
    static const ImWchar kIcons[] = {0xE005, 0xF8FF, 0};
    static const ImWchar kBrands[] = {0xE007, 0xF8E8, 0};
    const std::wstring folder = g_install + L"fonts\\imgui\\";
    const std::wstring agave = folder + L"Agave-Regular.ttf";
    struct Icons {
        const wchar_t* file;
        const ImWchar* ranges;
    };
    const Icons icons[] = {{L"fa-regular-400.ttf", kIcons}, {L"fa-brands-400.ttf", kBrands}, {L"fa-solid-900.ttf", kIcons}};
    // the text font at each size with the icons merged in; false when it can't be added (nothing is)
    const auto add_set = [&](const std::wstring& text) {
        std::wstring stem = text.substr(text.find_last_of(L'\\') + 1);
        stem.erase(std::min(stem.rfind(L'.'), stem.size())); // the extension off (none: all of it stays)
        std::replace(stem.begin(), stem.end(), L'-', L'_'); // Agave-Regular.ttf: "Agave_Regular, 18px", as Ashita's
        for (const float size : {18.0f, 24.0f, 32.0f}) {
            ImFontConfig config;
            config.PixelSnapH = true;
            config.OversampleH = 1; // snapped to whole pixels: more would only grow the texture (3x)
            std::snprintf(config.Name, sizeof config.Name, "%s, %.0fpx", utf8(stem).c_str(), size);
            if (io.Fonts->AddFontFromFileTTF(utf8(text).c_str(), size, &config) == nullptr) {
                if (size == 18.0f)
                    return false;
                continue;
            }
            for (const Icons& set : icons) {
                const std::wstring file = folder + set.file;
                if (!exists(file))
                    continue;
                ImFontConfig merge;
                merge.MergeMode = true;
                merge.PixelSnapH = true;
                merge.OversampleH = 1;
                merge.GlyphExtraAdvanceX = 4.0f;
                merge.GlyphOffset = ImVec2(0.0f, 1.0f);
                io.Fonts->AddFontFromFileTTF(utf8(file).c_str(), size, &merge, set.ranges);
            }
        }
        return true;
    };
    bool added = false;
    const std::wstring setting = font_setting();
    if (!setting.empty()) {
        const std::wstring chosen = find_font(setting);
        added = !chosen.empty() && add_set(chosen);
        if (added)
            log::write(L"ImGui: font %ls ([client] imgui.font=%ls)", chosen.c_str(), setting.c_str());
        else
            log::write(L"ImGui: [client] imgui.font=%ls isn't a font file that can be loaded: Agave instead", setting.c_str());
    }
    if (!added && exists(agave))
        add_set(agave);
    if (io.Fonts->Fonts.empty()) {
        log::write(L"ImGui: no %ls: ImGui's own font (ProggyClean) instead", agave.c_str());
        io.Fonts->AddFontDefault();
    }
    g_fonts_added = true;
}

// ---- Textures, as ImGui asks for them (the font atlas: ImGuiBackendFlags_RendererHasTextures) -----

// ImGui's RGBA pixels (a rectangle of the texture) into Direct3D's A8R8G8B8 (B, G, R, A).
void copy_pixels(ImTextureData& tex, int x, int y, int w, int h, BYTE* to, int pitch)
{
    for (int row = 0; row < h; ++row) {
        const auto* from = static_cast<const std::uint32_t*>(tex.GetPixelsAt(x, y + row));
        auto* out = reinterpret_cast<std::uint32_t*>(to + row * pitch);
        for (int col = 0; col < w; ++col) {
            const std::uint32_t c = from[col];
            out[col] = (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16);
        }
    }
}

void* texture_of(const ImTextureData& tex)
{
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(tex.TexID));
}

void update_texture(void* device, ImTextureData* tex)
{
    if (tex->Status == ImTextureStatus_WantCreate) {
        void* texture = nullptr;
        if (tex->Format != ImTextureFormat_RGBA32 ||
            FAILED(call(device, slot::CreateTexture, static_cast<UINT>(tex->Width), static_cast<UINT>(tex->Height), 1u, DWORD{0},
                        d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged, &texture))) {
            if (!std::exchange(g_texture_failed, true))
                log::write(L"ImGui: couldn't make a %dx%d texture", tex->Width, tex->Height);
            return; // asked for again next frame
        }
        d3d8::LockedRect locked{};
        if (SUCCEEDED(call(texture, slot::TextureLockRect, 0u, &locked, static_cast<const RECT*>(nullptr), DWORD{0}))) {
            copy_pixels(*tex, 0, 0, tex->Width, tex->Height, static_cast<BYTE*>(locked.bits), locked.pitch);
            call(texture, slot::TextureUnlockRect, 0u);
        }
        tex->SetTexID(static_cast<ImTextureID>(reinterpret_cast<std::uintptr_t>(texture)));
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantUpdates) {
        void* texture = texture_of(*tex);
        const RECT area{tex->UpdateRect.x, tex->UpdateRect.y, tex->UpdateRect.x + tex->UpdateRect.w, tex->UpdateRect.y + tex->UpdateRect.h};
        d3d8::LockedRect locked{};
        if (texture != nullptr && SUCCEEDED(call(texture, slot::TextureLockRect, 0u, &locked, &area, DWORD{0}))) {
            for (const ImTextureRect& r : tex->Updates)
                copy_pixels(*tex, r.x, r.y, r.w, r.h,
                            static_cast<BYTE*>(locked.bits) + (r.y - area.top) * locked.pitch + (r.x - area.left) * 4, locked.pitch);
            call(texture, slot::TextureUnlockRect, 0u);
        }
        tex->SetStatus(ImTextureStatus_OK);
    } else if (tex->Status == ImTextureStatus_WantDestroy) {
        if (void* texture = texture_of(*tex))
            call<ULONG>(texture, slot::Release);
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed); // made again on the next device, if it's still used
    }
}

// Every texture ImGui made: gone with the device.
void release_textures()
{
    if (g_context == nullptr)
        return;
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->RefCount != 1 || tex->TexID == ImTextureID_Invalid)
            continue;
        tex->SetStatus(ImTextureStatus_WantDestroy);
        update_texture(nullptr, tex);
    }
}

// Where the mouse is, in interface pixels (the window's client area may be another size).
ImVec2 mouse_position(HWND window, unsigned width, unsigned height)
{
    POINT at{};
    RECT client{};
    if (window == nullptr || !GetCursorPos(&at) || !ScreenToClient(window, &at) || !GetClientRect(window, &client) ||
        client.right <= 0 || client.bottom <= 0)
        return ImVec2(-FLT_MAX, -FLT_MAX);
    return ImVec2(static_cast<float>(at.x) * static_cast<float>(width) / static_cast<float>(client.right),
                  static_cast<float>(at.y) * static_cast<float>(height) / static_cast<float>(client.bottom));
}

// ImGui asked for the mouse to move (io.WantSetMousePos): interface pixels back to the screen's.
void set_mouse_position(HWND window, ImVec2 at, unsigned width, unsigned height)
{
    RECT client{};
    if (window == nullptr || width == 0 || height == 0 || !GetClientRect(window, &client))
        return;
    POINT to{static_cast<LONG>(at.x * static_cast<float>(client.right) / static_cast<float>(width)),
             static_cast<LONG>(at.y * static_cast<float>(client.bottom) / static_cast<float>(height))};
    if (ClientToScreen(window, &to))
        SetCursorPos(to.x, to.y);
}

// ImGui's cursor shapes (resize arrows, the text beam) over its windows, as Ashita sets them.
void update_cursor(const ImGuiIO& io)
{
    int cursor = io.MouseDrawCursor ? ImGuiMouseCursor_None : ImGui::GetMouseCursor();
    if (g_cursor_override.load(std::memory_order_relaxed)) {
        // Authoritative single hardware cursor while VanaCore-native Soulspire UI owns input.
        if (g_context != nullptr && g_context->IO.MouseDrawCursor)
            g_context->IO.MouseDrawCursor = false;
        if (cursor == ImGuiMouseCursor_None)
            cursor = ImGuiMouseCursor_Arrow;
    }
    if (cursor == g_cursor || (io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange) != 0)
        return;
    g_cursor = cursor;
    LPCWSTR shape = IDC_ARROW;
    switch (cursor) {
    case ImGuiMouseCursor_TextInput: shape = IDC_IBEAM; break;
    case ImGuiMouseCursor_ResizeAll: shape = IDC_SIZEALL; break;
    case ImGuiMouseCursor_ResizeEW: shape = IDC_SIZEWE; break;
    case ImGuiMouseCursor_ResizeNS: shape = IDC_SIZENS; break;
    case ImGuiMouseCursor_ResizeNESW: shape = IDC_SIZENESW; break;
    case ImGuiMouseCursor_ResizeNWSE: shape = IDC_SIZENWSE; break;
    case ImGuiMouseCursor_Hand: shape = IDC_HAND; break;
    case ImGuiMouseCursor_NotAllowed: shape = IDC_NO; break;
    default: break;
    }
    SetCursor(cursor == ImGuiMouseCursor_None ? nullptr : LoadCursorW(nullptr, shape));
}

// Who `window` is ("ashita/fancychat", layout::owner_of): a child, popup or menu by the top-level window it's part of
std::string owner_of_window(const ImGuiWindow* window)
{
    const ImGuiWindow* top = window->RootWindowPopupTree != nullptr ? window->RootWindowPopupTree : window;
    return layout::owner_of(top->Name);
}

// Where ImGui's windows take the mouse, for the mouse hook and DirectInput (window.cpp, input.cpp):
// each shown window's rectangle (with its resizing edges), unless it lets the mouse through.
void publish()
{
    std::vector<RECT> rects;
    std::vector<std::string> owners;
    bool capture_all = false;
    std::string capture_owner;
    if (shown()) {
        const ImGuiContext& g = *g_context;
        for (const ImGuiWindow* window : g.Windows) {
            if (!window->Active || window->Hidden || (window->Flags & ImGuiWindowFlags_NoMouseInputs) != 0)
                continue;
            const float edge = (window->Flags & ImGuiWindowFlags_NoResize) == 0 && g.IO.ConfigWindowsResizeFromEdges ? 4.0f : 0.0f;
            ImRect r = window->Rect();
            layout::visual_rect(window, r.Min.x, r.Min.y, r.Max.x, r.Max.y); // as it's drawn (Addons > Layout's scale)
            rects.push_back({static_cast<LONG>(r.Min.x - edge), static_cast<LONG>(r.Min.y - edge),
                             static_cast<LONG>(r.Max.x + edge + 1), static_cast<LONG>(r.Max.y + edge + 1)});
            owners.push_back(owner_of_window(window));
        }
        for (const RECT& r : g_taken_rects) {
            rects.push_back(r);
            owners.emplace_back();
        }
        capture_all = g.OpenPopupStack.Size > 0 && g.IO.WantCaptureMouse;
        if (capture_all && g.OpenPopupStack.back().Window != nullptr)
            capture_owner = owner_of_window(g.OpenPopupStack.back().Window);
    }
    AcquireSRWLockExclusive(&g_rects_lock);
    g_rects.swap(rects);
    g_rects_owner.swap(owners);
    g_capture_all = capture_all;
    g_capture_owner.swap(capture_owner);
    ReleaseSRWLockExclusive(&g_rects_lock);
    g_wants_keyboard = shown() && (g_context->IO.WantCaptureKeyboard || g_context->IO.WantTextInput);
    g_taken_rects.clear();
}

// The errors ImGui recovers from (a Begin without its End, a style pushed and never popped, an
// item used wrongly): each kind logged once, not shown on the screen and not asserted.
void recovery_log(ImGuiContext*, void*, const char* message)
{
    static std::set<std::string> seen;
    const char* window = GImGui != nullptr && GImGui->CurrentWindow != nullptr ? GImGui->CurrentWindow->Name : nullptr;
    char text[512];
    std::snprintf(text, sizeof text, "%s%s%s", message != nullptr ? message : "?", window != nullptr ? " (in window " : "",
                  window != nullptr ? window : "");
    if (seen.size() < 32 && seen.insert(text).second)
        log::write(L"ImGui: recovered (a plugin or addon used it wrongly): %hs%hs", text, window != nullptr ? ")" : "");
}

// ---- Drawing, on Direct3D 8 --------------------------------------------------------------------
// Pre-transformed vertices (XYZRHW) in the target's pixels, so the viewport is the clip rectangle
// (Direct3D 8 has no scissor test): each draw command gets its own.

struct Vertex {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
};
std::vector<Vertex> g_vertices;

void set_state(void* device)
{
    const auto render_state = [&](DWORD type, DWORD value) { call(device, slot::SetRenderState, type, value); };
    render_state(d3d8::rs::AlphaBlendEnable, TRUE);
    render_state(d3d8::rs::SrcBlend, d3d8::kBlendSrcAlpha);
    render_state(d3d8::rs::DestBlend, d3d8::kBlendInvSrcAlpha);
    const auto stage_state = [&](DWORD type, DWORD value) { call(device, slot::SetTextureStageState, DWORD{0}, type, value); };
    stage_state(d3d8::tss::ColorOp, d3d8::kTopModulate);
    stage_state(d3d8::tss::ColorArg1, d3d8::kTaTexture);
    stage_state(d3d8::tss::ColorArg2, d3d8::kTaDiffuse);
    stage_state(d3d8::tss::AlphaOp, d3d8::kTopModulate);
    stage_state(d3d8::tss::AlphaArg1, d3d8::kTaTexture);
    stage_state(d3d8::tss::AlphaArg2, d3d8::kTaDiffuse);
    stage_state(d3d8::tss::MagFilter, d3d8::kFilterLinear);
    stage_state(d3d8::tss::MinFilter, d3d8::kFilterLinear);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwDiffuseTex1);
}

// Back to what text::begin set, for VanaCore's own drawing after ImGui's.
void reset_state(void* device, unsigned width, unsigned height)
{
    const auto stage_state = [&](DWORD type, DWORD value) { call(device, slot::SetTextureStageState, DWORD{0}, type, value); };
    stage_state(d3d8::tss::ColorOp, d3d8::kTopSelectArg1);
    stage_state(d3d8::tss::ColorArg1, d3d8::kTaTexture);
    stage_state(d3d8::tss::AlphaOp, d3d8::kTopSelectArg1);
    stage_state(d3d8::tss::AlphaArg1, d3d8::kTaTexture);
    stage_state(d3d8::tss::MagFilter, d3d8::kFilterPoint);
    stage_state(d3d8::tss::MinFilter, d3d8::kFilterPoint);
    call(device, slot::SetVertexShader, d3d8::kFvfXyzRhwTex1);
    const d3d8::Viewport viewport{0, 0, width, height, 0.0f, 1.0f};
    call(device, slot::SetViewport, &viewport);
}

// The texture a draw command uses: the font atlas's (made by update_texture), or an addon's or a plugin's own
// IDirect3DTexture8. Null when the atlas's hasn't been made.
void* texture_of(const ImDrawCmd& command)
{
    const ImTextureID id = command.TexRef._TexData != nullptr ? command.TexRef._TexData->TexID : command.TexRef._TexID;
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(id));
}

// ImGui's draw data (the game's pixels) onto the `width` x `height` interface: positions and clip rectangles times
// `sx`, `sy` (its pixels to a game pixel).
void render(void* device, const ImDrawData& data, unsigned width, unsigned height, float sx, float sy)
{
    if (data.Textures != nullptr)
        for (ImTextureData* tex : *data.Textures)
            if (tex->Status != ImTextureStatus_OK)
                update_texture(device, tex);
    set_state(device);
    for (int n = 0; n < data.CmdListsCount; ++n) {
        const ImDrawList& list = *data.CmdLists[n];
        backdrop::before_list(&list); // (a window with a blurred backdrop: what's drawn before it goes into the layer the blur is of)
        g_vertices.resize(static_cast<size_t>(list.VtxBuffer.Size));
        for (int i = 0; i < list.VtxBuffer.Size; ++i) {
            const ImDrawVert& from = list.VtxBuffer.Data[i];
            const std::uint32_t c = from.col; // ABGR to ARGB
            // -0.5: Direct3D 8's pixel centres sit on whole coordinates
            g_vertices[i] = {from.pos.x * sx - 0.5f, from.pos.y * sy - 0.5f, 0.0f, 1.0f,
                             (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) | ((c & 0x000000FFu) << 16), from.uv.x, from.uv.y};
        }
        for (const ImDrawCmd& command : list.CmdBuffer) {
            if (command.UserCallback != nullptr) {
                if (command.UserCallback == ImDrawCallback_ResetRenderState)
                    set_state(device);
                else
                    command.UserCallback(&list, &command);
                continue;
            }
            const float clip_left = command.ClipRect.x * sx, clip_top = command.ClipRect.y * sy;
            const float clip_right = command.ClipRect.z * sx, clip_bottom = command.ClipRect.w * sy;
            const float left = clip_left < 0 ? 0 : std::floor(clip_left);
            const float top = clip_top < 0 ? 0 : std::floor(clip_top);
            const float right = clip_right > width ? static_cast<float>(width) : std::ceil(clip_right);
            const float bottom = clip_bottom > height ? static_cast<float>(height) : std::ceil(clip_bottom);
            void* texture = texture_of(command);
            if (right - left < 1.0f || bottom - top < 1.0f || command.ElemCount < 3 || texture == nullptr ||
                command.VtxOffset >= static_cast<unsigned>(list.VtxBuffer.Size))
                continue;
            const d3d8::Viewport clip{static_cast<DWORD>(left), static_cast<DWORD>(top), static_cast<DWORD>(right - left),
                                      static_cast<DWORD>(bottom - top), 0.0f, 1.0f};
            call(device, slot::SetViewport, &clip);
            call(device, slot::SetTexture, DWORD{0}, texture);
            // the command's vertices from VtxOffset (DrawIndexedPrimitiveUP has no base vertex)
            call(device, slot::DrawIndexedPrimitiveUP, d3d8::kTriangleList, 0u,
                 static_cast<UINT>(list.VtxBuffer.Size - static_cast<int>(command.VtxOffset)), static_cast<UINT>(command.ElemCount / 3),
                 static_cast<const void*>(list.IdxBuffer.Data + command.IdxOffset), d3d8::kFormatIndex16,
                 static_cast<const void*>(g_vertices.data() + command.VtxOffset), static_cast<UINT>(sizeof(Vertex)));
        }
    }
    call(device, slot::SetTexture, DWORD{0}, static_cast<void*>(nullptr));
    reset_state(device, width, height);
}

int button_of(int type)
{
    switch (type + WM_MOUSEFIRST) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return 0;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return 1;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return 2;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK: return 3;
    default: return -1;
    }
}

bool vk_down(int vk)
{
    return (GetKeyState(vk) & 0x8000) != 0;
}

// A key message's key as ImGui's (imgui_impl_win32's mapping): by its virtual key, else (punctuation, which moves
// with the keyboard's layout) by its scan code. Shift, Ctrl and Alt are their left and right keys (key_event).
ImGuiKey key_of(WPARAM vk, LPARAM lparam)
{
    if (vk == VK_RETURN && (HIWORD(lparam) & KF_EXTENDED) != 0)
        return ImGuiKey_KeypadEnter;
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU)
        return ImGuiKey_None;
    switch (LOBYTE(HIWORD(lparam))) {
    case 41: return ImGuiKey_GraveAccent;
    case 12: return ImGuiKey_Minus;
    case 13: return ImGuiKey_Equal;
    case 26: return ImGuiKey_LeftBracket;
    case 27: return ImGuiKey_RightBracket;
    case 86: return ImGuiKey_Oem102;
    case 43: return ImGuiKey_Backslash;
    case 39: return ImGuiKey_Semicolon;
    case 40: return ImGuiKey_Apostrophe;
    case 51: return ImGuiKey_Comma;
    case 52: return ImGuiKey_Period;
    case 53: return ImGuiKey_Slash;
    default: return imgui_legacy::vk_key(static_cast<int>(vk));
    }
}

void add_key(ImGuiIO& io, ImGuiKey key, bool down, int vk, int scancode)
{
    io.AddKeyEvent(key, down);
    io.SetKeyEventNativeData(key, vk, scancode); // GetKeyIndex and the legacy key numbers still work
}

void update_modifiers(ImGuiIO& io)
{
    io.AddKeyEvent(ImGuiMod_Ctrl, vk_down(VK_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, vk_down(VK_SHIFT));
    io.AddKeyEvent(ImGuiMod_Alt, vk_down(VK_MENU));
    io.AddKeyEvent(ImGuiMod_Super, false);
}

// The Win32 key code a key's state is read from (vk_down): each side's own for Shift, Ctrl and Alt (key_vk gives the
// code for either side), or 0 for a key Windows has no code for (the mouse buttons and wheel, a gamepad).
int state_vk(ImGuiKey key)
{
    switch (key) {
    case ImGuiKey_LeftShift: return VK_LSHIFT;
    case ImGuiKey_RightShift: return VK_RSHIFT;
    case ImGuiKey_LeftCtrl: return VK_LCONTROL;
    case ImGuiKey_RightCtrl: return VK_RCONTROL;
    case ImGuiKey_LeftAlt: return VK_LMENU;
    case ImGuiKey_RightAlt: return VK_RMENU;
    default: return imgui_legacy::key_vk(key);
    }
}

// A key ImGui holds down that Windows says is up is released: its key up never reached the game's window, so ImGui
// would keep it pressed, and repeating, until the key is pressed again. Windows may not send the first released
// Shift's key up when both were held; under CrossOver a macOS app switch with a key or Alt down loses the key up,
// and a Left Arrow stuck that way makes every text box type backwards (the repeat pulls the caret back each frame,
// so each character goes in before the last). A key named by its scan code (key_of) is read at its US layout code.
void release_stale_keys(ImGuiIO& io)
{
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const ImGuiKey key = static_cast<ImGuiKey>(k);
        if (!ImGui::IsKeyDown(key))
            continue;
        const int vk = state_vk(key);
        if (vk != 0 && !vk_down(vk))
            add_key(io, key, false, vk, -1);
    }
}

// The window a window's popups (a dropdown, a menu) belong to: a popup is named for its kind, not its window.
const ImGuiWindow* popup_root(const ImGuiWindow* window)
{
    return window->RootWindowPopupTree != nullptr ? window->RootWindowPopupTree : window;
}

bool own_window(const ImGuiWindow* window)
{
    const std::string_view name = popup_root(window)->Name;
    return name.rfind("VanaCore", 0) == 0 || name.find("##vana") != std::string_view::npos;
}

// The window a window belongs to for the order: a popup's opener. A popup begun outside its window's Begin/End (most
// Ashita addons: BeginPopup after End) has the implicit Debug window as its RootWindowPopupTree, not the window it
// opened from, so an open popup is taken to be the window that had the focus when it was opened (RestoreNavWindow).
const ImGuiWindow* order_root(const ImGuiWindow* window)
{
    const ImGuiContext& g = *GImGui;
    for (int depth = 0; depth < 8 && (window->Flags & ImGuiWindowFlags_Popup) != 0; ++depth) {
        const ImGuiWindow* opener = nullptr;
        for (const ImGuiPopupData& popup : g.OpenPopupStack)
            if (popup.Window == window)
                opener = popup.RestoreNavWindow;
        if (opener == nullptr || opener == window)
            break;
        window = opener->RootWindow != nullptr ? opener->RootWindow : opener;
    }
    return popup_root(window);
}

ImGuiID g_picked_addon = 0; // the addon's or plugin's window the player last clicked, over VanaCore's until one of its is

// VanaCore's own windows (/vana, its stats and graphs, its popups) over every addon's and plugin's window that raises
// itself (opens or focuses itself), so they're put back at the front, in the order they were, before the frame is
// drawn (and the next frame's mouse finds them first). A popup is VanaCore's when the window it opened from is, and
// stays over that window in the order. The one addon window the player clicked comes back over them, with its popups,
// until a click lands on one of VanaCore's windows (or it closes).
void raise_own_windows()
{
    ImGuiContext& g = *GImGui;
    if (g.HoveredWindow != nullptr) {
        for (int button = 0; button < ImGuiMouseButton_COUNT; ++button) {
            if (!g.IO.MouseClicked[button])
                continue;
            const ImGuiWindow* root = order_root(g.HoveredWindow->RootWindow);
            g_picked_addon = own_window(root) ? 0 : root->ID;
            break;
        }
    }
    // a window ImGui gave the focus (one opening, as a window of its own, from a click in the picked one: Sidekick's
    // settings) is the picked one from then, or it stays under the window it opened from (reported 2026-11-18)
    static ImGuiID focused = 0;
    const ImGuiID now = g.NavWindow != nullptr ? order_root(g.NavWindow->RootWindow)->ID : 0;
    if (now != focused) {
        focused = now;
        if (now != 0 && !own_window(order_root(g.NavWindow->RootWindow)))
            g_picked_addon = now;
    }
    std::vector<ImGuiWindow*> own;
    std::vector<ImGuiWindow*> picked;
    for (ImGuiWindow* window : g.Windows) {
        if (!window->Active || (window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) != 0)
            continue;
        if (own_window(window))
            own.push_back(window);
        else if (g_picked_addon != 0 && order_root(window)->ID == g_picked_addon && (window->Flags & ImGuiWindowFlags_Popup) == 0)
            picked.push_back(window);
    }
    // its open popups after it, in the order they were opened: always over the window they opened from (reported
    // 2026-11-17: YGO's gear list and export prompt, begun after its End, were raised under its window every frame)
    for (const ImGuiPopupData& popup : g.OpenPopupStack)
        if (popup.Window != nullptr && popup.Window->Active && g_picked_addon != 0 && !own_window(popup.Window) &&
            order_root(popup.Window)->ID == g_picked_addon)
            picked.push_back(popup.Window);
    if (g_picked_addon != 0 && picked.empty())
        g_picked_addon = 0; // closed
    for (ImGuiWindow* window : own)
        ImGui::BringWindowToDisplayFront(window);
    for (ImGuiWindow* window : picked)
        ImGui::BringWindowToDisplayFront(window);
}

} // namespace

void install(const wchar_t* install_dir, const wchar_t* settings_ini)
{
    g_settings = settings_ini != nullptr ? settings_ini : L"";
    g_install = install_dir != nullptr ? install_dir : L"";
    if (!g_install.empty() && g_install.back() != L'\\')
        g_install += L'\\';
    IMGUI_CHECKVERSION();
    g_context = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // as Ashita: its backends' names, ImGui's cursor shapes and mouse moves, textures made as ImGui asks
    io.BackendPlatformName = "vanacore_imgui_impl_ffxi";
    io.BackendRendererName = "vanacore_imgui_impl_dx8";
    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors | ImGuiBackendFlags_HasSetMousePos |
                       ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures |
                       ImGuiBackendFlags_HasGamepad;
    g_ini_file = utf8(g_install + L"config\\imgui.ini");
    CreateDirectoryW((g_install + L"config").c_str(), nullptr);
    io.IniFilename = g_ini_file.c_str();
    // what goes wrong is logged (recovery_log), not drawn over the game or asserted
    io.ConfigErrorRecovery = true;
    io.ConfigErrorRecoveryEnableAssert = false;
    io.ConfigErrorRecoveryEnableDebugLog = false;
    io.ConfigErrorRecoveryEnableTooltip = false;
    io.ConfigDebugHighlightIdConflicts = false;
    g_context->ErrorCallback = &recovery_log;
    ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
    if (platform.Platform_SetImeDataFn != nullptr) {
        g_default_ime = platform.Platform_SetImeDataFn;
        platform.Platform_SetImeDataFn = &set_ime_data;
    }
    QueryPerformanceFrequency(&g_frequency);
    log::write(L"ImGui %hs: for Ashita plugins and addons (settings in config\\imgui.ini)", ImGui::GetVersion());
}

void begin_frame(void* device, HWND window, unsigned width, unsigned height, unsigned game_width, unsigned game_height)
{
    if (g_context == nullptr || g_in_frame || width == 0 || height == 0)
        return;
    // ImGui in the game's pixels, as Ashita's: the interface's scaled to them
    if (game_width == 0 || game_height == 0)
        game_width = width, game_height = height;
    const float game_per_ui_x = static_cast<float>(game_width) / static_cast<float>(width);
    const float game_per_ui_y = static_cast<float>(game_height) / static_cast<float>(height);
    g_game_per_ui_x = game_per_ui_x;
    g_game_per_ui_y = game_per_ui_y;
    layout::set_own_scale(std::sqrt(game_per_ui_x * game_per_ui_y));
    width = game_width;
    height = game_height;
    ImGuiIO& io = ImGui::GetIO();
    if (device != g_device) {
        release();
        g_device = device;
    }
    soulspire::on_gui_device(g_device);
    // the IME's window, placed at the text box being typed in
    ImGui::GetMainViewport()->PlatformHandle = ImGui::GetMainViewport()->PlatformHandleRaw = window;
    if (!g_fonts_added)
        add_fonts(io);

    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    io.DeltaTime = g_last.QuadPart != 0 && now.QuadPart > g_last.QuadPart
                       ? static_cast<float>(now.QuadPart - g_last.QuadPart) / static_cast<float>(g_frequency.QuadPart)
                       : 1.0f / 60.0f;
    g_last = now;
    update_modifiers(io);
    release_stale_keys(io);
    if (io.WantSetMousePos)
        set_mouse_position(window, io.MousePos, width, height);
    ImVec2 mouse = mouse_position(window, width, height);
    // over a game window drawn over ImGui's (raise.cpp), none of ImGui's is under it; not while it holds a press (a drag)
    if (g_taken == 0 && covered(mouse.x, mouse.y))
        mouse = ImVec2(-FLT_MAX, -FLT_MAX);
    const ImVec2 on_screen = mouse; // (the HUD Manager's Addon View boxes are where they're drawn)
    layout::before_new_frame(mouse.x, mouse.y); // over a scaled window: in its own pixels
    io.AddMousePosEvent(mouse.x, mouse.y);
    update_cursor(io);

    {
        // the controller navigates an addon's window while it has it (vana.gamepad.nav)
        ImGuiIO& nav_io = ImGui::GetIO();
        if (wants_gamepad())
            nav_io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
        else
            nav_io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    }
    // ImGui keeps a window it places (not one its addon places each frame) this much on the screen; its 19 px default
    // left /vana stats at x 2541 on a 2560-wide screen, opened but a sliver. A 20th of the screen's height (80 px at
    // 1600, 54 at 1080) shows a window's title bar to drag it back. Set each frame: an addon may have set the style.
    // VanaCore's own windows only: an addon's or plugin's Begin has ImGui's 19 px (layout.cpp addon_padding).
    {
        const float keep = std::max(48.0f, std::floor(static_cast<float>(height) / 20.0f));
        ImGui::GetStyle().DisplayWindowPadding = ImVec2(keep, keep);
    }
    ImGui::NewFrame();
    layout::after_new_frame();
    addonbox::after_new_frame(on_screen.x, on_screen.y); // over an addon's box (Addon View), its window doesn't have the mouse
    if (g_gamepad_frames > 0)
        --g_gamepad_frames;
    g_in_frame = true;
}

void end_frame()
{
    if (!g_in_frame)
        return;
    mapwin::gui_frame(); // [client] map.*: Shift + drag on the map's box, and its border
    layout::end_frame(); // addons' and plugins' windows drawn at their scale, and the Layout tab's outline
    addonbox::end_frame(); // the HUD Manager's Addon View: a box over each, in its window's draw list
    raise_own_windows();
    ImGui::Render(); // (anything left open is closed first: recovery_log)
    g_in_frame = false;
    publish();
}

void draw(void* device, unsigned width, unsigned height)
{
    if (g_context == nullptr || !shown() || device != g_device)
        return;
    const ImDrawData* data = ImGui::GetDrawData();
    if (data == nullptr || !data->Valid || data->CmdListsCount == 0 || data->DisplaySize.x <= 0 || data->DisplaySize.y <= 0)
        return;
    render(device, *data, width, height, static_cast<float>(width) / data->DisplaySize.x,
           static_cast<float>(height) / data->DisplaySize.y);
}

void release()
{
    soulspire::on_gui_release();
    release_textures();
    g_texture_failed = false;
    g_device = nullptr;
}

void* context()
{
    return g_context;
}

bool in_frame()
{
    return g_in_frame;
}

// (from XI on Anything, rubymatrix/xi-on-anything 11d85de: each addon's frame recovered on its own)
namespace {
ImGuiErrorRecoveryState g_addon_state;
bool g_addon_saved = false;
} // namespace

void addon_frame_begin()
{
    g_addon_saved = g_in_frame && ImGui::GetCurrentContext() != nullptr;
    if (g_addon_saved)
        ImGui::ErrorRecoveryStoreState(&g_addon_state);
}

void addon_frame_end()
{
    if (g_addon_saved && g_in_frame && ImGui::GetCurrentContext() != nullptr)
        ImGui::ErrorRecoveryTryToRecoverState(&g_addon_state); // what it left open is logged (recovery_log)
    g_addon_saved = false;
}

bool visible()
{
    return g_visible;
}

void set_visible(bool visible)
{
    g_visible = visible;
}

bool on_mouse(int type, int x, int y, int delta)
{
    if (g_context == nullptr)
        return false;
    ImGuiIO& io = ImGui::GetIO();
    const UINT message = static_cast<UINT>(type) + WM_MOUSEFIRST;
    std::string owner;
    const bool over = blocks_mouse(x, y, &owner);
    // Set the pointer to where this click or wheel turn happened, before its button/wheel event, so ImGui evaluates it
    // at this position and not the one from the frame before. x and y are interface pixels (in_ui), ImGui's the game's,
    // the space begin_frame feeds each frame. Without this, a click after a fast move hits whatever was under the old
    // position. Over a game window drawn over ImGui's, nowhere ImGui has a window (as begin_frame has it). Over a scaled
    // window (VanaCore's own when the game's pixels aren't the interface's, Addons > Layout's scale), in its own pixels as
    // begin_frame has it: unscaled, a press on the /vana window missed its buttons and dragged the window instead.
    float game_x = static_cast<float>(x) * g_game_per_ui_x, game_y = static_cast<float>(y) * g_game_per_ui_y;
    if (g_taken == 0 && covered(game_x, game_y)) {
        io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
    } else {
        layout::mouse_at(game_x, game_y);
        io.AddMousePosEvent(game_x, game_y);
    }
    if (message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL) {
        if (!over)
            return false;
        g_last_owner = owner;
        const float wheel = static_cast<float>(delta) / WHEEL_DELTA;
        if (message == WM_MOUSEWHEEL)
            io.AddMouseWheelEvent(0.0f, wheel);
        else
            io.AddMouseWheelEvent(-wheel, 0.0f);
        return true;
    }
    const int button = button_of(type);
    if (button < 0 || button >= kButtons)
        return false;
    const bool press = message == WM_LBUTTONDOWN || message == WM_RBUTTONDOWN || message == WM_MBUTTONDOWN ||
                       message == WM_XBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDBLCLK ||
                       message == WM_MBUTTONDBLCLK || message == WM_XBUTTONDBLCLK;
    // ImGui sees every press (one outside its windows closes its popups), but takes only those over them
    io.AddMouseButtonEvent(button, press);
    if (press) {
        if (over) {
            g_taken |= 1u << button;
            g_taken_owner[button] = owner;
            g_last_owner = owner;
        }
        return over;
    }
    const bool taken = (g_taken & (1u << button)) != 0;
    if (taken)
        g_last_owner = g_taken_owner[button];
    g_taken &= ~(1u << button);
    return taken;
}

// A key's ImGuiKey events, as imgui_impl_win32 sends them (Shift, Ctrl and Alt as their left and right keys too).
static void key_event(ImGuiIO& io, WPARAM wparam, LPARAM lparam, bool down)
{
    update_modifiers(io);
    const int vk = static_cast<int>(wparam);
    const int scancode = LOBYTE(HIWORD(lparam));
    const ImGuiKey key = key_of(wparam, lparam);
    if (key == ImGuiKey_PrintScreen && !down) // Windows sends no key down for it
        add_key(io, key, true, vk, scancode);
    if (key != ImGuiKey_None)
        add_key(io, key, down, vk, scancode);
    const auto sides = [&](int left_vk, ImGuiKey left, int right_vk, ImGuiKey right) {
        if (vk_down(left_vk) == down)
            add_key(io, left, down, left_vk, scancode);
        if (vk_down(right_vk) == down)
            add_key(io, right, down, right_vk, scancode);
    };
    if (vk == VK_SHIFT)
        sides(VK_LSHIFT, ImGuiKey_LeftShift, VK_RSHIFT, ImGuiKey_RightShift);
    else if (vk == VK_CONTROL)
        sides(VK_LCONTROL, ImGuiKey_LeftCtrl, VK_RCONTROL, ImGuiKey_RightCtrl);
    else if (vk == VK_MENU)
        sides(VK_LMENU, ImGuiKey_LeftAlt, VK_RMENU, ImGuiKey_RightAlt);
}

bool on_key(UINT message, WPARAM wparam, LPARAM lparam)
{
    if (g_context == nullptr)
        return false;
    ImGuiIO& io = ImGui::GetIO();
    const bool wants = g_wants_keyboard;
    switch (message) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wparam < 256) {
            key_event(io, wparam, lparam, true);
            g_keys_taken[wparam] = wants;
        }
        return wants;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wparam < 256) {
            key_event(io, wparam, lparam, false);
            const bool taken = g_keys_taken[wparam];
            g_keys_taken[wparam] = false;
            return taken;
        }
        return false;
    case WM_CHAR: {
        if (!wants)
            return false;
        // The game's window is an ANSI one: characters come in its code page, a double-byte one in
        // two messages.
        const BYTE byte = static_cast<BYTE>(wparam);
        if (wparam > 0xFF) {
            io.AddInputCharacterUTF16(static_cast<ImWchar16>(wparam));
            return true;
        }
        char bytes[2] = {static_cast<char>(byte), 0};
        int count = 1;
        if (g_lead != 0) {
            bytes[0] = static_cast<char>(g_lead);
            bytes[1] = static_cast<char>(byte);
            count = 2;
            g_lead = 0;
        } else if (IsDBCSLeadByte(byte)) {
            g_lead = byte;
            return true;
        }
        wchar_t wide[2] = {};
        const int n = MultiByteToWideChar(CP_ACP, 0, bytes, count, wide, 2);
        for (int i = 0; i < n; ++i)
            io.AddInputCharacterUTF16(static_cast<ImWchar16>(wide[i]));
        return true;
    }
    default:
        return false;
    }
}

bool blocks_mouse(int ui_x, int ui_y)
{
    return blocks_mouse(ui_x, ui_y, nullptr);
}

bool blocks_mouse(int ui_x, int ui_y, std::string* owner)
{
    // the windows' rectangles are ImGui's, in the game's pixels
    const float x = static_cast<float>(ui_x) * g_game_per_ui_x, y = static_cast<float>(ui_y) * g_game_per_ui_y;
    bool over = false;
    AcquireSRWLockShared(&g_rects_lock);
    over = g_capture_all;
    if (owner != nullptr)
        *owner = g_capture_all ? g_capture_owner : std::string();
    // a game window drawn over ImGui's has the mouse there
    bool game = false;
    for (size_t i = 0; !over && !game && i < g_covered.size(); ++i) {
        const RECT& r = g_covered[i];
        game = x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }
    // back to front: whose window is there is the last one's, on top
    for (size_t i = 0; !g_capture_all && !game && i < g_rects.size() && (owner != nullptr || !over); ++i) {
        const RECT& r = g_rects[i];
        if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) {
            over = true;
            if (owner != nullptr)
                *owner = g_rects_owner[i];
        }
    }
    ReleaseSRWLockShared(&g_rects_lock);
    return over;
}

void take_mouse(float x0, float y0, float x1, float y1)
{
    if (g_in_frame)
        g_taken_rects.push_back({static_cast<LONG>(std::floor(x0)), static_cast<LONG>(std::floor(y0)),
                                 static_cast<LONG>(std::ceil(x1)), static_cast<LONG>(std::ceil(y1))});
}

const std::string& taken_by()
{
    return g_last_owner;
}

void set_covered(const std::vector<RECT>& rects)
{
    if (rects.empty() && g_covered.empty()) // the game's thread is the only one that changes it
        return;
    AcquireSRWLockExclusive(&g_rects_lock);
    g_covered = rects;
    ReleaseSRWLockExclusive(&g_rects_lock);
}

bool game_window_at(float x, float y)
{
    return covered(x, y);
}

void shown_windows(const char* prefix, std::vector<RECT>& out)
{
    if (g_context == nullptr || prefix == nullptr || !shown())
        return;
    const size_t length = std::strlen(prefix);
    const ImGuiContext& g = *g_context;
    for (const ImGuiWindow* window : g.Windows) {
        // the last frame's: Active until the next frame begins, then WasActive
        const bool was_shown = g_in_frame ? window->WasActive : window->Active;
        if (!was_shown || window->Hidden || window->Name == nullptr || std::strncmp(window->Name, prefix, length) != 0)
            continue;
        ImRect r = window->Rect();
        layout::visual_rect(window, r.Min.x, r.Min.y, r.Max.x, r.Max.y); // as it's drawn (Addons > Layout's scale)
        out.push_back({static_cast<LONG>(std::floor(r.Min.x)), static_cast<LONG>(std::floor(r.Min.y)),
                       static_cast<LONG>(std::ceil(r.Max.x)), static_cast<LONG>(std::ceil(r.Max.y))});
    }
}

void shown_windows_of(const std::vector<std::string>& owners, std::vector<RECT>& out)
{
    if (g_context == nullptr || owners.empty() || !shown())
        return;
    const ImGuiContext& g = *g_context;
    for (const ImGuiWindow* window : g.Windows) {
        const bool was_shown = g_in_frame ? window->WasActive : window->Active;
        if (!was_shown || window->Hidden || window->Name == nullptr || (window->Flags & ImGuiWindowFlags_ChildWindow) != 0)
            continue;
        const std::string owner = owner_of_window(window);
        if (std::find(owners.begin(), owners.end(), owner) == owners.end())
            continue;
        ImRect r = window->Rect();
        layout::visual_rect(window, r.Min.x, r.Min.y, r.Max.x, r.Max.y); // as it's drawn (Addons > Layout's scale)
        out.push_back({static_cast<LONG>(std::floor(r.Min.x)), static_cast<LONG>(std::floor(r.Min.y)),
                       static_cast<LONG>(std::ceil(r.Max.x)), static_cast<LONG>(std::ceil(r.Max.y))});
    }
}

void own_windows(std::vector<RECT>& out)
{
    if (g_context == nullptr || !shown())
        return;
    const ImGuiContext& g = *g_context;
    for (const ImGuiWindow* window : g.Windows) {
        const bool was_shown = g_in_frame ? window->WasActive : window->Active;
        if (!was_shown || window->Hidden || window->Name == nullptr ||
            (window->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) != 0)
            continue;
        const ImGuiWindow* root = window->RootWindowPopupTree != nullptr ? window->RootWindowPopupTree : window;
        const std::string_view name = root->Name;
        if (name.rfind("VanaCore", 0) != 0 && name.find("##vana") == std::string_view::npos)
            continue;
        ImRect r = window->Rect();
        layout::visual_rect(window, r.Min.x, r.Min.y, r.Max.x, r.Max.y); // as it's drawn (Addons > Layout's scale)
        out.push_back({static_cast<LONG>(std::floor(r.Min.x)), static_cast<LONG>(std::floor(r.Min.y)),
                       static_cast<LONG>(std::ceil(r.Max.x)), static_cast<LONG>(std::ceil(r.Max.y))});
    }
}

bool wants_keyboard()
{
    return g_wants_keyboard;
}

void gamepad_key(int key, bool down, float analog)
{
    if (g_context == nullptr)
        return;
    ImGuiIO& io = g_context->IO;
    if (analog >= 0.0f)
        io.AddKeyAnalogEvent(static_cast<ImGuiKey>(key), down, analog);
    else
        io.AddKeyEvent(static_cast<ImGuiKey>(key), down);
}

void want_gamepad(bool on)
{
    g_gamepad_frames = on ? 2 : 0;
}

bool wants_gamepad()
{
    return g_gamepad_frames > 0 && shown();
}

void set_cursor_override(bool visible)
{
    g_cursor_override.store(visible, std::memory_order_relaxed);
    if (visible && g_context != nullptr)
        g_context->IO.MouseDrawCursor = false;
}

void restore_cursor()
{
    g_cursor_override.store(false, std::memory_order_relaxed);
    if (g_context != nullptr)
        g_context->IO.MouseDrawCursor = false;
    g_cursor = -1;
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
}

} // namespace vanacore::gui
