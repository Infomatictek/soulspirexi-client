// settings.cpp / ini.cpp: the game's session copy of its settings file (VanaCore.exe's --ini, the launcher's for the
// account, else vanacore.ini). Touches temp files only (TMP/TEMP are pointed at a folder of its own), never the game, a
// vanacore.ini of anyone's or the GPU.
//
//   VanaCore.SettingsTest.exe        exit code 0: every check passed

#include "../../shared/vanacore/tls_client.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../ini.hpp"
#include "../loader_startup.hpp"
#include "../registry_policy.hpp"
#include "../settings.hpp"
#include "../soulspire/input_ownership.hpp"
#include "../soulspire/packets.hpp"
#include "../soulspire/state.hpp"
#include "../../launcher/login.hpp"
#include "../../shared/vanacore/boot_info.hpp"

namespace vanacore::log {
// settings.cpp's log lines, to the console here
void write(const wchar_t* format, ...)
{
    va_list args;
    va_start(args, format);
    wprintf(L"    log: ");
    vwprintf(format, args);
    wprintf(L"\n");
    va_end(args);
}
} // namespace vanacore::log

namespace {

int g_failed = 0;

void check(bool ok, const char* what)
{
    printf("%s: %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failed;
}

std::string read_bytes(const std::wstring& path)
{
    std::string bytes;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return bytes;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read != 0)
        bytes.append(buffer, read);
    CloseHandle(file);
    return bytes;
}

// a UTF-16 file's text (after its byte order mark)
std::wstring read_text(const std::wstring& path)
{
    const std::string bytes = read_bytes(path);
    if (bytes.size() < 2)
        return std::wstring();
    std::wstring text((bytes.size() - 2) / 2, L'\0');
    memcpy(text.data(), bytes.data() + 2, text.size() * 2);
    return text;
}

// What an external editor does: the whole file at once, UTF-16 with its byte order mark as the launcher writes it
void write_utf16(const std::wstring& path, const std::wstring& text)
{
    std::string bytes("\xFF\xFE", 2);
    bytes.append(reinterpret_cast<const char*>(text.data()), text.size() * 2);
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        printf("FAIL: couldn't write a test file\n");
        ++g_failed;
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
}

std::wstring value(const std::wstring& path, const wchar_t* section, const wchar_t* key)
{
    wchar_t text[256] = {};
    GetPrivateProfileStringW(section, key, L"<none>", text, 256, path.c_str());
    return text;
}

bool exists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool bom(const std::wstring& path)
{
    const std::string bytes = read_bytes(path);
    return bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE;
}

// no comment anywhere: no ; in the file at all (none of these tests' values has one)
bool plain(const std::wstring& path)
{
    const std::wstring text = read_text(path);
    return !text.empty() && text.find(L';') == std::wstring::npos;
}

// settings::tick looks once a second
void next_look()
{
    Sleep(1100);
    vanacore::settings::tick();
}

DWORD WINAPI plain_echo_server(void* p)
{
    SOCKET s = reinterpret_cast<SOCKET>(p);
    SOCKET c = ::accept(s, nullptr, nullptr);
    if (c != INVALID_SOCKET) {
        const char junk[] = "NOT_TLS_PLAINTEXT_BANNER\r\n";
        ::send(c, junk, static_cast<int>(sizeof(junk) - 1), 0);
        closesocket(c);
    }
    return 0;
}

} // namespace

int main()
{
    namespace settings = vanacore::settings;
    namespace ini = vanacore::ini;

    wchar_t temp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring base = std::wstring(temp) + L"VanaCoreSettingsTest." + std::to_wstring(GetCurrentProcessId());
    const std::wstring tmp = base + L"\\tmp";
    CreateDirectoryW(base.c_str(), nullptr);
    CreateDirectoryW(tmp.c_str(), nullptr);
    SetEnvironmentVariableW(L"TMP", tmp.c_str());
    SetEnvironmentVariableW(L"TEMP", tmp.c_str());

    // vanacore.ini as the launcher writes it: comments, a commented-out default, a comment after a value, the credentials
    const std::wstring file = base + L"\\vanacore.ini";
    write_utf16(file, L"; VanaCore's settings\r\n[client]\r\n;ini.upstream=0\r\npol=C:\\Games\\POL\r\nchat.color=#112233\r\n\r\n"
                      L"[server]\r\nuser=someone\r\npassword=secret\r\n\r\n[graphics]\r\n; the master switch\r\n"
                      L"effects=1 ; on by default\r\nrenderer=d3d12\r\n");

    // a copy of a process that ended, left behind
    const std::wstring folder = tmp + L"\\VanaCore";
    CreateDirectoryW(folder.c_str(), nullptr);
    const std::wstring stale = folder + L"\\vanacore.2147483000.ini";
    write_utf16(stale, L"[x]\r\n");
    const std::wstring other = folder + L"\\notes.ini";
    write_utf16(other, L"[x]\r\n");

    // 1. vanacore.ini (no --ini): the copy is made, plain, with the file's settings
    const std::wstring copy = settings::install(file, file);
    check(copy != file, "install returns a path of its own");
    check(copy == folder + L"\\vanacore." + std::to_wstring(GetCurrentProcessId()) + L".ini", "the copy is %TEMP%\\VanaCore\\vanacore.<pid>.ini");
    check(exists(copy), "the copy is there");
    check(!exists(stale), "a copy of a process that has ended is removed");
    check(exists(other), "a file that isn't a copy is left alone");
    check(value(copy, L"client", L"pol") == L"C:\\Games\\POL" && value(copy, L"server", L"password") == L"secret" &&
              value(copy, L"graphics", L"renderer") == L"d3d12",
          "Windows reads the copy as the file");
    check(value(copy, L"graphics", L"effects") == L"1", "a value's comment isn't in the copy (Windows read \"1 ; on by default\")");
    check(plain(copy) && bom(copy), "the copy is plain (no comments), UTF-16 with its byte order mark");
    check(!settings::upstream() && !settings::pending(), "upstream off, nothing pending");
    check(settings::file() == file && settings::shared() == file, "the file and vanacore.ini are one");

    // 2. something else writes the file: the game doesn't see it, and it's pending
    write_utf16(file, L"; VanaCore's settings\r\n[client]\r\n;ini.upstream=0\r\npol=C:\\Games\\POL\r\nchat.color=#112233\r\n\r\n"
                      L"[server]\r\nuser=someone\r\npassword=secret\r\n\r\n[graphics]\r\n; the master switch\r\n"
                      L"effects=0 ; on by default\r\nrenderer=d3d12\r\nantialias=fxaa\r\n");
    check(settings::pending(), "an external write is pending");
    next_look();
    check(value(copy, L"graphics", L"effects") == L"1" && value(copy, L"graphics", L"antialias") == L"<none>",
          "the copy doesn't take it (upstream off)");
    check(settings::pending(), "still pending after a tick");

    // 3. the game writes a key: the file has it and its external edits, the copy has it and none of them; the copy at
    // once, the file a few seconds after the game's last change
    check(ini::set(copy, L"graphics", L"shadows", L"0"), "ini::set on the copy succeeds");
    check(ini::set(copy, L"graphics", L"shadows", L"1 ; a note"), "and again (a slider dragged), with a comment");
    check(value(copy, L"graphics", L"shadows") == L"1", "the copy has it at once, without the comment");
    check(value(file, L"graphics", L"shadows") == L"<none>", "the file doesn't yet");
    check(!settings::pending(), "the game's own change waiting isn't taken for the file's");
    next_look();
    check(value(file, L"graphics", L"shadows") == L"<none>", "a second after, the file still doesn't");
    Sleep(2100);
    settings::tick();
    check(value(file, L"graphics", L"shadows") == L"1", "the key is in the file 3 seconds after the last change, without the comment");
    check(value(file, L"graphics", L"effects") == L"0 ; on by default" && value(file, L"graphics", L"antialias") == L"fxaa",
          "the file keeps the external edits, and its own comments");
    check(value(copy, L"graphics", L"effects") == L"1" && value(copy, L"graphics", L"antialias") == L"<none>",
          "the copy took none of the external edits");
    check(settings::pending(), "still pending");
    check(bom(file) && bom(copy), "UTF-16 byte order marks kept");
    check(ini::set(copy, L"client", L"chat.color", nullptr) && value(copy, L"client", L"chat.color") == L"<none>" && plain(copy),
          "a key removed from the copy is its line taken out");
    ini::save(true);
    check(read_text(file).find(L";chat.color=#112233") != std::wstring::npos, "in vanacore.ini it's commented out where it was");

    // 4. ini.upstream=1 (set on the copy, so also in the file): the next look takes the file
    check(ini::set(copy, L"client", L"ini.upstream", L"1"), "ini.upstream=1 set");
    check(settings::upstream(), "upstream on");
    ini::save(true);
    check(value(file, L"client", L"ini.upstream") == L"1", "ini.upstream=1 is in the file");
    next_look();
    check(value(copy, L"graphics", L"effects") == L"0" && value(copy, L"graphics", L"antialias") == L"fxaa", "taken up");
    check(!settings::pending(), "nothing pending after");
    check(plain(copy), "the copy is still plain");

    // 5. upstream off again; an external write is left; reload() takes it
    check(ini::set(copy, L"client", L"ini.upstream", L"0"), "ini.upstream=0 set");
    check(!settings::upstream(), "upstream off");
    ini::save(true);
    write_utf16(file, L"[client]\r\nini.upstream=0\r\npol=C:\\Games\\POL\r\n\r\n[server]\r\nuser=someone\r\npassword=secret\r\n\r\n"
                      L"[graphics]\r\neffects=2\r\nrenderer=d3d12\r\nantialias=fxaa\r\nshadows=1\r\n");
    next_look();
    check(value(copy, L"graphics", L"effects") == L"0", "an external write isn't taken up (upstream off)");
    check(settings::pending(), "pending");
    check(settings::reload(), "reload succeeds");
    check(value(copy, L"graphics", L"effects") == L"2", "reload took it up");
    check(!settings::pending(), "nothing pending after reload");

    settings::shutdown();
    check(!exists(copy), "shutdown deletes the copy");

    // 6. an account's own file (--ini, the launcher's: made from its profile, plain): the game reads and writes it alone
    const std::wstring mine = base + L"\\vanacore.b26bdff30f24.ini";
    write_utf16(file, L"[graphics]\r\neffects=9\r\nrenderer=d3d11\r\n\r\n[characters]\r\nmain=Player\r\n");
    write_utf16(mine, L"[client]\r\npol=C:\\Games\\POL\r\n\r\n[ffxi]\r\nresolution.game=2560,1440\r\n\r\n"
                      L"[graphics.effects]\r\ndebug=5\r\n\r\n[graphics]\r\ngrass.rings.show=1\r\n");
    const std::wstring copy2 = settings::install(mine, file);
    check(value(copy2, L"graphics.effects", L"debug") == L"<none>" && value(copy2, L"graphics", L"grass.rings.show") == L"<none>",
          "a debug view saved by an older VanaCore isn't in the copy: the game starts without one");
    check(!settings::pending(), "and the file having it isn't a change pending");
    check(ini::set(copy2, L"graphics.effects", L"debug", L"3"), "a debug view picked");
    ini::save(true);
    check(value(copy2, L"graphics.effects", L"debug") == L"3" && value(mine, L"graphics.effects", L"debug") == L"5",
          "the copy has it, the file never gets it");
    check(settings::reload() && value(copy2, L"graphics.effects", L"debug") == L"3", "reload keeps the view up");
    check(ini::set(copy2, L"graphics.effects", L"debug", nullptr), "the view put away");
    check(settings::file() == mine && settings::shared() == file, "the account's file, and vanacore.ini shared");
    check(value(copy2, L"ffxi", L"resolution.game") == L"2560,1440" && value(copy2, L"graphics", L"effects") == L"<none>" &&
              value(copy2, L"graphics", L"renderer") == L"<none>",
          "the copy is the account's file alone: nothing of vanacore.ini's (a missing key is VanaCore's default)");
    check(ini::set(copy2, L"graphics", L"bloom", L"1 ; my note"), "a setting set");
    check(ini::set(copy2, L"ffxi", L"resolution.game", nullptr), "a setting reset");
    ini::save(true);
    check(value(mine, L"graphics", L"bloom") == L"1" && value(mine, L"ffxi", L"resolution.game") == L"<none>",
          "they go to the account's file, without the comment");
    check(read_text(mine).find(L';') == std::wstring::npos, "which keeps no comment");
    check(value(file, L"graphics", L"bloom") == L"<none>" && value(file, L"graphics", L"effects") == L"9",
          "vanacore.ini isn't touched");
    write_utf16(file, L"[graphics]\r\neffects=5\r\n\r\n[characters]\r\nmain=Player\r\n");
    check(!settings::pending(), "a change to vanacore.ini isn't this game's");
    write_utf16(mine, L"[client]\r\npol=C:\\Games\\POL\r\n\r\n[graphics]\r\nbloom=0\r\n");
    check(settings::pending(), "a hand edit of the account's file is pending");
    check(ini::set(copy2, L"client", L"ini.upstream", L"1"), "upstream on");
    ini::save(true); // (the file is looked at once the game's own changes are in it)
    write_utf16(mine, L"[client]\r\npol=C:\\Games\\POL\r\nini.upstream=1\r\n\r\n[graphics]\r\nbloom=0\r\n");
    next_look();
    check(value(copy2, L"graphics", L"bloom") == L"0", "upstream on: the account's file is watched, and taken up");

    // 7. merge(): another game's settings made this one's
    const std::wstring other_game = folder + L"\\vanacore.2147483001.ini";
    write_utf16(other_game, L"[client]\r\npol=D:\\Elsewhere\\POL\r\nchat.color=#445566 ; theirs\r\n\r\n[graphics]\r\neffects=7\r\n"
                            L"grass=1\r\nrenderer=d3d9\r\n\r\n[vana]\r\nx=1\r\n\r\n[mcp]\r\nport=1\r\n");
    check(ini::set(copy2, L"vana", L"x", L"500"), "this window's place is set here");
    check(ini::set(copy2, L"mcp", L"port", L"47800"), "an MCP port is set here");
    const int changed = settings::merge(other_game);
    check(changed >= 3, "merge reports what it changed");
    check(value(copy2, L"graphics", L"effects") == L"7" && value(copy2, L"graphics", L"grass") == L"1" &&
              value(copy2, L"graphics", L"renderer") == L"d3d9",
          "their settings are taken");
    check(value(copy2, L"client", L"chat.color") == L"#445566", "a value's comment isn't");
    check(value(copy2, L"graphics", L"bloom") == L"<none>", "a setting they haven't is gone (its default)");
    check(value(copy2, L"client", L"pol") == L"C:\\Games\\POL" && value(copy2, L"mcp", L"port") == L"47800" &&
              value(copy2, L"vana", L"x") == L"500",
          "the install's path, MCP and the window's place are kept");
    ini::save(true);
    check(value(mine, L"graphics", L"effects") == L"7" && value(mine, L"graphics", L"bloom") == L"<none>", "the file has it too");
    check(settings::merge(other_game) == 0, "a second merge changes nothing");
    check(settings::merge(folder + L"\\no-such-game.ini") == -1, "a game that isn't there is refused");
    DeleteFileW(other_game.c_str());

    // 8. the copy goes with the game; the file stays, with a change still waiting saved
    check(ini::set(copy2, L"graphics", L"bloom", L"1"), "a change made just before the game ends");
    settings::shutdown();
    check(!exists(copy2), "shutdown deletes the copy");
    check(exists(mine), "the file stays");
    check(value(mine, L"graphics", L"bloom") == L"1", "the change waiting is saved as the game ends");
    check(!exists(copy2 + L".writing"), "no .writing file left");

    // cleanup: temp files only
    for (const std::wstring& path : {file, mine, other})
        DeleteFileW(path.c_str());
    RemoveDirectoryW(folder.c_str());
    RemoveDirectoryW(tmp.c_str());
    RemoveDirectoryW(base.c_str());

    // 9. Authentication / TLS 1.3 (SCH_CREDENTIALS v5) & failure classification regressions
    {
        namespace login = vanacore::login;
        check(login::kVersion == login::Version{3, 0, 0}, "default login protocol version matches xiloader 3.0.0");

        login::Version parsed{};
        check(login::parse_version(L"3.0.0", parsed) && parsed == login::Version{3, 0, 0}, "parse_version accepts 3.0.0");
        check(login::parse_version(L"3.0", parsed) && parsed == login::Version{3, 0, 0}, "parse_version accepts 3.0");
        check(!login::parse_version(L"bad.ver", parsed), "parse_version rejects non-numeric version");

        // Verify Windows Schannel SCH_CREDENTIALS v5 outbound credential handle acquisition (TLS 1.3 capable)
        CredHandle cred{};
        bool used_v5 = false;
        const SECURITY_STATUS cred_status = vanacore::tls::acquire_outbound_credentials(cred, used_v5);
        check(cred_status == SEC_E_OK, "Schannel outbound credentials acquired cleanly");
        check(used_v5, "Schannel acquired SCH_CREDENTIALS v5 (TLS 1.3 enabled)");
        if (cred_status == SEC_E_OK)
            FreeCredentialsHandle(&cred);

        // Verify DNS failure classification
        std::uint32_t resolved_addr = 0;
        std::wstring dns_err;
        check(!login::resolve(L"nonexistent.invalid.soulspirexi.test", resolved_addr, dns_err) && !dns_err.empty(),
              "DNS failure returns actionable error");

        // Verify TCP connection refused classification on closed loopback port (zero secret leakage)
        const std::wstring secret_pw = L"SuperSecretRegressionPw!99";
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        SOCKET temp_sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in closed_addr{};
        closed_addr.sin_family = AF_INET;
        closed_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int addr_len = sizeof(closed_addr);
        std::uint16_t closed_port = 0;
        if (temp_sock != INVALID_SOCKET &&
            ::bind(temp_sock, reinterpret_cast<const sockaddr*>(&closed_addr), sizeof(closed_addr)) == 0 &&
            ::getsockname(temp_sock, reinterpret_cast<sockaddr*>(&closed_addr), &addr_len) == 0) {
            closed_port = ntohs(closed_addr.sin_port);
        }
        if (temp_sock != INVALID_SOCKET)
            closesocket(temp_sock);
        if (closed_port != 0) {
            const login::Result tcp_res =
                login::log_in(L"127.0.0.1", htonl(INADDR_LOOPBACK), closed_port, L"testuser", secret_pw);
            check(tcp_res.outcome == login::Outcome::ServerProblem &&
                      tcp_res.error_category == login::ErrorCategory::TcpConnectFailed,
                  "closed loopback port classified as TcpConnectFailed");
            check(tcp_res.message.find(secret_pw) == std::wstring::npos,
                  "TCP error message never leaks password");
        }

        // Verify TLS handshake failure classification against a non-TLS loopback listener
        SOCKET plain_listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in plain_addr{};
        plain_addr.sin_family = AF_INET;
        plain_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int plain_len = sizeof(plain_addr);
        if (plain_listener != INVALID_SOCKET &&
            ::bind(plain_listener, reinterpret_cast<const sockaddr*>(&plain_addr), sizeof(plain_addr)) == 0 &&
            ::listen(plain_listener, 1) == 0 &&
            ::getsockname(plain_listener, reinterpret_cast<sockaddr*>(&plain_addr), &plain_len) == 0) {
            const std::uint16_t plain_port = ntohs(plain_addr.sin_port);
            HANDLE srv = CreateThread(nullptr, 0, &plain_echo_server, reinterpret_cast<void*>(plain_listener), 0, nullptr);
            const login::Result tls_res =
                login::log_in(L"127.0.0.1", htonl(INADDR_LOOPBACK), plain_port, L"testuser", secret_pw);
            if (srv != nullptr) {
                WaitForSingleObject(srv, 3000);
                CloseHandle(srv);
            }
            closesocket(plain_listener);
            check(tls_res.outcome == login::Outcome::ServerProblem &&
                      tls_res.error_category == login::ErrorCategory::TlsHandshakeFailed,
                  "non-TLS server classified as TlsHandshakeFailed");
            check(tls_res.message.find(secret_pw) == std::wstring::npos,
                  "TLS error message never leaks password");
        } else if (plain_listener != INVALID_SOCKET) {
            closesocket(plain_listener);
        }
        WSACleanup();
    }

    // ---- SoulspireXI VanaCore-native state & packet engine tests (CLIENT-MODERN-001 Phase A+B) ----
    {
        using namespace vanacore::soulspire;
        EngineState st;
        st.set_enabled(true);

        // 1. Authoritative JSON identity registry loading (Shinobi job=13 branch=1, Berserker job=1 branch=2)
        const char* ident_json = R"({
            "schemaVersion": 1,
            "registryHash": "cf86aa59ff8d16c6",
            "souls": [
                {"jobId": 13, "branchId": 1, "key": "shinobi", "name": "Shinobi", "abbr": "SHN"},
                {"jobId": 1,  "branchId": 2, "key": "berserker", "name": "Berserker", "abbr": "BSK"}
            ]
        })";
        check(st.load_identities_json(ident_json, "unit_test"), "Soulspire loads soul-identities JSON");
        std::string abbr, full;
        check(st.resolve_label(13, 1, false, abbr) && abbr == "SHN", "Shinobi short label resolves to SHN");
        check(st.resolve_label(13, 1, true, full) && full == "Shinobi", "Shinobi full label resolves to Shinobi");
        std::uint8_t j = 0, b = 0;
        check(st.soul_key_to_pair("Shinobi", j, b) && j == 13 && b == 1, "soul_key_to_pair resolves Shinobi -> (13,1)");

        // 2. Binary 0x1FF CRYS & 0x061 job/level packet decoding
        std::uint8_t pkt_61[16]{};
        pkt_61[0x0C] = 13; // NIN
        pkt_61[0x0D] = 75; // Lv.75
        packets::on_incoming(st, 0x061, pkt_61, sizeof(pkt_61), 100);

        std::uint8_t pkt_1ff[16]{};
        std::memcpy(pkt_1ff + 4, "CRYS", 4);
        pkt_1ff[8] = 1;  // ver
        pkt_1ff[9] = 1;  // op
        pkt_1ff[10] = 13; // NIN
        pkt_1ff[11] = 1;  // Shinobi
        const std::uint32_t cid = 424242;
        std::memcpy(pkt_1ff + 12, &cid, 4);
        packets::on_incoming(st, 0x1FF, pkt_1ff, sizeof(pkt_1ff), 200);
        const ActiveSoul active = st.active_soul();
        check(active.known && active.job == 13 && active.branch == 1 && active.effective_level == 75 && active.character_id == cid,
              "0x1FF CRYS + 0x061 activates Shinobi Lv.75");

        // 3. Action catalog + Shadow Squadron: Ni compaction & description formatting
        ActionRecord ss_ni{};
        ss_ni.job = 13;
        ss_ni.branch = 1;
        ss_ni.kind = ActionKind::Spell;
        ss_ni.action_id = 339;
        ss_ni.name = "Shadow Squadron: Ni";
        ss_ni.description = "Summons 3 offensive shadow clones that independently strike your target.";
        check(st.replace_actions({ss_ni}) == 1, "replace_actions stores Shadow Squadron: Ni");
        ActionRecord found{};
        check(st.lookup_action(ActionKind::Spell, 339, found) && found.name == "Shadow Squadron: Ni",
              "lookup_action returns Shadow Squadron: Ni for active Shinobi");
        const std::string row16 = compact_action_row_name("Shadow Squadron: Ni", 16);
        check(row16.size() <= 16 && row16.find(": Ni") != std::string::npos,
              "compact_action_row_name preserves ': Ni' tier suffix within 16 chars");

        // 4. Level-gated visibility at Lv.10/30/50/75 for Spells AND Abilities (preserving unmanaged vanilla fail-open)
        VisibilityRecord vis_ss_ichi{13, 1, ActionKind::Spell, 338, 12, VisibilityRuleAction::Show};
        VisibilityRecord vis_ss_ni{13, 1, ActionKind::Spell, 339, 37, VisibilityRuleAction::Show};
        VisibilityRecord vis_ss_san{13, 1, ActionKind::Spell, 340, 75, VisibilityRuleAction::Show};
        VisibilityRecord vis_ja_lvl30{13, 1, ActionKind::Ability, 610, 30, VisibilityRuleAction::Show};
        VisibilityRecord vis_ja_lvl50{13, 1, ActionKind::Ability, 611, 50, VisibilityRuleAction::Show};
        VisibilityRecord vis_ja_hide{13, 1, ActionKind::Ability, 612, 1, VisibilityRuleAction::Hide};
        check(st.replace_visibility({vis_ss_ichi, vis_ss_ni, vis_ss_san, vis_ja_lvl30, vis_ja_lvl50, vis_ja_hide}) == 6,
              "replace_visibility stores Lv.10/30/50/75 spell & ability rules");

        // Lv.10: all three Shadow Squadron tiers and Lv.30/50 JAs hidden; unmanaged vanilla spells stay Unmanaged (fail-open)
        check(st.decide_visibility(ActionKind::Spell, 338, 10) == VisibilityDecision::Hide, "Shadow Squadron: Ichi hidden at Lv.10");
        check(st.decide_visibility(ActionKind::Spell, 339, 10) == VisibilityDecision::Hide, "Shadow Squadron: Ni hidden at Lv.10");
        check(st.decide_visibility(ActionKind::Ability, 610, 10) == VisibilityDecision::Hide, "Lv.30 ability hidden at Lv.10");
        check(st.decide_visibility(ActionKind::Spell, 320, 10) == VisibilityDecision::Unmanaged, "unmanaged vanilla spell remains fail-open at Lv.10");
        // Lv.30: Ichi & Lv.30 JA unlocked; Ni/San/Lv.50 JA still hidden
        check(st.decide_visibility(ActionKind::Spell, 338, 30) == VisibilityDecision::Show, "Shadow Squadron: Ichi shown at Lv.30");
        check(st.decide_visibility(ActionKind::Spell, 339, 30) == VisibilityDecision::Hide, "Shadow Squadron: Ni hidden at Lv.30");
        check(st.decide_visibility(ActionKind::Ability, 610, 30) == VisibilityDecision::Show, "Lv.30 ability shown at Lv.30");
        check(st.decide_visibility(ActionKind::Ability, 611, 30) == VisibilityDecision::Hide, "Lv.50 ability hidden at Lv.30");
        // Lv.50: Ichi, Ni & Lv.50 JA unlocked; San still hidden
        check(st.decide_visibility(ActionKind::Spell, 339, 50) == VisibilityDecision::Show, "Shadow Squadron: Ni shown at Lv.50");
        check(st.decide_visibility(ActionKind::Spell, 340, 50) == VisibilityDecision::Hide, "Shadow Squadron: San hidden at Lv.50");
        check(st.decide_visibility(ActionKind::Ability, 611, 50) == VisibilityDecision::Show, "Lv.50 ability shown at Lv.50");
        // Lv.75: all unlocked except explicit Hide rule; unmanaged vanilla remains Unmanaged
        check(st.decide_visibility(ActionKind::Spell, 340, 75) == VisibilityDecision::Show, "Shadow Squadron: San shown at Lv.75");
        check(st.decide_visibility(ActionKind::Ability, 612, 75) == VisibilityDecision::Hide, "explicitly hidden ability suppressed at Lv.75");
        check(st.decide_visibility(ActionKind::Ability, 999, 75) == VisibilityDecision::Unmanaged, "unmanaged vanilla ability remains fail-open");

        // 4b. Action search & HelpWind description formatting
        const auto search_hits = st.search_actions("squadron");
        check(search_hits.size() == 1 && search_hits[0].action_id == 339, "search_actions matches case-insensitively");
        const std::string fmt_desc = format_action_description(ss_ni.description, 120);
        check(!fmt_desc.empty() && fmt_desc.size() <= 120, "format_action_description bounds HelpWind length");

        // 5. Weapon rules JSON (Phase D: Berserker DUAL_2H Great Axe + Flintlock appearanceMasks/rangedOnly)
        const char* weap_json = R"({
            "weaponConfigHash": "85de194dbb914f94",
            "rules": [
                {"jobId": 1, "branch": 2, "minEffectiveLevel": 10, "mode": "dual_2h", "mainSkillMask": 64, "subSkillMask": 64}
            ],
            "appearanceMasks": [
                {"jobId": 17, "branch": 1, "hideMain": true, "hideSub": true}
            ],
            "rangedOnly": [
                {"jobId": 17, "branch": 1, "firearmOnly": true, "autoFacing": true}
            ]
        })";
        check(st.load_weapon_config_json(weap_json), "load_weapon_config_json parses Berserker + Flintlock rules");
        st.set_active_soul(cid, 1, 2, 75, true, "unit_test", 250);
        check(st.allows_sub_weapon(6, 18000, 6, 18001), "Berserker Lv.75 allows Dual Great Axe");
        check(st.allows_dual_2h_menu_item(6, 18001), "Berserker Sub-slot picker allows Great Axe");
        check(st.allows_2h_main_with_sub(6, 18000, 6, 18001), "Berserker allows 2H Great Axe Main with Great Axe Sub");
        st.record_local_equipped_weapons(18000, 6, 18001, 6);
        const bool local_remap_ok = st.authorizes_sub_main_namespace(cid, 0x0120u, 255);
        if (!local_remap_ok) {
            const auto a = st.active_soul();
            const auto d = st.diagnostics();
            printf("FAIL: diag enabled=%d cid=%u a.cid=%u job=%u br=%u lv=%u known=%d rules=%u\n",
                   (int)st.enabled(), cid, a.character_id, a.job, a.branch, a.effective_level, (int)a.known, d.weapon_rule_count);
        }
        check(local_remap_ok,
              "Berserker dual Great Axe authorizes PC Sub->Main FTABLE model namespace");
        check(!st.authorizes_sub_main_namespace(cid, 0x0010u, 255) && !st.authorizes_sub_main_namespace(cid, 0x0080u, 255),
              "Vanilla shield model ranges NEVER remap to Main FTABLE namespace");
        check(!st.should_hide_weapon_slot(cid, 6u, 255) && !st.should_hide_weapon_slot(cid, 7u, 255),
              "Berserker melee weapon models are NEVER hidden by Flintlock appearanceMasks");
        // Non-Berserker WAR Souls (Commander branch 1, Spartan branch 3) are strictly denied
        st.set_active_soul(cid, 1, 1, 75, true, "unit_test", 256);
        check(!st.allows_sub_weapon(6, 18000, 6, 18001) && !st.authorizes_sub_main_namespace(cid, 0x0120u, 256),
              "WAR Commander denied Berserker Dual Great Axe & model remap");
        st.set_active_soul(cid, 1, 3, 75, true, "unit_test", 257);
        check(!st.allows_sub_weapon(6, 18000, 6, 18001) && !st.authorizes_sub_main_namespace(cid, 0x0120u, 257),
              "WAR Spartan denied Berserker Dual Great Axe & model remap");
        // Flintlock (COR branch 1) hides Main/Sub melee models and sets rangedOnly; other COR Souls do NOT
        st.set_active_soul(cid, 17, 1, 75, true, "unit_test", 258);
        check(st.is_ranged_only_active() && st.should_hide_weapon_slot(cid, 6u, 258) && st.should_hide_weapon_slot(cid, 7u, 258),
              "Flintlock activates rangedOnly and hides Main/Sub melee weapon models");
        st.set_active_soul(cid, 17, 2, 75, true, "unit_test", 259);
        check(!st.is_ranged_only_active() && !st.should_hide_weapon_slot(cid, 6u, 259) && !st.should_hide_weapon_slot(cid, 7u, 259),
              "COR Gambler does NOT inherit Flintlock rangedOnly or appearanceMasks");
        st.set_active_soul(cid, 17, 3, 75, true, "unit_test", 259);
        check(!st.is_ranged_only_active() && !st.should_hide_weapon_slot(cid, 6u, 259) && !st.should_hide_weapon_slot(cid, 7u, 259),
              "COR Buccaneer does NOT inherit Flintlock rangedOnly or appearanceMasks");
        // Remote peer Berserker via 0x1FC OXSI authorizes remap strictly from peer's own Soul
        std::uint8_t pkt_oxsi[16]{};
        std::memcpy(pkt_oxsi + 4, "OXSI", 4);
        pkt_oxsi[8] = 1;  // ver
        pkt_oxsi[9] = 1;  // WAR
        pkt_oxsi[10] = 2; // Berserker
        pkt_oxsi[11] = 75;
        const std::uint32_t peer_id = 999001u;
        std::memcpy(pkt_oxsi + 12, &peer_id, 4);
        packets::on_incoming(st, 0x1FC, pkt_oxsi, sizeof(pkt_oxsi), 260);
        check(st.authorizes_sub_main_namespace(peer_id, 0x0120u, 265) &&
                  !st.authorizes_sub_main_namespace(peer_id, 84u, 265),
              "Remote peer Berserker OXSI authorizes Sub->Main remap and denies fallback model 84");
        st.set_active_soul(cid, 13, 1, 75, true, "unit_test", 260);
        check(!st.allows_sub_weapon(6, 18000, 6, 18001), "Shinobi denied Berserker Dual Great Axe");

        // 6. 0x017 [OXHUD]| and [OXMOB]| parsing + chat suppression (normal chat untouched)
        auto build_017 = [](const char* text) {
            std::vector<std::uint8_t> buf(24 + std::strlen(text) + 1, 0);
            std::memcpy(buf.data() + 23, text, std::strlen(text) + 1);
            return buf;
        };
        const auto p_hud = build_017("[OXHUD]|shinobi|sh=3|shmax=3|feint=1");
        packets::on_incoming(st, 0x017, p_hud.data(), p_hud.size(), 300);
        check(packets::should_suppress_for_game(st, 0x017, p_hud.data(), p_hud.size()), "[OXHUD]| suppressed from stock FFXI chat");
        const HudSnapshot hud = st.hud_snapshot();
        check(hud.soul_key == "shinobi" && hud.kv.at("sh") == "3" && hud.kv.at("feint") == "1",
              "[OXHUD]| parsed Shinobi shadows=3 and feint=1");

        const auto p_mob = build_017("[OXMOB]|17199452|1|1|");
        packets::on_incoming(st, 0x017, p_mob.data(), p_mob.size(), 310);
        check(packets::should_suppress_for_game(st, 0x017, p_mob.data(), p_mob.size()), "[OXMOB]| suppressed from stock FFXI chat");
        MobStatusSnapshot mob{};
        check(st.mob_status(17199452u, mob) && mob.payload == "1|1|", "[OXMOB]| tracked mob 17199452");

        const auto p_normal = build_017("Hello Qufim Island!");
        packets::on_incoming(st, 0x017, p_normal.data(), p_normal.size(), 320);
        check(!packets::should_suppress_for_game(st, 0x017, p_normal.data(), p_normal.size()),
              "ordinary player chat is NEVER suppressed");

        // 6b. 0x1FC OXHD (binary unlocked/job/soul_key), 64-byte 0x1FF CRYS, 0x1FD OXTL, 0x1FE OXKE
        {
            std::vector<std::uint8_t> pkt_oxhd(16 + 9, 0);
            std::memcpy(pkt_oxhd.data() + 4, "OXHD", 4);
            pkt_oxhd[8] = 1;  // ver
            pkt_oxhd[9] = 1;  // unlocked=true (must NOT be misread as job=1)
            pkt_oxhd[10] = 1; // job_id=1 WAR (must NOT be misread as branch=1 Commander!)
            pkt_oxhd[11] = 9; // key_len="berserker"
            std::memcpy(pkt_oxhd.data() + 16, "berserker", 9);
            packets::on_incoming(st, 0x1FC, pkt_oxhd.data(), pkt_oxhd.size(), 322);
            const ActiveSoul after_oxhd = st.active_soul();
            check(after_oxhd.known && after_oxhd.job == 1 && after_oxhd.branch == 2,
                  "binary 0x1FC OXHD decodes berserker -> WAR branch 2 (not Commander branch 1)");

            std::uint8_t pkt_crys64[64]{};
            pkt_crys64[0] = 0xFF; pkt_crys64[1] = 0x21;
            std::memcpy(pkt_crys64 + 4, "CRYS", 4);
            pkt_crys64[8] = 1;  // ver
            pkt_crys64[9] = 0;  // result=ok
            pkt_crys64[10] = 0x01; pkt_crys64[11] = 0x40; // request_id=0x4001
            pkt_crys64[12] = 1; // WAR
            pkt_crys64[13] = 24;
            pkt_crys64[14] = 75;
            pkt_crys64[16 + 1 * 2] = 0x0E; // owned_mask
            pkt_crys64[16 + 1 * 2 + 1] = 2; // active=Berserker
            packets::on_incoming(st, 0x1FF, pkt_crys64, sizeof(pkt_crys64), 323);
            const ActiveSoul after_crys64 = st.active_soul();
            check(after_crys64.known && after_crys64.job == 1 && after_crys64.branch == 2 && after_crys64.effective_level == 75,
                  "64-byte 0x1FF CRYS decodes WAR Lv.75 Berserker cleanly");
        }
        std::uint8_t pkt_1fd[12]{};
        std::memcpy(pkt_1fd + 4, "OXTL", 4);
        packets::on_incoming(st, 0x1FD, pkt_1fd, sizeof(pkt_1fd), 325);
        std::uint8_t pkt_1fe[12]{};
        std::memcpy(pkt_1fe + 4, "OXKE", 4);
        packets::on_incoming(st, 0x1FE, pkt_1fe, sizeof(pkt_1fe), 326);
        const auto p_tal = build_017("[OXTAL]|META|NIN|75|5|0|5|9|0|Shinobi");
        packets::on_incoming(st, 0x017, p_tal.data(), p_tal.size(), 327);
        check(packets::should_suppress_for_game(st, 0x017, p_tal.data(), p_tal.size()), "[OXTAL]| suppressed from chat");
        st.set_hook_statuses({
            {"packet_lifecycle", "packets", "vanacore_shared", "replaced", 0, true, 1, "ok"},
            {"ability_list_filter", "visibility", "soulspire", "migrated", 0x22562Fu, true, 2, "installed"},
        }, kCapVanaCoreNative | kCapAbilityVisibility);
        const DiagnosticsSnapshot diag = st.diagnostics();
        check(diag.packets_1fd == 1 && diag.packets_1fe == 1 && diag.packets_oxtal == 1 &&
              diag.hooks_installed == 2 && diag.hooks_total == 2 &&
              diag.hooks[1].disposition == "migrated" && (diag.capabilities & kCapAbilityVisibility) != 0 &&
              st.all_mob_statuses().size() == 1,
              "Phase B.2 diagnostics report packet counts, Hook #20 capability, disposition, and mob statuses");

        // 6c. Native Commands menu (Status -> Soulspire -> Equipment) & open_core queue
        st.request_open_core();
        check(st.consume_open_core() == 1 && st.consume_open_core() == 0,
              "request_open_core / consume_open_core atomically delivers menu activation");

        // 7. Zone transition clears stale session state cleanly & releases UI input ownership
        InputOwnershipState& io = input_ownership();
        io.release_all("test_reset", 350);
        const UiInputTransition acq = io.acquire("soulspire_core", {true, true, true, 1500}, 360);
        check(acq.became_active && acq.active && acq.block_mouse && acq.capture_gamepad && acq.show_cursor,
              "acquire_ui_input transitions Gameplay -> UiActive with cursor visible");
        check(io.wants_visible_cursor() && io.snapshot().owner_count == 1,
              "active UI input owner requests single visible cursor");

        std::uint8_t pkt_00b[8]{};
        packets::on_incoming(st, 0x00B, pkt_00b, sizeof(pkt_00b), 400);
        check(!st.active_soul().known && st.tracked_mob_count() == 0, "0x00B zone-out resets stale Soul and mob state");
        check(!io.active() && !io.wants_visible_cursor() && io.snapshot().last_release_reason == "zone_out",
              "0x00B zone-out releases UI input ownership and restores gameplay cursor");

        // 8. Focus loss (Alt-Tab) and lease expiry auto-release UI input ownership
        io.acquire("soulspire_core", {true, true, true, 1000}, 500);
        const UiInputTransition bg = io.tick(550, true);
        check(bg.became_inactive && !bg.active && io.snapshot().last_release_reason == "focus_loss",
              "window focus loss (Alt-Tab) automatically releases UI input ownership");

        io.acquire("soulspire_core", {true, true, true, 500}, 600);
        const UiInputTransition exp = io.tick(1200, false);
        check(exp.became_inactive && !exp.active && io.snapshot().last_release_reason == "lease_expired",
              "expired UI input lease automatically restores gameplay ownership");

        // 9. Repeated open/close cycles keep acquire/release counts balanced with zero drift
        const auto before = io.snapshot();
        for (int cycle = 0; cycle < 10; ++cycle) {
            const auto t_base = static_cast<std::uint64_t>(2000 + cycle * 100);
            const auto open_tr = io.acquire("soulspire_core", {true, true, true, 1500}, t_base);
            const auto close_tr = io.release("soulspire_core", "closed", t_base + 50);
            check(open_tr.became_active && close_tr.became_inactive && !io.active(),
                  "repeated open/close cycle acquires and restores cleanly");
        }
        const auto after = io.snapshot();
        check(after.acquire_count == before.acquire_count + 10 && after.release_count == before.release_count + 10,
              "10 repeated open/close cycles have balanced acquire/release counts");
    }

    // 10. Retail PlayOnline Interface registry policy & silent pre-D3D GameStart exit regressions
    {
        namespace policy = vanacore::registry::policy;

        // Direct HKLM, WOW6432Node, kernel path, trailing slash, and stand-in-relative paths all resolve to real HKLM Interface
        check(policy::real_interface_subkey(L"HKLM\\SOFTWARE\\PlayOnlineUS\\Interface") ==
                  std::optional<std::wstring>(L"SOFTWARE\\PlayOnlineUS\\Interface"),
              "HKLM PlayOnlineUS\\Interface resolves to real HKLM subkey");
        check(policy::real_interface_subkey(L"HKLM\\SOFTWARE\\WOW6432Node\\PlayOnlineUS\\Interface") ==
                  std::optional<std::wstring>(L"SOFTWARE\\PlayOnlineUS\\Interface"),
              "HKLM WOW6432Node PlayOnlineUS\\Interface resolves to real HKLM subkey");
        check(policy::real_interface_subkey(L"\\REGISTRY\\MACHINE\\SOFTWARE\\WOW6432Node\\PlayOnlineEU\\Interface\\") ==
                  std::optional<std::wstring>(L"SOFTWARE\\PlayOnlineEU\\Interface"),
              "kernel-form PlayOnlineEU\\Interface with trailing slash resolves to real HKLM subkey");
        check(policy::real_interface_subkey(L"\\REGISTRY\\MACHINE\\SOFTWARE\\PlayOnline\\Interface") ==
                  std::optional<std::wstring>(L"SOFTWARE\\PlayOnline\\Interface"),
              "JP PlayOnline\\Interface resolves to real HKLM subkey");
        check(policy::real_interface_subkey(
                  L"\\REGISTRY\\USER\\S-1-5-21-1000\\Software\\VanaCore\\Registry\\PlayOnlineUS\\Interface") ==
                  std::optional<std::wstring>(L"SOFTWARE\\PlayOnlineUS\\Interface"),
              "stand-in-relative PlayOnlineUS\\Interface resolves to real HKLM subkey (not empty HKCU stand-in)");
        check(policy::real_interface_subkey(L"HKCU\\Software\\VanaCore\\Registry\\PlayOnlineUS\\interface") ==
                  std::optional<std::wstring>(L"SOFTWARE\\PlayOnlineUS\\Interface"),
              "case-insensitive stand-in Interface resolves to real HKLM subkey");

        // Non-Interface PlayOnline keys remain stand-ins; unrelated Interface keys are never intercepted
        check(!policy::real_interface_subkey(L"HKLM\\SOFTWARE\\PlayOnlineUS\\InstallFolder").has_value(),
              "InstallFolder stays on stand-in/served path");
        check(!policy::real_interface_subkey(L"HKLM\\SOFTWARE\\PlayOnlineUS\\SquareEnix\\FinalFantasyXI").has_value(),
              "FinalFantasyXI settings key stays on stand-in path");
        check(!policy::real_interface_subkey(L"HKLM\\SOFTWARE\\PlayOnline\\SQUARE\\FinalFantasyXI").has_value(),
              "JP SQUARE\\FinalFantasyXI settings key stays on stand-in path");
        check(!policy::real_interface_subkey(L"HKLM\\SOFTWARE\\PlayOnlineUS\\Product").has_value(),
              "Product key stays on stand-in path");
        check(!policy::real_interface_subkey(L"HKLM\\SOFTWARE\\PlayOnlineUS").has_value(),
              "PlayOnlineUS root key stays on stand-in path");
        check(!policy::real_interface_subkey(L"HKCR\\Interface").has_value(),
              "unrelated HKCR\\Interface is never redirected");
        check(!policy::real_interface_subkey(L"HKCU\\Software\\OtherApp\\Interface").has_value(),
              "unrelated HKCU app Interface key is never redirected");

        // Relative "Interface" subkey detection
        check(policy::is_interface_subkey("Interface") && policy::is_interface_subkey("interface") &&
                  policy::is_interface_subkey("\\Interface\\"),
              "is_interface_subkey matches Interface regardless of case or surrounding slashes");
        check(!policy::is_interface_subkey(nullptr) && !policy::is_interface_subkey("") &&
                  !policy::is_interface_subkey("InstallFolder") && !policy::is_interface_subkey("Interface\\Sub"),
              "is_interface_subkey rejects null, empty, sibling, and nested subkeys");

        // Real HKLM Interface opens are strictly read-only and default to 32-bit WOW64 view
        // Note: KEY_READ and KEY_WRITE both include READ_CONTROL (STANDARD_RIGHTS_READ == STANDARD_RIGHTS_WRITE),
        // so verify no mutating registry access bits remain.
        constexpr REGSAM kMutatingBits = KEY_SET_VALUE | KEY_CREATE_SUB_KEY | DELETE | WRITE_DAC | WRITE_OWNER;
        const REGSAM stripped_all = policy::interface_read_only_access(KEY_ALL_ACCESS);
        check(stripped_all == (KEY_READ | KEY_WOW64_32KEY) && (stripped_all & kMutatingBits) == 0,
              "interface_read_only_access strips KEY_ALL_ACCESS to KEY_READ|KEY_WOW64_32KEY");
        const REGSAM kept_64 = policy::interface_read_only_access(KEY_WRITE | KEY_WOW64_64KEY);
        check(kept_64 == (KEY_READ | KEY_WOW64_64KEY) && (kept_64 & kMutatingBits) == 0,
              "interface_read_only_access strips write rights while preserving explicit KEY_WOW64_64KEY");

        // Silent pre-Direct3D GameStart return (S_OK / 0x00000000) is classified as startup failure
        const auto silent_pre_d3d =
            vanacore::loader::classify_game_start_exit(S_OK, false, vanacore::kExitGameStartFailed);
        check(silent_pre_d3d.outcome == vanacore::loader::GameStartOutcome::ExitedBeforeDirect3D &&
                  silent_pre_d3d.exit_code == vanacore::kExitGameStartFailed && silent_pre_d3d.exit_code != 0,
              "GameStart S_OK before Direct3DCreate8 classifies as ExitedBeforeDirect3D with non-zero exit");
        const auto err_pre_d3d =
            vanacore::loader::classify_game_start_exit(E_FAIL, false, vanacore::kExitGameStartFailed);
        check(err_pre_d3d.outcome == vanacore::loader::GameStartOutcome::ExitedBeforeDirect3D &&
                  err_pre_d3d.exit_code == vanacore::kExitGameStartFailed,
              "GameStart failure HRESULT before Direct3DCreate8 classifies as ExitedBeforeDirect3D");
        const auto clean_exit =
            vanacore::loader::classify_game_start_exit(S_OK, true, vanacore::kExitGameStartFailed);
        check(clean_exit.outcome == vanacore::loader::GameStartOutcome::CleanExit && clean_exit.exit_code == 0,
              "GameStart return after Direct3DCreate8 classifies as CleanExit (exit 0)");

        // PLAYER-CMD-001: Verify [mcp] available and [developer] enabled default to 0 (fail-closed)
        CreateDirectoryW(base.c_str(), nullptr);
        const std::wstring default_ini = base + L"\\player_default.ini";
        write_utf16(default_ini, L"[server]\r\nserver=play.soulspirexi.com\r\n");
        check(GetPrivateProfileIntW(L"mcp", L"available", 0, default_ini.c_str()) == 0,
              "MCP [mcp] available defaults to 0 when omitted");
        check(GetPrivateProfileIntW(L"developer", L"enabled", 0, default_ini.c_str()) == 0,
              "Developer [developer] enabled defaults to 0 when omitted");
        const std::wstring optin_ini = base + L"\\dev_optin.ini";
        write_utf16(optin_ini, L"[mcp]\r\navailable=1\r\n[developer]\r\nenabled=1\r\n");
        check(GetPrivateProfileIntW(L"mcp", L"available", 0, optin_ini.c_str()) == 1,
              "MCP [mcp] available honors explicit available=1 opt-in");
        check(GetPrivateProfileIntW(L"developer", L"enabled", 0, optin_ini.c_str()) == 1,
              "Developer [developer] enabled honors explicit enabled=1 opt-in");
        DeleteFileW(default_ini.c_str());
        DeleteFileW(optin_ini.c_str());
        RemoveDirectoryW(base.c_str());
    }

    printf("\n%s (%d failed)\n", g_failed == 0 ? "PASS" : "FAIL", g_failed);
    return g_failed == 0 ? 0 : 1;
}
