#pragma once

#include <cstdint>
#include <string>

// VanaCore-native ImGui texture loader and lifetime manager for Soulspire Core.
// Decodes PNG/BMP/JPEG/GIF via VanaCore's WIC image::decode_file() and uploads directly
// to the active D3D8 device (D3DPOOL_MANAGED) with automatic cleanup on device release
// and shutdown. Zero Ashita or d3dx8/d3d8 FFI dependencies.
namespace vanacore::soulspire::textures {

struct TextureInfo {
    std::uint32_t id = 0;
    int width = 0;
    int height = 0;
};

struct TextureStats {
    std::uint32_t loaded = 0;
    std::uint64_t bytes = 0;
    std::uint32_t hits = 0;
    std::uint32_t misses = 0;
    std::uint32_t failures = 0;
    bool device_ready = false;
};

// Called from gui::begin_frame() / gui::release() on the game thread.
void set_device(void* device);
void release_all();

// Loads (or returns cached) texture from a UTF-8 file path. Game thread only.
bool load(const std::string& utf8_path, TextureInfo& out, std::string& err);
bool free_by_id(std::uint32_t id);
std::uint32_t release_all_count();
TextureStats stats();

} // namespace vanacore::soulspire::textures
