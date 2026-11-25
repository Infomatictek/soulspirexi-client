// IAshitaCore and the managers that aren't game data, fonts or resources: properties, chat,
// configuration, logging, offsets, pointers, plugins, packets and input.

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../../addon_trace.hpp"
#include "../../addons.hpp"
#include "../../aliases.hpp"
#include "../../binds.hpp"
#include "../../chat.hpp"
#include "../../gamepad.hpp"
#include "../../graphics.hpp"
#include "../../ini.hpp"
#include "../../log.hpp"
#include "../../packets.hpp"
#include "../../pivot.hpp"
#include "../../plugins.hpp"
#include "../../scan.hpp"
#include "../../window.hpp"
#include "backend.hpp"

namespace vanacore::ashita {
namespace {

HMODULE self()
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&self), &module);
    return module;
}

std::string format(const char* text, va_list args)
{
    if (text == nullptr)
        return {};
    char buffer[4096];
    const int length = vsnprintf(buffer, sizeof(buffer), text, args);
    return length < 0 ? std::string(text) : std::string(buffer, static_cast<size_t>(length) < sizeof(buffer) ? length : sizeof(buffer) - 1);
}

std::string source_name()
{
    const std::string_view plugin = calling();
    return plugin.empty() ? std::string("Plugin") : std::string(plugin);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// IAshitaCore

template <typename Function>
Function system_function(const wchar_t* dll, const char* name)
{
    HMODULE module = GetModuleHandleW(dll);
    if (module == nullptr)
        module = LoadLibraryW(dll);
    return module != nullptr ? reinterpret_cast<Function>(GetProcAddress(module, name)) : nullptr;
}

// cexidats (before its 1.17) and cexislots look for CatsEyeXI's overlays at <install>\polplugins\DATs\<overlay>\ themselves.
// Ashita's folder has that; VanaCore's doesn't (the launcher mirrors plugins, addons, resources and config, and the overlays
// are too big to copy). So for those two the install path is the folder whose polplugins\DATs is the pivot root, as in
// Ashita; every other plugin keeps VanaCore's, where its config and resources are. nullptr: the pivot root isn't in that
// shape (or pivot is off), and they get VanaCore's.
const char* overlay_install_path()
{
    static std::mutex lock;
    static std::string path;
    static bool tried = false;
    const std::lock_guard guard(lock);
    if (tried)
        return path.empty() ? nullptr : path.c_str();

    const std::wstring& root = pivot::overlay_root();
    if (root.empty())
        return nullptr; // pivot isn't installed yet (or it's off): ask again later
    tried = true;

    constexpr std::wstring_view tail = L"\\polplugins\\DATs";
    if (root.size() <= tail.size() || _wcsicmp(root.c_str() + root.size() - tail.size(), tail.data()) != 0) {
        log::write(L"Ashita plugins: [pivot] rootfolder %ls doesn't end in polplugins\\DATs: cexidats and cexislots get VanaCore's folder",
                   root.c_str());
        return nullptr;
    }
    const std::wstring parent = root.substr(0, root.size() - tail.size()) + L"\\";
    BOOL lossy = FALSE;
    BOOL* check = GetACP() == CP_UTF8 ? nullptr : &lossy;
    const int length = WideCharToMultiByte(CP_ACP, 0, parent.c_str(), -1, nullptr, 0, nullptr, check);
    if (length <= 1 || lossy) {
        log::write(L"Ashita plugins: %ls can't be told to cexidats and cexislots in the ANSI code page", parent.c_str());
        return nullptr;
    }
    path.assign(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_ACP, 0, parent.c_str(), -1, path.data(), length, nullptr, nullptr);
    path.pop_back();
    log::write(L"Ashita plugins: cexidats and cexislots are told the install path is %ls (their polplugins\\DATs is the pivot root)",
               parent.c_str());
    return path.c_str();
}

bool reads_overlays(std::string_view)
{
    return false;
}

struct CoreImpl {
    static HMODULE GetHandle(void*) { return self(); }
    static const char* GetInstallPath(void*)
    {
        if (reads_overlays(calling()))
            if (const char* path = overlay_install_path())
                return path;
        return install_path().c_str();
    }
    static void* GetDirect3DDevice(void*) { return device(); }
    static void* GetProperties(void* c) { return links(c).properties; }
    static void* GetChatManager(void* c) { return links(c).chat; }
    static void* GetConfigurationManager(void* c) { return links(c).config; }
    static void* GetFontManager(void* c) { return links(c).fonts; }
    static void* GetGuiManager(void* c) { return links(c).gui; }
    static void* GetInputManager(void* c) { return links(c).input; }
    static void* GetMemoryManager(void* c) { return links(c).memory; }
    static void* GetOffsetManager(void* c) { return links(c).offsets; }
    static void* GetPacketManager(void* c) { return links(c).packets; }
    static void* GetPluginManager(void* c) { return links(c).plugins; }
    static void* GetPolPluginManager(void* c) { return links(c).pol_plugins; }
    static void* GetPointerManager(void* c) { return links(c).pointers; }
    static void* GetPrimitiveManager(void* c) { return links(c).primitives; }
    static void* GetResourceManager(void* c) { return links(c).resources; }

    // What Ashita hooks and forwards: here, straight to Windows.
    static void* Direct3DCreate8(void*, UINT sdk_version)
    {
        const auto create = system_function<void*(WINAPI*)(UINT)>(L"d3d8.dll", "Direct3DCreate8");
        return create != nullptr ? create(sdk_version) : nullptr;
    }
    static HRESULT DirectInput8Create(void*, HINSTANCE instance, DWORD version, REFIID riid, LPVOID* out, LPUNKNOWN outer)
    {
        using Create = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
        const auto create = system_function<Create>(L"dinput8.dll", "DirectInput8Create");
        return create != nullptr ? create(instance, version, riid, out, outer) : E_FAIL;
    }
    static HANDLE CreateMutexA(void*, LPSECURITY_ATTRIBUTES a, BOOL b, LPCSTR c) { return ::CreateMutexA(a, b, c); }
    static HANDLE CreateMutexW(void*, LPSECURITY_ATTRIBUTES a, BOOL b, LPCWSTR c) { return ::CreateMutexW(a, b, c); }
    static HANDLE OpenMutexA(void*, DWORD a, BOOL b, LPCSTR c) { return ::OpenMutexA(a, b, c); }
    static HANDLE OpenMutexW(void*, DWORD a, BOOL b, LPCWSTR c) { return ::OpenMutexW(a, b, c); }
    static LSTATUS RegQueryValueExA(void*, HKEY a, LPCSTR b, LPDWORD c, LPDWORD d, LPBYTE e, LPDWORD f)
    {
        return ::RegQueryValueExA(a, b, c, d, e, f);
    }
    static HWND CreateWindowExA(void*, DWORD a, LPCSTR b, LPCSTR c, DWORD d, int e, int f, int g, int h, HWND i, HMENU j,
                                HINSTANCE k, LPVOID l)
    {
        return ::CreateWindowExA(a, b, c, d, e, f, g, h, i, j, k, l);
    }
    static HWND CreateWindowExW(void*, DWORD a, LPCWSTR b, LPCWSTR c, DWORD d, int e, int f, int g, int h, HWND i,
                                HMENU j, HINSTANCE k, LPVOID l)
    {
        return ::CreateWindowExW(a, b, c, d, e, f, g, h, i, j, k, l);
    }
    static void ExitProcess(void*, UINT code) { ::ExitProcess(code); }
    static int GetSystemMetrics(void*, int index) { return ::GetSystemMetrics(index); }
    static int GetWindowTextA(void*, HWND a, LPSTR b, int c) { return ::GetWindowTextA(a, b, c); }
    static int GetWindowTextW(void*, HWND a, LPWSTR b, int c) { return ::GetWindowTextW(a, b, c); }
    static HBITMAP LoadBitmapW(void*, HINSTANCE a, LPCWSTR b) { return ::LoadBitmapW(a, b); }
    static ATOM RegisterClassA(void*, const WNDCLASSA* a) { return ::RegisterClassA(a); }
    static ATOM RegisterClassW(void*, const WNDCLASSW* a) { return ::RegisterClassW(a); }
    static ATOM RegisterClassExA(void*, const WNDCLASSEXA* a) { return ::RegisterClassExA(a); }
    static ATOM RegisterClassExW(void*, const WNDCLASSEXW* a) { return ::RegisterClassExW(a); }
    static BOOL RemoveMenu(void*, HMENU a, UINT b, UINT c) { return ::RemoveMenu(a, b, c); }
    static LRESULT SendMessageA(void*, HWND a, UINT b, WPARAM c, LPARAM d) { return ::SendMessageA(a, b, c, d); }
    static BOOL SetCursorPos(void*, int x, int y) { return ::SetCursorPos(x, y); }
    static HWND SetFocus(void*, HWND a) { return ::SetFocus(a); }
    static BOOL SetForegroundWindow(void*, HWND a) { return ::SetForegroundWindow(a); }
    static BOOL SetPriorityClass(void*, HANDLE a, DWORD b) { return ::SetPriorityClass(a, b); }
    static HHOOK SetWindowsHookExA(void*, int a, HOOKPROC b, HINSTANCE c, DWORD d) { return ::SetWindowsHookExA(a, b, c, d); }
    static BOOL SetWindowTextA(void*, HWND a, LPCSTR b) { return ::SetWindowTextA(a, b); }
    static BOOL SetWindowTextW(void*, HWND a, LPCWSTR b) { return ::SetWindowTextW(a, b); }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IProperties: the game's window (PlayOnline's windows don't exist while FFXI runs).

struct PropertiesImpl {
    static HWND GetFinalFantasyHwnd(void*) { return window::handle(); }
    static uint32_t GetFinalFantasyStyle(void*)
    {
        const HWND w = window::handle();
        return w != nullptr ? static_cast<uint32_t>(GetWindowLongA(w, GWL_STYLE)) : 0;
    }
    static uint32_t GetFinalFantasyStyleEx(void*)
    {
        const HWND w = window::handle();
        return w != nullptr ? static_cast<uint32_t>(GetWindowLongA(w, GWL_EXSTYLE)) : 0;
    }
    static RECT GetFinalFantasyRect(void*)
    {
        RECT rect{};
        if (const HWND w = window::handle())
            GetWindowRect(w, &rect);
        return rect;
    }
    static HWND GetPlayOnlineHwnd(void*) { return nullptr; }
    static uint32_t GetPlayOnlineStyle(void*) { return 0; }
    static uint32_t GetPlayOnlineStyleEx(void*) { return 0; }
    static RECT GetPlayOnlineRect(void*) { return {}; }
    static HWND GetPlayOnlineMaskHwnd(void*) { return nullptr; }
    static uint32_t GetPlayOnlineMaskStyle(void*) { return 0; }
    static uint32_t GetPlayOnlineMaskStyleEx(void*) { return 0; }
    static RECT GetPlayOnlineMaskRect(void*) { return {}; }
    // Direct3D: the fill mode is /fillmode's (graphics.cpp: 1 point, 2 wireframe, 3 solid; another value is ignored). The
    // ambient light isn't done.
    static uint32_t GetD3DFillMode(void*) { return static_cast<uint32_t>(graphics::fill_mode()); }
    static void SetD3DFillMode(void*, uint32_t mode) { graphics::set_fill_mode(mode <= 3 ? static_cast<int>(mode) : 0); }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IChatManager

// The chat line, in the game's chat manager object (Ashita's ashita.offsets.ini [chat.inputtext]).
constexpr std::uintptr_t kInputMain = 0x7E30;
constexpr std::uintptr_t kInputRaw = kInputMain + 0x06C4;
constexpr std::uintptr_t kInputRawLength = kInputMain + 0x0888;
constexpr std::uintptr_t kInputRawCaret = kInputMain + 0x0A58;
constexpr std::uintptr_t kInputParsed = kInputMain + 0x088C;
constexpr std::uintptr_t kInputParsedLength = kInputMain + 0x0A50;
constexpr std::uintptr_t kInputParsedLengthMax = kInputMain + 0x0A54;
constexpr std::uintptr_t kInputDisplay = kInputMain + 0x6E04;

template <typename T>
T input_value(std::uintptr_t offset)
{
    const std::uintptr_t object = chat::input_object();
    T value{};
    if (object != 0)
        std::memcpy(&value, reinterpret_cast<const void*>(object + offset), sizeof(T));
    return value;
}

const char* input_text(std::uintptr_t offset)
{
    const std::uintptr_t object = chat::input_object();
    return object != 0 ? reinterpret_cast<const char*>(object + offset) : "";
}

struct ChatImpl {
    static void ParseCommand(void*, int32_t mode, const char* command)
    {
        if (command != nullptr && addon_trace::any())
            addon_trace::queued_command(calling(), mode, command, false);
        run_command(mode, command, false);
    }
    static void QueueCommand(void*, int32_t mode, const char* command)
    {
        if (command != nullptr && addon_trace::any())
            addon_trace::queued_command(calling(), mode, command, true);
        run_command(mode, command, true);
    }
    static void Write(void*, int32_t mode, bool, const char* message)
    {
        if (message != nullptr) {
            if (addon_trace::any())
                addon_trace::chat_line(calling(), mode, message);
            chat::write(mode, message, source_name());
        }
    }
    static void WritefV(void*, int32_t mode, bool, const char* text, va_list args)
    {
        const std::string message = format(text, args);
        if (addon_trace::any())
            addon_trace::chat_line(calling(), mode, message);
        chat::write(mode, message, source_name());
    }
    static void AddChatMessage(void*, int32_t mode, bool, const char* message)
    {
        if (message != nullptr) {
            if (addon_trace::any())
                addon_trace::chat_line(calling(), mode, message);
            chat::write(mode, message, source_name());
        }
    }
    // /exec and /execstr as Ashita runs them (addons.hpp): a script's waits hold up only its own commands after them, never
    // the caller (Ashita sleeps on a worker thread for a queued one)
    static void ExecuteScript(void*, const char* file, const char* args, bool)
    {
        if (file == nullptr)
            return;
        if (addon_trace::any())
            addon_trace::queued_command(calling(), 1, std::string("/exec ") + file, true);
        addons::execute_script(file, args != nullptr ? args : "");
    }
    static void ExecuteScriptString(void*, const char* script, const char* args, bool queued)
    {
        if (script == nullptr)
            return;
        if (addon_trace::any())
            addon_trace::queued_command(calling(), 1, std::string("/execstr ") + script, queued);
        addons::execute_script_string(script, args != nullptr ? args : "", queued);
    }
    static const char* GetInputTextRaw(void*) { return input_text(kInputRaw); }
    static uint32_t GetInputTextRawLength(void*) { return input_value<uint32_t>(kInputRawLength); }
    static uint32_t GetInputTextRawCaretPosition(void*) { return input_value<uint32_t>(kInputRawCaret); }
    static const char* GetInputTextParsed(void*) { return input_text(kInputParsed); }
    static uint32_t GetInputTextParsedLength(void*) { return input_value<uint32_t>(kInputParsedLength); }
    static uint32_t GetInputTextParsedLengthMax(void*) { return input_value<uint32_t>(kInputParsedLengthMax); }
    static const char* GetInputTextDisplay(void*) { return input_text(kInputDisplay); }
    // Ashita's ChatInputOpenStatus, from the line's state as Ashita.dll maps it: 4 (the chat line is open) gives 0x11,
    // 5 (another text field is: a bazaar or search comment) 0x21, anything else 0. Addons test for 0x11 (fancychat's
    // input bar, frenslist) and for 0x00 (closed), so a plain 1 was never either
    static uint8_t IsInputOpen(void*)
    {
        const std::uint32_t state = chat::input_state();
        return state == 4 ? 0x11 : state == 5 ? 0x21 : 0x00;
    }
    static bool GetSilentAliases(void*) { return aliases::silent(); }
    static void SetSilentAliases(void*, bool silent) { aliases::set_silent(silent); }

    static int32_t ParseAutoTranslate(void*, const char* message, char* buffer, int32_t size, bool brackets)
    {
        if (message == nullptr || buffer == nullptr || size <= 0)
            return -1;
        const std::string out = chat::parse_auto_translate(message, brackets);
        const size_t n = std::min(out.size(), static_cast<size_t>(size - 1));
        std::memcpy(buffer, out.data(), n);
        buffer[n] = 0;
        return static_cast<int32_t>(n);
    }

    // The chat line's text set: the raw text the game edits (and its length and caret), its parsed copy and what it
    // shows, each the text given (plain text: auto-translate phrases aren't made up here)
    static void put(std::uintptr_t offset, const char* text, size_t length)
    {
        const std::uintptr_t object = chat::input_object();
        if (object == 0)
            return;
        constexpr size_t kCapacity = 0x1C4; // each buffer's room (the raw text's, to its length's field)
        length = std::min(length, kCapacity - 1);
        auto* to = reinterpret_cast<char*>(object + offset);
        std::memcpy(to, text, length);
        to[length] = 0;
    }
    static void SetInputTextRaw(void*, const char* text, uint32_t length)
    {
        if (text == nullptr)
            return;
        const size_t n = std::min<size_t>(length, std::strlen(text));
        put(kInputRaw, text, n);
        const std::uintptr_t object = chat::input_object();
        if (object != 0) {
            const uint32_t stored = static_cast<uint32_t>(std::min<size_t>(n, 0x1C3));
            std::memcpy(reinterpret_cast<void*>(object + kInputRawLength), &stored, 4);
            std::memcpy(reinterpret_cast<void*>(object + kInputRawCaret), &stored, 4);
        }
    }
    static void SetInputTextParsed(void*, const char* text)
    {
        if (text == nullptr)
            return;
        const size_t n = std::strlen(text);
        put(kInputParsed, text, n);
        const std::uintptr_t object = chat::input_object();
        if (object != 0) {
            const uint32_t stored = static_cast<uint32_t>(std::min<size_t>(n, 0x1C3));
            std::memcpy(reinterpret_cast<void*>(object + kInputParsedLength), &stored, 4);
        }
    }
    static void SetInputTextDisplay(void*, const char* text)
    {
        if (text != nullptr)
            put(kInputDisplay, text, std::strlen(text));
    }
    static void SetInputText(void* c, const char* text)
    {
        if (text == nullptr)
            return;
        SetInputTextRaw(c, text, static_cast<uint32_t>(std::strlen(text)));
        SetInputTextParsed(c, text);
        SetInputTextDisplay(c, text);
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IConfigurationManager: INI files under <VanaCore>\config, by alias. 'boot' is vanacore.ini
// (what the game starts with), read-only.
//
// A value a plugin sets (SetValue) goes to the alias's file at once, as Ashita's does: Minimap sets each change (a
// /minimap command, a slider in its editor) with SetValue and never calls Save, so only this keeps its position and
// opacity from one game to the next. The key is written on its own line in the file as it is (ini::set: the layout and
// comments kept, a new key at the end of its section), so two games sharing a file only ever change the keys they set.
// Save(alias, file) writes every key the same way; with no file named, to the file the alias was loaded from.

struct Section {
    std::string name;
    std::vector<std::pair<std::string, std::string>> values;
};
using Document = std::vector<Section>;

std::recursive_mutex g_config_lock;
std::map<std::string, Document> g_documents; // by lower-case alias
std::map<std::string, std::string> g_files;  // by lower-case alias: the file it was loaded from, or last saved to
std::map<std::string, bool> g_noted;         // by lower-case alias: whether its last logged write worked

std::string trim(std::string_view text)
{
    const size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const size_t last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

bool read_document(const std::string& path, Document& out)
{
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "rb") != 0 || file == nullptr)
        return false;
    std::string text;
    char buffer[4096];
    size_t read = 0;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0)
        text.append(buffer, read);
    fclose(file);
    if (text.starts_with("\xEF\xBB\xBF"))
        text.erase(0, 3);
    out.clear();
    Section* section = nullptr;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const std::string line = trim(std::string_view(text).substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']') {
            out.push_back({trim(std::string_view(line).substr(1, line.size() - 2)), {}});
            section = &out.back();
            continue;
        }
        const size_t equals = line.find('=');
        if (section == nullptr || equals == std::string::npos)
            continue;
        std::string value = trim(std::string_view(line).substr(equals + 1));
        // A comment after the value (not inside quotes).
        if (!value.empty() && value.front() != '"') {
            const size_t comment = value.find(" ;");
            if (comment != std::string::npos)
                value = trim(std::string_view(value).substr(0, comment));
        }
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        section->values.emplace_back(trim(std::string_view(line).substr(0, equals)), value);
    }
    return true;
}

// The whole document written plain (key = value lines, no comments) to `path`, its folder made first: for a file that
// isn't there yet.
bool write_plain(const std::string& path, const Document& document)
{
    // Its folder first (config\plugins\<name>\...).
    for (size_t at = path.find('\\', 3); at != std::string::npos; at = path.find('\\', at + 1))
        CreateDirectoryA(path.substr(0, at).c_str(), nullptr);
    FILE* file = nullptr;
    if (fopen_s(&file, path.c_str(), "wb") != 0 || file == nullptr)
        return false;
    for (const Section& section : document) {
        fprintf(file, "[%s]\r\n", section.name.c_str());
        for (const auto& [key, value] : section.values)
            fprintf(file, "%s = %s\r\n", key.c_str(), value.c_str());
        fprintf(file, "\r\n");
    }
    fclose(file);
    return true;
}

std::wstring wide(const std::string& text)
{
    const int length = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    std::wstring out(length > 1 ? static_cast<size_t>(length) : 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, out.data(), length);
    out.resize(out.size() - 1);
    return out;
}

ini::Change change(const std::string& section, const std::string& key, const std::string& value)
{
    return {wide(section), wide(key), wide(value)};
}

std::vector<ini::Change> every_key(const Document& document)
{
    std::vector<ini::Change> changes;
    for (const Section& section : document)
        for (const auto& [key, value] : section.values)
            changes.push_back(change(section.name, key, value));
    return changes;
}

// `changes` (keys of `document`) written into the file at `path` in place, each on its own line (a file that isn't
// there yet is made from the whole document, plain: ini::set leaves a missing file alone).
bool write_file(const std::string& path, const Document& document, const std::vector<ini::Change>& changes)
{
    if (path.empty())
        return false;
    const std::wstring file = wide(path);
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES)
        return write_plain(path, document);
    return ini::set(file, changes);
}

// One key a plugin set, written to the alias's file (if it was loaded from, or saved to, one). The first write, and
// each change between working and failing, is logged: a file that can't be written is settings lost at the next start.
void write_through(const std::string& alias, const Document& document, const std::string& section, const std::string& key,
                   const std::string& value)
{
    const auto file = g_files.find(alias);
    if (file == g_files.end() || file->second.empty())
        return;
    const bool ok = write_file(file->second, document, {change(section, key, value)});
    const auto noted = g_noted.find(alias);
    if (noted != g_noted.end() && noted->second == ok)
        return;
    g_noted[alias] = ok;
    if (ok)
        log::write(L"Ashita plugins: %hs's %hs settings are written to %hs as they change", source_name().c_str(),
                   alias.c_str(), file->second.c_str());
    else
        log::write(L"Ashita plugins: %hs couldn't write its %hs settings to %hs: what it sets won't last past this game",
                   source_name().c_str(), alias.c_str(), file->second.c_str());
}

std::string config_path(const char* file)
{
    std::string name = file != nullptr ? file : "";
    if (name.size() > 2 && (name[1] == ':' || name.starts_with("\\\\")))
        return name; // a full path
    // Minimap loads "\minimap\minimap.ini": a leading separator is config\ all the same
    size_t skip = 0;
    while (skip < name.size() && (name[skip] == '\\' || name[skip] == '/'))
        ++skip;
    return install_path() + "config\\" + name.substr(skip);
}

Document* document(const char* alias)
{
    if (alias == nullptr)
        return nullptr;
    const std::string key = lower(alias);
    if (key == "boot" && g_documents.find(key) == g_documents.end()) {
        Document boot;
        const std::wstring& ini = settings_ini();
        const int length = WideCharToMultiByte(CP_ACP, 0, ini.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string path(length > 1 ? static_cast<size_t>(length) : 1, '\0');
        WideCharToMultiByte(CP_ACP, 0, ini.c_str(), -1, path.data(), length, nullptr, nullptr);
        path.resize(path.size() - 1);
        read_document(path, boot);
        // Their section names for what vanacore.ini has.
        for (Section& section : boot)
            if (same(section.name, "language"))
                boot.push_back({"ashita.language", section.values});
        g_documents[key] = std::move(boot);
    }
    const auto it = g_documents.find(key);
    return it != g_documents.end() ? &it->second : nullptr;
}

const std::string* config_value(const char* alias, const char* section, const char* key)
{
    const Document* doc = document(alias);
    if (doc == nullptr || section == nullptr || key == nullptr)
        return nullptr;
    for (const Section& s : *doc)
        if (same(s.name, section))
            for (const auto& [name, value] : s.values)
                if (same(name, key))
                    return &value;
    return nullptr;
}

template <typename T>
T config_number(const char* alias, const char* section, const char* key, T otherwise)
{
    const std::lock_guard lock(g_config_lock);
    const std::string* value = config_value(alias, section, key);
    if (value == nullptr || value->empty())
        return otherwise;
    char* end = nullptr;
    if constexpr (std::is_floating_point_v<T>) {
        const double number = std::strtod(value->c_str(), &end);
        return end != value->c_str() ? static_cast<T>(number) : otherwise;
    } else if constexpr (std::is_signed_v<T>) {
        const long long number = std::strtoll(value->c_str(), &end, 0);
        return end != value->c_str() ? static_cast<T>(number) : otherwise;
    } else {
        const unsigned long long number = std::strtoull(value->c_str(), &end, 0);
        return end != value->c_str() ? static_cast<T>(number) : otherwise;
    }
}

// Names separated by '\0', ending with an extra '\0' (as far as `size` allows); the bytes needed.
uint32_t name_list(const std::vector<std::string>& names, char* buffer, uint32_t size)
{
    uint32_t needed = 1;
    for (const std::string& name : names)
        needed += static_cast<uint32_t>(name.size() + 1);
    if (buffer != nullptr && size > 0) {
        uint32_t at = 0;
        for (const std::string& name : names) {
            if (at + name.size() + 2 > size)
                break;
            std::memcpy(buffer + at, name.c_str(), name.size() + 1);
            at += static_cast<uint32_t>(name.size() + 1);
        }
        buffer[at < size ? at : size - 1] = '\0';
    }
    return needed;
}

struct ConfigImpl {
    static bool Load(void*, const char* alias, const char* file)
    {
        if (alias == nullptr || same(alias, "boot"))
            return false;
        const std::lock_guard lock(g_config_lock);
        const std::string key = lower(alias);
        const std::string path = config_path(file);
        if (file != nullptr && *file != '\0')
            g_files[key] = path; // where what the plugin sets goes from now on, whether or not the file is there yet
        Document doc;
        if (!read_document(path, doc))
            return false;
        g_documents[key] = std::move(doc);
        return true;
    }
    static bool Save(void*, const char* alias, const char* file)
    {
        if (alias == nullptr || same(alias, "boot"))
            return false;
        const std::lock_guard lock(g_config_lock);
        const std::string key = lower(alias);
        const Document* doc = document(alias);
        if (doc == nullptr)
            return false;
        std::string path;
        if (file != nullptr && *file != '\0')
            path = config_path(file);
        else if (const auto it = g_files.find(key); it != g_files.end())
            path = it->second; // no file named: the one it was loaded from
        if (!write_file(path, *doc, every_key(*doc)))
            return false;
        g_files[key] = path;
        return true;
    }
    static void Delete(void*, const char* alias)
    {
        const std::lock_guard lock(g_config_lock);
        if (alias == nullptr || same(alias, "boot"))
            return;
        const std::string key = lower(alias);
        g_documents.erase(key);
        g_files.erase(key);
        g_noted.erase(key);
    }
    static uint32_t GetSections(void*, const char* alias, char* buffer, uint32_t size)
    {
        const std::lock_guard lock(g_config_lock);
        std::vector<std::string> names;
        if (const Document* doc = document(alias))
            for (const Section& s : *doc)
                names.push_back(s.name);
        return name_list(names, buffer, size);
    }
    static uint32_t GetSectionKeys(void*, const char* alias, const char* section, char* buffer, uint32_t size)
    {
        const std::lock_guard lock(g_config_lock);
        std::vector<std::string> names;
        if (const Document* doc = document(alias))
            for (const Section& s : *doc)
                if (section != nullptr && same(s.name, section))
                    for (const auto& [key, value] : s.values)
                        names.push_back(key);
        return name_list(names, buffer, size);
    }
    static const char* GetString(void*, const char* alias, const char* section, const char* key)
    {
        const std::lock_guard lock(g_config_lock);
        const std::string* value = config_value(alias, section, key);
        return value != nullptr ? value->c_str() : nullptr;
    }
    static void SetValue(void*, const char* alias, const char* section, const char* key, const char* value)
    {
        if (alias == nullptr || section == nullptr || key == nullptr || same(alias, "boot"))
            return;
        const std::lock_guard lock(g_config_lock);
        const std::string alias_key = lower(alias);
        const std::string text = value != nullptr ? value : "";
        Document& doc = g_documents[alias_key];
        Section* target = nullptr;
        for (Section& s : doc)
            if (same(s.name, section))
                target = &s;
        if (target == nullptr) {
            doc.push_back({section, {}});
            target = &doc.back();
        }
        bool found = false;
        bool changed = true;
        for (auto& [name, current] : target->values) {
            if (same(name, key)) {
                changed = current != text;
                current = text;
                found = true;
                break;
            }
        }
        if (!found)
            target->values.emplace_back(key, text);
        // To its file at once (the same value again isn't a write: a slider dragged sets its key every frame)
        if (changed)
            write_through(alias_key, doc, section, key, text);
    }
    static bool GetBool(void*, const char* alias, const char* section, const char* key, bool otherwise)
    {
        const std::lock_guard lock(g_config_lock);
        const std::string* value = config_value(alias, section, key);
        if (value == nullptr || value->empty())
            return otherwise;
        const std::string text = lower(*value);
        return text == "1" || text == "true" || text == "yes" || text == "on" || text == "enabled";
    }
    static uint8_t GetUInt8(void*, const char* a, const char* s, const char* k, uint8_t d) { return config_number(a, s, k, d); }
    static uint16_t GetUInt16(void*, const char* a, const char* s, const char* k, uint16_t d) { return config_number(a, s, k, d); }
    static uint32_t GetUInt32(void*, const char* a, const char* s, const char* k, uint32_t d) { return config_number(a, s, k, d); }
    static uint64_t GetUInt64(void*, const char* a, const char* s, const char* k, uint64_t d) { return config_number(a, s, k, d); }
    static int8_t GetInt8(void*, const char* a, const char* s, const char* k, int8_t d) { return config_number(a, s, k, d); }
    static int16_t GetInt16(void*, const char* a, const char* s, const char* k, int16_t d) { return config_number(a, s, k, d); }
    static int32_t GetInt32(void*, const char* a, const char* s, const char* k, int32_t d) { return config_number(a, s, k, d); }
    static int64_t GetInt64(void*, const char* a, const char* s, const char* k, int64_t d) { return config_number(a, s, k, d); }
    static float GetFloat(void*, const char* a, const char* s, const char* k, float d) { return config_number(a, s, k, d); }
    static double GetDouble(void*, const char* a, const char* s, const char* k, double d) { return config_number(a, s, k, d); }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// ILogManager: into VanaCore's log.

uint32_t g_log_level = 4; // Ashita::LogLevel::Info

struct LogImpl {
    static bool Log(void*, uint32_t level, const char* source, const char* message)
    {
        if (level > g_log_level || level == 0)
            return false;
        static constexpr const char* kLevels[] = {"", "critical", "error", "warning", "info", "debug"};
        log::write(L"[%hs] %hs: %hs", source != nullptr ? source : "?", level < 6 ? kLevels[level] : "?",
                   message != nullptr ? message : "");
        return true;
    }
    static bool LogfV(void* c, uint32_t level, const char* source, const char* text, va_list args)
    {
        return Log(c, level, source, format(text, args).c_str());
    }
    static uint32_t GetLogLevel(void*) { return g_log_level; }
    static void SetLogLevel(void*, uint32_t level) { g_log_level = level; }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IOffsetManager and IPointerManager

std::mutex g_offsets_lock;
std::map<std::pair<std::string, std::string>, int32_t> g_offsets; // (section, key), lower case

struct OffsetImpl {
    static void Add(void*, const char* section, const char* key, int32_t offset)
    {
        if (section == nullptr || key == nullptr)
            return;
        const std::lock_guard lock(g_offsets_lock);
        g_offsets[{lower(section), lower(key)}] = offset;
    }
    static int32_t Get(void*, const char* section, const char* key)
    {
        if (section == nullptr || key == nullptr)
            return 0;
        const std::lock_guard lock(g_offsets_lock);
        const auto it = g_offsets.find({lower(section), lower(key)});
        return it != g_offsets.end() ? it->second : 0;
    }
    static void Delete(void*, const char* section)
    {
        if (section == nullptr)
            return;
        const std::lock_guard lock(g_offsets_lock);
        const std::string name = lower(section);
        for (auto it = g_offsets.begin(); it != g_offsets.end();)
            it = it->first.first == name ? g_offsets.erase(it) : std::next(it);
    }
    static void Delete(void*, const char* section, const char* key)
    {
        if (section == nullptr || key == nullptr)
            return;
        const std::lock_guard lock(g_offsets_lock);
        g_offsets.erase({lower(section), lower(key)});
    }
};

// The pointers Ashita finds for itself, by the names plugins ask for them by
// (config\ashita\ashita.pointers.ini in an Ashita install).
struct KnownPointer {
    const char* name;
    const char* modules;
    const char* pattern;
    int32_t offset;
    uint32_t count;
};
constexpr KnownPointer kKnownPointers[] = {
    {"chat.addchatmessage", "FFXiMain.dll", "81EC????????535556578BE96A086A0089", 0, 0},
    {"chat.parseautotranslate.this", "FFXiMain.dll", "8B??83C9FF33C06A00F2AEF7D1496A01518B0D", 19, 0},
    {"chat.parseautotranslate", "FFXiMain.dll", "5153558B6C24145685ED578BF1C64424????C64424????C64424", 0, 0},
    {"chat.parseinputtext.this", "FFXiMain.dll", "81EC????????8B8C24????????5556578D", 34, 0},
    {"chat.parseinputtext", "FFXiMain.dll", "81EC????????8B8C24????????5556578D", 0, 0},
    {"chat.inputtext.this", "FFXiMain.dll", "8B0D????????6A??53538B11536A??6A??536A", 2, 0},
    {"chat.logfile", "FFXiMain.dll", "50E8????????84C075388B0D", -4, 0},
    {"packets.decryptbuffer", "FFXiMain.dll", "53558BAC24C8050000563BE857", -10, 0},
    {"packets.encryptbuffer", "FFXiMain.dll", "53558B6C247456578BF08BFD83C51CF3A5", -12, 0},
    {"packets.queuepacket1", "FFXiMain.dll", "A1????????5685C0570F??????????8B74240C81FE????00000F8DCD0000008B", 0, 0},
    {"packets.queuepacket2", "FFXiMain.dll", "8B44240C8B4C240803C18BC881E103000080", 0, 0},
    {"autofollow", "FFXiMain.dll", "8BCFE8????????8B0D????????E8????????8BE885ED750CB9", 25, 0},
    {"castbar", "FFXiMain.dll", "85F674??A1????????85C074??8B4808", 5, 0},
    {"entitymap", "FFXiMain.dll", "8B560C8B042A8B0485", 9, 0},
    {"entitymap.size", "FFXiMain.dll", "4781FF????????7C??89", 3, 0},
    {"inventory", "FFXiMain.dll", "A1????????8D9488????????8990????????E9", 1, 0},
    {"inventory.selecteditem", "FFXiMain.dll", "8B0D????????85C9742B", 2, 0},
    {"menu", "FFXiMain.dll", "8B480C85C974??8B510885D274??3B05", 16, 0},
    {"party", "FFXiMain.dll", "0FBEC38D0C5256578BF58D0448", 23, 0},
    {"party.statusicons", "FFXiMain.dll", "B93C0000008D7004BF????????F3A5", 9, 0},
    {"player", "FFXiMain.dll", "6A018D44242C68808080806683????506683????5152E8????????A1", 28, 0},
    {"player.hasability", "FFXiMain.dll", "A1????????85C074??8A88????????84C974??8A88????????84C974??05????????C333C0C3", 0, 0},
    {"player.haskeyitem", "FFXiMain.dll", "8B44240485C07C1683F8707D118B4C2408", 20, 0},
    {"player.hasspell", "FFXiMain.dll", "A1????????85C075??C38A88????????84C975??33C0C305????????C3", 0, 2},
    {"player.joblevels", "FFXiMain.dll", "8B0D????????85C974??8B4424043C1073??25FF000000", 0, 0},
    {"player.jobmasterlevels", "FFXiMain.dll", "8B0D????????85C974??8B4424043C1873??25FF000000", 0, 0},
    {"player.loginstatus", "FFXiMain.dll", "6605????6685C074??8B0D????????25????000050", 11, 0},
    {"player.petmp", "FFXiMain.dll", "84C0750333C0C333C0A0????????C3", 10, 0},
    {"player.pettp", "FFXiMain.dll", "8B44240C5657668B48048D70048BC183E03F83E802", 61, 0},
    {"recast.abilities", "FFXiMain.dll", "894124E9????????8B46??6A006A00508BCEE8", 25, 0},
    {"recast.spells", "FFXiMain.dll", "56BE????????66833E00??????????????662906", 2, 0},
    {"target.target", "FFXiMain.dll", "53568BF18B480433DB3BCB75065E33C05B59C38B0D", 45, 0},
    {"target.settarget", "FFXiMain.dll", "8B0D????????83C40885C974??85C074??6A006A0150E8", 0, 0},
    {"clientversion", "FFXiMain.dll", "81EC240200008B0D????????C6442400BFC644240100C644240200", 12, 0},
    {"config.autodisconnect", "polcore.dll,polcoreeu.dll", "8B44240883C40485C075??A3????????A3", 12, 0},
    {"config.languagefilter", "FFXiMain.dll", "8B0D????????85C975??83C8??C38B44240450E8????????C3", 2, 0},
};

std::mutex g_pointers_lock;
std::map<std::string, uintptr_t> g_pointers; // lower-case name

// Scans each module in the comma- (or semicolon-) separated list for the `count`th match of
// `pattern`; the match's address plus `offset`, or 0.
uintptr_t find_pattern(const char* modules, const char* pattern, int32_t offset, uint32_t count)
{
    if (modules == nullptr || pattern == nullptr)
        return 0;
    std::string list(modules);
    size_t start = 0;
    while (start <= list.size()) {
        size_t end = list.find_first_of(",;", start);
        if (end == std::string::npos)
            end = list.size();
        const std::string name = trim(std::string_view(list).substr(start, end - start));
        start = end + 1;
        const HMODULE module = name.empty() ? nullptr : GetModuleHandleA(name.c_str());
        if (module == nullptr)
            continue;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(module) + dos->e_lfanew);
        const uintptr_t found = scan::find(reinterpret_cast<uintptr_t>(module), nt->OptionalHeader.SizeOfImage, pattern, count);
        if (found != 0)
            return found + offset;
    }
    return 0;
}

struct PointerImpl {
    static void Add(void*, const char* name, uintptr_t pointer)
    {
        if (name == nullptr)
            return;
        const std::lock_guard lock(g_pointers_lock);
        g_pointers[lower(name)] = pointer;
    }
    static uintptr_t Add(void*, const char* name, const char* modules, const char* pattern, int32_t offset, uint32_t count)
    {
        const uintptr_t address = find_pattern(modules, pattern, offset, count);
        if (name != nullptr) {
            const std::lock_guard lock(g_pointers_lock);
            g_pointers[lower(name)] = address;
        }
        return address;
    }
    static uintptr_t Get(void*, const char* name)
    {
        if (name == nullptr)
            return 0;
        const std::string key = lower(name);
        {
            const std::lock_guard lock(g_pointers_lock);
            const auto it = g_pointers.find(key);
            if (it != g_pointers.end())
                return it->second;
        }
        for (const KnownPointer& known : kKnownPointers) {
            if (key == known.name) {
                const uintptr_t address = find_pattern(known.modules, known.pattern, known.offset, known.count);
                if (address != 0) {
                    const std::lock_guard lock(g_pointers_lock);
                    g_pointers[key] = address;
                }
                return address;
            }
        }
        return 0;
    }
    static void Delete(void*, const char* name)
    {
        if (name == nullptr)
            return;
        const std::lock_guard lock(g_pointers_lock);
        g_pointers.erase(lower(name));
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IPluginManager, IPolPluginManager (no POL plugins: PlayOnline isn't running), IPacketManager

struct PluginManagerImpl {
    static bool Load(void*, const char* name, const char* args) { return plugin_load(name, args); }
    static bool Unload(void*, const char* name) { return plugin_unload(name); }
    static void UnloadAll(void*) { plugin_unload_all(); }
    static bool IsLoaded(void*, const char* name) { return plugin_loaded(name); }
    static void* Get(void*, const char* name) { return plugin_get(name); }
    static void* Get(void*, uint32_t index) { return plugin_get(index); }
    static uint32_t Count(void*) { return plugin_count(); }
    static void RaiseEvent(void*, const char* name, const void* data, uint32_t size) { plugin_raise(name, data, size); }
    static bool GetSilentPlugins(void*) { return plugins::silent(); }
    static void SetSilentPlugins(void*, bool silent) { plugins::set_silent(silent); }
};

struct PolPluginManagerImpl {
    static bool Load(void*, const char*, const char*) { return false; }
    static bool Unload(void*, const char*) { return false; }
    static void UnloadAll(void*) {}
    static bool IsLoaded(void*, const char*) { return false; }
    static void* Get(void*, const char*) { return nullptr; }
    static void* Get(void*, uint32_t) { return nullptr; }
    static uint32_t Count(void*) { return 0; }
    static void RaiseEvent(void*, const char*, const void*, uint32_t) {}
};

// The game's own outgoing queue (Ashita's packets.queuepacket1 and 2): a slot for a packet, its header's id set (null
// when the game holds that id back: with `p1` 0, an id is sent at most once per its interval), which the caller fills,
// then sealed with its length (the header's size, `length + align` in 4-byte words).
using QueueSlot = uint8_t*(__cdecl*)(int id, uint32_t p1, uint32_t p2);
using QueueSeal = void(__cdecl*)(uint8_t* packet, int length, int align);

template <typename Fill>
bool queue_outgoing(uint16_t id, uint16_t length, uint16_t align, uint32_t p1, uint32_t p2, Fill fill)
{
    static QueueSlot slot = nullptr;
    static QueueSeal seal = nullptr;
    if (slot == nullptr || seal == nullptr) {
        slot = reinterpret_cast<QueueSlot>(
            find_pattern("FFXiMain.dll", "A1????????5685C0570F??????????8B74240C81FE????00000F8DCD0000008B", 0, 0));
        seal = reinterpret_cast<QueueSeal>(find_pattern("FFXiMain.dll", "8B44240C8B4C240803C18BC881E103000080", 0, 0));
        if (slot == nullptr || seal == nullptr)
            return false;
    }
    uint8_t* packet = slot(id, p1, p2);
    if (packet == nullptr)
        return false;
    fill(packet);
    seal(packet, length, align);
    return true;
}

struct PacketImpl {
    // The plugin (or addon, "addon <name>") adding a packet doesn't see it go through the plugins and addons (packets::inject).
    static std::string_view injector()
    {
        return calling();
    }
    static void AddIncomingPacket(void*, uint16_t id, uint32_t size, uint8_t* data) { packets::inject(true, id, size, data, injector()); }
    static void AddOutgoingPacket(void*, uint16_t id, uint32_t size, uint8_t* data) { packets::inject(false, id, size, data, injector()); }
    static bool QueueOutgoingPacket(void*, uint16_t id, uint16_t length, uint16_t align, uint32_t p1, uint32_t p2,
                                    std::function<void(uint8_t*)> callback)
    {
        return queue_outgoing(id, length, align, p1, p2, [&](uint8_t* packet) {
            if (callback)
                callback(packet);
        });
    }
    static bool QueueOutgoingPacket(void*, uint16_t id, uint16_t length, uint16_t align, uint32_t p1, uint32_t p2,
                                    sdk::queuepacketcallback_f callback, void* param)
    {
        return queue_outgoing(id, length, align, p1, p2, [&](uint8_t* packet) {
            if (callback != nullptr)
                callback(packet, param);
        });
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// IInputManager and its devices: what VanaCore doesn't do yet stays unimplemented (logged).

struct InputImpl {
    static void* GetController(void* c) { return links(c).controller; }
    static void* GetKeyboard(void* c) { return links(c).keyboard; }
    static void* GetMouse(void* c) { return links(c).mouse; }
    static void* GetXInput(void* c) { return links(c).xinput; }
};

struct KeyboardImpl {
    // Binds, as /bind makes them (binds.cpp): on the key's press (`down`) or release
    static unsigned modifiers(bool alt, bool apps, bool ctrl, bool shift, bool win)
    {
        return (alt ? binds::kAlt : 0) | (apps ? binds::kApps : 0) | (ctrl ? binds::kCtrl : 0) | (shift ? binds::kShift : 0) |
               (win ? binds::kWin : 0);
    }
    static void Bind(void*, uint32_t key, bool down, bool alt, bool apps, bool ctrl, bool shift, bool win, const char* command)
    {
        binds::add(key, modifiers(alt, apps, ctrl, shift, win), !down, command != nullptr ? command : "");
    }
    static void Unbind(void*, uint32_t key, bool down, bool alt, bool apps, bool ctrl, bool shift, bool win)
    {
        binds::remove(key, modifiers(alt, apps, ctrl, shift, win), !down);
    }
    static void UnbindAll(void*) { binds::clear(); }
    static bool IsBound(void*, uint32_t key, bool down, bool alt, bool apps, bool ctrl, bool shift, bool win)
    {
        return binds::bound(key, modifiers(alt, apps, ctrl, shift, win), !down);
    }
    // DirectInput key codes are keyboard scan codes (DIK_*); 0x80 and up are the extended keys.
    static uint32_t V2D(void*, uint32_t key)
    {
        const UINT scan = MapVirtualKeyA(key, MAPVK_VK_TO_VSC_EX);
        return (scan & 0xFF) | ((scan & 0xE000) == 0xE000 ? 0x80 : 0);
    }
    static uint32_t D2V(void*, uint32_t key)
    {
        const UINT scan = (key & 0x7F) | ((key & 0x80) != 0 ? 0xE000 : 0);
        return MapVirtualKeyA(scan, MAPVK_VSC_TO_VK_EX);
    }
    static const char* D2S(void*, uint32_t key)
    {
        static thread_local char name[64];
        const LONG lparam = static_cast<LONG>(((key & 0x7F) << 16) | ((key & 0x80) != 0 ? (1 << 24) : 0));
        if (GetKeyNameTextA(lparam, name, sizeof(name)) <= 0)
            name[0] = '\0';
        return name;
    }
    static uint32_t S2D(void*, const char* text)
    {
        if (text == nullptr || text[0] == '\0')
            return 0;
        for (uint32_t key = 1; key < 0x100; ++key) {
            char name[64] = {};
            const LONG lparam = static_cast<LONG>(((key & 0x7F) << 16) | ((key & 0x80) != 0 ? (1 << 24) : 0));
            if (GetKeyNameTextA(lparam, name, sizeof(name)) > 0 && _stricmp(name, text) == 0)
                return key;
        }
        return 0;
    }
    // Called with the game's DirectInput keyboard reads and its keys' window messages (host.cpp)
    static void AddCallback(void*, const char* alias, sdk::getdevicedatacallback_f data, sdk::getdevicestatecallback_f state,
                            sdk::keyboardcallback_f keyboard)
    {
        input_add_keyboard(alias, data, state, keyboard);
    }
    static void RemoveCallback(void*, const char* alias) { input_remove(alias, InputDevice::Keyboard); }
    static bool GetBlockBindsDuringInput(void*) { return binds::block_during_input(); }
    static void SetBlockBindsDuringInput(void*, bool block) { binds::set_block_during_input(block); }
};

struct MouseImpl {
    // Called with the mouse's window messages over the game window, in the game's pixels (host.cpp)
    static void AddCallback(void*, const char* alias, sdk::mousecallback_f mouse) { input_add_mouse(alias, mouse); }
    static void RemoveCallback(void*, const char* alias) { input_remove(alias, InputDevice::Mouse); }
};

// IController (DirectInput controllers) and IXInput: callbacks for each read and each change (gamepad.cpp), presses
// queued as if made, and whether movement inside the dead zone raises anything
struct ControllerImpl {
    static void AddCallback(void*, const char* alias, sdk::getdevicedatacallback_f data, sdk::getdevicestatecallback_f state,
                            sdk::controllercallback_f controller)
    {
        input_add_controller(alias, data, state, controller);
    }
    static void RemoveCallback(void*, const char* alias) { input_remove(alias, InputDevice::Controller); }
    static void QueueButtonData(void*, DWORD offset, DWORD data) { gamepad::queue_dinput(offset, data); }
    static bool GetTrackDeadZone(void*) { return gamepad::track_dead_zone(false); }
    static void SetTrackDeadZone(void*, bool track) { gamepad::set_track_dead_zone(false, track); }
};

struct XInputImpl {
    static void AddCallback(void*, const char* alias, sdk::xinputgetstatecallback_f state, sdk::xinputcallback_f xinput)
    {
        input_add_xinput(alias, state, xinput);
    }
    static void RemoveCallback(void*, const char* alias) { input_remove(alias, InputDevice::XInput); }
    static void QueueButtonData(void*, uint8_t button, int16_t state) { gamepad::queue_xinput(button, state); }
    static bool GetTrackDeadZone(void*) { return gamepad::track_dead_zone(true); }
    static void SetTrackDeadZone(void*, bool track) { gamepad::set_track_dead_zone(true, track); }
};

} // namespace

void run_command(int32_t mode, const char* command, bool queue)
{
    if (command == nullptr)
        return;
    // Ashita's own modes (parse, script, force-handle) run as typed.
    const int game_mode = mode < 0 ? 1 : mode;
    if (queue)
        chat::run(game_mode, command);
    else
        chat::execute(game_mode, command);
}

bool input_insert(const std::string& text)
{
    bool open = false;
    std::string current;
    const std::uintptr_t object = chat::input_object();
    if (!chat::input_line(open, current) || !open || object == 0)
        return false;
    const uint32_t at = ChatImpl::GetInputTextRawCaretPosition(nullptr);
    const size_t caret = std::min<size_t>(at, current.size());
    ChatImpl::SetInputText(nullptr, (current.substr(0, caret) + text + current.substr(caret)).c_str());
    const uint32_t after = static_cast<uint32_t>(std::min<size_t>(caret + text.size(), 0x1C3));
    std::memcpy(reinterpret_cast<void*>(object + kInputRawCaret), &after, 4);
    return true;
}

bool queue_packet(uint16_t id, uint16_t length, uint16_t align, uint32_t p1, uint32_t p2,
                  void (*fill)(uint8_t* packet, void* param), void* param)
{
    return queue_outgoing(id, length, align, p1, p2, [&](uint8_t* packet) { fill(packet, param); });
}

void build_core(Links& a, Links& b)
{
    namespace v416 = sdk::v416;
    namespace v430 = sdk::v430;
    a.core = make<v416::AshitaCoreObject, CoreImpl>(&a);
    b.core = make<v430::AshitaCoreObject, CoreImpl>(&b);
    a.properties = make<v416::PropertiesObject, PropertiesImpl>(&a);
    b.properties = make<v430::PropertiesObject, PropertiesImpl>(&b);
    a.chat = make<v416::ChatManagerObject, ChatImpl>(&a);
    b.chat = make<v430::ChatManagerObject, ChatImpl>(&b);
    a.config = make<v416::ConfigurationManagerObject, ConfigImpl>(&a);
    b.config = make<v430::ConfigurationManagerObject, ConfigImpl>(&b);
    a.log = make<v416::LogManagerObject, LogImpl>(&a);
    b.log = make<v430::LogManagerObject, LogImpl>(&b);
    a.offsets = make<v416::OffsetManagerObject, OffsetImpl>(&a);
    b.offsets = make<v430::OffsetManagerObject, OffsetImpl>(&b);
    a.pointers = make<v416::PointerManagerObject, PointerImpl>(&a);
    b.pointers = make<v430::PointerManagerObject, PointerImpl>(&b);
    a.plugins = make<v416::PluginManagerObject, PluginManagerImpl>(&a);
    b.plugins = make<v430::PluginManagerObject, PluginManagerImpl>(&b);
    a.pol_plugins = make<v416::PolPluginManagerObject, PolPluginManagerImpl>(&a);
    b.pol_plugins = make<v430::PolPluginManagerObject, PolPluginManagerImpl>(&b);
    a.packets = make<v416::PacketManagerObject, PacketImpl>(&a);
    b.packets = make<v430::PacketManagerObject, PacketImpl>(&b);
    a.input = make<v416::InputManagerObject, InputImpl>(&a);
    b.input = make<v430::InputManagerObject, InputImpl>(&b);
    struct Nothing {};
    a.controller = make<v416::ControllerObject, ControllerImpl>(&a);
    b.controller = make<v430::ControllerObject, ControllerImpl>(&b);
    a.keyboard = make<v416::KeyboardObject, KeyboardImpl>(&a);
    b.keyboard = make<v430::KeyboardObject, KeyboardImpl>(&b);
    a.mouse = make<v416::MouseObject, MouseImpl>(&a);
    b.mouse = make<v430::MouseObject, MouseImpl>(&b);
    a.xinput = make<v416::XInputObject, XInputImpl>(&a);
    b.xinput = make<v430::XInputObject, XInputImpl>(&b);
    // the GUI manager: gui.cpp
}

} // namespace vanacore::ashita
