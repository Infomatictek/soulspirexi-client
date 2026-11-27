#pragma once

#include <cstddef>
#include <cstdint>

// VanaCore-native Soulspire packet lifecycle integration.
// Plugs directly into vanacore::packets::process() with zero competing detours on 0xDD6C0/0xDF7C0.
namespace vanacore::soulspire {

class EngineState;

namespace packets {

// Observes an incoming server packet before/alongside addon dispatch.
// Decodes 0x1FF (CRYS), 0x1FC (SSSI/SSHD; legacy OXSI/OXHD compat), 0x1FD (SSTL; legacy OXTL),
// 0x1FE (SSKE; legacy OXKE), 0x017 ([SSHUD]|[SSMOB]|[SSTAL]|[SSBST]; legacy [OX*] compat),
// 0x061 (job/level), and 0x00A/0x00B (zone transition hygiene).
void on_incoming(EngineState& st, std::uint16_t id, const std::uint8_t* data, std::size_t length, std::uint64_t now_ms);

// Returns true when an incoming packet is an internal Soulspire transport frame that must NEVER
// reach stock FFXI chat/UI (checked AFTER Lua addons have had a chance to observe it).
// Ordinary player/NPC/combat chat packets ALWAYS return false.
bool should_suppress_for_game(EngineState& st, std::uint16_t id, const std::uint8_t* data, std::size_t length);

} // namespace packets
} // namespace vanacore::soulspire
