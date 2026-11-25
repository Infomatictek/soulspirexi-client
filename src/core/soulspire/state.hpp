#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// SoulspireXI VanaCore-Native Soul State Engine (CLIENT-MODERN-001, Phase A+B).
//
// Authoritative in-process state shared across VanaCore's native packet lifecycle,
// signature-validated game hooks, the `vana.soulspire` Lua API, and Client Sentinel
// diagnostics. Never duplicates registry definitions: loads existing authoritative
// `soul-identities.json` and `soul-weapon-config.json` from the client install.
namespace vanacore::soulspire {

constexpr std::uint32_t kNativeApiVersion = 1;

// Native capability bitmask (compatible with Soulspire Core & server capability sync).
constexpr std::uint32_t kCapEquipSoulName     = 1u << 0;
constexpr std::uint32_t kCapSearchSoulLabels  = 1u << 1;
constexpr std::uint32_t kCapMagicNames        = 1u << 2;
constexpr std::uint32_t kCapMagicDescs        = 1u << 3;
constexpr std::uint32_t kCapMagicTarget       = 1u << 4;
constexpr std::uint32_t kCapMagicIcons        = 1u << 5;
constexpr std::uint32_t kCapAbilityNames      = 1u << 6;
constexpr std::uint32_t kCapAbilityDescs      = 1u << 7;
constexpr std::uint32_t kCapAbilityTarget     = 1u << 8;
constexpr std::uint32_t kCapAbilityIcons      = 1u << 9;
constexpr std::uint32_t kCapMagicVisibility   = 1u << 10;
constexpr std::uint32_t kCapAbilityVisibility = 1u << 11;
constexpr std::uint32_t kCapSpellEligibility  = 1u << 12;
constexpr std::uint32_t kCapSubWeaponRules    = 1u << 13;
constexpr std::uint32_t kCapSub2HRules        = 1u << 14;
constexpr std::uint32_t kCapDatVirtualization = 1u << 15;
constexpr std::uint32_t kCapCommandsMenuEntry = 1u << 16;
constexpr std::uint32_t kCapWeaponModelResolve = 1u << 17;
constexpr std::uint32_t kCapWeaponOffhandBone = 1u << 18;
constexpr std::uint32_t kCapVanaCoreNative    = 1u << 19;

enum class ActionKind : std::uint32_t {
    Spell       = 1,
    Ability     = 2,
    WeaponSkill = 3,
};

enum class VisibilityRuleAction : std::uint8_t {
    Hide = 0,
    Show = 1,
};

enum class VisibilityDecision : std::uint8_t {
    Unmanaged = 0,
    Hide      = 1,
    Show      = 2,
};

enum class WeaponRuleMode : std::uint8_t {
    Dual1H        = 1,
    Dual2H        = 2,
    TwoHandShield = 3,
};

struct SoulIdentityEntry {
    bool present = false;
    std::string key;
    std::string name;
    std::string abbr;
};

struct ActiveSoul {
    std::uint32_t character_id = 0;
    std::uint8_t job = 0;
    std::uint8_t branch = 0;
    std::uint8_t effective_level = 0;
    bool known = false;
    std::string source; // "packet_1ff", "packet_1fc", "lua_bridge", "none"
    std::uint64_t updated_ms = 0;
};

struct ActionRecord {
    std::uint8_t job = 0;
    std::uint8_t branch = 0;
    ActionKind kind = ActionKind::Spell;
    std::uint16_t action_id = 0;
    std::uint16_t icon_id = 0xFFFFu;
    std::uint16_t icon_id_v2 = 0xFFFFu;
    std::string name;
    std::string description;
};

struct VisibilityRecord {
    std::uint8_t job = 0;
    std::uint8_t branch = 0;
    ActionKind kind = ActionKind::Spell;
    std::uint16_t action_id = 0;
    std::uint8_t min_effective_level = 1;
    VisibilityRuleAction action = VisibilityRuleAction::Show;
};

struct WeaponRuleRecord {
    std::uint8_t job = 0;
    std::uint8_t branch = 0;
    std::uint8_t min_effective_level = 10;
    WeaponRuleMode mode = WeaponRuleMode::Dual1H;
    std::uint32_t main_skill_mask = 0;
    std::uint32_t sub_skill_mask = 0;
    std::uint16_t main_item_id = 0;
    std::uint16_t sub_item_id = 0;
};

struct HudSnapshot {
    std::string soul_key;
    std::unordered_map<std::string, std::string> kv;
    std::uint64_t updated_ms = 0;
    std::uint32_t packet_count = 0;
    std::uint32_t boost_packet_count = 0;
    std::string last_boost_payload;
};

struct MobStatusSnapshot {
    std::uint32_t server_id = 0;
    std::string payload;
    std::uint64_t updated_ms = 0;
    std::uint32_t packet_count = 0;
};

struct HookStatus {
    std::string id;
    std::string group;
    std::string owner; // "soulspire", "vanacore_shared", "vanacore_api"
    std::uint32_t rva = 0;
    bool installed = false;
    std::uint32_t hit_count = 0;
    std::string detail;
};

struct DiagnosticsSnapshot {
    bool enabled = false;
    std::uint32_t api_version = kNativeApiVersion;
    std::uint32_t capabilities = 0;
    std::uint32_t hooks_installed = 0;
    std::uint32_t hooks_total = 0;
    std::uint32_t registry_soul_count = 0;
    std::string registry_hash;
    std::string registry_source;
    std::string weapon_config_hash;
    std::uint32_t action_count = 0;
    std::uint32_t action_generation = 0;
    std::uint32_t action_row_limit = 16;
    std::uint32_t visibility_count = 0;
    std::uint32_t weapon_rule_count = 0;
    std::uint32_t packets_1ff = 0;
    std::uint32_t packets_1fc = 0;
    std::uint32_t packets_1fd = 0;
    std::uint32_t packets_1fe = 0;
    std::uint32_t packets_oxhud = 0;
    std::uint32_t packets_oxmob = 0;
    std::uint32_t packets_oxtal = 0;
    std::uint32_t suppressed_chat_frames = 0;
    ActiveSoul active_soul;
    std::vector<HookStatus> hooks;
};

// Pure text helpers (tested in VanaCore.SettingsTest).
std::string sanitize_ascii(std::string_view src, std::size_t max_len);
std::string compact_action_row_name(std::string_view full, std::size_t max_chars);
std::string format_action_description(std::string_view full, std::size_t max_chars);

class EngineState {
public:
    EngineState() = default;

    void reset_all();
    void reset_session();

    void set_enabled(bool on) noexcept { enabled_.store(on, std::memory_order_release); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_acquire); }

    // Registry & config loading from authoritative JSON payloads.
    bool load_identities_json(std::string_view json_text, std::string_view source_label);
    bool load_weapon_config_json(std::string_view json_text);

    // Identity queries & updates.
    bool resolve_label(std::uint8_t job, std::uint8_t branch, bool full, std::string& out) const;
    bool soul_key_to_pair(std::string_view key, std::uint8_t& job, std::uint8_t& branch) const;
    bool pair_to_soul_key(std::uint8_t job, std::uint8_t branch, std::string& out_key) const;

    void set_active_soul(std::uint32_t char_id, std::uint8_t job, std::uint8_t branch,
                         std::uint8_t effective_level, bool known, std::string_view source,
                         std::uint64_t now_ms);
    void update_live_job_level(std::uint8_t job, std::uint8_t effective_level);
    void clear_active_soul(std::string_view reason);
    ActiveSoul active_soul() const;

    // Action catalog & visibility.
    std::uint32_t replace_actions(std::vector<ActionRecord> records);
    bool upsert_action(ActionRecord record);
    void clear_actions();
    bool set_action_row_limit(std::uint32_t limit);
    std::uint32_t action_row_limit() const noexcept { return action_row_limit_.load(std::memory_order_relaxed); }

    bool lookup_action(ActionKind kind, std::uint16_t action_id, ActionRecord& out) const;

    std::uint32_t replace_visibility(std::vector<VisibilityRecord> records);
    void clear_visibility();
    VisibilityDecision decide_visibility(ActionKind kind, std::uint16_t action_id, std::uint8_t level_override = 0) const;

    // Weapon eligibility queries.
    bool allows_sub_weapon(std::uint16_t main_skill, std::uint16_t main_item_id,
                           std::uint16_t sub_skill, std::uint16_t sub_item_id) const;
    bool allows_dual_2h_menu_item(std::uint16_t sub_skill, std::uint16_t sub_item_id) const;
    bool allows_2h_main_with_sub(std::uint16_t main_skill, std::uint16_t main_item_id,
                                 std::uint16_t sub_skill, std::uint16_t sub_item_id) const;

    // Packet-driven HUD / Mob / Soulforge state updates.
    bool observe_oxhud_frame(std::string_view payload, std::uint64_t now_ms);
    bool observe_oxbst_frame(std::string_view payload, std::uint64_t now_ms);
    bool observe_oxmob_frame(std::string_view payload, std::uint64_t now_ms);
    void note_packet_1ff();
    void note_packet_1fc();
    void note_packet_1fd();
    void note_packet_1fe();
    void note_oxtal_frame();
    void note_suppressed_chat_frame();

    HudSnapshot hud_snapshot() const;
    bool mob_status(std::uint32_t server_id, MobStatusSnapshot& out) const;
    std::uint32_t tracked_mob_count() const;

    // Hook diagnostics & capability tracking.
    void set_hook_statuses(std::vector<HookStatus> hooks, std::uint32_t capabilities);
    void increment_hook_hit(std::string_view id);
    std::uint32_t capabilities() const noexcept { return capabilities_.load(std::memory_order_acquire); }
    void request_open_core() noexcept { pending_core_opens_.fetch_add(1, std::memory_order_acq_rel); }
    std::uint32_t consume_open_core() noexcept { return pending_core_opens_.exchange(0, std::memory_order_acq_rel); }

    DiagnosticsSnapshot diagnostics() const;

private:
    static std::uint64_t make_action_key(std::uint8_t job, std::uint8_t branch, ActionKind kind, std::uint16_t id) noexcept;

    std::atomic<bool> enabled_{false};
    std::atomic<std::uint32_t> capabilities_{0};
    std::atomic<std::uint32_t> action_row_limit_{16};
    std::atomic<std::uint32_t> pending_core_opens_{0};

    mutable std::mutex mutex_;
    std::array<std::array<SoulIdentityEntry, 4>, 23> identities_{}; // [job 1..22][branch 1..3]
    std::unordered_map<std::string, std::uint16_t> key_to_job_branch_; // (job << 8) | branch
    std::uint32_t registry_soul_count_ = 0;
    std::string registry_hash_;
    std::string registry_source_ = "unloaded";

    ActiveSoul active_{};

    std::unordered_map<std::uint64_t, ActionRecord> actions_;
    std::uint32_t action_generation_ = 0;

    std::unordered_map<std::uint64_t, VisibilityRecord> visibility_;

    std::vector<WeaponRuleRecord> weapon_rules_;
    std::string weapon_config_hash_;

    HudSnapshot hud_{};
    std::unordered_map<std::uint32_t, MobStatusSnapshot> mobs_;

    std::uint32_t packets_1ff_ = 0;
    std::uint32_t packets_1fc_ = 0;
    std::uint32_t packets_1fd_ = 0;
    std::uint32_t packets_1fe_ = 0;
    std::uint32_t packets_oxhud_ = 0;
    std::uint32_t packets_oxmob_ = 0;
    std::uint32_t packets_oxtal_ = 0;
    std::uint32_t suppressed_chat_frames_ = 0;

    std::vector<HookStatus> hooks_;
};

// Process-global VanaCore-native Soulspire state instance.
EngineState& engine();

} // namespace vanacore::soulspire
