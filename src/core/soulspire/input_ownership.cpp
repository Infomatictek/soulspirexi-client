#include "input_ownership.hpp"

#include <algorithm>

namespace vanacore::soulspire {
namespace {

constexpr std::uint32_t kMinLeaseMs = 250;
constexpr std::uint32_t kMaxLeaseMs = 10000;

std::uint32_t clamp_lease(std::uint32_t ms) noexcept
{
    if (ms < kMinLeaseMs)
        return kMinLeaseMs;
    if (ms > kMaxLeaseMs)
        return kMaxLeaseMs;
    return ms;
}

} // namespace

InputOwnershipState& input_ownership()
{
    static InputOwnershipState g_state;
    return g_state;
}

UiInputTransition InputOwnershipState::compute_locked(bool was_active) const
{
    UiInputTransition tr{};
    tr.active = !leases_.empty();
    tr.became_active = !was_active && tr.active;
    tr.became_inactive = was_active && !tr.active;
    for (const auto& [_, lease] : leases_) {
        if (lease.opts.block_mouse)
            tr.block_mouse = true;
        if (lease.opts.capture_gamepad)
            tr.capture_gamepad = true;
        if (lease.opts.show_cursor)
            tr.show_cursor = true;
    }
    return tr;
}

UiInputTransition InputOwnershipState::acquire(const std::string& owner, const UiInputOptions& opts, std::uint64_t now_ms)
{
    std::lock_guard lock(mu_);
    const bool was_active = !leases_.empty();
    const std::string key = owner.empty() ? std::string("soulspire_core") : owner;
    UiInputOptions sanitized = opts;
    sanitized.lease_ms = clamp_lease(sanitized.lease_ms);

    auto it = leases_.find(key);
    if (it == leases_.end()) {
        Lease lease{};
        lease.opts = sanitized;
        lease.acquired_ms = now_ms;
        lease.refreshed_ms = now_ms;
        leases_.emplace(key, lease);
        if (!was_active)
            ++acquire_count_;
    } else {
        it->second.opts = sanitized;
        it->second.refreshed_ms = now_ms;
    }
    return compute_locked(was_active);
}

UiInputTransition InputOwnershipState::release(const std::string& owner, const std::string& reason, std::uint64_t /*now_ms*/)
{
    std::lock_guard lock(mu_);
    const bool was_active = !leases_.empty();
    const std::string key = owner.empty() ? std::string("soulspire_core") : owner;
    leases_.erase(key);
    if (was_active && leases_.empty()) {
        ++release_count_;
        last_release_reason_ = reason.empty() ? std::string("released") : reason;
    }
    return compute_locked(was_active);
}

UiInputTransition InputOwnershipState::release_all(const std::string& reason, std::uint64_t /*now_ms*/)
{
    std::lock_guard lock(mu_);
    const bool was_active = !leases_.empty();
    if (!leases_.empty()) {
        leases_.clear();
        ++release_count_;
        last_release_reason_ = reason.empty() ? std::string("release_all") : reason;
    }
    return compute_locked(was_active);
}

UiInputTransition InputOwnershipState::tick(std::uint64_t now_ms, bool window_in_background)
{
    std::lock_guard lock(mu_);
    const bool was_active = !leases_.empty();
    if (!was_active)
        return {};

    if (window_in_background) {
        leases_.clear();
        ++release_count_;
        last_release_reason_ = "focus_loss";
        return compute_locked(was_active);
    }

    bool expired_any = false;
    for (auto it = leases_.begin(); it != leases_.end();) {
        const std::uint64_t age = now_ms >= it->second.refreshed_ms ? (now_ms - it->second.refreshed_ms) : 0;
        if (age > static_cast<std::uint64_t>(it->second.opts.lease_ms)) {
            it = leases_.erase(it);
            expired_any = true;
        } else {
            ++it;
        }
    }
    if (expired_any && leases_.empty()) {
        ++release_count_;
        last_release_reason_ = "lease_expired";
    }
    return compute_locked(was_active);
}

bool InputOwnershipState::active() const
{
    std::lock_guard lock(mu_);
    return !leases_.empty();
}

bool InputOwnershipState::wants_visible_cursor() const
{
    std::lock_guard lock(mu_);
    for (const auto& [_, lease] : leases_) {
        if (lease.opts.show_cursor)
            return true;
    }
    return false;
}

UiInputSnapshot InputOwnershipState::snapshot() const
{
    std::lock_guard lock(mu_);
    UiInputSnapshot snap{};
    snap.active = !leases_.empty();
    snap.owner_count = static_cast<std::uint32_t>(leases_.size());
    snap.acquire_count = acquire_count_;
    snap.release_count = release_count_;
    snap.last_release_reason = last_release_reason_;
    snap.owners.reserve(leases_.size());
    for (const auto& [name, lease] : leases_) {
        snap.owners.push_back(name);
        if (lease.opts.block_mouse)
            snap.block_mouse = true;
        if (lease.opts.capture_gamepad)
            snap.capture_gamepad = true;
        if (lease.opts.show_cursor)
            snap.show_cursor = true;
    }
    std::sort(snap.owners.begin(), snap.owners.end());
    return snap;
}

} // namespace vanacore::soulspire
