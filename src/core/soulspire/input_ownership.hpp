#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// Single authoritative UI input & cursor ownership state machine for VanaCore-native Soulspire UI.
// Pure C++20 (unit-tested in VanaCore.SettingsTest) — platform side effects (window::block_mouse,
// gui::want_gamepad, gui::restore_cursor) are applied centrally from soulspire::on_frame().
namespace vanacore::soulspire {

struct UiInputOptions {
    bool block_mouse = true;
    bool capture_gamepad = true;
    bool show_cursor = true;
    std::uint32_t lease_ms = 2000;
};

struct UiInputSnapshot {
    bool active = false;
    bool block_mouse = false;
    bool capture_gamepad = false;
    bool show_cursor = false;
    std::uint32_t owner_count = 0;
    std::vector<std::string> owners;
    std::uint32_t acquire_count = 0;
    std::uint32_t release_count = 0;
    std::string last_release_reason;
};

struct UiInputTransition {
    bool became_active = false;
    bool became_inactive = false;
    bool active = false;
    bool block_mouse = false;
    bool capture_gamepad = false;
    bool show_cursor = false;
};

class InputOwnershipState {
public:
    UiInputTransition acquire(const std::string& owner, const UiInputOptions& opts, std::uint64_t now_ms);
    UiInputTransition release(const std::string& owner, const std::string& reason, std::uint64_t now_ms);
    UiInputTransition release_all(const std::string& reason, std::uint64_t now_ms);
    UiInputTransition tick(std::uint64_t now_ms, bool window_in_background);

    bool active() const;
    bool wants_visible_cursor() const;
    UiInputSnapshot snapshot() const;

private:
    struct Lease {
        UiInputOptions opts{};
        std::uint64_t acquired_ms = 0;
        std::uint64_t refreshed_ms = 0;
    };

    UiInputTransition compute_locked(bool was_active) const;

    mutable std::mutex mu_;
    std::unordered_map<std::string, Lease> leases_;
    std::uint32_t acquire_count_ = 0;
    std::uint32_t release_count_ = 0;
    std::string last_release_reason_;
};

InputOwnershipState& input_ownership();

} // namespace vanacore::soulspire
