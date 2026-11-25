#include "settings.hpp"

#include <filesystem>

#include "ini.hpp"

namespace vanacore {

Settings load_settings(const std::wstring& vanacore_dir, const std::wstring& ini)
{
    namespace fs = std::filesystem;
    const fs::path dir(vanacore_dir);

    Settings settings;
    // An absolute path replaces dir; lexically_normal() also collapses ".\\pol.exe"-style doubled backslashes.
    // None: VanaCore logs in itself.
    const std::wstring bootloader = ini_read(ini, L"client", L"bootloader");
    if (!bootloader.empty())
        settings.bootloader = (dir / bootloader).lexically_normal().wstring();
    settings.attached = GetPrivateProfileIntW(L"client", L"attached", 0, ini.c_str()) != 0;
    settings.autoclose = GetPrivateProfileIntW(L"client", L"autoclose", 0, ini.c_str()) != 0;
    settings.retail = GetPrivateProfileIntW(L"client", L"retail", 0, ini.c_str()) != 0;
    settings.retail_region = ini_read(ini, L"client", L"retail.region");
    // Unset is FFXI's quick start button; set, even to nothing, is what it says
    settings.retail_args = ini_read(ini, L"client", L"retail.args", kDefaultRetailArgs);
    // Not pol=: that is the CatsEyeXI client's PlayOnline, which a vanacore.ini made for it keeps whether or not retail=1
    const std::wstring retail_pol = ini_read(ini, L"client", L"retail.pol");
    if (!retail_pol.empty())
        settings.retail_pol = (dir / retail_pol).lexically_normal().wstring();
    settings.server = ini_read(ini, L"server", L"server", kDefaultServer);
    settings.user = ini_read(ini, L"server", L"user");
    std::wstring ini_password = ini_read(ini, L"server", L"password");
    settings.plaintext_password_in_ini = !ini_password.empty();
    if (!ini_password.empty()) {
        SecureZeroMemory(ini_password.data(), ini_password.size() * sizeof(wchar_t));
        ini_password.clear();
    }
    settings.login_version = ini_read(ini, L"server", L"loginversion");
    return settings;
}

} // namespace vanacore
