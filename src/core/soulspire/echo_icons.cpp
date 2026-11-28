#include "echo_icons.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../d3d8.hpp"
#include "../image.hpp"
#include "../log.hpp"
#include "../nameplates.hpp"
#include "../text.hpp"
#include "../uispace.hpp"
#include "state.hpp"

namespace vanacore::soulspire::echo_icons {
namespace {

constexpr std::ptrdiff_t kEntityUniqueNo = 0x74;
constexpr int kMipSizes = 4;
constexpr std::size_t kMaxPartyRows = 6;

struct Vertex {
    float x, y, z, rhw;
    DWORD colour;
    float u, v;
};

struct CachedIcon {
    bool attempted = false;
    text::Sprite sizes[kMipSizes];
    int count = 0;
};

struct PartyIconDraw {
    int x = 0;
    int y = 0;
    std::string icon_asset;
};

std::mutex g_cache_mutex;
std::wstring g_install_dir;
std::unordered_map<std::string, std::unique_ptr<CachedIcon>> g_icons;
void* g_device = nullptr;

// Per-nameplate active Echo state (game render thread only).
bool g_plate_active = false;
bool g_plate_drawn = false;
std::string g_plate_icon_asset;

// Per-frame queued native party HUD Echo icons (game render thread only).
PartyIconDraw g_party_draws[kMaxPartyRows];
std::size_t g_party_draw_count = 0;

// Alpha-weighted 2x downscale (matches gmicons.cpp::halved for crisp non-shimmering scaling).
image::Pixels halved(const image::Pixels& in)
{
    image::Pixels out;
    out.width = std::max(1, in.width / 2);
    out.height = std::max(1, in.height / 2);
    out.argb.resize(static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height));
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            unsigned sum[4] = {};
            for (int k = 0; k < 4; ++k) {
                const int sx = std::min(in.width - 1, x * 2 + (k & 1));
                const int sy = std::min(in.height - 1, y * 2 + k / 2);
                const std::uint32_t p = in.argb[static_cast<std::size_t>(sy) * static_cast<std::size_t>(in.width) +
                                                static_cast<std::size_t>(sx)];
                const unsigned a = p >> 24;
                sum[0] += a;
                for (int c = 1; c < 4; ++c)
                    sum[c] += (p >> (24 - 8 * c) & 0xFFu) * a;
            }
            std::uint32_t pixel = ((sum[0] + 2u) / 4u) << 24;
            for (int c = 1; c < 4 && sum[0] > 0; ++c)
                pixel |= std::min(255u, (sum[c] + sum[0] / 2u) / sum[0]) << (24 - 8 * c);
            out.argb[static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                     static_cast<std::size_t>(x)] = pixel;
        }
    }
    return out;
}

std::wstring to_wide_relpath(std::string_view rel)
{
    std::wstring out;
    out.reserve(rel.size());
    for (char c : rel) {
        if (c == '/')
            out.push_back(L'\\');
        else if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7F)
            out.push_back(static_cast<wchar_t>(c));
    }
    return out;
}

bool decode_shared_icon_file(const std::wstring& install_dir, std::string_view icon_asset, image::Pixels& out)
{
    if (install_dir.empty() || icon_asset.empty())
        return false;
    const std::wstring rel = to_wide_relpath(icon_asset);
    const std::wstring candidates[3] = {
        install_dir + L"\\addons\\soulspire_core\\" + rel,
        install_dir + L"\\addons\\vanacore\\soulspire_core\\" + rel,
        install_dir + L"\\" + rel,
    };
    bool ok = false;
    // Decode on a worker thread so the game thread's COM apartment stays untouched (matches gmicons.cpp).
    std::thread([&] {
        for (const auto& path : candidates) {
            if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES && image::decode_file(path, out)) {
                ok = true;
                return;
            }
        }
    }).join();
    return ok;
}

text::Sprite* get_sized_sprite(const std::string& icon_asset, float pixels, void* device)
{
    if (icon_asset.empty() || device == nullptr)
        return nullptr;

    std::lock_guard lock(g_cache_mutex);
    if (device != g_device) {
        for (auto& [key, entry] : g_icons) {
            if (entry) {
                for (text::Sprite& sp : entry->sizes)
                    sp.release();
            }
        }
        g_device = device;
    }

    auto& ptr = g_icons[icon_asset];
    if (!ptr)
        ptr = std::make_unique<CachedIcon>();

    CachedIcon& icon = *ptr;
    if (!icon.attempted) {
        icon.attempted = true;
        image::Pixels px;
        if (decode_shared_icon_file(g_install_dir, icon_asset, px) && !px.argb.empty()) {
            image::Pixels cur = std::move(px);
            while (icon.count < kMipSizes && !cur.argb.empty()) {
                image::Pixels next = (cur.width > 1 && cur.height > 1) ? halved(cur) : image::Pixels{};
                icon.sizes[icon.count++].set_image(std::move(cur.argb), cur.width, cur.height);
                cur = std::move(next);
            }
        }
    }

    if (icon.count <= 0)
        return nullptr;

    int idx = 0;
    while (idx + 1 < icon.count && static_cast<float>(icon.sizes[idx + 1].height()) >= pixels)
        ++idx;

    text::Sprite* sprite = &icon.sizes[idx];
    if (!sprite->upload(device) || sprite->texture() == nullptr || sprite->texture_width() == 0 ||
        sprite->texture_height() == 0) {
        return nullptr;
    }
    return sprite;
}

DWORD shown_alpha(void* device, DWORD letter)
{
    DWORD op = 0;
    d3d8::call(device, d3d8::slot::GetTextureStageState, DWORD{0}, d3d8::tss::AlphaOp, &op);
    if (op == d3d8::kTopSelectArg1 || op == d3d8::kTopDisable)
        return 0xFFFFFFFFu;
    const float times = op == d3d8::kTopModulate4x ? 4.0f : op == d3d8::kTopModulate2x ? 2.0f : 1.0f;
    const float alpha = std::min(255.0f, static_cast<float>(letter >> 24) * times);
    return (static_cast<DWORD>(std::lround(alpha)) << 24) | 0xFFFFFFu;
}

void draw_picture_quad(void* renderer, DrawFn draw, void* device, const text::Sprite& sprite, const Vertex (&quad)[4])
{
    using d3d8::call;
    namespace slot = d3d8::slot;
    namespace tss = d3d8::tss;
    DWORD alpha_ref = 0;
    call(device, slot::GetRenderState, d3d8::rs::AlphaRef, &alpha_ref);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, DWORD{1});

    struct Saved {
        DWORD stage, type, wanted, value;
    };
    Saved saved[] = {
        {0, tss::ColorOp, d3d8::kTopModulate, 0},   {0, tss::ColorArg1, d3d8::kTaTexture, 0},
        {0, tss::ColorArg2, d3d8::kTaDiffuse, 0},   {0, tss::AlphaOp, d3d8::kTopModulate, 0},
        {0, tss::AlphaArg1, d3d8::kTaTexture, 0},   {0, tss::AlphaArg2, d3d8::kTaDiffuse, 0},
        {0, tss::MagFilter, d3d8::kFilterLinear, 0}, {0, tss::MinFilter, d3d8::kFilterLinear, 0},
        {0, tss::MipFilter, d3d8::kFilterNone, 0},  {0, tss::AddressU, d3d8::kAddressClamp, 0},
        {0, tss::AddressV, d3d8::kAddressClamp, 0}, {1, tss::ColorOp, d3d8::kTopDisable, 0},
        {1, tss::AlphaOp, d3d8::kTopDisable, 0},
    };
    for (Saved& s : saved) {
        call(device, slot::GetTextureStageState, s.stage, s.type, &s.value);
        call(device, slot::SetTextureStageState, s.stage, s.type, s.wanted);
    }
    void* prev_tex = nullptr;
    call(device, slot::GetTexture, DWORD{0}, &prev_tex);
    call(device, slot::SetTexture, DWORD{0}, sprite.texture());
    draw(renderer, nullptr, d3d8::kTriangleStrip, 2u, static_cast<const void*>(quad), static_cast<unsigned>(sizeof(Vertex)));
    call(device, slot::SetTexture, DWORD{0}, prev_tex);
    if (prev_tex != nullptr)
        call<ULONG>(prev_tex, slot::Release);
    for (const Saved& s : saved)
        call(device, slot::SetTextureStageState, s.stage, s.type, s.value);
    if (alpha_ref > 1)
        call(device, slot::SetRenderState, d3d8::rs::AlphaRef, alpha_ref);
}

} // namespace

void set_install_dir(std::wstring install_dir)
{
    std::lock_guard lock(g_cache_mutex);
    g_install_dir = std::move(install_dir);
}

NameScope::NameScope(const void* entity) noexcept
{
    g_plate_active = false;
    g_plate_drawn = false;
    g_plate_icon_asset.clear();
    if (entity == nullptr || !engine().has_active_echoes())
        return;

    std::uint32_t entity_id = 0;
    std::memcpy(&entity_id, static_cast<const char*>(entity) + kEntityUniqueNo, sizeof(entity_id));
    if (entity_id == 0)
        return;

    ActiveEchoIdentity echo{};
    if (!engine().lookup_active_echo_by_id(entity_id, echo) || echo.icon_asset.empty())
        return;

    g_plate_active = true;
    g_plate_icon_asset = std::move(echo.icon_asset);
}

NameScope::~NameScope() noexcept
{
    g_plate_active = false;
    g_plate_drawn = false;
    g_plate_icon_asset.clear();
}

void draw_nameplate_icon(void* renderer, DrawFn draw, unsigned prim, unsigned count,
                         const void* vertices, unsigned stride) noexcept
{
    if (!g_plate_active || g_plate_drawn || draw == nullptr || prim != d3d8::kTriangleStrip ||
        count != 2u || stride != sizeof(Vertex) || vertices == nullptr) {
        return;
    }
    nameplates::Pass pass;
    if (!nameplates::pass(pass) || pass.device == nullptr)
        return;

    const Vertex* const letter = static_cast<const Vertex*>(vertices);
    float left = letter[0].x, top = letter[0].y;
    float right = left, bottom = top;
    for (int i = 1; i < 4; ++i) {
        left = std::min(left, letter[i].x);
        right = std::max(right, letter[i].x);
        top = std::min(top, letter[i].y);
        bottom = std::max(bottom, letter[i].y);
    }
    const float height = (bottom - top) * 1.15f;
    if (!(height > 1.0f))
        return;

    const text::Sprite* const sprite = get_sized_sprite(g_plate_icon_asset, height * pass.ky, pass.device);
    if (sprite == nullptr) {
        g_plate_drawn = true;
        return;
    }

    const float width = (pass.kx > 0.0001f) ? (height * pass.ky / pass.kx) : height;
    const float gap = std::max(2.0f, width * 0.22f);
    const float middle = (top + bottom) * 0.5f;
    const float x1 = left - gap;
    const float x0 = x1 - width;
    const float y0 = middle - height * 0.5f;
    const float y1 = middle + height * 0.5f;
    const float u = static_cast<float>(sprite->width()) / static_cast<float>(sprite->texture_width());
    const float v = static_cast<float>(sprite->height()) / static_cast<float>(sprite->texture_height());
    const DWORD colour = shown_alpha(pass.device, letter[0].colour);

    const Vertex quad[4] = {
        {x0, y0, letter[0].z, letter[0].rhw, colour, 0.0f, 0.0f},
        {x1, y0, letter[1].z, letter[1].rhw, colour, u, 0.0f},
        {x0, y1, letter[2].z, letter[2].rhw, colour, 0.0f, v},
        {x1, y1, letter[3].z, letter[3].rhw, colour, u, v},
    };
    draw_picture_quad(renderer, draw, pass.device, *sprite, quad);
    g_plate_drawn = true;
}

std::string build_party_echo_label(std::string_view name, std::string_view abbr, std::size_t max_chars)
{
    if (name.empty())
        return {};
    if (abbr.empty() || max_chars < 6)
        return std::string(name.substr(0, max_chars));

    // Format "<Name> [<ABBR>]" while keeping total length <= max_chars so HP/MP/TP columns never clip.
    const std::string suffix = " [" + std::string(abbr.substr(0, 3)) + "]";
    if (name.size() + suffix.size() <= max_chars)
        return std::string(name) + suffix;
    const std::size_t keep = (max_chars > suffix.size()) ? (max_chars - suffix.size()) : 0;
    if (keep < 3)
        return std::string(name.substr(0, max_chars));
    return std::string(name.substr(0, keep)) + suffix;
}

const char* format_party_echo_name(int x, int y, const char* text, char* buf,
                                   std::size_t buf_sz, int& out_x) noexcept
{
    out_x = x;
    if (text == nullptr || text[0] == '\0' || !engine().has_active_echoes())
        return text;

    ActiveEchoIdentity echo{};
    if (!engine().lookup_active_echo_by_name(text, echo))
        return text;

    if (!echo.icon_asset.empty() && g_party_draw_count < kMaxPartyRows) {
        g_party_draws[g_party_draw_count++] = PartyIconDraw{x, y, echo.icon_asset};
        out_x = x + 14;
    }

    if (buf != nullptr && buf_sz > 1 && !echo.abbr.empty()) {
        const std::string formatted = build_party_echo_label(text, echo.abbr, std::min<std::size_t>(15, buf_sz - 1));
        std::memcpy(buf, formatted.data(), formatted.size());
        buf[formatted.size()] = '\0';
        return buf;
    }
    return text;
}

void draw_party_icons(void* device, unsigned width, unsigned height) noexcept
{
    const std::size_t count = g_party_draw_count;
    g_party_draw_count = 0;
    if (count == 0 || device == nullptr)
        return;

    constexpr float kIconUnits = 12.0f;
    for (std::size_t i = 0; i < count; ++i) {
        const PartyIconDraw& row = g_party_draws[i];
        float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f, scale = 0.0f;
        const float ux = static_cast<float>(row.x);
        const float uy = static_cast<float>(row.y) + 1.0f;
        if (!uispace::to_pixels(ux, uy, width, height, left, top, scale) ||
            !uispace::to_pixels(ux + kIconUnits, uy + kIconUnits, width, height, right, bottom, scale)) {
            continue;
        }
        const float tall = bottom - top;
        if (!(tall > 1.0f))
            continue;
        const text::Sprite* const sprite = get_sized_sprite(row.icon_asset, tall, device);
        if (sprite == nullptr)
            continue;
        const float times = tall / static_cast<float>(sprite->height());
        const float wide = static_cast<float>(sprite->width()) * times;
        sprite->draw(device, static_cast<int>(std::lround((left + right - wide) * 0.5f)),
                     static_cast<int>(std::lround(top)), 1.0f, times);
    }
}

} // namespace vanacore::soulspire::echo_icons
