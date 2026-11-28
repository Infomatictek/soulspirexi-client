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
constexpr char kPatSpellNameLookup[] = "51 8B 4C 24 08 53 55 83 C8 FF 56 57 85 C9 0F 84 ?? ?? ?? ?? 80 39 00 0F 84 ?? ?? ?? ?? 8B 0D ?? ?? ?? ?? 85 C9";
constexpr std::ptrdiff_t kSpellLookupTableOperand = 0x1F;
constexpr char kPatMenuSpellReject[] = "E8 ?? ?? ?? ?? 83 C4 28 84 C0 75 09 5F 5E B8 02 00 00 00 5B C3";
constexpr char kPatMenuSpellRange[] = "81 FF 00 02 00 00 7C 5C 81 FF E0 02 00 00 7D 5C 66 83 3D ?? ?? ?? ?? 00 7E 09 5F 5E B8 06 00 00 00 5B C3";
constexpr char kPatMenuSpellSuccess[] = "81 FF 00 03 00 00 7D 11 68 E5 01 00 00 E8 ?? ?? ?? ?? 83 C4 04 3C 01 75 06 5F 5E 33 C0 5B C3";
constexpr char kPatItemJobLabel[]     = "E8 ?? ?? ?? ?? 50 8D 4C 24 1C 68 ?? ?? ?? ?? 51 E8 ?? ?? ?? ?? 83 C4 14";
constexpr char kPatFullJob1[]         = "E8 ?? ?? ?? ?? 83 C4 08 8D 54 24 1C 83 C6 04 50 52 56 8D 44 24 38 68 20";
constexpr char kPatFullJob2[]         = "E8 ?? ?? ?? ?? 50 8D 44 24 20 83 C6 04 50 56 8D 4C 24 38 68 ?? ?? ?? ??";
// Native Commands menu ("menu    menuwind") hooks #28-#31 (Status -> Soulspire -> Equipment).
constexpr char kPatMenuWindRestoreCached[] = "E8 ?? ?? ?? ?? 83 7E 10 0E 74 24 8A 46 6E 84 C0 75 1D 8B 4E 0C 85 C9 0F";
constexpr char kPatMenuWindRestoreFresh[]  = "E8 ?? ?? ?? ?? 8B 4E 0C 85 C9 74 05 8B 11 FF 52 04 8A 44 24 38 84 C0 74";
constexpr char kPatMenuWindNextButton[]    = "E8 ?? ?? ?? ?? 8B F8 66 85 FF 0F 8C 84 01 00 00 57 8B CE E8";
constexpr char kPatMenuWindKeyItem[]       = "0F BF 44 24 04 83 C0 FD 56 83 F8 19 8B F1 0F 87 CF 02 00 00 33 C9 8A 88";
// Phase D weapon equipment & 3D presentation hooks (#22, #25-#27, #32-#35).
constexpr char kPatCanEquipInSlot[]        = "E8 ?? ?? ?? ?? 84 C0 75 24 FE 8E B3 01 00 00 5F 5E 5D 32 C0 5B 83 C4 10";
constexpr char kPatSubWeaponEquip[]        = "68 8B 00 00 00 E8 ?? ?? ?? ?? E9 D7 00 00 00 8A 44 24 10 84 C0 0F 84 DD";
constexpr char kPatSubMenuFilter[]         = "85 46 F4 74 5F 8B 42 0C 33 DB 8A 5C 10 18 8D 04 C0 8D 04 C0 03 D8 66 89";
constexpr char kPatSub2HMainEquip[]        = "68 99 00 00 00 E8 ?? ?? ?? ?? 8A 8E B3 01 00 00 FE C9 88 8E B3 01 00 00";
constexpr char kPatGetInventoryItem[]      = "8B 15 ?? ?? ?? ?? 85 D2 75 03 33 C0 C3 8B 44 24 04 8B 4C 24 08 56 8B F1";
constexpr char kPatGetItemResource[]       = "B8 AC 15 00 00 E8 ?? ?? ?? ?? 56 8B B4 24 B4 15 00 00 57 8B F9 85 F6 0F";
constexpr char kPatPcSubModelResolve[]     = "8D 4C 6D 00 C1 E1 04 8B 04 95 ?? ?? ?? ?? 89 44 24 24 8D 34 01 8B 04 01";
constexpr char kPatAttachEquipModel[]      = "8A 44 24 10 53 8B 5C 24 0C 55 C1 FB 0C 83 E3 0F 56 84 C0 57 8B F1 0F 84";
constexpr char kPatModelPartAddMesh[]      = "E8 ?? ?? ?? ?? 8B 0E E8 ?? ?? ?? ?? 6A 00 8D 4C 24 14 E8 ?? ?? ?? ?? 8B";
constexpr char kPatModelMeshDraw[]         = "8B 54 24 24 55 52 8B 54 24 34 53 52 FF 50 1C 8B 3F 83 C7 04 8B 37 85 F6";

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

// imports::jump writes five bytes, even when the relocated instruction is longer.
struct JumpPatch {
    std::uint8_t* site = nullptr;
    std::uint8_t original[5]{};
    std::uint8_t installed[5]{};

    bool install(std::uintptr_t address, void* replacement, std::size_t prologue = 0,
                 void** trampoline = nullptr) noexcept
    {
        auto* code = reinterpret_cast<std::uint8_t*>(address);
        std::memcpy(original, code, sizeof(original));
        if (trampoline != nullptr) {
            *trampoline = imports::detour(code, prologue, replacement);
            if (*trampoline == nullptr)
                return false;
        } else if (!imports::jump(code, replacement)) {
            return false;
        }
        std::memcpy(installed, code, sizeof(installed));
        site = code;
        return true;
    }

    void uninstall() noexcept
    {
        if (site == nullptr || std::memcmp(site, installed, sizeof(installed)) != 0)
            return;
        DWORD protect = 0;
        if (!VirtualProtect(site, sizeof(original), PAGE_EXECUTE_READWRITE, &protect))
            return;
        std::memcpy(site, original, sizeof(original));
        FlushInstructionCache(GetCurrentProcess(), site, sizeof(original));
        VirtualProtect(site, sizeof(original), protect, &protect);
        site = nullptr;
    }
};

using ActionIconFn   = void*(__cdecl*)(std::uint32_t, std::uint32_t);
using SpellNameLookupFn = int(__cdecl*)(const char*, int, int);
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
using GetInventoryItemFn     = const std::uint8_t*(__cdecl*)(std::uint32_t, std::uint32_t);
using GetItemResourceFn      = const std::uint8_t*(__fastcall*)(void*, void*, std::uint32_t);
using CanEquipItemInSlotFn   = std::uint8_t(__fastcall*)(void*, void*, const std::uint8_t*, std::uint32_t);
using AttachEquipModelFn     = void(__fastcall*)(void*, void*, void*, std::uint32_t, std::uint32_t, std::uint8_t);
using ModelPartAddMeshFn     = void*(__fastcall*)(void*, void*, void*);
using LookupBoneIndexFn      = std::uint32_t(__fastcall*)(void*, void*, std::int32_t);

// Saved originals & call patches
CallPatch g_magic_row_patch{};
CallPatch g_magic_desc_patch{};
CallPatch g_magic_target_patch{};
CallPatch g_magic_icon_patch{};
CallPatch g_ability_row_patch{};
CallPatch g_ability_desc_patch{};
CallPatch g_ability_target_patch{};
CallPatch g_ability_icon_patch{};
CallPatch g_ability_filter_call_patch{};
JumpPatch g_magic_list_patch{};
JumpPatch g_spell_lookup_patch{};
JumpPatch g_menu_spell_patch{};
CallPatch g_item_job_patch{};
CallPatch g_full_job1_patch{};
CallPatch g_full_job2_patch{};
CallPatch g_menuwind_restore_cached_patch{};
CallPatch g_menuwind_restore_fresh_patch{};
CallPatch g_menuwind_next_button_patch{};
CallPatch g_can_equip_in_slot_patch{};
CallPatch g_model_part_add_mesh_patch{};

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
SpellNameLookupFn g_orig_spell_name_lookup = nullptr;
std::uint8_t** g_spell_lookup_table_slot = nullptr;
std::uintptr_t g_menu_spell_range_resume = 0;
std::uintptr_t g_menu_spell_success_resume = 0;
std::uintptr_t g_menu_spell_reject_resume = 0;
std::uintptr_t g_magic_list_blu_check_resume = 0;
std::uintptr_t g_magic_list_add_row_resume = 0;
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

// Phase D weapon equipment & 3D presentation state/callees (#22, #25-#27, #32-#35).
void**                 g_item_res_mgr_slot = nullptr;
GetInventoryItemFn     g_orig_get_inv_item = nullptr;
GetItemResourceFn      g_orig_get_item_res = nullptr;
CanEquipItemInSlotFn   g_orig_can_equip_in_slot = nullptr;
std::uintptr_t         g_sub_weapon_allow_resume = 0;
std::uintptr_t         g_sub_weapon_deny_resume = 0;
std::uintptr_t         g_sub_menu_allow_resume = 0;
std::uintptr_t         g_sub_menu_skip_resume = 0;
std::uintptr_t         g_sub_2h_main_allow_resume = 0;
std::uintptr_t         g_sub_2h_main_deny_resume = 0;
std::uintptr_t         g_pc_sub_model_resume = 0;
AttachEquipModelFn     g_orig_attach_equip_model = nullptr;
ModelPartAddMeshFn     g_orig_model_part_add_mesh = nullptr;
LookupBoneIndexFn      g_orig_lookup_bone_index = nullptr;
std::uintptr_t         g_model_mesh_draw_resume = 0;

struct SubOffhandMeshEntry {
    void* actor = nullptr;
    void* model_part = nullptr;
    void* mesh = nullptr;
};
constexpr std::size_t kMaxSubOffhandMeshes = 64u;
std::mutex g_sub_offhand_mesh_mutex;
SubOffhandMeshEntry g_sub_offhand_meshes[kMaxSubOffhandMeshes]{};
std::atomic<std::uint32_t> g_sub_offhand_mesh_count{0};

thread_local bool  g_tls_sub_offhand_attach_active = false;
thread_local void* g_tls_sub_offhand_attach_actor = nullptr;

struct SavedBoneMatrixState {
    bool   active = false;
    float* target = nullptr;
    float  saved[16]{};
};
thread_local SavedBoneMatrixState g_tls_saved_main_weapon_bone{};

inline std::uint16_t read_u16_le(const std::uint8_t* p) noexcept
{
    std::uint16_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

inline std::uint32_t read_u32_le(const std::uint8_t* p) noexcept
{
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

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

std::uint8_t* g_spell_res_table_base = nullptr;
std::uint32_t g_spell_res_applied_gen = 0;
std::unordered_map<std::uint16_t, std::array<std::uint8_t, 100>> g_spell_res_originals;

void restore_dynamic_spell_res_table() noexcept
{
    if (g_spell_res_table_base == nullptr || g_spell_res_originals.empty())
        return;
    for (const auto& [sid, orig] : g_spell_res_originals) {
        if (sid >= 1 && sid <= 1023) {
            std::memcpy(g_spell_res_table_base + static_cast<std::size_t>(sid) * 100u, orig.data(), orig.size());
        }
    }
    g_spell_res_originals.clear();
    g_spell_res_applied_gen = 0;
}

void sync_dynamic_spell_res_table(const std::uint8_t* observed_spell_res, std::uint16_t observed_id) noexcept
{
    if (observed_spell_res == nullptr || observed_id == 0 || observed_id > 1023)
        return;
    const std::uint32_t gen = engine().dynamic_spellbook_generation();
    if (gen == 0 || (g_spell_res_table_base != nullptr && g_spell_res_applied_gen == gen))
        return;

    // Derive base of FFXiMain's 1024x100-byte spell resource table and sanity-check entry #1 (Cure).
    std::uint8_t* candidate_base = const_cast<std::uint8_t*>(observed_spell_res) - static_cast<std::size_t>(observed_id) * 100u;
    std::uint16_t cure_id = 0;
    std::uint16_t cure_type = 0;
    std::memcpy(&cure_id, candidate_base + 100u, sizeof(cure_id));
    std::memcpy(&cure_type, candidate_base + 102u, sizeof(cure_type));
    if (cure_id != 1u || cure_type != 1u)
        return;

    g_spell_res_table_base = candidate_base;
    const auto spells = engine().all_dynamic_spells();
    for (const auto& sp : spells) {
        if (sp.spell_id == 0 || sp.spell_id > 1023)
            continue;
        std::uint8_t* entry = g_spell_res_table_base + static_cast<std::size_t>(sp.spell_id) * 100u;
        if (g_spell_res_originals.find(sp.spell_id) == g_spell_res_originals.end()) {
            std::array<std::uint8_t, 100> snap{};
            std::memcpy(snap.data(), entry, snap.size());
            g_spell_res_originals.emplace(sp.spell_id, snap);
        }
        std::memcpy(entry + 0, &sp.spell_id, sizeof(sp.spell_id));
        std::memcpy(entry + 2, &sp.client_magic_type, sizeof(sp.client_magic_type));
        std::memcpy(entry + 4, &sp.client_element, sizeof(sp.client_element));
        std::memcpy(entry + 6, &sp.client_valid_targets, sizeof(sp.client_valid_targets));
        std::memcpy(entry + 8, &sp.server_skill, sizeof(sp.server_skill));
        std::memcpy(entry + 10, &sp.mp_cost, sizeof(sp.mp_cost));
        entry[12] = sp.client_cast_qsec;
        entry[13] = sp.client_recast_qsec;
        // Retail FFXI spell_res stores 24 uint16_t job unlock levels at +14..+61 (48 bytes).
        // Set all job levels to 0xFFFF except the owning Soul's jobId at uint16_t(unlock_level).
        std::memset(entry + 14, 0xFF, 48);
        std::memset(entry + 92, 0, 4); // No inherited retail job/level rule.
        if (sp.job_id >= 1 && sp.job_id <= 22) {
            const std::uint16_t ulvl = static_cast<std::uint16_t>(sp.unlock_level);
            std::memcpy(entry + 14u + static_cast<std::size_t>(sp.job_id) * 2u, &ulvl, sizeof(ulvl));
        }
        // Ensure valid Black Magic icon slot (v1=8, v2=240) when repurposing unused BLU spell slots.
        const std::uint16_t dsb_icon_v1 = 8u;
        const std::uint16_t dsb_icon_v2 = 240u;
        std::memcpy(entry + 64u, &dsb_icon_v1, sizeof(dsb_icon_v1));
        std::memcpy(entry + 66u, &dsb_icon_v2, sizeof(dsb_icon_v2));
        // Clear BLU-inherited requirements/range/AoE/cursor fields (SOUL-ACTION-002):
        // Self spells (0x01) use range=0, cursor=0x01; enemy/ally spells use range=12 (20y), cursor=0x0D/0x00.
        entry[68] = 0u;
        entry[69] = (sp.client_valid_targets == 0x01u) ? 0u : 12u;
        entry[70] = 0u;
        entry[71] = 0u;
        entry[72] = (sp.client_valid_targets == 0x01u)
            ? 0x01u
            : ((sp.client_valid_targets & 0x20u) != 0u ? 0x0Du : 0x00u);
    }
    g_spell_res_applied_gen = gen;
}

// SOUL-ACTION-002: Consolidates magic_list_filter (0x22DAB9 -> 0x22DAC9: `cmp esi, 0x200; jl +0x64`).
// Retail Magic::Populate gates all spell IDs >= 512 through Blue Magic set-spell checks even when
// CanShowSpell (0x22CDE0) returns 1 and spell_res->Type is Black Magic. For active non-BLU dynamic
// spells in the retail BLU range (512..735), jump to the add-row path (0x22DB35).
// Otherwise the thunk replays the comparison and resumes at the original branch.
std::uint8_t __stdcall should_bypass_magic_list_blu_check(std::uint32_t spell_id) noexcept
{
    if (spell_id >= 0x200u && spell_id < 0x2E0u &&
        engine().is_dynamic_non_blu_spell_allowed(static_cast<std::uint16_t>(spell_id))) {
        engine().increment_hook_hit("magic_list_filter");
        return 1;
    }
    return 0;
}

std::uint8_t __cdecl hook_magic_filter(const std::uint8_t* spell_res)
{
    if (spell_res != nullptr) {
        std::uint16_t spell_id = 0;
        std::memcpy(&spell_id, spell_res, sizeof(spell_id));
        if (spell_id > 0 && spell_id <= 1023) {
            sync_dynamic_spell_res_table(spell_res, spell_id);
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

int __cdecl hook_spell_name_lookup(const char* name, int magic_type, int check_eligibility)
{
    auto* table = *g_spell_lookup_table_slot;
    if (name != nullptr && *name != '\0' && table != nullptr) {
        sync_dynamic_spell_res_table(table + 100u, 1);
        if (const int spell_id = engine().resolve_dynamic_spell_name(name, magic_type); spell_id != 0) {
            if (check_eligibility != 0)
                engine().increment_hook_hit("missing_spell_bit");
            return spell_id;
        }
    }

    const int result = g_orig_spell_name_lookup(name, magic_type, check_eligibility);
    if (result != -2 || table == nullptr)
        return result;

    // The original missing-bit hook let explicitly shown spells through when the
    // learned bit was absent. Retry only that retail failure with the check disabled.
    const int spell_id = g_orig_spell_name_lookup(name, magic_type, 0);
    if (spell_id > 0 && spell_id <= 1023 &&
        engine().decide_visibility(ActionKind::Spell, static_cast<std::uint16_t>(spell_id)) ==
            VisibilityDecision::Show) {
        engine().increment_hook_hit("missing_spell_bit");
        return spell_id;
    }
    return result;
}

std::uint8_t __stdcall decide_menu_spell_range(std::uint32_t spell_id) noexcept
{
    if (spell_id > 1023u)
        return 0;

    const auto id = static_cast<std::uint16_t>(spell_id);
    if (engine().decide_visibility(ActionKind::Spell, id) == VisibilityDecision::Hide) {
        engine().increment_hook_hit("menu_spell_elig");
        return 2;
    }
    if (spell_id >= 0x200u && spell_id < 0x300u && engine().is_dynamic_non_blu_spell_allowed(id)) {
        engine().increment_hook_hit("menu_spell_elig");
        return 1;
    }
    return 0;
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

__declspec(naked) void thunk_magic_list_blu_bypass()
{
    __asm {
        pushfd
        pushad
        push esi
        call should_bypass_magic_list_blu_check
        test al, al
        jnz add_row
        popad
        popfd
        cmp esi, 200h
        jmp dword ptr [g_magic_list_blu_check_resume]
    add_row:
        popad
        popfd
        jmp dword ptr [g_magic_list_add_row_resume]
    }
}

__declspec(naked) void thunk_menu_spell_range()
{
    __asm {
        pushfd
        pushad
        push edi
        call decide_menu_spell_range
        cmp al, 2
        je rejected
        test al, al
        jnz allowed
        popad
        popfd
        cmp edi, 200h
        jmp dword ptr [g_menu_spell_range_resume]
    allowed:
        popad
        popfd
        jmp dword ptr [g_menu_spell_success_resume]
    rejected:
        popad
        popfd
        jmp dword ptr [g_menu_spell_reject_resume]
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

// ============================================================================
// Phase D — Native Weapon Equipment & 3D Presentation Hooks (#22, #25-#27, #32-#35)
// ============================================================================

std::uint8_t __stdcall soul_allows_sub_weapon(const std::uint8_t* sub_item_res, const std::uint8_t* stack_frame) noexcept
{
    if (!engine().enabled() || sub_item_res == nullptr || stack_frame == nullptr)
        return 0;
    if (g_orig_get_inv_item == nullptr || g_orig_get_item_res == nullptr || g_item_res_mgr_slot == nullptr)
        return 0;
    if (!engine().has_weapon_rules_for_active())
        return 0;

    const std::uint8_t main_slot = stack_frame[0x10];
    if (main_slot == 0 || main_slot == 0xFFu)
        return 0;
    const std::uint32_t main_container = read_u32_le(stack_frame + 0x18);
    const std::uint8_t* main_inv = g_orig_get_inv_item(main_container, static_cast<std::uint32_t>(main_slot));
    if (main_inv == nullptr)
        return 0;
    const std::uint16_t main_item_id = read_u16_le(main_inv);
    if (main_item_id == 0)
        return 0;
    void* res_mgr = *g_item_res_mgr_slot;
    if (res_mgr == nullptr)
        return 0;
    const std::uint8_t* main_res = g_orig_get_item_res(res_mgr, nullptr, static_cast<std::uint32_t>(main_item_id));
    if (main_res == nullptr)
        return 0;

    const std::uint16_t main_skill = read_u16_le(main_res + 0x10);
    const std::uint16_t sub_item_id = read_u16_le(sub_item_res);
    const std::uint16_t sub_skill = read_u16_le(sub_item_res + 0x10);
    if (engine().allows_sub_weapon(main_skill, main_item_id, sub_skill, sub_item_id)) {
        engine().increment_hook_hit("sub_weapon_equip");
        return 1;
    }
    return 0;
}

__declspec(naked) void thunk_sub_weapon_equip()
{
    __asm {
        pushfd
        pushad
        lea eax, [esp + 36]
        push eax
        push esi
        call soul_allows_sub_weapon
        test al, al
        jz stock
        popad
        popfd
        jmp dword ptr [g_sub_weapon_allow_resume]
    stock:
        popad
        popfd
        push 0x8b
        jmp dword ptr [g_sub_weapon_deny_resume]
    }
}

std::uint8_t __stdcall soul_allows_dual_2h_menu_item(std::uint32_t item_id) noexcept
{
    if (!engine().enabled() || item_id == 0 || item_id > 0xFFFFu)
        return 0;
    if (g_item_res_mgr_slot == nullptr || g_orig_get_item_res == nullptr)
        return 0;
    if (!engine().has_weapon_rules_for_active())
        return 0;
    void* res_mgr = *g_item_res_mgr_slot;
    if (res_mgr == nullptr)
        return 0;
    const std::uint8_t* sub_res = g_orig_get_item_res(res_mgr, nullptr, item_id);
    if (sub_res == nullptr)
        return 0;
    const std::uint16_t sub_skill = read_u16_le(sub_res + 0x10);
    if (engine().allows_dual_2h_menu_item(sub_skill, static_cast<std::uint16_t>(item_id))) {
        engine().increment_hook_hit("sub_menu_filter");
        return 1;
    }
    return 0;
}

__declspec(naked) void thunk_sub_menu_filter()
{
    __asm {
        test [esi - 0x0C], eax
        jnz allow_item
        cmp eax, 2
        jne skip_item
        test dword ptr [esi - 0x0C], 1
        jz skip_item
        pushfd
        pushad
        movzx eax, di
        push eax
        call soul_allows_dual_2h_menu_item
        test al, al
        jz deny_item
        popad
        popfd
    allow_item:
        jmp dword ptr [g_sub_menu_allow_resume]
    deny_item:
        popad
        popfd
    skip_item:
        jmp dword ptr [g_sub_menu_skip_resume]
    }
}

std::uint8_t __stdcall soul_allows_2h_main_with_sub(const std::uint8_t* main_res, const std::uint8_t* sub_res) noexcept
{
    if (!engine().enabled() || main_res == nullptr || sub_res == nullptr)
        return 0;
    const std::uint16_t main_item_id = read_u16_le(main_res);
    const std::uint16_t main_skill = read_u16_le(main_res + 0x10);
    const std::uint16_t sub_item_id = read_u16_le(sub_res);
    const std::uint16_t sub_skill = read_u16_le(sub_res + 0x10);
    if (engine().allows_2h_main_with_sub(main_skill, main_item_id, sub_skill, sub_item_id)) {
        engine().record_local_equipped_weapons(main_item_id, main_skill, sub_item_id, sub_skill);
        engine().increment_hook_hit("sub_2h_main_equip");
        return 1;
    }
    return 0;
}

__declspec(naked) void thunk_sub_2h_main_equip()
{
    __asm {
        pushfd
        pushad
        push esi
        push edi
        call soul_allows_2h_main_with_sub
        test al, al
        jz stock
        popad
        popfd
        jmp dword ptr [g_sub_2h_main_allow_resume]
    stock:
        popad
        popfd
        push 0x99
        jmp dword ptr [g_sub_2h_main_deny_resume]
    }
}

std::uint8_t __fastcall hook_can_equip_in_slot(
    void* self, void*, const std::uint8_t* parsed_item, std::uint32_t equip_slot) noexcept
{
    if (g_orig_can_equip_in_slot == nullptr)
        return 0;
    if (g_orig_can_equip_in_slot(self, nullptr, parsed_item, equip_slot) != 0)
        return 1;
    if (!engine().enabled() || equip_slot != 1u || parsed_item == nullptr)
        return 0;
    if (g_orig_can_equip_in_slot(self, nullptr, parsed_item, 0u) == 0)
        return 0;
    const std::uint16_t sub_item_id = read_u16_le(parsed_item + 0x14B0);
    const std::uint16_t* skill_ptr = nullptr;
    std::memcpy(&skill_ptr, parsed_item + 0x14E4, sizeof(skill_ptr));
    if (skill_ptr == nullptr)
        return 0;
    if (engine().allows_dual_2h_menu_item(*skill_ptr, sub_item_id)) {
        engine().increment_hook_hit("can_equip_in_slot");
        return 1;
    }
    return 0;
}

bool safe_read_actor_server_id(const std::uint8_t* actor, std::uint32_t& out_actor_id) noexcept
{
    out_actor_id = 0;
    if (actor == nullptr)
        return false;
    __try {
        const std::uint8_t* ent = nullptr;
        std::memcpy(&ent, actor + 0x70, sizeof(ent));
        if (ent == nullptr)
            return false;
        std::memcpy(&out_actor_id, ent + 0x78, sizeof(out_actor_id));
        return out_actor_id != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out_actor_id = 0;
        return false;
    }
}

void clear_sub_offhand_meshes_for_actor(void* actor) noexcept
{
    if (actor == nullptr)
        return;
    std::lock_guard lock(g_sub_offhand_mesh_mutex);
    std::uint32_t count = 0;
    for (auto& e : g_sub_offhand_meshes) {
        if (e.actor == actor) {
            e = {};
        } else if (e.mesh != nullptr) {
            ++count;
        }
    }
    g_sub_offhand_mesh_count.store(count, std::memory_order_release);
}

void clear_all_sub_offhand_meshes() noexcept
{
    std::lock_guard lock(g_sub_offhand_mesh_mutex);
    for (auto& e : g_sub_offhand_meshes)
        e = {};
    g_sub_offhand_mesh_count.store(0u, std::memory_order_release);
}

void register_sub_offhand_mesh(void* actor, void* model_part, void* mesh) noexcept
{
    if (actor == nullptr || model_part == nullptr || mesh == nullptr)
        return;
    std::lock_guard lock(g_sub_offhand_mesh_mutex);
    SubOffhandMeshEntry* free_slot = nullptr;
    std::uint32_t count = 0;
    bool exists = false;
    for (auto& e : g_sub_offhand_meshes) {
        if (e.model_part == model_part && e.mesh == mesh) {
            e.actor = actor;
            exists = true;
        }
        if (e.mesh != nullptr) {
            ++count;
        } else if (free_slot == nullptr) {
            free_slot = &e;
        }
    }
    if (!exists && free_slot != nullptr) {
        free_slot->actor = actor;
        free_slot->model_part = model_part;
        free_slot->mesh = mesh;
        ++count;
    }
    g_sub_offhand_mesh_count.store(count, std::memory_order_release);
}

bool is_registered_sub_offhand_mesh(void* model_part, void* mesh) noexcept
{
    if (model_part == nullptr || mesh == nullptr)
        return false;
    if (g_sub_offhand_mesh_count.load(std::memory_order_acquire) == 0u)
        return false;
    std::lock_guard lock(g_sub_offhand_mesh_mutex);
    for (const auto& e : g_sub_offhand_meshes) {
        if (e.model_part == model_part && e.mesh == mesh)
            return true;
    }
    return false;
}

std::uint32_t __stdcall soul_resolve_pc_sub_model_table_slot(const std::uint8_t* actor, std::uint32_t look_word) noexcept
{
    if (!engine().enabled())
        return 7u;
    const std::uint16_t sub_model_id = static_cast<std::uint16_t>(look_word & 0x0FFFu);
    std::uint32_t actor_id = 0;
    if (!safe_read_actor_server_id(actor, actor_id))
        return 7u;
    if (engine().authorizes_sub_main_namespace(actor_id, sub_model_id, GetTickCount64())) {
        engine().increment_hook_hit("pc_sub_model_resolve");
        return 6u;
    }
    return 7u;
}

__declspec(naked) void thunk_pc_sub_model_resolve()
{
    __asm {
        cmp ebp, 7
        jne stock
        pushfd
        push eax
        push edx
        push ebx
        push edi
        call soul_resolve_pc_sub_model_table_slot
        mov ecx, eax
        pop edx
        pop eax
        popfd
        lea ecx, [ecx + ecx*2]
        shl ecx, 4
        jmp dword ptr [g_pc_sub_model_resume]
    stock:
        lea ecx, [ebp + ebp*2]
        shl ecx, 4
        jmp dword ptr [g_pc_sub_model_resume]
    }
}

void __fastcall hook_attach_equip_model(
    void* actor, void* edx, void* res_handle, std::uint32_t slot_and_model, std::uint32_t actor_gen, std::uint8_t flag)
{
    const std::uint32_t slot = (slot_and_model >> 12u) & 0x0Fu;
    const bool prev_active = g_tls_sub_offhand_attach_active;
    void* const prev_actor = g_tls_sub_offhand_attach_actor;

    if (engine().enabled() && actor != nullptr && (slot == 6u || slot == 7u)) {
        std::uint32_t actor_id = 0;
        const bool have_actor_id = safe_read_actor_server_id(static_cast<const std::uint8_t*>(actor), actor_id);
        const std::uint64_t now_ms = GetTickCount64();

        // Flintlock (and any appearanceMasks Soul) hides melee Main/Sub weapon models cleanly.
        if (flag != 0u && have_actor_id && engine().should_hide_weapon_slot(actor_id, slot, now_ms)) {
            if (slot == 7u)
                clear_sub_offhand_meshes_for_actor(actor);
            engine().increment_hook_hit("attach_equip_model");
            if (g_orig_attach_equip_model != nullptr) {
                g_orig_attach_equip_model(actor, edx, nullptr, slot_and_model & 0xF000u, actor_gen, 0u);
            }
            return;
        }

        // Berserker Dual-2H Sub-slot offhand bone (0x7F left-hand) attachment registration.
        if (slot == 7u) {
            clear_sub_offhand_meshes_for_actor(actor);
            const std::uint16_t sub_model_id = static_cast<std::uint16_t>(slot_and_model & 0x0FFFu);
            if (flag != 0u && res_handle != nullptr && have_actor_id &&
                engine().authorizes_sub_main_namespace(actor_id, sub_model_id, now_ms)) {
                engine().increment_hook_hit("attach_equip_model");
                g_tls_sub_offhand_attach_actor = actor;
                g_tls_sub_offhand_attach_active = true;
            } else {
                g_tls_sub_offhand_attach_actor = nullptr;
                g_tls_sub_offhand_attach_active = false;
            }
        }
    }

    if (g_orig_attach_equip_model != nullptr) {
        g_orig_attach_equip_model(actor, edx, res_handle, slot_and_model, actor_gen, flag);
    }
    g_tls_sub_offhand_attach_active = prev_active;
    g_tls_sub_offhand_attach_actor = prev_actor;
}

void* __fastcall hook_model_part_add_mesh(void* model_part, void* edx, void* os2_res)
{
    void* mesh = (g_orig_model_part_add_mesh != nullptr)
        ? g_orig_model_part_add_mesh(model_part, edx, os2_res)
        : nullptr;
    if (g_tls_sub_offhand_attach_active && g_tls_sub_offhand_attach_actor != nullptr &&
        model_part != nullptr && mesh != nullptr) {
        register_sub_offhand_mesh(g_tls_sub_offhand_attach_actor, model_part, mesh);
        engine().increment_hook_hit("model_part_add_mesh");
    }
    return mesh;
}

void soul_pre_mesh_draw_swap_impl(void* model_part, void* mesh) noexcept
{
    g_tls_saved_main_weapon_bone.active = false;
    g_tls_saved_main_weapon_bone.target = nullptr;
    if (!engine().enabled() || g_orig_lookup_bone_index == nullptr)
        return;
    if (!is_registered_sub_offhand_mesh(model_part, mesh))
        return;

    const std::uint32_t idx_main = g_orig_lookup_bone_index(model_part, nullptr, 0x7E) & 0xFFu;
    const std::uint32_t idx_sub  = g_orig_lookup_bone_index(model_part, nullptr, 0x7F) & 0xFFu;
    if (idx_main == 0u || idx_sub == 0u || idx_main == idx_sub || idx_main >= 256u || idx_sub >= 256u)
        return;

    float* matrices = nullptr;
    std::memcpy(&matrices, reinterpret_cast<const std::uint8_t*>(model_part) + 0x14u, sizeof(matrices));
    if (matrices == nullptr)
        return;

    float* main_mat = matrices + (static_cast<std::size_t>(idx_main) * 16u);
    float* sub_mat  = matrices + (static_cast<std::size_t>(idx_sub) * 16u);
    std::memcpy(g_tls_saved_main_weapon_bone.saved, main_mat, sizeof(g_tls_saved_main_weapon_bone.saved));
    std::memcpy(main_mat, sub_mat, sizeof(g_tls_saved_main_weapon_bone.saved));
    g_tls_saved_main_weapon_bone.target = main_mat;
    g_tls_saved_main_weapon_bone.active = true;
    engine().increment_hook_hit("model_mesh_draw");
}

void __stdcall soul_pre_mesh_draw_swap(void* model_part, void* mesh) noexcept
{
    __try {
        soul_pre_mesh_draw_swap_impl(model_part, mesh);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_tls_saved_main_weapon_bone.active = false;
        g_tls_saved_main_weapon_bone.target = nullptr;
    }
}

void __stdcall soul_post_mesh_draw_restore() noexcept
{
    if (g_tls_saved_main_weapon_bone.active && g_tls_saved_main_weapon_bone.target != nullptr) {
        std::memcpy(g_tls_saved_main_weapon_bone.target, g_tls_saved_main_weapon_bone.saved,
                    sizeof(g_tls_saved_main_weapon_bone.saved));
    }
    g_tls_saved_main_weapon_bone.active = false;
    g_tls_saved_main_weapon_bone.target = nullptr;
}

__declspec(naked) void thunk_model_mesh_draw()
{
    __asm {
        push eax
        push ecx
        push edx
        mov edx, dword ptr [esp + 0x30]
        push ecx
        push edx
        call soul_pre_mesh_draw_swap
        pop edx
        pop ecx
        pop eax
        mov edx, dword ptr [esp + 0x24]
        push ebp
        push edx
        mov edx, dword ptr [esp + 0x34]
        push ebx
        push edx
        call dword ptr [eax + 0x1c]
        push eax
        push ecx
        push edx
        call soul_post_mesh_draw_restore
        pop edx
        pop ecx
        pop eax
        jmp dword ptr [g_model_mesh_draw_resume]
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

    // Consolidated magic_list_filter (0x22DAB9 + 0x10 = 0x22DAC9: `81 FE 00 02 00 00 7C 64`).
    // `can_use_spell` (0x22CDE0) already detours the call at 0x22DAB9; patching 0x22DAC9 lets active
    // non-BLU dynamic spells on >=512 IDs skip the Blue Magic equipped-slot filter (zero new hooks).
    {
        HookStatus st{};
        st.id = "magic_list_filter";
        st.group = "visibility";
        st.owner = "soulspire";
        st.disposition = "migrated";
        const std::uintptr_t magic_filter_at = scan::find(ffxi, kPatMagicFilterCall);
        if (magic_filter_at != 0) {
            const std::uintptr_t blu_cmp_at = magic_filter_at + 0x10u; // 0x22DAC9: cmp esi, 0x200; jl +0x64
            st.rva = static_cast<std::uint32_t>(blu_cmp_at - base);
            g_magic_list_blu_check_resume = blu_cmp_at + 6u;             // 0x22DACF (original JL)
            g_magic_list_add_row_resume   = magic_filter_at + 0x18u + 0x64u; // 0x22DB35 (add row to Magic List)
            if (g_magic_list_patch.install(blu_cmp_at, reinterpret_cast<void*>(&thunk_magic_list_blu_bypass))) {
                st.installed = true;
                st.detail = "consolidated BLU-range bypass jump installed (5B)";
                caps |= kCapMagicVisibility;
            } else {
                st.detail = "jump patch failed";
            }
        } else {
            st.detail = "signature not found";
        }
        statuses.push_back(std::move(st));
    }

    // Hook #20 (Phase B.2): Ability/WeaponSkill list filter & compaction.
    install_call("ability_list_filter", "visibility", scan::find(ffxi, kPatAbilityFilterCall),
                 g_ability_filter_call_patch, reinterpret_cast<void*>(&thunk_ability_list_filter),
                 kCapAbilityVisibility, g_orig_set_list_count);

    {
        HookStatus st{};
        st.id = "missing_spell_bit";
        st.group = "visibility";
        st.owner = "soulspire";
        st.disposition = "migrated";
        const std::uintptr_t lookup_at = scan::find(ffxi, kPatSpellNameLookup);
        if (lookup_at != 0) {
            st.rva = static_cast<std::uint32_t>(lookup_at - base);
            g_spell_lookup_table_slot = reinterpret_cast<std::uint8_t**>(
                scan::operand(lookup_at, kSpellLookupTableOperand));
            void* trampoline = nullptr;
            if (g_spell_lookup_patch.install(lookup_at, reinterpret_cast<void*>(&hook_spell_name_lookup),
                                             5, &trampoline)) {
                g_orig_spell_name_lookup = reinterpret_cast<SpellNameLookupFn>(trampoline);
                st.installed = true;
                st.detail = "spell name lookup detour installed (5B)";
                caps |= kCapSpellEligibility;
            } else {
                st.detail = "detour install failed";
            }
        } else {
            st.detail = "signature not found";
        }
        statuses.push_back(std::move(st));
    }

    {
        HookStatus st{};
        st.id = "menu_spell_elig";
        st.group = "visibility";
        st.owner = "soulspire";
        st.disposition = "migrated";
        // The range gate follows retail action state, recast, job/level and MP checks.
        // Verify the original upper-range branch lands beside the expected success epilogue.
        const std::uintptr_t range_at = scan::find(ffxi, kPatMenuSpellRange);
        const std::uintptr_t success_at = range_at == 0 ? 0 :
            scan::find(range_at + 0x6Cu, 0x1Fu, kPatMenuSpellSuccess);
        const std::uintptr_t reject_at = range_at == 0 ? 0 :
            scan::find(range_at - 0x54u, 0x15u, kPatMenuSpellReject);
        if (success_at != 0 && reject_at != 0) {
            st.rva = static_cast<std::uint32_t>(range_at - base);
            g_menu_spell_range_resume = range_at + 6u;   // original JL
            g_menu_spell_success_resume = success_at + 0x19u; // return 0
            g_menu_spell_reject_resume = reject_at + 0x0Cu; // return 2
            if (g_menu_spell_patch.install(range_at, reinterpret_cast<void*>(&thunk_menu_spell_range))) {
                st.installed = true;
                st.detail = "menu spell range gate installed (5B)";
                caps |= kCapSpellEligibility;
            } else {
                st.detail = "jump patch failed";
            }
        } else {
            st.detail = "range or continuation signature not found";
        }
        statuses.push_back(std::move(st));
    }

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

    // Phase D weapon equipment & 3D presentation hooks (#22, #25-#27, #32-#35).
    g_item_res_mgr_slot = reinterpret_cast<void**>(base + 0x6229C0u);
    const std::uintptr_t get_inv_at = scan::find(ffxi, kPatGetInventoryItem);
    const std::uintptr_t get_res_at = scan::find(ffxi, kPatGetItemResource);
    g_orig_get_inv_item = reinterpret_cast<GetInventoryItemFn>(get_inv_at != 0 ? get_inv_at : (base + 0x0EB5D0u));
    g_orig_get_item_res = reinterpret_cast<GetItemResourceFn>(get_res_at != 0 ? get_res_at : (base + 0x15D210u));
    g_orig_lookup_bone_index = reinterpret_cast<LookupBoneIndexFn>(base + 0x02A9B0u);

    const bool w_can_equip = install_call("can_equip_in_slot", "weapons", scan::find(ffxi, kPatCanEquipInSlot),
                                          g_can_equip_in_slot_patch, reinterpret_cast<void*>(&hook_can_equip_in_slot),
                                          0u, g_orig_can_equip_in_slot);

    auto install_jump = [&](const char* id, const char* group, const char* pat, void* repl,
                            std::uintptr_t& out_at) {
        HookStatus st{};
        st.id = id;
        st.group = group;
        st.owner = "soulspire";
        st.disposition = "migrated";
        out_at = scan::find(ffxi, pat);
        if (out_at != 0) {
            st.rva = static_cast<std::uint32_t>(out_at - base);
            if (imports::jump(reinterpret_cast<void*>(out_at), repl)) {
                st.installed = true;
                st.detail = "prologue jump installed (5B)";
            } else {
                st.detail = "jump patch failed";
            }
        } else {
            st.detail = "signature not found";
        }
        const bool ok = st.installed;
        statuses.push_back(std::move(st));
        return ok;
    };

    std::uintptr_t sub_weap_at = scan::find(ffxi, kPatSubWeaponEquip);
    if (sub_weap_at != 0) {
        g_sub_weapon_deny_resume  = sub_weap_at + 5u;
        g_sub_weapon_allow_resume = sub_weap_at + 0x0Fu;
    }
    const bool w_sub_equip = install_jump("sub_weapon_equip", "weapons", kPatSubWeaponEquip,
                                          reinterpret_cast<void*>(&thunk_sub_weapon_equip), sub_weap_at);

    std::uintptr_t sub_menu_at = scan::find(ffxi, kPatSubMenuFilter);
    if (sub_menu_at != 0) {
        g_sub_menu_allow_resume = sub_menu_at + 5u;
        g_sub_menu_skip_resume  = sub_menu_at + 0x64u;
    }
    const bool w_sub_menu = install_jump("sub_menu_filter", "weapons", kPatSubMenuFilter,
                                         reinterpret_cast<void*>(&thunk_sub_menu_filter), sub_menu_at);

    std::uintptr_t sub_2h_at = scan::find(ffxi, kPatSub2HMainEquip);
    if (sub_2h_at != 0) {
        g_sub_2h_main_deny_resume  = sub_2h_at + 5u;
        g_sub_2h_main_allow_resume = sub_2h_at + 0x32u;
    }
    const bool w_sub_2h = install_jump("sub_2h_main_equip", "weapons", kPatSub2HMainEquip,
                                       reinterpret_cast<void*>(&thunk_sub_2h_main_equip), sub_2h_at);

    if (w_sub_equip)
        caps |= kCapSubWeaponRules;
    if (w_can_equip && w_sub_equip && w_sub_menu && w_sub_2h)
        caps |= kCapSub2HRules;

    std::uintptr_t pc_sub_at = scan::find(ffxi, kPatPcSubModelResolve);
    if (pc_sub_at != 0)
        g_pc_sub_model_resume = pc_sub_at + 7u;
    const bool w_pc_sub = install_jump("pc_sub_model_resolve", "weapons", kPatPcSubModelResolve,
                                       reinterpret_cast<void*>(&thunk_pc_sub_model_resolve), pc_sub_at);
    if (w_pc_sub)
        caps |= kCapWeaponModelResolve;

    clear_all_sub_offhand_meshes();
    bool w_attach = false;
    const std::uintptr_t attach_at = scan::find(ffxi, kPatAttachEquipModel);
    {
        HookStatus st{};
        st.id = "attach_equip_model";
        st.group = "weapons";
        st.owner = "soulspire";
        st.disposition = "migrated";
        if (attach_at != 0) {
            st.rva = static_cast<std::uint32_t>(attach_at - base);
            if (imports::detour<AttachEquipModelFn>(reinterpret_cast<AttachEquipModelFn>(attach_at), 5,
                                                    &hook_attach_equip_model, g_orig_attach_equip_model)) {
                st.installed = true;
                st.detail = "detour installed (5B)";
                w_attach = true;
            } else {
                st.detail = "detour install failed";
            }
        } else {
            st.detail = "signature not found";
        }
        statuses.push_back(std::move(st));
    }

    const bool w_add_mesh = install_call("model_part_add_mesh", "weapons", scan::find(ffxi, kPatModelPartAddMesh),
                                         g_model_part_add_mesh_patch, reinterpret_cast<void*>(&hook_model_part_add_mesh),
                                         0u, g_orig_model_part_add_mesh);

    std::uintptr_t mesh_draw_at = scan::find(ffxi, kPatModelMeshDraw);
    if (mesh_draw_at != 0)
        g_model_mesh_draw_resume = mesh_draw_at + 15u;
    const bool w_mesh_draw = install_jump("model_mesh_draw", "weapons", kPatModelMeshDraw,
                                          reinterpret_cast<void*>(&thunk_model_mesh_draw), mesh_draw_at);

    if (w_attach && w_add_mesh && w_mesh_draw)
        caps |= kCapWeaponOffhandBone;

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
    g_menu_spell_patch.uninstall();
    g_spell_lookup_patch.uninstall();
    g_magic_list_patch.uninstall();
    g_magic_row_patch.uninstall();
    g_magic_desc_patch.uninstall();
    g_magic_target_patch.uninstall();
    g_magic_icon_patch.uninstall();
    g_ability_row_patch.uninstall();
    g_ability_desc_patch.uninstall();
    g_ability_target_patch.uninstall();
    g_ability_icon_patch.uninstall();
    g_ability_filter_call_patch.uninstall();
    g_item_job_patch.uninstall();
    g_full_job1_patch.uninstall();
    g_full_job2_patch.uninstall();
    g_menuwind_restore_cached_patch.uninstall();
    g_menuwind_restore_fresh_patch.uninstall();
    g_menuwind_next_button_patch.uninstall();
    g_can_equip_in_slot_patch.uninstall();
    g_model_part_add_mesh_patch.uninstall();
    restore_dynamic_spell_res_table();
    clear_all_sub_offhand_meshes();
}

} // namespace vanacore::soulspire::hooks
