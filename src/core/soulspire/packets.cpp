#include "packets.hpp"

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

    // 0x00A (Zone-In) / 0x00B (Zone-Out): clear stale per-zone Soul & mob state.
    if (id == 0x00A || id == 0x00B) {
        st.reset_session();
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
        // Payload format: [4..7]="CRYS", [8]=version(1), [9]=op, [10]=job, [11]=branch, [12..15]=char_id
        const std::uint8_t ver = data[8];
        const std::uint8_t job = data[10];
        const std::uint8_t branch = data[11];
        const std::uint32_t char_id = read_u32_le(data + 12);
        if (ver == 1 && job >= 1 && job <= 22 && branch <= 3) {
            st.set_active_soul(char_id, job, branch, 0, branch > 0, "packet_1ff", now_ms);
        }
        return;
    }

    // 0x1FC (Soulspire HUD Identity / OXSI peer identity).
    if (id == 0x1FC && length >= 12) {
        st.note_packet_1fc();
        // Self OXHD frame: [4..7]="OXHD", [8]=version(1), [9]=job, [10]=branch, [11]=flags/level
        if (std::memcmp(data + 4, "OXHD", 4) == 0) {
            const std::uint8_t ver = data[8];
            const std::uint8_t job = data[9];
            const std::uint8_t branch = data[10];
            if (ver == 1 && job >= 1 && job <= 22 && branch <= 3) {
                st.set_active_soul(0, job, branch, 0, branch > 0, "packet_1fc", now_ms);
            }
        }
        return;
    }

    // 0x1FD (Soulforge OXTL).
    if (id == 0x1FD && length >= 8 && std::memcmp(data + 4, "OXTL", 4) == 0) {
        st.note_packet_1fd();
        return;
    }

    // 0x1FE (Mob Status OXKE).
    if (id == 0x1FE && length >= 8 && std::memcmp(data + 4, "OXKE", 4) == 0) {
        st.note_packet_1fe();
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
