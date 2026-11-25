#include "hooks.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

#include "../imports.hpp"
#include "../log.hpp"
#include "../scan.hpp"
#include "commands_menu.hpp"
#include "state.hpp"

namespace vanacore::soulspire::hooks {
namespace {

constexpr std::size_t kHelpWindDescMaxChars = 120;

// Verified FFXiMain.dll call-site & prologue patterns (wildcards for ASLR-relocated immediates).
// Derived from audited SoulspireXI FFXiMain.dll layout; each hook resolves its original callee
// dynamically from the 0xE8 displacement at runtime and fails closed individually if absent.
constexpr char kPatMagicRowName[]     = "E8 ?? ?? ?? ?? 8B 5C 24 24 8B 74 24 28 50 56 8D 53 12 52 E8";
constexpr char kPatMagicDesc[]        = "E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 6A 02 50 E8 ?? ?? ?? ?? 8B 86 C0 00 00";
constexpr char kPatMagicTarget[]      = "E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 5F B0 01 5E C3 90 90";
constexpr char kPatMagicIcon[]        = "E8 ?? ?? ?? ?? 83 C4 1C 85 C0 5D 74 12 6A 00 6A 00 68 80 80 80 80 56 53";
constexpr char kPatAbilityDesc[]      = "E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 6A 02 50 E8 ?? ?? ?? ?? 8B 0D ?? ?? ??";
constexpr char kPatAbilityTarget[]    = "E8 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 50 E8 ?? ?? ?? ?? 5F 5E B0 01 5B C3 90";
constexpr char kPatAbilityIcon[]      = "E8 ?? ?? ?? ?? 83 C4 1C 85 C0 74 12 6A 00 6A 00 68 80 80 80 80 56 53 8B";
constexpr char kPatMagicFilterCall[]  = "E8 ?? ?? ?? ?? 83 C4 04 84 C0 0F 84 C0 00 00 00 81 FE 00 02 00 00 7C 64";
constexpr char kPatAbilityFilterCall[] = "E8 ?? ?? ?? ?? 8B 44 24 1C C7 47 58 01 00 00 00 83 F8 FF 74 08 50 8B CF";
constexpr char kPatCanUseSpell[]      = "A1 ?? ?? ?? ?? 56 8B C8 BA 01 00 00 00 81 E1 FF 00 00 00 57 8B 7C 24 0C";
constexpr char kPatMissingSpellBit[]  = "BD FE FF FF FF 57 E8 ?? ?? ?? ?? 83 C4 04 84 C0 75 0B 5F 5E 5D B8 FE FF";
constexpr char kPatMenuSpellElig[]    = "E8 ?? ?? ?? ?? 83 C4 28 84 C0 75 09 5F 5E B8 02 00 00 00 5B C3 6A 00 E8";
constexpr char kPatItemJobLabel[]     = "E8 ?? ?? ?? ?? 50 8D 4C 24 1C 68 ?? ?? ?? ?? 51 E8 ?? ?? ?? ?? 83 C4 14";
constexpr char kPatFullJob1[]         = "E8 ?? ?? ?? ?? 83 C4 08 8D 54 24 1C 83 C6 04 50 52 56 8D 44 24 38 68 20";
constexpr char kPatFullJob2[]         = "E8 ?? ?? ?? ?? 50 8D 44 24 20 83 C6 04 50 56 8D 4C 24 38 68 ?? ?? ?? ??";
// Native Commands menu ("menu    menuwind") hooks #28-#31 (Status -> Soulspire -> Equipment).
constexpr char kPatMenuWindRestoreCached[] = "E8 ?? ?? ?? ?? 83 7E 10 0E 74 24 8A 46 6E 84 C0 75 1D 8B 4E 0C 85 C9 0F";
constexpr char kPatMenuWindRestoreFresh[]  = "E8 ?? ?? ?? ?? 8B 4E 0C 85 C9 74 05 8B 11 FF 52 04 8A 44 24 38 84 C0 74";
constexpr char kPatMenuWindNextButton[]    = "E8 ?? ?? ?? ?? 8B F8 66 85 FF 0F 8C 84 01 00 00 57 8B CE E8";
constexpr char kPatMenuWindKeyItem[]       = "0F BF 44 24 04 83 C0 FD 56 83 F8 19 8B F1 0F 87 CF 02 00 00 33 C9 8A 88";

// Disambiguators for magic vs ability row name/desc/icon (both share similar surrounding code):
// MagicRowName has `0x5D` (pop ebp) right after icon call; AbilityRowName does not.
// We verify exact unique target callees after scan so magic and ability hooks never cross-bind.

struct CallPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t original[5]{};
    std::uint8_t installed[5]{};
    std::uintptr_t original_callee = 0;

    bool install(std::uintptr_t address, void* replacement) noexcept
    {
        if (address == 0 || replacement == nullptr)
            return false;
        auto* p = reinterpret_cast<std::uint8_t*>(address);
        if (p[0] != 0xE8u)
            return false;
        std::int32_t orig_rel = 0;
        std::memcpy(&orig_rel, p + 1, 4);
        original_callee = address + 5 + static_cast<std::uintptr_t>(static_cast<std::intptr_t>(orig_rel));
        std::memcpy(original, p, 5);
        installed[0] = 0xE8u;
        const std::int32_t new_rel = static_cast<std::int32_t>(
            reinterpret_cast<std::uintptr_t>(replacement) - (address + 5));
        std::memcpy(installed + 1, &new_rel, 4);

        DWORD protect = 0, ignored = 0;
        if (!VirtualProtect(p, 5, PAGE_EXECUTE_READWRITE, &protect))
            return false;
        std::int32_t expected_rel = 0;
        std::memcpy(&expected_rel, original + 1, 4);
        const LONG prev = InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(p + 1),
            static_cast<LONG>(new_rel),
            static_cast<LONG>(expected_rel));
        FlushInstructionCache(GetCurrentProcess(), p, 5);
        VirtualProtect(p, 5, protect, &ignored);
        if (prev != static_cast<LONG>(expected_rel))
            return false;
        site = p;
        return true;
    }

    void uninstall() noexcept
    {
        if (site == nullptr)
            return;
        if (std::memcmp(site, installed, 5) == 0) {
            DWORD protect = 0, ignored = 0;
            if (VirtualProtect(site, 5, PAGE_EXECUTE_READWRITE, &protect)) {
                std::int32_t cur_rel = 0, orig_rel = 0;
                std::memcpy(&cur_rel, installed + 1, 4);
                std::memcpy(&orig_rel, original + 1, 4);
                InterlockedCompareExchange(
                    reinterpret_cast<volatile LONG*>(site + 1),
                    static_cast<LONG>(orig_rel),
                    static_cast<LONG>(cur_rel));
                FlushInstructionCache(GetCurrentProcess(), site, 5);
                VirtualProtect(site, 5, protect, &ignored);
            }
        }
        site = nullptr;
    }
};

using ActionIconFn   = void*(__cdecl*)(std::uint32_t, std::uint32_t);
using CanShowSpellFn = std::uint8_t(__cdecl*)(const std::uint8_t*);
using JobLookupFn    = const char*(__cdecl*)(int, int);
using FfxiOpNewFn            = void*(__cdecl*)(std::uint32_t);
using FfxiOpDeleteFn         = void(__cdecl*)(void*);
using FfxiPlayConfirmSoundFn = void(__cdecl*)(void);
using FfxiFrameUpdateBoxFn   = void(__fastcall*)(void*, void*, std::uint8_t);
using FfxiBtnCtorFn          = void*(__fastcall*)(void*, void*, void*);
using FfxiBtnInitFn          = void(__fastcall*)(void*, void*, const void*);
using FfxiBtnRecalcRectFn    = void(__fastcall*)(void*, void*, std::int32_t, std::int32_t, std::uint8_t);
using FfxiBtnUpdateSpriteFn  = void(__fastcall*)(void*, void*, std::uint8_t);
using FfxiBtnSetStateFn      = void(__fastcall*)(void*, void*, std::uint32_t);
using FfxiPrimSetSelectedFn  = void(__fastcall*)(void*, void*, std::int32_t);
using FfxiListPushBackFn     = void(__fastcall*)(void*, void*, void*);
using FfxiListRemoveNodeFn   = void(__fastcall*)(void*, void*, void*);
using FfxiCloseMenuFn        = void(__fastcall*)(void*, void*, void*, std::int32_t);

// Saved originals & call patches
CallPatch g_magic_row_patch{};
CallPatch g_magic_desc_patch{};
CallPatch g_magic_target_patch{};
CallPatch g_magic_icon_patch{};
CallPatch g_ability_row_patch{};
CallPatch g_ability_desc_patch{};
CallPatch g_ability_target_patch{};
CallPatch g_ability_icon_patch{};
CallPatch g_magic_filter_call_patch{};
CallPatch g_ability_filter_call_patch{};
CallPatch g_menu_spell_elig_patch{};
CallPatch g_item_job_patch{};
CallPatch g_full_job1_patch{};
CallPatch g_full_job2_patch{};
CallPatch g_menuwind_restore_cached_patch{};
CallPatch g_menuwind_restore_fresh_patch{};
CallPatch g_menuwind_next_button_patch{};

std::uintptr_t g_orig_magic_name = 0;
std::uintptr_t g_orig_magic_desc = 0;
std::uintptr_t g_orig_magic_target = 0;
ActionIconFn   g_orig_magic_icon = nullptr;
std::uintptr_t g_orig_ability_name = 0;
std::uintptr_t g_orig_ability_desc = 0;
std::uintptr_t g_orig_ability_target = 0;
ActionIconFn   g_orig_ability_icon = nullptr;
CanShowSpellFn g_orig_can_use_spell = nullptr;
std::uintptr_t g_orig_set_list_count = 0;
std::uint8_t** g_ability_list_slot = nullptr;
std::uintptr_t g_orig_menu_spell_elig = 0;
std::uintptr_t g_missing_spell_resume = 0;
JobLookupFn    g_orig_item_job = nullptr;
JobLookupFn    g_orig_full_job1 = nullptr;
JobLookupFn    g_orig_full_job2 = nullptr;

// Native Commands menu ("menu    menuwind") state & callees (Hooks #28-#31).
std::uintptr_t         g_orig_restore_saved_cursor = 0;
std::uintptr_t         g_orig_get_next_button = 0;
std::uintptr_t         g_menuwind_key_item_resume = 0;
std::uint8_t**         g_menuwind_singleton_slot = nullptr;
void*                  g_menu_manager_instance = nullptr;
FfxiOpNewFn            g_orig_ffxi_new = nullptr;
FfxiOpDeleteFn         g_orig_ffxi_delete = nullptr;
FfxiPlayConfirmSoundFn g_orig_play_confirm_sound = nullptr;
FfxiFrameUpdateBoxFn   g_orig_frame_update_box = nullptr;
FfxiBtnCtorFn          g_orig_btn_ctor = nullptr;
FfxiBtnInitFn          g_orig_btn_init = nullptr;
FfxiBtnRecalcRectFn    g_orig_btn_recalc_rect = nullptr;
FfxiBtnUpdateSpriteFn  g_orig_btn_update_sprite = nullptr;
FfxiBtnSetStateFn      g_orig_btn_set_state = nullptr;
FfxiPrimSetSelectedFn  g_orig_prim_set_selected = nullptr;
FfxiListPushBackFn     g_orig_list_push_back = nullptr;
FfxiListRemoveNodeFn   g_orig_list_remove_node = nullptr;
FfxiCloseMenuFn        g_orig_close_menu = nullptr;

std::mutex g_commands_menu_mutex;
const ::soulspire::native::NativeMenuButtonRes g_soulspire_btn_res = ::soulspire::native::make_soulspire_button_res();
const ::soulspire::native::NativeMenuButtonData g_soulspire_btn_data = {
    &g_soulspire_btn_res,
    nullptr,
    ::soulspire::native::kCommandsSoulspireHelp,
    ::soulspire::native::kCommandsSoulspireTitle,
    0u
};
std::unique_ptr<::soulspire::native::SoulspireMenuSpriteBank> g_commands_sprite_banks[::soulspire::native::kMaxCommandsSpriteBanks]{};
std::size_t g_last_commands_sprite_bank = 0;
std::atomic<const std::uint8_t*> g_active_menuwind_data{nullptr};
std::atomic<std::uint32_t> g_commands_injected{0};
std::atomic<bool> g_commands_ever_injected{false};

// Thread-local string buffers for FFXiMain.dll UI callbacks (game thread only).
thread_local std::int32_t g_tls_magic_id = -1;
thread_local std::int32_t g_tls_ability_id = -1;
thread_local char g_tls_name[96]{};
thread_local char g_tls_row[96]{};
thread_local char g_tls_desc[208]{};
thread_local char g_tls_job_short[16]{};
thread_local char g_tls_job_full[48]{};

bool g_attempted = false;

ActionKind kind_for_ability(std::int32_t id) noexcept
{
    return (id >= 0 && id < 512) ? ActionKind::WeaponSkill : ActionKind::Ability;
}

const char* __stdcall resolve_magic_row(std::int32_t spell_id) noexcept
{
    g_tls_magic_id = spell_id;
    if (spell_id < 0 || spell_id > 1023)
        return nullptr;
    ActionRecord rec{};
    if (!engine().lookup_action(ActionKind::Spell, static_cast<std::uint16_t>(spell_id), rec) || rec.name.empty())
        return nullptr;
    const std::string compacted = compact_action_row_name(rec.name, engine().action_row_limit());
    std::snprintf(g_tls_row, sizeof(g_tls_row), "%s", compacted.c_str());
    engine().increment_hook_hit("magic_row_name");
    return g_tls_row;
}

const char* __stdcall resolve_magic_target(std::int32_t spell_id) noexcept
{
    if (spell_id < 0 || spell_id > 1023)
        return nullptr;
    ActionRecord rec{};
    if (!engine().lookup_action(ActionKind::Spell, static_cast<std::uint16_t>(spell_id), rec) || rec.name.empty())
        return nullptr;
    std::snprintf(g_tls_name, sizeof(g_tls_name), "%s", rec.name.c_str());
    engine().increment_hook_hit("magic_target");
    return g_tls_name;
}

const char* __stdcall resolve_magic_desc(std::int32_t spell_id) noexcept
{
    if (spell_id < 0 || spell_id > 1023)
        return nullptr;
    ActionRecord rec{};
    if (!engine().lookup_action(ActionKind::Spell, static_cast<std::uint16_t>(spell_id), rec) || rec.description.empty())
        return nullptr;
    const std::string fmt = format_action_description(rec.description, kHelpWindDescMaxChars);
    std::snprintf(g_tls_desc, sizeof(g_tls_desc), "%s", fmt.c_str());
    engine().increment_hook_hit("magic_desc");
    return g_tls_desc;
}

void* __cdecl hook_magic_icon(std::uint32_t v1, std::uint32_t v2) noexcept
{
    const std::int32_t id = g_tls_magic_id;
    g_tls_magic_id = -1;
    if (id >= 0 && id <= 1023) {
        ActionRecord rec{};
        if (engine().lookup_action(ActionKind::Spell, static_cast<std::uint16_t>(id), rec)) {
            bool changed = false;
            if (rec.icon_id != 0xFFFFu) { v1 = rec.icon_id; changed = true; }
            if (rec.icon_id_v2 != 0xFFFFu) { v2 = rec.icon_id_v2; changed = true; }
            if (changed)
                engine().increment_hook_hit("magic_icon");
        }
    }
    return g_orig_magic_icon(v1, v2);
}

const char* __stdcall resolve_ability_row(std::int32_t ability_id) noexcept
{
    g_tls_ability_id = ability_id;
    if (ability_id < 0 || ability_id > 2047)
        return nullptr;
    ActionRecord rec{};
    if (!engine().lookup_action(kind_for_ability(ability_id), static_cast<std::uint16_t>(ability_id), rec) || rec.name.empty())
        return nullptr;
    const std::string compacted = compact_action_row_name(rec.name, engine().action_row_limit());
    std::snprintf(g_tls_row, sizeof(g_tls_row), "%s", compacted.c_str());
    engine().increment_hook_hit("ability_row_name");
    return g_tls_row;
}

const char* __stdcall resolve_ability_target(std::int32_t ability_id) noexcept
{
    if (ability_id < 0 || ability_id > 2047)
        return nullptr;
    ActionRecord rec{};
    if (!engine().lookup_action(kind_for_ability(ability_id), static_cast<std::uint16_t>(ability_id), rec) || rec.name.empty())
        return nullptr;
    std::snprintf(g_tls_name, sizeof(g_tls_name), "%s", rec.name.c_str());
    engine().increment_hook_hit("ability_target");
    return g_tls_name;
}

const char* __stdcall resolve_ability_desc(std::int32_t ability_id) noexcept
{
    if (ability_id < 0 || ability_id > 2047)
        return nullptr;
    ActionRecord rec{};
    if (!engine().lookup_action(kind_for_ability(ability_id), static_cast<std::uint16_t>(ability_id), rec) || rec.description.empty())
        return nullptr;
    const std::string fmt = format_action_description(rec.description, kHelpWindDescMaxChars);
    std::snprintf(g_tls_desc, sizeof(g_tls_desc), "%s", fmt.c_str());
    engine().increment_hook_hit("ability_desc");
    return g_tls_desc;
}

void* __cdecl hook_ability_icon(std::uint32_t v1, std::uint32_t v2) noexcept
{
    const std::int32_t id = g_tls_ability_id;
    g_tls_ability_id = -1;
    if (id >= 0 && id <= 2047) {
        ActionRecord rec{};
        if (engine().lookup_action(kind_for_ability(id), static_cast<std::uint16_t>(id), rec)) {
            bool changed = false;
            if (rec.icon_id != 0xFFFFu) { v1 = rec.icon_id; changed = true; }
            if (rec.icon_id_v2 != 0xFFFFu) { v2 = rec.icon_id_v2; changed = true; }
            if (changed)
                engine().increment_hook_hit("ability_icon");
        }
    }
    return g_orig_ability_icon(v1, v2);
}

std::uint8_t __cdecl hook_magic_filter(const std::uint8_t* spell_res)
{
    if (spell_res != nullptr) {
        std::uint16_t spell_id = 0;
        std::memcpy(&spell_id, spell_res, sizeof(spell_id));
        if (spell_id > 0 && spell_id <= 1023) {
            const VisibilityDecision d = engine().decide_visibility(ActionKind::Spell, spell_id);
            if (d == VisibilityDecision::Hide) {
                engine().increment_hook_hit("magic_list_filter");
                return 0;
            }
            if (d == VisibilityDecision::Show) {
                engine().increment_hook_hit("magic_list_filter");
                return 1;
            }
        }
    }
    return g_orig_can_use_spell(spell_res);
}

std::uint8_t __stdcall hook_knows_missing_spell(const std::uint8_t* spell_res) noexcept
{
    if (spell_res == nullptr)
        return 0;
    std::uint16_t spell_id = 0;
    std::memcpy(&spell_id, spell_res, sizeof(spell_id));
    if (spell_id == 0 || spell_id > 1023)
        return 0;
    if (engine().decide_visibility(ActionKind::Spell, spell_id) == VisibilityDecision::Show) {
        engine().increment_hook_hit("missing_spell_bit");
        return 1;
    }
    return 0;
}

std::int32_t __stdcall decide_menu_spell_elig(const std::uint8_t* spell_res) noexcept
{
    if (spell_res == nullptr)
        return -1;
    std::uint16_t spell_id = 0;
    std::memcpy(&spell_id, spell_res, sizeof(spell_id));
    if (spell_id == 0 || spell_id > 1023)
        return -1;
    const VisibilityDecision d = engine().decide_visibility(ActionKind::Spell, spell_id);
    if (d == VisibilityDecision::Show) {
        engine().increment_hook_hit("menu_spell_elig");
        return 1;
    }
    if (d == VisibilityDecision::Hide) {
        engine().increment_hook_hit("menu_spell_elig");
        return 0;
    }
    return -1;
}

// Phase B.2 Hook #20 (ability_list_filter @ 0x22562F):
// Compacts Ability / WeaponSkill menu rows in-place before SetListCount,
// preserving the fail-open Unmanaged policy for vanilla actions.
void __stdcall compact_ability_menu_rows(std::uint8_t* menu_obj, std::int32_t* count_ptr, std::int32_t* saved_sel_ptr) noexcept
{
    if (menu_obj == nullptr || count_ptr == nullptr || *count_ptr <= 0)
        return;
    if (g_ability_list_slot == nullptr)
        return;
    const ActiveSoul soul = engine().active_soul();
    if (!soul.known || soul.effective_level == 0)
        return;

    std::uint8_t* list_obj = *g_ability_list_slot;
    if (list_obj == nullptr)
        return;
    auto* rows = *reinterpret_cast<std::uint32_t**>(menu_obj + 0x20);
    const std::int32_t capacity = *reinterpret_cast<const std::int32_t*>(menu_obj + 0x44);
    if (rows == nullptr || capacity <= 0)
        return;

    const std::int32_t total_raw = *reinterpret_cast<const std::int32_t*>(list_obj + 0x1760);
    if (total_raw <= 0 || total_raw > 0xB00)
        return;
    const auto* raw_ids = reinterpret_cast<const std::uint16_t*>(list_obj + 0x160);

    const std::int32_t count = (*count_ptr < capacity) ? *count_ptr : capacity;
    const std::int32_t orig_sel = (saved_sel_ptr != nullptr) ? *saved_sel_ptr : -1;
    std::int32_t new_sel = -1;
    std::int32_t write = 0;
    bool hid_any = false;

    for (std::int32_t i = 0; i < count; ++i) {
        const std::uint32_t raw_idx = rows[i * 2];
        bool hide = false;
        if (raw_idx < static_cast<std::uint32_t>(total_raw)) {
            const std::uint16_t ability_id = raw_ids[raw_idx];
            if (engine().decide_visibility(kind_for_ability(ability_id), ability_id) == VisibilityDecision::Hide) {
                hide = true;
            }
        }
        if (hide) {
            hid_any = true;
            continue;
        }
        if (i == orig_sel)
            new_sel = write;
        if (write != i) {
            rows[write * 2] = rows[i * 2];
            rows[write * 2 + 1] = rows[i * 2 + 1];
        }
        ++write;
    }

    if (hid_any) {
        engine().increment_hook_hit("ability_list_filter");
        *count_ptr = write;
        if (saved_sel_ptr != nullptr && orig_sel >= 0 && orig_sel < count) {
            *saved_sel_ptr = new_sel;
        }
    }
}

const char* __cdecl hook_item_job_label(int table, int job) noexcept
{
    if (table == 2 && job >= 1 && job <= 22) {
        const ActiveSoul s = engine().active_soul();
        if (s.known && s.job == static_cast<std::uint8_t>(job) && s.branch >= 1 && s.branch <= 3) {
            std::string abbr;
            if (engine().resolve_label(s.job, s.branch, false, abbr)) {
                std::snprintf(g_tls_job_short, sizeof(g_tls_job_short), "%s", abbr.c_str());
                engine().increment_hook_hit("item_job_label");
                return g_tls_job_short;
            }
        }
    }
    return g_orig_item_job(table, job);
}

const char* __cdecl hook_full_job_1(int table, int job) noexcept
{
    if (table == 1 && job >= 1 && job <= 22) {
        const ActiveSoul s = engine().active_soul();
        if (s.known && s.job == static_cast<std::uint8_t>(job) && s.branch >= 1 && s.branch <= 3) {
            std::string full;
            if (engine().resolve_label(s.job, s.branch, true, full)) {
                std::snprintf(g_tls_job_full, sizeof(g_tls_job_full), "%s", full.c_str());
                engine().increment_hook_hit("full_job_name_1");
                return g_tls_job_full;
            }
        }
    }
    return g_orig_full_job1(table, job);
}

const char* __cdecl hook_full_job_2(int table, int job) noexcept
{
    if (table == 1 && job >= 1 && job <= 22) {
        const ActiveSoul s = engine().active_soul();
        if (s.known && s.job == static_cast<std::uint8_t>(job) && s.branch >= 1 && s.branch <= 3) {
            std::string full;
            if (engine().resolve_label(s.job, s.branch, true, full)) {
                std::snprintf(g_tls_job_full, sizeof(g_tls_job_full), "%s", full.c_str());
                engine().increment_hook_hit("full_job_name_2");
                return g_tls_job_full;
            }
        }
    }
    return g_orig_full_job2(table, job);
}

// x86 naked thunks preserving __thiscall ECX and stack conventions cleanly.
__declspec(naked) void thunk_magic_row_name()
{
    __asm {
        pushad
        mov eax, [esp + 36]
        push eax
        call resolve_magic_row
        test eax, eax
        jz stock
        mov [esp + 28], eax
        popad
        ret 4
    stock:
        popad
        jmp dword ptr [g_orig_magic_name]
    }
}

__declspec(naked) void thunk_magic_desc()
{
    __asm {
        pushad
        mov eax, [esp + 36]
        push eax
        call resolve_magic_desc
        test eax, eax
        jz stock
        mov [esp + 28], eax
        popad
        ret 4
    stock:
        popad
        jmp dword ptr [g_orig_magic_desc]
    }
}

__declspec(naked) void thunk_magic_target()
{
    __asm {
        pushad
        mov eax, [esp + 36]
        push eax
        call resolve_magic_target
        test eax, eax
        jz stock
        mov [esp + 28], eax
        popad
        ret 4
    stock:
        popad
        jmp dword ptr [g_orig_magic_target]
    }
}

__declspec(naked) void thunk_ability_row_name()
{
    __asm {
        pushad
        mov eax, [esp + 36]
        push eax
        call resolve_ability_row
        test eax, eax
        jz stock
        mov [esp + 28], eax
        popad
        ret 4
    stock:
        popad
        jmp dword ptr [g_orig_ability_name]
    }
}

__declspec(naked) void thunk_ability_desc()
{
    __asm {
        pushad
        mov eax, [esp + 36]
        push eax
        call resolve_ability_desc
        test eax, eax
        jz stock
        mov [esp + 28], eax
        popad
        ret 4
    stock:
        popad
        jmp dword ptr [g_orig_ability_desc]
    }
}

__declspec(naked) void thunk_ability_target()
{
    __asm {
        pushad
        mov eax, [esp + 36]
        push eax
        call resolve_ability_target
        test eax, eax
        jz stock
        mov [esp + 28], eax
        popad
        ret 4
    stock:
        popad
        jmp dword ptr [g_orig_ability_target]
    }
}

__declspec(naked) void thunk_missing_spell_bit()
{
    __asm {
        pushfd
        pushad
        push edi
        call hook_knows_missing_spell
        test al, al
        jz stock
        popad
        popfd
        jmp dword ptr [g_missing_spell_resume]
    stock:
        popad
        popfd
        mov ebp, 0FFFFFFFEh
        jmp dword ptr [g_missing_spell_resume]
    }
}

__declspec(naked) void thunk_menu_spell_elig()
{
    __asm {
        pushad
        push esi
        call decide_menu_spell_elig
        test eax, eax
        js stock
        mov [esp + 28], eax
        popad
        ret
    stock:
        popad
        jmp dword ptr [g_orig_menu_spell_elig]
    }
}

__declspec(naked) void thunk_ability_list_filter()
{
    __asm {
        pushad
        lea eax, [esp + 36]
        lea edx, [esp + 72]
        push edx
        push eax
        push ecx
        call compact_ability_menu_rows
        popad
        jmp dword ptr [g_orig_set_list_count]
    }
}

// ============================================================================
// Native FFXI Commands Menu ("menu    menuwind") — Status -> Soulspire -> Equipment
// ============================================================================
std::uint8_t* acquire_soulspire_menu_sprite(
    const std::uint8_t* const src_sprites[::soulspire::native::kCommandsSourceSpriteCount]) noexcept
{
    using namespace ::soulspire::native;
    if (src_sprites == nullptr || src_sprites[0] == nullptr) return nullptr;
    const std::uint8_t* status_node0 = nullptr;
    const std::uint8_t* status_tile0 = nullptr;
    if (!sprite_list_tile_at(src_sprites[0], 0, status_node0, status_tile0)) return nullptr;

    std::lock_guard lock(g_commands_menu_mutex);
    if (g_last_commands_sprite_bank < kMaxCommandsSpriteBanks &&
        g_commands_sprite_banks[g_last_commands_sprite_bank] != nullptr &&
        g_commands_sprite_banks[g_last_commands_sprite_bank]->active &&
        commands_sprite_bank_key_matches(g_commands_sprite_banks[g_last_commands_sprite_bank]->key, src_sprites, status_tile0)) {
        return g_commands_sprite_banks[g_last_commands_sprite_bank]->sprite;
    }
    for (std::size_t i = 0; i < kMaxCommandsSpriteBanks; ++i) {
        if (g_commands_sprite_banks[i] != nullptr &&
            g_commands_sprite_banks[i]->active &&
            commands_sprite_bank_key_matches(g_commands_sprite_banks[i]->key, src_sprites, status_tile0)) {
            g_last_commands_sprite_bank = i;
            return g_commands_sprite_banks[i]->sprite;
        }
    }
    for (std::size_t i = 0; i < kMaxCommandsSpriteBanks; ++i) {
        if (g_commands_sprite_banks[i] == nullptr) {
            auto fresh = std::unique_ptr<SoulspireMenuSpriteBank>(new (std::nothrow) SoulspireMenuSpriteBank{});
            if (fresh == nullptr) break;
            if (!synthesize_soulspire_menu_sprite(*fresh, src_sprites)) break;
            std::uint8_t* sp = fresh->sprite;
            g_commands_sprite_banks[i] = std::move(fresh);
            g_last_commands_sprite_bank = i;
            return sp;
        }
    }
    return nullptr;
}

bool inject_commands_menu_entry_impl(std::uint8_t* prim) noexcept
{
    using namespace ::soulspire::native;
    if (prim == nullptr) return false;
    std::uint8_t* menu_data = nullptr;
    std::memcpy(&menu_data, prim + 0x04, sizeof(menu_data));
    if (!is_commands_menu_form(menu_data)) return false;

    if (g_orig_ffxi_new == nullptr || g_orig_btn_ctor == nullptr ||
        g_orig_btn_init == nullptr || g_orig_btn_recalc_rect == nullptr ||
        g_orig_btn_update_sprite == nullptr || g_orig_btn_set_state == nullptr ||
        g_orig_list_push_back == nullptr || g_orig_frame_update_box == nullptr) {
        return false;
    }

    std::uint8_t* frame = nullptr;
    std::uint8_t* existing_btn16 = nullptr;
    std::uint8_t* shifted_buttons[16]{};
    std::size_t shifted_count = 0;
    std::size_t total_button_count = 0;
    const std::uint8_t* src_sprites[kCommandsSourceSpriteCount]{};

    std::uint8_t* node = nullptr;
    std::memcpy(&node, prim + 0x14, sizeof(node));
    std::uint32_t guard = 0;
    while (node != nullptr && guard < 128u) {
        ++guard;
        if (node[0x14] == 0) {
            std::uint8_t* elem = nullptr;
            std::memcpy(&elem, node + 0x10, sizeof(elem));
            if (elem != nullptr) {
                std::int16_t elem_type = -1;
                std::memcpy(&elem_type, elem, sizeof(elem_type));
                if (elem_type == 0 && frame == nullptr) {
                    frame = elem;
                } else if (elem_type == 1) {
                    ++total_button_count;
                    std::int16_t btn_id = 0;
                    std::memcpy(&btn_id, elem + 0x3A, sizeof(btn_id));
                    if (btn_id == kCommandsSoulspireButtonId) {
                        existing_btn16 = elem;
                    } else {
                        if (is_commands_shifted_row_button(btn_id) && shifted_count < 16u) {
                            shifted_buttons[shifted_count++] = elem;
                        }
                        for (std::size_t s = 0; s < kCommandsSourceSpriteCount; ++s) {
                            if (btn_id == kCommandsSourceButtonIds[s]) {
                                const std::uint8_t* sp = nullptr;
                                std::memcpy(&sp, elem + 0x04, sizeof(sp));
                                src_sprites[s] = sp;
                            }
                        }
                    }
                }
            }
        }
        std::uint8_t* next = nullptr;
        std::memcpy(&next, node + 0x00, sizeof(next));
        node = next;
    }

    // Fail-closed guard against [client] singlemenu=1 (27-button combined menuwind where ID 16 = Missions).
    if (total_button_count > static_cast<std::size_t>(kCommandsExpandedButtonCount)) {
        return false;
    }

    std::uint8_t* soulspire_sprite = acquire_soulspire_menu_sprite(src_sprites);
    if (soulspire_sprite == nullptr) return false;

    std::int16_t origin_x = 0, origin_y = 0;
    std::memcpy(&origin_x, prim + 0x52, sizeof(origin_x));
    std::memcpy(&origin_y, prim + 0x54, sizeof(origin_y));
    const std::uint8_t use_scale = prim[0x99];

    if (frame != nullptr) {
        std::uint8_t* frame_data = nullptr;
        std::memcpy(&frame_data, frame + 0x30, sizeof(frame_data));
        std::uint8_t* frame_res = nullptr;
        if (frame_data != nullptr) std::memcpy(&frame_res, frame_data, sizeof(frame_res));
        if (frame_res != nullptr) {
            std::int16_t res_h = 0;
            std::memcpy(&res_h, frame_res + 0x0C, sizeof(res_h));
            if (res_h > kCommandsExpandedFrameH) return false;
            if (res_h == kCommandsVanillaFrameH) {
                res_h = kCommandsExpandedFrameH;
                std::memcpy(frame_res + 0x0C, &res_h, sizeof(res_h));
            }
            if (use_scale == 0) {
                std::int16_t scr_h = 0;
                std::memcpy(&scr_h, frame + 0x24, sizeof(scr_h));
                if (scr_h == kCommandsVanillaFrameH) {
                    scr_h = kCommandsExpandedFrameH;
                    std::memcpy(frame + 0x24, &scr_h, sizeof(scr_h));
                }
            }
            g_orig_frame_update_box(frame, nullptr, use_scale);
            std::int16_t frame_y = 0, frame_h = 0;
            std::memcpy(&frame_y, frame + 0x1C, sizeof(frame_y));
            std::memcpy(&frame_h, frame + 0x24, sizeof(frame_h));
            const std::int16_t new_bottom = static_cast<std::int16_t>(frame_y + frame_h);
            std::memcpy(prim + 0x40, &new_bottom, sizeof(new_bottom));
            std::memcpy(prim + 0x48, &new_bottom, sizeof(new_bottom));
        }
    }

    for (std::size_t i = 0; i < shifted_count; ++i) {
        std::uint8_t* btn = shifted_buttons[i];
        std::int16_t btn_id = 0, rel_top = 0, rel_bottom = 0;
        std::memcpy(&btn_id, btn + 0x3A, sizeof(btn_id));
        std::memcpy(&rel_top, btn + 0x34, sizeof(rel_top));
        std::memcpy(&rel_bottom, btn + 0x38, sizeof(rel_bottom));
        const std::int16_t vanilla_y = commands_vanilla_row_y(btn_id);
        if (vanilla_y >= 0 && rel_top == vanilla_y) {
            rel_top = static_cast<std::int16_t>(rel_top + kCommandsRowPitchPx);
            rel_bottom = static_cast<std::int16_t>(rel_bottom + kCommandsRowPitchPx);
            std::memcpy(btn + 0x34, &rel_top, sizeof(rel_top));
            std::memcpy(btn + 0x38, &rel_bottom, sizeof(rel_bottom));
            g_orig_btn_recalc_rect(btn, nullptr, static_cast<std::int32_t>(origin_x), static_cast<std::int32_t>(origin_y), use_scale);
        }
    }

    if (existing_btn16 == nullptr) {
        void* raw = g_orig_ffxi_new(0x60u);
        if (raw == nullptr) return false;
        std::uint8_t* btn16 = static_cast<std::uint8_t*>(g_orig_btn_ctor(raw, nullptr, prim));
        if (btn16 == nullptr) {
            if (g_orig_ffxi_delete != nullptr) g_orig_ffxi_delete(raw);
            return false;
        }
        std::int16_t btn_count = 0;
        std::memcpy(&btn_count, prim + 0x58, sizeof(btn_count));
        ++btn_count;
        std::memcpy(prim + 0x58, &btn_count, sizeof(btn_count));

        g_orig_btn_init(btn16, nullptr, &g_soulspire_btn_data);
        std::memcpy(btn16 + 0x08, &soulspire_sprite, sizeof(soulspire_sprite));
        btn16[0x48] = 0;
        g_orig_btn_recalc_rect(btn16, nullptr, static_cast<std::int32_t>(origin_x), static_cast<std::int32_t>(origin_y), use_scale);
        g_orig_btn_update_sprite(btn16, nullptr, use_scale);
        g_orig_btn_set_state(btn16, nullptr, kCommandsButtonStateEnabled);
        g_orig_list_push_back(prim + 0x14, nullptr, btn16);
        engine().increment_hook_hit("menuwind_restore_cached");
    } else {
        std::memcpy(existing_btn16 + 0x08, &soulspire_sprite, sizeof(soulspire_sprite));
        std::memcpy(existing_btn16 + 0x04, &soulspire_sprite, sizeof(soulspire_sprite));
        existing_btn16[0x48] = 0;
        g_orig_btn_recalc_rect(existing_btn16, nullptr, static_cast<std::int32_t>(origin_x), static_cast<std::int32_t>(origin_y), use_scale);
        g_orig_btn_update_sprite(existing_btn16, nullptr, use_scale);
        std::uint32_t cur_state = 0;
        std::memcpy(&cur_state, existing_btn16 + 0x10, sizeof(cur_state));
        if (cur_state == kCommandsButtonStatePressed) {
            g_orig_btn_set_state(existing_btn16, nullptr, kCommandsButtonStateEnabled);
        }
    }

    g_active_menuwind_data.store(menu_data, std::memory_order_release);
    g_commands_ever_injected.store(true, std::memory_order_release);
    g_commands_injected.store(1u, std::memory_order_release);
    return true;
}

void __stdcall on_menuwind_before_restore_cursor(std::uint8_t* prim) noexcept
{
    if (!engine().enabled() || prim == nullptr) return;
    __try {
        inject_commands_menu_entry_impl(prim);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

std::int32_t __stdcall resolve_menuwind_next_button_hook(
    const std::uint8_t* menu_data,
    std::int32_t cur_btn,
    std::int32_t dir_key) noexcept
{
    if (!engine().enabled() || menu_data == nullptr) return 0;
    if (menu_data != g_active_menuwind_data.load(std::memory_order_acquire)) return 0;
    const bool injected = g_commands_injected.load(std::memory_order_acquire) != 0;
    const std::int16_t override_btn = ::soulspire::native::resolve_commands_menu_next_button(
        static_cast<std::int16_t>(cur_btn), static_cast<std::int16_t>(dir_key), injected);
    if (override_btn > 0) {
        engine().increment_hook_hit("menuwind_next_button");
        return static_cast<std::int32_t>(override_btn);
    }
    return 0;
}

std::uint8_t __stdcall handle_menuwind_key_item(
    std::uint8_t* menuwind,
    std::int32_t key,
    std::int32_t btn_id) noexcept
{
    using namespace ::soulspire::native;
    if (!engine().enabled()) return 0;
    if (key != kCommandsKeyConfirm || btn_id != kCommandsSoulspireButtonId) return 0;
    if (g_commands_injected.load(std::memory_order_acquire) == 0) return 0;

    if (g_orig_play_confirm_sound != nullptr) {
        g_orig_play_confirm_sound();
    }
    if (menuwind != nullptr && g_orig_close_menu != nullptr && g_menu_manager_instance != nullptr) {
        std::uint8_t* prim = nullptr;
        std::memcpy(&prim, menuwind + 0x08, sizeof(prim));
        if (prim != nullptr) {
            g_orig_close_menu(g_menu_manager_instance, nullptr, prim, 1);
        }
    }
    engine().increment_hook_hit("menuwind_key_item");
    engine().request_open_core();
    return 1;
}

__declspec(naked) void thunk_menuwind_restore_cursor()
{
    __asm {
        pushad
        push ecx
        call on_menuwind_before_restore_cursor
        popad
        jmp dword ptr [g_orig_restore_saved_cursor]
    }
}

__declspec(naked) void thunk_menuwind_next_button()
{
    __asm {
        pushad
        movsx eax, word ptr [esp + 40]
        movsx edx, word ptr [esp + 36]
        push eax
        push edx
        push ecx
        call resolve_menuwind_next_button_hook
        test eax, eax
        jz stock
        mov [esp + 28], eax
        popad
        ret 8
    stock:
        popad
        jmp dword ptr [g_orig_get_next_button]
    }
}

__declspec(naked) void thunk_menuwind_key_item()
{
    __asm {
        pushad
        movsx eax, word ptr [esp + 40]
        movsx edx, word ptr [esp + 36]
        push eax
        push edx
        push ecx
        call handle_menuwind_key_item
        test al, al
        jz stock
        popad
        ret 8
    stock:
        popad
        movsx eax, word ptr [esp + 4]
        jmp dword ptr [g_menuwind_key_item_resume]
    }
}

// Finds a pattern starting strictly AFTER `after` inside FFXiMain.dll (for disambiguating twin sites).
std::uintptr_t scan_in_module(HMODULE mod, const char* pattern, std::uintptr_t after = 0)
{
    const auto base = reinterpret_cast<std::uintptr_t>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;
    const std::uintptr_t end = base + nt->OptionalHeader.SizeOfImage;
    const std::uintptr_t start = (after > base && after < end) ? after : base;
    return scan::find(start, static_cast<std::size_t>(end - start), pattern);
}

} // namespace

void install_if_ready()
{
    if (g_attempted || !engine().enabled())
        return;

    const HMODULE ffxi = GetModuleHandleW(L"FFXiMain.dll");
    if (ffxi == nullptr)
        return;

    g_attempted = true;
    const auto base = reinterpret_cast<std::uintptr_t>(ffxi);

    // Guard: refuse to double-patch if legacy external soulspire_native.dll is already loaded.
    if (GetModuleHandleW(L"soulspire_native.dll") != nullptr) {
        log::write(L"Soulspire: legacy soulspire_native.dll is loaded; skipping VanaCore-native FFXiMain hook patching to prevent double-hooking (packet & Lua state bridge remain active)");
        std::vector<HookStatus> statuses;
        statuses.push_back({"packet_lifecycle", "packets", "vanacore_shared", "replaced", 0, true, 0, "vanacore::packets integrated"});
        statuses.push_back({"legacy_guard", "guard", "soulspire", "pending", 0, false, 0, "skipped: soulspire_native.dll loaded"});
        engine().set_hook_statuses(std::move(statuses), kCapVanaCoreNative | kCapNativeTextures | kCapNativeInputOwnership);
        return;
    }

    std::vector<HookStatus> statuses;
    std::uint32_t caps = kCapVanaCoreNative | kCapNativeTextures | kCapNativeInputOwnership;
    g_ability_list_slot = reinterpret_cast<std::uint8_t**>(base + 0x668308u);

    // 1. Coordinated VanaCore-owned packet pipeline (zero competing detours on 0xDD6C0/0xDF7C0).
    statuses.push_back({"packet_lifecycle", "packets", "vanacore_shared", "replaced", 0, true, 0, "coordinated via vanacore::packets::process"});

    auto install_call = [&](const char* id, const char* group, std::uintptr_t at, CallPatch& patch,
                            void* repl, std::uint32_t cap_bit, auto& orig_out) {
        HookStatus st{};
        st.id = id;
        st.group = group;
        st.owner = "soulspire";
        st.disposition = "migrated";
        if (at == 0) {
            st.detail = "signature not found";
            statuses.push_back(std::move(st));
            return false;
        }
        st.rva = static_cast<std::uint32_t>(at - base);
        if (!patch.install(at, repl)) {
            st.detail = "call site patch failed or already patched";
            statuses.push_back(std::move(st));
            return false;
        }
        using TargetT = std::remove_reference_t<decltype(orig_out)>;
        if constexpr (std::is_pointer_v<TargetT>) {
            orig_out = reinterpret_cast<TargetT>(patch.original_callee);
        } else {
            orig_out = static_cast<TargetT>(patch.original_callee);
        }
        st.installed = true;
        st.detail = "installed";
        caps |= cap_bit;
        statuses.push_back(std::move(st));
        return true;
    };

    // Disambiguate MagicRowName (higher RVA ~0x22E4F5) vs AbilityRowName (lower RVA ~0x226129).
    const std::uintptr_t row1 = scan_in_module(ffxi, kPatMagicRowName);
    const std::uintptr_t row2 = row1 != 0 ? scan_in_module(ffxi, kPatMagicRowName, row1 + 16) : 0;
    const std::uintptr_t ability_row_at = (row1 != 0 && row2 != 0) ? row1 : 0;
    const std::uintptr_t magic_row_at   = (row1 != 0 && row2 != 0) ? row2 : 0;

    install_call("magic_row_name",   "actions", magic_row_at, g_magic_row_patch,
                 reinterpret_cast<void*>(&thunk_magic_row_name), kCapMagicNames, g_orig_magic_name);
    install_call("magic_desc",       "actions", scan::find(ffxi, kPatMagicDesc), g_magic_desc_patch,
                 reinterpret_cast<void*>(&thunk_magic_desc), kCapMagicDescs, g_orig_magic_desc);
    install_call("magic_target",     "actions", scan::find(ffxi, kPatMagicTarget), g_magic_target_patch,
                 reinterpret_cast<void*>(&thunk_magic_target), kCapMagicTarget, g_orig_magic_target);
    install_call("magic_icon",       "actions", scan::find(ffxi, kPatMagicIcon), g_magic_icon_patch,
                 reinterpret_cast<void*>(&hook_magic_icon), kCapMagicIcons, g_orig_magic_icon);

    install_call("ability_row_name", "actions", ability_row_at, g_ability_row_patch,
                 reinterpret_cast<void*>(&thunk_ability_row_name), kCapAbilityNames, g_orig_ability_name);
    install_call("ability_desc",     "actions", scan::find(ffxi, kPatAbilityDesc), g_ability_desc_patch,
                 reinterpret_cast<void*>(&thunk_ability_desc), kCapAbilityDescs, g_orig_ability_desc);
    install_call("ability_target",   "actions", scan::find(ffxi, kPatAbilityTarget), g_ability_target_patch,
                 reinterpret_cast<void*>(&thunk_ability_target), kCapAbilityTarget, g_orig_ability_target);
    install_call("ability_icon",     "actions", scan::find(ffxi, kPatAbilityIcon), g_ability_icon_patch,
                 reinterpret_cast<void*>(&hook_ability_icon), kCapAbilityIcons, g_orig_ability_icon);

    // Visibility & Eligibility hooks
    const std::uintptr_t can_use_at = scan::find(ffxi, kPatCanUseSpell);
    {
        HookStatus st{};
        st.id = "can_use_spell";
        st.group = "visibility";
        st.owner = "soulspire";
        st.disposition = "migrated";
        if (can_use_at != 0) {
            st.rva = static_cast<std::uint32_t>(can_use_at - base);
            if (imports::detour<CanShowSpellFn>(reinterpret_cast<CanShowSpellFn>(can_use_at), 5, &hook_magic_filter, g_orig_can_use_spell)) {
                st.installed = true;
                st.detail = "detour installed (5B)";
                caps |= kCapMagicVisibility;
            } else {
                st.detail = "detour install failed";
            }
        } else {
            st.detail = "signature not found";
        }
        statuses.push_back(std::move(st));
    }

    if (g_orig_can_use_spell != nullptr) {
        CanShowSpellFn dummy_orig = nullptr;
        install_call("magic_list_filter", "visibility", scan::find(ffxi, kPatMagicFilterCall),
                     g_magic_filter_call_patch, reinterpret_cast<void*>(&hook_magic_filter),
                     kCapMagicVisibility, dummy_orig);
    }

    // Hook #20 (Phase B.2): Ability/WeaponSkill list filter & compaction.
    install_call("ability_list_filter", "visibility", scan::find(ffxi, kPatAbilityFilterCall),
                 g_ability_filter_call_patch, reinterpret_cast<void*>(&thunk_ability_list_filter),
                 kCapAbilityVisibility, g_orig_set_list_count);

    const std::uintptr_t missing_bit_at = scan::find(ffxi, kPatMissingSpellBit);
    {
        HookStatus st{};
        st.id = "missing_spell_bit";
        st.group = "visibility";
        st.owner = "soulspire";
        st.disposition = "migrated";
        if (missing_bit_at != 0) {
            st.rva = static_cast<std::uint32_t>(missing_bit_at - base);
            g_missing_spell_resume = missing_bit_at + 5;
            if (imports::jump(reinterpret_cast<void*>(missing_bit_at), reinterpret_cast<void*>(&thunk_missing_spell_bit))) {
                st.installed = true;
                st.detail = "prologue jump installed (5B)";
                caps |= kCapSpellEligibility;
            } else {
                st.detail = "jump patch failed";
            }
        } else {
            st.detail = "signature not found";
        }
        statuses.push_back(std::move(st));
    }

    install_call("menu_spell_elig", "visibility", scan::find(ffxi, kPatMenuSpellElig),
                 g_menu_spell_elig_patch, reinterpret_cast<void*>(&thunk_menu_spell_elig),
                 kCapSpellEligibility, g_orig_menu_spell_elig);

    // Job Identity label hooks (item tooltips + full Soul name)
    install_call("item_job_label",  "identity", scan::find(ffxi, kPatItemJobLabel), g_item_job_patch,
                 reinterpret_cast<void*>(&hook_item_job_label), kCapEquipSoulName, g_orig_item_job);
    install_call("full_job_name_1", "identity", scan::find(ffxi, kPatFullJob1), g_full_job1_patch,
                 reinterpret_cast<void*>(&hook_full_job_1), kCapEquipSoulName, g_orig_full_job1);
    install_call("full_job_name_2", "identity", scan::find(ffxi, kPatFullJob2), g_full_job2_patch,
                 reinterpret_cast<void*>(&hook_full_job_2), kCapEquipSoulName, g_orig_full_job2);

    // Native Commands menu ("menu    menuwind") hooks #28-#31 (Status -> Soulspire -> Equipment).
    g_menuwind_singleton_slot = reinterpret_cast<std::uint8_t**>(base + 0x5793F4u);
    g_menu_manager_instance   = reinterpret_cast<void*>(base + 0x6229F0u);
    g_orig_ffxi_new           = reinterpret_cast<FfxiOpNewFn>(base + 0x3131FBu);
    g_orig_ffxi_delete        = reinterpret_cast<FfxiOpDeleteFn>(base + 0x3131F0u);
    g_orig_play_confirm_sound = reinterpret_cast<FfxiPlayConfirmSoundFn>(base + 0x037B90u);
    g_orig_frame_update_box   = reinterpret_cast<FfxiFrameUpdateBoxFn>(base + 0x11A9E0u);
    g_orig_btn_ctor           = reinterpret_cast<FfxiBtnCtorFn>(base + 0x11AD60u);
    g_orig_btn_init           = reinterpret_cast<FfxiBtnInitFn>(base + 0x11AEE0u);
    g_orig_btn_recalc_rect    = reinterpret_cast<FfxiBtnRecalcRectFn>(base + 0x11AF60u);
    g_orig_btn_update_sprite  = reinterpret_cast<FfxiBtnUpdateSpriteFn>(base + 0x11B090u);
    g_orig_btn_set_state      = reinterpret_cast<FfxiBtnSetStateFn>(base + 0x11AE00u);
    g_orig_prim_set_selected  = reinterpret_cast<FfxiPrimSetSelectedFn>(base + 0x11A320u);
    g_orig_list_push_back     = reinterpret_cast<FfxiListPushBackFn>(base + 0x127910u);
    g_orig_list_remove_node   = reinterpret_cast<FfxiListRemoveNodeFn>(base + 0x127A10u);
    g_orig_close_menu         = reinterpret_cast<FfxiCloseMenuFn>(base + 0x15FD60u);

    const bool mw_c = install_call("menuwind_restore_cached", "commands_menu", scan::find(ffxi, kPatMenuWindRestoreCached),
                                   g_menuwind_restore_cached_patch, reinterpret_cast<void*>(&thunk_menuwind_restore_cursor),
                                   0u, g_orig_restore_saved_cursor);
    std::uintptr_t orig_restore_fresh = 0;
    const bool mw_f = install_call("menuwind_restore_fresh", "commands_menu", scan::find(ffxi, kPatMenuWindRestoreFresh),
                                   g_menuwind_restore_fresh_patch, reinterpret_cast<void*>(&thunk_menuwind_restore_cursor),
                                   0u, orig_restore_fresh);
    if (g_orig_restore_saved_cursor == 0 && orig_restore_fresh != 0)
        g_orig_restore_saved_cursor = orig_restore_fresh;
    const bool mw_n = install_call("menuwind_next_button", "commands_menu", scan::find(ffxi, kPatMenuWindNextButton),
                                   g_menuwind_next_button_patch, reinterpret_cast<void*>(&thunk_menuwind_next_button),
                                   0u, g_orig_get_next_button);

    bool mw_k = false;
    const std::uintptr_t mw_key_at = scan::find(ffxi, kPatMenuWindKeyItem);
    {
        HookStatus st{};
        st.id = "menuwind_key_item";
        st.group = "commands_menu";
        st.owner = "soulspire";
        st.disposition = "migrated";
        if (mw_key_at != 0) {
            st.rva = static_cast<std::uint32_t>(mw_key_at - base);
            g_menuwind_key_item_resume = mw_key_at + 5;
            if (imports::jump(reinterpret_cast<void*>(mw_key_at), reinterpret_cast<void*>(&thunk_menuwind_key_item))) {
                st.installed = true;
                st.detail = "prologue jump installed (5B)";
                mw_k = true;
            } else {
                st.detail = "jump patch failed";
            }
        } else {
            st.detail = "signature not found";
        }
        statuses.push_back(std::move(st));
    }
    if (mw_c && mw_f && mw_n && mw_k) {
        caps |= kCapCommandsMenuEntry;
    }

    std::uint32_t ok_count = 0;
    for (const auto& h : statuses) {
        if (h.installed)
            ++ok_count;
        log::write(L"Soulspire hook [%hs] (%hs, owner=%hs): %ls at +0x%X (%hs)",
                   h.id.c_str(), h.group.c_str(), h.owner.c_str(),
                   h.installed ? L"OK" : L"FAIL", h.rva, h.detail.c_str());
    }
    engine().set_hook_statuses(std::move(statuses), caps);
    log::write(L"Soulspire: VanaCore-native hooks ready (%u/%u active, caps=0x%X)",
               ok_count, static_cast<unsigned>(engine().diagnostics().hooks_total), caps);
}

void uninstall()
{
    g_magic_row_patch.uninstall();
    g_magic_desc_patch.uninstall();
    g_magic_target_patch.uninstall();
    g_magic_icon_patch.uninstall();
    g_ability_row_patch.uninstall();
    g_ability_desc_patch.uninstall();
    g_ability_target_patch.uninstall();
    g_ability_icon_patch.uninstall();
    g_magic_filter_call_patch.uninstall();
    g_ability_filter_call_patch.uninstall();
    g_menu_spell_elig_patch.uninstall();
    g_item_job_patch.uninstall();
    g_full_job1_patch.uninstall();
    g_full_job2_patch.uninstall();
    g_menuwind_restore_cached_patch.uninstall();
    g_menuwind_restore_fresh_patch.uninstall();
    g_menuwind_next_button_patch.uninstall();
}

} // namespace vanacore::soulspire::hooks
