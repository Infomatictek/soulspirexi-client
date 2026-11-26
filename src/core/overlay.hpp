#pragma once

#include <string>

namespace vanacore::overlay {

// Hooks FFXiMain.dll's Direct3D 8 device: once it's created, the game window's title becomes
// `title` (the character's name while one is in the world), and `label` is drawn in the top-left
// corner, after the image `logo` (a file; none if it can't be read), as the game starts. With
// [client] fps.display=1 in `ini` (the default), an FPS counter (updated every 0.25s) is drawn in the
// corner fps.position names: 0 top-left (under the label), 1 top-right (the default), 2 bottom-right,
// 3 bottom-left.
// [client] font.family, font.size (pixels) and font.background (#RRGGBB or #RRGGBBAA) style both.
// The window's icon becomes `icon_file`'s (VanaCore.exe's) in place of the game's.
void install(const wchar_t* ini, std::wstring title, std::wstring label, std::wstring logo, std::wstring icon_file);

// vanacore.ini was just written (an addon's vana.config.set): its live settings are read again at the
// next frame, instead of at the next half-second look. Any thread.
void settings_changed();

// True inside the game's Present, while VanaCore's hook runs there (plugins', addons' and VanaCore's own frame). The game's thread.
bool in_present();

// True once FFXiMain.dll has invoked Direct3DCreate8 in this process. Any thread.
bool direct3d_reached();

} // namespace vanacore::overlay
