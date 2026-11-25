#pragma once

// VanaCore-native coordinated FFXiMain.dll hooks for SoulspireXI (Phase B).
//
// Each hook is independently pattern-scanned and fail-closed verified against FFXiMain.dll
// (no brittle all-or-nothing PE timestamp cliff, no competing detours against VanaCore's own
// chatfont/buttons/packets hooks). Every hook's status, RVA, owner, and hit count is published
// into vanacore::soulspire::engine() for logs\vanacore.log, /soulspire status, and Client Sentinel.
namespace vanacore::soulspire::hooks {

void install_if_ready();
void uninstall();

} // namespace vanacore::soulspire::hooks
