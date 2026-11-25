#pragma once

#include <string>

namespace vanacore {

inline constexpr wchar_t kDefaultServer[] = L"play.soulspirexi.com";

// What Windower starts pol.exe with: /game eAZcFcB turns on FFXI's quick start button
inline constexpr wchar_t kDefaultRetailArgs[] = L"/game eAZcFcB";

// vanacore.ini next to VanaCore.exe, or the file --ini names (the launcher's per account). Every key is optional; relative
// paths are from VanaCore.exe's folder. [client] pol= and ffxi= are read by VanaCore.Core (game_files.cpp), [ffxi] and
// [ffxi.registry] by registry.cpp; with retail=1 pol= and ffxi= are not used (retail.pol= picks the pol.exe to start).
struct Settings {
    std::wstring bootloader; // [client] bootloader= a bootloader (xiloader) to start the game; default: none,
                             //                   VanaCore logs in itself (login.cpp)
    bool attached = false;   // [client] attached=   1: stay with the game (closing VanaCore closes it)
    bool autoclose = false;  // [client] autoclose=  1: close the window when the game exits normally
    bool retail = false;     // [client] retail=     1: PlayOnline as installed (retail, Steam): no login, bootloader or [server];
                             //                   pol.exe from the registry (playonline.hpp) is started with VanaCore.Core in it
    std::wstring retail_region; // [client] retail.region= jp, us or eu; default: the first install the registry has
    std::wstring retail_args;   // [client] retail.args=   pol.exe's arguments; default /game eAZcFcB (FFXI's quick start button)
    std::wstring retail_pol; // [client] retail.pol= PlayOnline's folder in place of the registry's; empty: the registry's
    std::wstring server;     // [server] server=     default: play.soulspirexi.com
    std::wstring user;       // [server] user=       the login's (--user); interactive prompt if omitted
    std::wstring password;   // always empty from INI (plaintext [server] password= is refused for security)
    bool plaintext_password_in_ini = false; // true when [server] password= was present in INI
    std::wstring login_version; // [server] loginversion= the version VanaCore's login reports (2.0.1);
                                //                        default: its own, 2.1.2
};

// `ini`'s settings (vanacore.ini, or --ini's). The bootloader path comes back absolute and normalised, or empty.
Settings load_settings(const std::wstring& vanacore_dir, const std::wstring& ini);

} // namespace vanacore
