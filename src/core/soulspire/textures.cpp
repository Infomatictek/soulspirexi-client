#include "textures.hpp"

#include <windows.h>

#include <cstring>
#include <unordered_map>

#include "../d3d8.hpp"
#include "../image.hpp"

namespace vanacore::soulspire::textures {
namespace {

struct Entry {
    void* tex = nullptr;
    int width = 0;
    int height = 0;
    std::uint64_t bytes = 0;
    std::string path;
};

void* g_device = nullptr;
std::unordered_map<std::uint32_t, Entry> g_by_id;
std::unordered_map<std::string, std::uint32_t> g_by_path;
std::uint64_t g_total_bytes = 0;
std::uint32_t g_hits = 0;
std::uint32_t g_misses = 0;
std::uint32_t g_failures = 0;

std::wstring utf8_to_wide(const std::string& s)
{
    if (s.empty())
        return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::string normalize_key(const std::string& path)
{
    std::string out = path;
    for (char& c : out) {
        if (c == '/')
            c = '\\';
        else if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

} // namespace

void set_device(void* device)
{
    if (device != g_device) {
        release_all();
        g_device = device;
    }
}

void release_all()
{
    for (auto& [_, entry] : g_by_id) {
        if (entry.tex != nullptr) {
            d3d8::call<ULONG>(entry.tex, d3d8::slot::Release);
            entry.tex = nullptr;
        }
    }
    g_by_id.clear();
    g_by_path.clear();
    g_total_bytes = 0;
    g_device = nullptr;
}

std::uint32_t release_all_count()
{
    const auto count = static_cast<std::uint32_t>(g_by_id.size());
    void* saved_device = g_device;
    release_all();
    g_device = saved_device;
    return count;
}

bool load(const std::string& utf8_path, TextureInfo& out, std::string& err)
{
    out = {};
    if (utf8_path.empty()) {
        err = "empty path";
        ++g_failures;
        return false;
    }
    if (g_device == nullptr) {
        err = "graphics device not ready";
        ++g_failures;
        return false;
    }

    const std::string key = normalize_key(utf8_path);
    if (auto it = g_by_path.find(key); it != g_by_path.end()) {
        if (auto entry_it = g_by_id.find(it->second); entry_it != g_by_id.end()) {
            ++g_hits;
            out.id = entry_it->first;
            out.width = entry_it->second.width;
            out.height = entry_it->second.height;
            return true;
        }
        g_by_path.erase(it);
    }

    ++g_misses;
    const std::wstring wpath = utf8_to_wide(utf8_path);
    if (wpath.empty()) {
        err = "invalid utf8 path";
        ++g_failures;
        return false;
    }

    image::Pixels pixels{};
    if (!image::decode_file(wpath, pixels) || pixels.width <= 0 || pixels.height <= 0 || pixels.argb.empty()) {
        err = "decode failed";
        ++g_failures;
        return false;
    }

    void* tex = nullptr;
    const UINT w = static_cast<UINT>(pixels.width);
    const UINT h = static_cast<UINT>(pixels.height);
    if (FAILED(d3d8::call(g_device, d3d8::slot::CreateTexture, w, h, 1u, DWORD{0},
                          d3d8::kFormatA8R8G8B8, d3d8::kPoolManaged, &tex)) ||
        tex == nullptr) {
        err = "CreateTexture failed";
        ++g_failures;
        return false;
    }

    d3d8::LockedRect locked{};
    if (FAILED(d3d8::call(tex, d3d8::slot::TextureLockRect, 0u, &locked, static_cast<const RECT*>(nullptr), DWORD{0})) ||
        locked.bits == nullptr) {
        d3d8::call<ULONG>(tex, d3d8::slot::Release);
        err = "LockRect failed";
        ++g_failures;
        return false;
    }

    const std::size_t row_bytes = static_cast<std::size_t>(w) * 4u;
    for (UINT y = 0; y < h; ++y) {
        BYTE* dst = static_cast<BYTE*>(locked.bits) + static_cast<std::size_t>(y) * static_cast<std::size_t>(locked.pitch);
        const std::uint32_t* src = pixels.argb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(w);
        std::memcpy(dst, src, row_bytes);
    }
    d3d8::call(tex, d3d8::slot::TextureUnlockRect, 0u);

    const auto id = static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(tex));
    const std::uint64_t bytes = static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) * 4ull;
    g_by_id[id] = Entry{tex, pixels.width, pixels.height, bytes, key};
    g_by_path[key] = id;
    g_total_bytes += bytes;

    out.id = id;
    out.width = pixels.width;
    out.height = pixels.height;
    return true;
}

bool free_by_id(std::uint32_t id)
{
    auto it = g_by_id.find(id);
    if (it == g_by_id.end())
        return false;
    if (it->second.tex != nullptr)
        d3d8::call<ULONG>(it->second.tex, d3d8::slot::Release);
    if (g_total_bytes >= it->second.bytes)
        g_total_bytes -= it->second.bytes;
    else
        g_total_bytes = 0;
    g_by_path.erase(it->second.path);
    g_by_id.erase(it);
    return true;
}

TextureStats stats()
{
    TextureStats s{};
    s.loaded = static_cast<std::uint32_t>(g_by_id.size());
    s.bytes = g_total_bytes;
    s.hits = g_hits;
    s.misses = g_misses;
    s.failures = g_failures;
    s.device_ready = (g_device != nullptr);
    return s;
}

} // namespace vanacore::soulspire::textures
