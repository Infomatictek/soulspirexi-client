#include "packets.hpp"

#include "input_ownership.hpp"
#include "state.hpp"

#include <cstring>
#include <string_view>

namespace vanacore::soulspire::packets {
namespace {

std::uint32_t read_u32_le(const std::uint8_t* p) noexcept
{
    std::uint32_t v = 0;
    std::memcpy(&v, p, sizeof(v));
    return v;
}

// Extracts the null-terminated message payload from an S2C 0x017 chat packet (offset 23, 0-based).
// Matches Lua `string.sub(buf, 24):match('^([^%z]*)')` (1-based index 24 == 0-based offset 23).
std::string_view extract_0x017_message(const std::uint8_t* data, std::size_t length) noexcept
{
    if (data == nullptr || length < 25)
        return {};
    const char* start = reinterpret_cast<const char*>(data + 23);
    const std::size_t max_len = length - 23;
    std::size_t len = 0;
    while (len < max_len && start[len] != '\0')
        ++len;
    return std::string_view(start, len);
}

bool is_internal_transport_prefix(std::string_view msg) noexcept
{
    return msg.starts_with("[OXHUD]|") ||
           msg.starts_with("[OXMOB]|") ||
           msg.starts_with("[OXTAL]|") ||
           msg.starts_with("[OXBST]|");
}

} // namespace

void on_incoming(EngineState& st, std::uint16_t id, const std::uint8_t* data, std::size_t length, std::uint64_t now_ms)
{
    if (!st.enabled() || data == nullptr || length < 4)
        return;

    // 0x00A (Zone-In) / 0x00B (Zone-Out): clear stale per-zone Soul & mob state and release UI capture.
    if (id == 0x00A || id == 0x00B) {
        st.reset_session();
        input_ownership().release_all(id == 0x00A ? "zone_in" : "zone_out", now_ms);
        return;
    }

    // 0x061 (Char Stats): main job (+0x0C) and main job level (+0x0D).
    if (id == 0x061 && length >= 0x0E) {
        const std::uint8_t main_job = data[0x0C];
        const std::uint8_t main_lvl = data[0x0D];
        if (main_job >= 1 && main_job <= 22 && main_lvl >= 1 && main_lvl <= 99) {
            st.update_live_job_level(main_job, main_lvl);
        }
        return;
    }

    // 0x1FF (CrystalSocket / Soulspire Identity packet: "CRYS" at +4..+7).
    if (id == 0x1FF && length >= 16 && std::memcmp(data + 4, "CRYS", 4) == 0) {
        st.note_packet_1ff();
        st.increment_hook_hit("packet_lifecycle");
        const std::uint8_t ver = data[8];
        if (ver == 1 && length >= 64 && data[13] == 24) {
            // Authoritative 64-byte GP_SERV_COMMAND_CRYSTAL_COLLECTION reply:
            // [8]=ver(1), [9]=result, [10..11]=requestId, [12]=job, [13]=entryCount(24), [14]=level,
            // [16 + job*2]=ownedMask, [16 + job*2 + 1]=activeBranch
            const std::uint8_t result = data[9];
            const std::uint8_t job = data[12];
            const std::uint8_t level = data[14];
            if (result == 0 && job >= 1 && job <= 22) {
                const std::uint8_t branch = data[16 + static_cast<std::size_t>(job) * 2u + 1u];
                if (branch <= 3) {
                    st.set_active_soul(0, job, branch, level, branch > 0, "packet_1ff", now_ms);
                }
            }
        } else if (ver == 1 && length == 16) {
            // Compact 16-byte CRYS identity frame (unit/synthetic harness): [10]=job, [11]=branch, [12..15]=char_id
            const std::uint8_t job = data[10];
            const std::uint8_t branch = data[11];
            const std::uint32_t char_id = read_u32_le(data + 12);
            if (job >= 1 && job <= 22 && branch <= 3) {
                st.set_active_soul(char_id, job, branch, 0, branch > 0, "packet_1ff", now_ms);
            }
        }
        return;
    }

    // 0x1FC (Soulspire HUD Identity / OXSI peer identity).
    if (id == 0x1FC && length >= 12) {
        st.note_packet_1fc();
        st.increment_hook_hit("packet_lifecycle");
        if (length >= 16 && std::memcmp(data + 4, "OXHD", 4) == 0) {
            // Authoritative binary OXHD frame:
            // [8]=ver(1), [9]=unlocked(0/1), [10]=job_id, [11]=key_len, [12..13]=kv_len, [16..]=soul_key
            const std::uint8_t ver = data[8];
            const bool unlocked = data[9] == 1;
            const std::uint8_t job_id = data[10];
            const std::uint8_t key_len = data[11];
            if (ver == 1 && unlocked && job_id >= 1 && job_id <= 22 && key_len > 0 &&
                static_cast<std::size_t>(16u + key_len) <= length) {
                const std::string_view key(reinterpret_cast<const char*>(data + 16), key_len);
                std::uint8_t resolved_job = 0, resolved_branch = 0;
                if (st.soul_key_to_pair(key, resolved_job, resolved_branch) && resolved_job == job_id) {
                    st.set_active_soul(0, resolved_job, resolved_branch, 0, resolved_branch > 0, "packet_1fc", now_ms);
                }
            }
        } else if (length >= 16 && std::memcmp(data + 4, "OXSI", 4) == 0) {
            // Peer OXSI frame: [4..7]="OXSI", [8]=version(1), [9]=job, [10]=branch, [11]=effective_level, [12..15]=actor_id
            const std::uint8_t ver = data[8];
            const std::uint8_t job = data[9];
            const std::uint8_t branch = data[10];
            const std::uint8_t eff_level = data[11];
            const std::uint32_t actor_id = read_u32_le(data + 12);
            if (ver == 1 && actor_id != 0 && job >= 1 && job <= 22 && branch >= 1 && branch <= 3) {
                st.upsert_peer_soul(actor_id, job, branch, eff_level, now_ms);
            }
        }
        return;
    }

    // 0x1FD (Soulforge OXTL).
    if (id == 0x1FD && length >= 8 && std::memcmp(data + 4, "OXTL", 4) == 0) {
        st.note_packet_1fd();
        st.increment_hook_hit("packet_lifecycle");
        return;
    }

    // 0x1FE (Mob Status OXKE).
    if (id == 0x1FE && length >= 8 && std::memcmp(data + 4, "OXKE", 4) == 0) {
        st.note_packet_1fe();
        st.increment_hook_hit("packet_lifecycle");
        return;
    }

    // 0x017 (Silent Soulspire transport frames inside chat packets).
    if (id == 0x017 && length >= 25) {
        const std::string_view msg = extract_0x017_message(data, length);
        if (msg.starts_with("[OXHUD]|")) {
            st.observe_oxhud_frame(msg.substr(8), now_ms);
        } else if (msg.starts_with("[OXBST]|")) {
            st.observe_oxbst_frame(msg.substr(8), now_ms);
        } else if (msg.starts_with("[OXMOB]|")) {
            st.observe_oxmob_frame(msg.substr(8), now_ms);
        } else if (msg.starts_with("[OXTAL]|")) {
            st.note_oxtal_frame();
        }
    }
}

bool should_suppress_for_game(EngineState& st, std::uint16_t id, const std::uint8_t* data, std::size_t length)
{
    if (!st.enabled() || id != 0x017 || data == nullptr || length < 25)
        return false;
    const std::string_view msg = extract_0x017_message(data, length);
    if (is_internal_transport_prefix(msg)) {
        st.note_suppressed_chat_frame();
        return true;
    }
    return false;
}

} // namespace vanacore::soulspire::packets
