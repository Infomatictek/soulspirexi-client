#pragma once

namespace vanacore::loader {

enum class GameStartOutcome {
    CleanExit,
    ExitedBeforeDirect3D,
};

struct GameStartClassification {
    GameStartOutcome outcome;
    int exit_code;
};

// Classifies IFFXiEntry::GameStart's return. Retail FFXI returns S_OK (0x00000000) even when
// GameMain aborts silently before Direct3D initialization (e.g., polcore patch.ver decryption failure).
// Any exit before Direct3DCreate8 is reached must be reported as a startup failure.
inline constexpr GameStartClassification classify_game_start_exit(long /*ended_hr*/, bool direct3d_reached,
                                                                  int failure_exit_code)
{
    if (!direct3d_reached)
        return {GameStartOutcome::ExitedBeforeDirect3D, failure_exit_code};
    return {GameStartOutcome::CleanExit, 0};
}

} // namespace vanacore::loader
