#include "lua_api.hpp"

#include <windows.h>

#include <commdlg.h>
#include <mmsystem.h>
#include <shellapi.h>

#include <lua.hpp>

#include <vanacore/version.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <regex>
#include <string>
#include <type_traits>
#include <vector>

#include "addonbox.hpp"
#include "backdrop.hpp"
#include "addonpkg_lua.hpp"
#include "addons.hpp"
#include "asm_text.hpp"
#include "binds.hpp"
#include "buffwin.hpp"
#include "castbar.hpp"
#include "hudwin.hpp"
#include "hwmon.hpp"
#include "camera.hpp"
#include "damagetext.hpp"
#include "chat.hpp"
#include "client.hpp"
#include "clients.hpp"
#include "clipboard.hpp"
#include "compat/ashita/host.hpp"
#include "compat/windower/objects.hpp"
#include "dats.hpp"
#include "equipstats.hpp"
#include "font_folder.hpp"
#include "game.hpp"
#include "gametext.hpp"
#include "gameui.hpp"
#include "graphics.hpp"
#include "grass_assets.hpp"
#include "gamepad.hpp"
#include "hideui.hpp"
#include "gui.hpp"
#include "ini.hpp"
#include "input.hpp"
#include "ipc.hpp"
#include "layout.hpp"
#include "log.hpp"
#include "lua_ashita.hpp"
#include "lua_entity.hpp"
#include "lua_maxhp.hpp"
#include "zoneload.hpp"
#include "lua_gamewin.hpp"
#include "lua_imgui.hpp"
#include "widgets.hpp"
#include "macrobar.hpp"
#include "mapwin.hpp"
#include "mcp.hpp"
#include "outline.hpp"
#include "overlay.hpp"
#include "packets.hpp"
#include "plugin_memory.hpp"
#include "plugins.hpp"
#include "raise.hpp"
#include "registry.hpp"
#include "settings.hpp"
#include "targetwin.hpp"
#include "scan.hpp"
#include "text.hpp"
#include "ui.hpp"
#include "uiscale.hpp"
#include "window.hpp"
#include "winimage.hpp"
#include "zoneweather.hpp"

// LuaSocket's C half (third_party\luasocket), built in
extern "C" int luaopen_socket_core(lua_State* L);
extern "C" int luaopen_mime_core(lua_State* L);

#pragma comment(lib, "winmm.lib") // PlaySound

// A Lua error unwinds past C++ destructors, so these functions raise one (luaL_check*, luaL_error)
// only while no C++ object that needs destroying is alive: arguments are checked first, and work
// that allocates happens in helpers or blocks that end before any error.

namespace vanacore::lua_api {
namespace {

const char kContextKey = 0; // registry key (its address): the state's Context*
constexpr char kBoxType[] = "vana.ui.box";

std::wstring widen(const char* text, UINT codepage = CP_UTF8)
{
    const int length = MultiByteToWideChar(codepage, 0, text, -1, nullptr, 0);
    if (length <= 1)
        return {};
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codepage, 0, text, -1, out.data(), length);
    out.resize(static_cast<size_t>(length) - 1);
    return out;
}

std::string narrow(const std::wstring& text, UINT codepage = CP_UTF8)
{
    const int length = WideCharToMultiByte(codepage, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(codepage, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    out.resize(static_cast<size_t>(length) - 1);
    return out;
}

void set_function(lua_State* L, const char* name, lua_CFunction function)
{
    lua_pushcfunction(L, function);
    lua_setfield(L, -2, name);
}

void set_string(lua_State* L, const char* name, const std::string& value)
{
    lua_pushlstring(L, value.data(), value.size());
    lua_setfield(L, -2, name);
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.memory: the game's memory. Addresses are numbers.

std::uintptr_t check_address(lua_State* L, int index)
{
    const lua_Number n = luaL_checknumber(L, index);
    return static_cast<std::uintptr_t>(static_cast<std::uint32_t>(static_cast<std::int64_t>(n)));
}

bool copy_from(void* out, std::uintptr_t address, size_t size)
{
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(address), size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool copy_to(std::uintptr_t address, const void* data, size_t size)
{
    __try {
        std::memcpy(reinterpret_cast<void*>(address), data, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Writes `size` bytes at `address`. Pages that aren't writable (code, constants) are made writable
// for the write and get their protection back after it.
bool write_memory(std::uintptr_t address, const void* data, size_t size)
{
    if (size == 0)
        return true;
    MEMORY_BASIC_INFORMATION info{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT)
        return false;
    constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    const std::uintptr_t region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    const bool unprotect = (info.Protect & kWritable) == 0 || (info.Protect & PAGE_GUARD) != 0 || address + size > region_end;
    DWORD old = 0;
    if (unprotect && !VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old))
        return false;
    const bool written = copy_to(address, data, size);
    if (unprotect)
        VirtualProtect(reinterpret_cast<void*>(address), size, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
    return written;
}

// read_T(address): the value, or 0 if the memory can't be read.
template <typename T>
int read_value(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    T value{};
    if (!copy_from(&value, address, sizeof(T)))
        value = T{};
    lua_pushnumber(L, static_cast<lua_Number>(value));
    return 1;
}

// write_T(address, value): true if written.
template <typename T>
int write_value(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    const lua_Number n = lua_isboolean(L, 2) ? (lua_toboolean(L, 2) ? 1 : 0) : luaL_checknumber(L, 2);
    T value{};
    if constexpr (std::is_floating_point_v<T>)
        value = static_cast<T>(n);
    else
        value = static_cast<T>(static_cast<std::int64_t>(n)); // negative values wrap, as in C
    lua_pushboolean(L, write_memory(address, &value, sizeof(T)));
    return 1;
}

// read_string(address[, size]): up to `size` bytes (default 1024), stopping at a NUL.
int read_string(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    const lua_Integer size = luaL_optinteger(L, 2, 1024);
    if (size <= 0) {
        lua_pushliteral(L, "");
        return 1;
    }
    // Scratch space the GC owns, so nothing leaks if pushing the result fails.
    auto* buffer = static_cast<char*>(lua_newuserdata(L, static_cast<size_t>(size)));
    size_t length = 0;
    while (length < static_cast<size_t>(size) && copy_from(buffer + length, address + length, 1) && buffer[length] != '\0')
        ++length;
    lua_pushlstring(L, buffer, length);
    return 1;
}

// read_array(address, size): a table of `size` bytes (1-based), or nil if unreadable.
int read_array(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    const lua_Integer size = luaL_checkinteger(L, 2);
    if (size <= 0) {
        lua_newtable(L);
        return 1;
    }
    auto* buffer = static_cast<unsigned char*>(lua_newuserdata(L, static_cast<size_t>(size)));
    if (!copy_from(buffer, address, static_cast<size_t>(size))) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, static_cast<int>(size), 0);
    for (lua_Integer i = 0; i < size; ++i) {
        lua_pushnumber(L, buffer[i]);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// read_bytes(address, size): `size` bytes as a string, NULs and all, or nil if unreadable or `size` is 0.
int read_bytes(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    const lua_Integer size = luaL_checkinteger(L, 2);
    if (size <= 0) {
        lua_pushnil(L);
        return 1;
    }
    auto* buffer = static_cast<char*>(lua_newuserdata(L, static_cast<size_t>(size)));
    if (copy_from(buffer, address, static_cast<size_t>(size)))
        lua_pushlstring(L, buffer, static_cast<size_t>(size));
    else
        lua_pushnil(L);
    return 1;
}

// write_string(address, text): the text and a NUL after it.
int write_string(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    size_t length = 0;
    const char* text = luaL_checklstring(L, 2, &length);
    lua_pushboolean(L, write_memory(address, text, length + 1));
    return 1;
}

// write_array(address, bytes): a table of byte values (1-based).
int write_array(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    const size_t size = lua_objlen(L, 2);
    if (size == 0) {
        lua_pushboolean(L, 1);
        return 1;
    }
    auto* buffer = static_cast<unsigned char*>(lua_newuserdata(L, size));
    for (size_t i = 0; i < size; ++i) {
        lua_rawgeti(L, 2, static_cast<int>(i + 1));
        buffer[i] = static_cast<unsigned char>(static_cast<std::int64_t>(lua_tonumber(L, -1)));
        lua_pop(L, 1);
    }
    lua_pushboolean(L, write_memory(address, buffer, size));
    return 1;
}

// find(module name or base address, size (0: the whole module), pattern[, offset[, count]]): the
// address of the `count`th match (0: the first) plus `offset`, or 0.
int find(lua_State* L)
{
    std::uintptr_t begin = 0;
    size_t size = 0;
    if (lua_type(L, 1) == LUA_TSTRING) {
        const HMODULE module = GetModuleHandleA(lua_tostring(L, 1));
        size = static_cast<size_t>(luaL_optnumber(L, 2, 0));
        if (module != nullptr) {
            begin = reinterpret_cast<std::uintptr_t>(module);
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(begin + dos->e_lfanew);
            if (size == 0 || size > nt->OptionalHeader.SizeOfImage)
                size = nt->OptionalHeader.SizeOfImage;
        }
    } else {
        begin = check_address(L, 1);
        size = static_cast<size_t>(luaL_checknumber(L, 2));
    }
    const char* pattern = luaL_checkstring(L, 3);
    const auto offset = static_cast<std::intptr_t>(luaL_optnumber(L, 4, 0));
    const auto count = static_cast<unsigned>(luaL_optinteger(L, 5, 0));
    const std::uintptr_t found = begin != 0 ? scan::find(begin, size, pattern, count) : 0;
    lua_pushnumber(L, found != 0 ? static_cast<lua_Number>(static_cast<std::uint32_t>(found + offset)) : 0);
    return 1;
}

// module([name]): the loaded module's base address and size (with no name, the process's own exe's), or nil.
int module(lua_State* L)
{
    const HMODULE handle = GetModuleHandleA(luaL_optstring(L, 1, nullptr));
    if (handle == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(handle);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(handle) + dos->e_lfanew);
    lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(handle)));
    lua_pushnumber(L, nt->OptionalHeader.SizeOfImage);
    return 2;
}

// vana.memory.assembler (asm_text.hpp): x86 assembly text as Ashita 4.30's ashita.memory.assembler reads it.
// assemble(text[, base]): the bytes it makes, encoded to run at `base` (default 0), as a string; nil and the error.
int assembler_assemble(lua_State* L)
{
    std::vector<std::uint8_t> code;
    std::string error;
    if (!asm_text::assemble(luaL_checkstring(L, 1), static_cast<std::uint32_t>(luaL_optnumber(L, 2, 0)), code, error)) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(code.data()), code.size());
    return 1;
}

// cave(text): a code cave of it (executable memory of its own), its address; nil and the error.
int assembler_cave(lua_State* L)
{
    std::string error;
    const std::uintptr_t address = asm_text::cave(luaL_checkstring(L, 1), error);
    if (address == 0) {
        lua_pushnil(L);
        lua_pushstring(L, error.c_str());
        return 2;
    }
    lua_pushnumber(L, static_cast<lua_Number>(address));
    return 1;
}

// release(address): frees a cave cave() made; false if it isn't one.
int assembler_release(lua_State* L)
{
    lua_pushboolean(L, asm_text::release(check_address(L, 1)));
    return 1;
}

// call(address, target, nops) and jmp(...): a call (or jump) to `target` written at `address`, then `nops` NOPs.
int assembler_call(lua_State* L)
{
    asm_text::branch(check_address(L, 1), 0xE8, check_address(L, 2), static_cast<std::uint32_t>(luaL_optinteger(L, 3, 0)));
    return 0;
}

int assembler_jmp(lua_State* L)
{
    asm_text::branch(check_address(L, 1), 0xE9, check_address(L, 2), static_cast<std::uint32_t>(luaL_optinteger(L, 3, 0)));
    return 0;
}

// nop(address, count): `count` NOPs written at `address`.
int assembler_nop(lua_State* L)
{
    asm_text::nop(check_address(L, 1), static_cast<std::uint32_t>(luaL_optinteger(L, 2, 0)));
    return 0;
}

// alloc(size): zeroed, readable and writable memory, or 0.
int alloc(lua_State* L)
{
    const auto size = static_cast<size_t>(luaL_checkinteger(L, 1));
    void* memory = size > 0 ? VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) : nullptr;
    lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(memory)));
    return 1;
}

// free(address): memory from alloc(). Freeing twice is harmless.
int free_memory(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    lua_pushboolean(L, address != 0 && VirtualFree(reinterpret_cast<void*>(address), 0, MEM_RELEASE));
    return 1;
}

// protect(address, size, protection): the old protection, or nil.
int protect(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    const auto size = static_cast<size_t>(luaL_checkinteger(L, 2));
    const auto protection = static_cast<DWORD>(luaL_checkinteger(L, 3));
    DWORD old = 0;
    if (VirtualProtect(reinterpret_cast<void*>(address), size, protection, &old))
        lua_pushnumber(L, old);
    else
        lua_pushnil(L);
    return 1;
}

// unprotect(address, size): makes the memory readable, writable and executable.
int unprotect(lua_State* L)
{
    const std::uintptr_t address = check_address(L, 1);
    const auto size = static_cast<size_t>(luaL_checkinteger(L, 2));
    DWORD old = 0;
    lua_pushboolean(L, VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &old));
    return 1;
}

void open_memory(lua_State* L)
{
    lua_newtable(L);
    set_function(L, "find", &find);
    set_function(L, "module", &module);
    set_function(L, "read_int8", &read_value<std::int8_t>);
    set_function(L, "read_uint8", &read_value<std::uint8_t>);
    set_function(L, "read_int16", &read_value<std::int16_t>);
    set_function(L, "read_uint16", &read_value<std::uint16_t>);
    set_function(L, "read_int32", &read_value<std::int32_t>);
    set_function(L, "read_uint32", &read_value<std::uint32_t>);
    set_function(L, "read_int64", &read_value<std::int64_t>);
    set_function(L, "read_uint64", &read_value<std::uint64_t>);
    set_function(L, "read_float", &read_value<float>);
    set_function(L, "read_double", &read_value<double>);
    set_function(L, "read_string", &read_string);
    set_function(L, "read_array", &read_array);
    set_function(L, "read_bytes", &read_bytes);
    set_function(L, "write_int8", &write_value<std::int8_t>);
    set_function(L, "write_uint8", &write_value<std::uint8_t>);
    set_function(L, "write_int16", &write_value<std::int16_t>);
    set_function(L, "write_uint16", &write_value<std::uint16_t>);
    set_function(L, "write_int32", &write_value<std::int32_t>);
    set_function(L, "write_uint32", &write_value<std::uint32_t>);
    set_function(L, "write_int64", &write_value<std::int64_t>);
    set_function(L, "write_uint64", &write_value<std::uint64_t>);
    set_function(L, "write_float", &write_value<float>);
    set_function(L, "write_double", &write_value<double>);
    set_function(L, "write_string", &write_string);
    set_function(L, "write_array", &write_array);
    set_function(L, "alloc", &alloc);
    set_function(L, "free", &free_memory);
    set_function(L, "protect", &protect);
    set_function(L, "unprotect", &unprotect);
    lua_newtable(L);
    set_function(L, "assemble", &assembler_assemble);
    set_function(L, "cave", &assembler_cave);
    set_function(L, "release", &assembler_release);
    set_function(L, "call", &assembler_call);
    set_function(L, "jmp", &assembler_jmp);
    set_function(L, "nop", &assembler_nop);
    lua_setfield(L, -2, "assembler");
    lua_setfield(L, -2, "memory");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.fs: files and folders. Paths are in the ANSI code page, as Lua's io library uses.

int exists(lua_State* L)
{
    lua_pushboolean(L, GetFileAttributesA(luaL_checkstring(L, 1)) != INVALID_FILE_ATTRIBUTES);
    return 1;
}

// is_dir(path): true if it's a folder.
int is_dir(lua_State* L)
{
    const DWORD attributes = GetFileAttributesA(luaL_checkstring(L, 1));
    lua_pushboolean(L, attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    return 1;
}

bool create_directories(const char* path)
{
    std::string partial;
    for (const char* at = path; *at != '\0'; ++at) {
        if ((*at == '\\' || *at == '/') && !partial.empty() && partial.back() != ':')
            CreateDirectoryA(partial.c_str(), nullptr);
        partial += *at;
    }
    if (!partial.empty())
        CreateDirectoryA(partial.c_str(), nullptr);
    const DWORD attributes = GetFileAttributesA(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// create_dir(path): the folder and any missing parents; true if it exists afterwards.
int create_dir(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    lua_pushboolean(L, create_directories(path));
    return 1;
}

// remove(path): a file, or an empty folder.
int remove_path(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    const DWORD attributes = GetFileAttributesA(path);
    const bool removed = attributes != INVALID_FILE_ATTRIBUTES &&
                         ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ? RemoveDirectoryA(path) : DeleteFileA(path));
    lua_pushboolean(L, removed);
    return 1;
}

// write(path, data): the file holds `data`, whole or not at all: written beside it (path .. '.tmp') and swapped in, so a
// write cut short (the game closing, a full disk) leaves the old file as it was. True, or nil and why.
int write_file(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    size_t size = 0;
    const char* data = luaL_checklstring(L, 2, &size);
    const std::string temp = std::string(path) + ".tmp";
    const HANDLE file = CreateFileA(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD error = 0;
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
    } else {
        DWORD written = 0;
        if (size != 0 && (!WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) || written != size)) {
            const DWORD last = GetLastError();
            error = last != 0 ? last : ERROR_WRITE_FAULT; // a short write that Windows didn't call an error
        }
        CloseHandle(file);
        if (error == 0 && !MoveFileExA(temp.c_str(), path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            error = GetLastError();
        if (error != 0)
            DeleteFileA(temp.c_str());
    }
    if (error != 0) {
        lua_pushnil(L);
        // 5: denied (read-only, or a folder's permissions), 32: another program has it open
        lua_pushfstring(L, "can't write %s (Windows error %d)", path, static_cast<int>(error));
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

enum class Kind { All, Files, Folders };

struct ListOptions {
    Kind kind = Kind::All;
    bool deep = false;       // every subfolder's entries too (never through a link), each after its folder
    bool exact_case = false; // the filter matches case
};

// Adds the entries of `folder` to `names`, each as `prefix` + its name; `pattern` (null for all) is matched against
// the name alone.
void list_into(const std::string& folder, const std::string& prefix, const std::regex* pattern,
               const ListOptions& options, std::vector<std::string>& names)
{
    std::string base = folder;
    if (!base.empty() && base.back() != '\\' && base.back() != '/')
        base += '\\';
    WIN32_FIND_DATAA found{};
    HANDLE handle = FindFirstFileA((base + '*').c_str(), &found);
    if (handle == INVALID_HANDLE_VALUE)
        return;
    do {
        const std::string name = found.cFileName;
        if (name == "." || name == "..")
            continue;
        const bool folder_entry = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        const bool wanted = !((options.kind == Kind::Files && folder_entry) || (options.kind == Kind::Folders && !folder_entry));
        if (wanted && (pattern == nullptr || std::regex_match(name, *pattern)))
            names.push_back(prefix + name);
        if (options.deep && folder_entry && (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
            list_into(base + name, prefix + name + '\\', pattern, options, names);
    } while (FindNextFileA(handle, &found));
    FindClose(handle);
}

// The names of the `options.kind` entries in `folder` matching `filter` (a regular expression for the whole
// name; empty for all); with `options.deep`, below the top they're paths from `folder` ("sub\\name"). `error` is set
// if the filter isn't a regular expression.
std::vector<std::string> list_folder(const std::string& folder, const std::string& filter, const ListOptions& options, bool& error)
{
    std::vector<std::string> names;
    error = false;
    std::regex pattern;
    try {
        if (!filter.empty())
            pattern = std::regex(filter, options.exact_case ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
    } catch (const std::regex_error&) {
        error = true;
        return names;
    }
    list_into(folder, "", filter.empty() ? nullptr : &pattern, options, names);
    return names;
}

// list(folder[, filter[, which]]): a table of names (not paths), or nil and a message. `which` is "files" or
// "folders", or a table { which = "files" or "folders", deep = true (subfolders too, as paths from folder),
// case = true (the filter matches case) }.
int list(lua_State* L)
{
    const char* folder = luaL_checkstring(L, 1);
    const char* filter = luaL_optstring(L, 2, "");
    ListOptions options;
    std::string which;
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "which");
        if (lua_type(L, -1) == LUA_TSTRING)
            which = lua_tostring(L, -1);
        lua_getfield(L, 3, "deep");
        options.deep = lua_toboolean(L, -1) != 0;
        lua_getfield(L, 3, "case");
        options.exact_case = lua_toboolean(L, -1) != 0;
        lua_pop(L, 3);
    } else {
        which = luaL_optstring(L, 3, "");
    }
    options.kind = which == "files" ? Kind::Files : which == "folders" ? Kind::Folders : Kind::All;
    bool error = false;
    {
        const std::vector<std::string> names = list_folder(folder, filter, options, error);
        lua_createtable(L, static_cast<int>(names.size()), 0);
        for (size_t i = 0; i < names.size(); ++i) {
            lua_pushlstring(L, names[i].data(), names[i].size());
            lua_rawseti(L, -2, static_cast<int>(i + 1));
        }
    }
    if (error) {
        lua_pushnil(L);
        lua_pushfstring(L, "not a regular expression: %s", filter);
        return 2;
    }
    return 1;
}

void open_fs(lua_State* L)
{
    lua_newtable(L);
    set_function(L, "exists", &exists);
    set_function(L, "is_dir", &is_dir);
    set_function(L, "create_dir", &create_dir);
    set_function(L, "remove", &remove_path);
    set_function(L, "write", &write_file);
    set_function(L, "list", &list);
    lua_setfield(L, -2, "fs");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.binds: key binds (/bind), for the keybinds window

void bind_bool(lua_State* L, const char* name, bool value)
{
    lua_pushboolean(L, value ? 1 : 0);
    lua_setfield(L, -2, name);
}

void push_modifiers(lua_State* L, unsigned modifiers)
{
    bind_bool(L, "ctrl", (modifiers & binds::kCtrl) != 0);
    bind_bool(L, "alt", (modifiers & binds::kAlt) != 0);
    bind_bool(L, "shift", (modifiers & binds::kShift) != 0);
    bind_bool(L, "win", (modifiers & binds::kWin) != 0);
    bind_bool(L, "apps", (modifiers & binds::kApps) != 0);
}

void push_bind_entries(lua_State* L, const std::vector<binds::Entry>& list)
{
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (size_t i = 0; i < list.size(); ++i) {
        const binds::Entry& entry = list[i];
        lua_createtable(L, 0, 14);
        set_string(L, "key", entry.key);
        lua_pushinteger(L, static_cast<lua_Integer>(entry.vk));
        lua_setfield(L, -2, "vk");
        set_string(L, "spec", entry.spec);
        set_string(L, "command", entry.command);
        bind_bool(L, "up", entry.up);
        lua_pushstring(L, entry.when == 'c' ? "closed" : entry.when == 'o' ? "open" : "always");
        lua_setfield(L, -2, "when");
        push_modifiers(L, entry.modifiers);
        lua_pushnumber(L, entry.fired_ago);
        lua_setfield(L, -2, "fired_ago");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
}

// binds.list(): the binds, in the order they were made, each {key = 'F6', vk, spec = '^f6', command, up, when = 'always' |
// 'closed' | 'open', ctrl, alt, shift, win, apps, fired_ago = seconds since it ran (-1 never)}.
int binds_list(lua_State* L)
{
    push_bind_entries(L, binds::entries());
    return 1;
}

// binds.base(): the binds there were when scripts\keybinds.txt first replayed (boot.txt's and the addons'), as binds.list()
// gives them; nil before that. A key the keybinds file removes needs a /unbind line only if it's one of these.
int binds_base(lua_State* L)
{
    std::vector<binds::Entry> list;
    if (!binds::base_entries(list)) {
        lua_pushnil(L);
        return 1;
    }
    push_bind_entries(L, list);
    return 1;
}

// binds.keys(): the names of the keys /bind takes (A-Z, 0-9, F1-F24, numpad0-9, then up, insert, ...), once each.
int binds_keys(lua_State* L)
{
    const std::vector<std::string> keys = binds::key_list();
    lua_createtable(L, static_cast<int>(keys.size()), 0);
    for (size_t i = 0; i < keys.size(); ++i) {
        lua_pushlstring(L, keys[i].data(), keys[i].size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// binds.add(spec, command[, up]): /bind without a word in the chat log (spec as /bind takes the key: ^!f1, %f2, $return).
// True, or nil and why.
int binds_add(lua_State* L)
{
    const char* spec = luaL_checkstring(L, 1);
    const char* command = luaL_checkstring(L, 2);
    const bool up = lua_toboolean(L, 3) != 0;
    std::string error;
    if (binds::set(spec, up, command, error)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushlstring(L, error.data(), error.size());
    return 2;
}

// binds.remove(bind) with a bind from binds.list(), or binds.remove(spec[, up]): /unbind without a word in the chat log.
// True if one was bound.
int binds_remove(lua_State* L)
{
    bool removed = false;
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "vk");
        const unsigned vk = static_cast<unsigned>(lua_tointeger(L, -1));
        lua_pop(L, 1);
        unsigned modifiers = 0;
        const struct {
            const char* name;
            unsigned bit;
        } kMods[] = {{"ctrl", binds::kCtrl}, {"alt", binds::kAlt}, {"shift", binds::kShift}, {"win", binds::kWin}, {"apps", binds::kApps}};
        for (const auto& mod : kMods) {
            lua_getfield(L, 1, mod.name);
            if (lua_toboolean(L, -1) != 0)
                modifiers |= mod.bit;
            lua_pop(L, 1);
        }
        lua_getfield(L, 1, "up");
        const bool up = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, 1, "when");
        const char* when_text = lua_tostring(L, -1);
        const char when = when_text != nullptr && std::strcmp(when_text, "closed") == 0 ? 'c'
                          : when_text != nullptr && std::strcmp(when_text, "open") == 0 ? 'o'
                                                                                         : 'a';
        lua_pop(L, 1);
        removed = binds::unset(vk, modifiers, up, when);
    } else {
        const char* spec = luaL_checkstring(L, 1);
        removed = binds::unset(spec, lua_toboolean(L, 2) != 0);
    }
    lua_pushboolean(L, removed ? 1 : 0);
    return 1;
}

// binds.capture_begin(), binds.capture_end(), binds.capture(): "press a key". capture() is {active, done, cancelled, key =
// the key's name once done, vk, ctrl, alt, shift, win, apps (following the keys held until it's done)}.
int binds_capture_begin(lua_State*)
{
    binds::capture_begin();
    return 0;
}

int binds_capture_end(lua_State*)
{
    binds::capture_end();
    return 0;
}

int binds_capture(lua_State* L)
{
    const binds::CaptureState state = binds::capture_state();
    lua_createtable(L, 0, 10);
    bind_bool(L, "active", state.active);
    bind_bool(L, "done", state.done);
    bind_bool(L, "cancelled", state.cancelled);
    if (state.done && !state.cancelled) {
        set_string(L, "key", binds::key_name_of(state.key));
        lua_pushinteger(L, static_cast<lua_Integer>(state.key));
        lua_setfield(L, -2, "vk");
    }
    push_modifiers(L, state.modifiers);
    return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.chat, vana.config, vana.time, vana.system

// log(...): a line in VanaCore's log (not the chat log), as from the addon.
int log_line(lua_State* L)
{
    const int count = lua_gettop(L);
    luaL_Buffer buffer;
    luaL_buffinit(L, &buffer);
    for (int i = 1; i <= count; ++i) {
        if (i > 1)
            luaL_addchar(&buffer, ' ');
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        luaL_addvalue(&buffer);
    }
    luaL_pushresult(&buffer);
    {
        const std::string line = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "";
        log::write(L"%hs: %hs", context(L).name.c_str(), line.c_str());
    }
    return 0;
}

// chat.write(text[, mode]): a line (or lines) in the chat log.
int chat_write(lua_State* L)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    const int mode = static_cast<int>(luaL_optinteger(L, 2, chat::kDefaultMode));
    chat::write(mode, std::string_view(text, length), context(L).name);
    return 0;
}

// chat.run(line[, mode]): runs the line as if typed, on the next frame.
int chat_run(lua_State* L)
{
    size_t length = 0;
    const char* line = luaL_checklstring(L, 1, &length);
    const int mode = static_cast<int>(luaL_optinteger(L, 2, 1));
    chat::run(mode, std::string(line, length));
    return 0;
}

// chat.preview(key, value): a chat window's place ([client] chat.x / chat.y / chat.width, chat2.*) as the /vana window's
// slider is dragged, shown at the next frame without vanacore.ini (the window writes it when the slider is let go)
int chat_preview(lua_State* L)
{
    chat::preview(luaL_checkstring(L, 1), luaL_checknumber(L, 2));
    return 0;
}

// config.get(section, key[, default]): vanacore.ini's value (UTF-8), or `default` (nil) if it has none.
int config_get(lua_State* L)
{
    const char* section = luaL_checkstring(L, 1);
    const char* key = luaL_checkstring(L, 2);
    bool found = false;
    {
        constexpr wchar_t kMissing[] = L"\x1F<none>\x1F";
        wchar_t value[1024] = {};
        GetPrivateProfileStringW(widen(section).c_str(), widen(key).c_str(), kMissing, value, 1024,
                                 context(L).settings_ini.c_str());
        found = std::wcscmp(value, kMissing) != 0;
        if (found) {
            const std::string utf8 = narrow(value);
            lua_pushlstring(L, utf8.data(), utf8.size());
        }
    }
    if (!found)
        lua_settop(L, 3); // the default, or nil
    return 1;
}

// config.reload(): takes vanacore.ini up now. The game works from a copy of it (settings.hpp), which what changes the file
// meanwhile doesn't reach unless [client] ini.upstream=1; this is the one-off. True if it was taken up.
int config_reload(lua_State* L)
{
    const bool ok = settings::reload();
    if (ok)
        overlay::settings_changed(); // read again at the next frame, not the next half-second look
    lua_pushboolean(L, ok);
    return 1;
}

// config.info(): { upstream = [client] ini.upstream is on, changed = the settings file holds a change the game hasn't
// taken up, file = its path (UTF-8: VanaCore.exe's --ini, the launcher's for the account, else vanacore.ini) }.
int config_info(lua_State* L)
{
    lua_createtable(L, 0, 3);
    lua_pushboolean(L, settings::upstream());
    lua_setfield(L, -2, "upstream");
    lua_pushboolean(L, settings::pending());
    lua_setfield(L, -2, "changed");
    set_string(L, "file", narrow(settings::file()));
    return 1;
}
// config.game(name): the game's own setting `name` (its registry value name: '0001', 'padmode000') as
// vanacore.ini hands it to the game, [ffxi]'s by name or else [ffxi.registry]'s ('0034': [ffxi] mode's
// 0-3), as text; nil when it's left to the game's registry. As the game started.
int config_game(lua_State* L)
{
    const std::wstring value = registry::setting(luaL_checkstring(L, 1));
    if (value.empty()) {
        lua_pushnil(L);
        return 1;
    }
    const std::string utf8 = narrow(value);
    lua_pushlstring(L, utf8.data(), utf8.size());
    return 1;
}

// config.set(section, key, value): writes vanacore.ini in place (value as text; nil comments the key
// out where it is: ini.hpp). True if it was written. What takes effect while the game runs sees it at the next frame, or at once after
// vana.graphics.reload().
int config_set(lua_State* L)
{
    const char* section = luaL_checkstring(L, 1);
    const char* key = luaL_checkstring(L, 2);
    std::wstring value;
    const bool remove = lua_isnoneornil(L, 3);
    if (!remove) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, 3);
        lua_call(L, 1, 1);
        value = widen(lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "");
        lua_pop(L, 1);
    }
    const std::wstring& ini = context(L).settings_ini;
    const bool ok = ini::set(ini, widen(section).c_str(), widen(key).c_str(), remove ? nullptr : value.c_str());
    if (ok)
        overlay::settings_changed(); // read again at the next frame, not the next half-second look
    lua_pushboolean(L, ok);
    return 1;
}

// clients.list(): the games running on this PC, { { pid =, name = the character ('' before it's in the world), self = this
// game, main = the Main }, ... }.
int clients_list(lua_State* L)
{
    const std::vector<clients::Client> list = clients::list();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    int index = 0;
    for (const clients::Client& client : list) {
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, static_cast<lua_Integer>(client.pid));
        lua_setfield(L, -2, "pid");
        set_string(L, "name", narrow(client.name));
        lua_pushboolean(L, client.self);
        lua_setfield(L, -2, "self");
        lua_pushboolean(L, client.main);
        lua_setfield(L, -2, "main");
        lua_rawseti(L, -2, ++index);
    }
    return 1;
}

// clients.set_main(name): that character is the Main ([characters] main in vanacore.ini); nil or '' for none. True if it
// was written.
int clients_set_main(lua_State* L)
{
    const std::wstring name = widen(luaL_optstring(L, 1, ""));
    lua_pushboolean(L, clients::set_main(name));
    return 1;
}

// clients.copy(): the Main's settings to every other game running (they take them up within a second). The number of games
// they went to, or nil and why.
int clients_copy(lua_State* L)
{
    std::wstring why;
    const int sent = clients::copy_main_to_alts(why);
    if (sent < 0) {
        lua_pushnil(L);
        const std::string text = narrow(why);
        lua_pushlstring(L, text.data(), text.size());
        return 2;
    }
    lua_pushinteger(L, sent);
    return 1;
}

// clients.front(pid): that game's window in front (from this game while it's in front). True if it is now.
int clients_front(lua_State* L)
{
    lua_pushboolean(L, window::front_process(static_cast<unsigned long>(luaL_checkinteger(L, 1))));
    return 1;
}

// graphics.info(): { renderer = 'd3d8', 'd3d9', 'd3d11' or 'd3d12' (what vanacore.ini asks for), running = the one the game draws
// with (d3d8 when the one asked for didn't come up), active = true when the game draws with VanaCore's own Direct3D 8 (on
// Direct3D 12 or 11), effects = true when its scene effects can run (on either), gpu = the graphics card's name ('' with the
// game's own Direct3D 8), monitor_width, monitor_height = the size in pixels of the screen the game's window is on, work_width,
// work_height = that screen less the taskbar (its work area: [ffxi] resolution.game=auto-taskbar), ui_fit_width, ui_fit_height =
// the menus' size [ffxi] resolution.ui=window comes to now (the game's resolution, fitted to [ui] aspect's shape) }.
int graphics_info(lua_State* L)
{
    const graphics::Info info = graphics::info();
    lua_createtable(L, 0, 20);
    set_string(L, "renderer", info.renderer);
    set_string(L, "running", info.running);
    lua_pushboolean(L, info.effects);
    lua_setfield(L, -2, "effects");
    lua_pushboolean(L, info.active);
    lua_setfield(L, -2, "active");
    set_string(L, "gpu", info.adapter);
    set_string(L, "driver", info.driver);
    set_string(L, "driver_version", info.driver_version);
    set_string(L, "driver_date", info.driver_date);
    set_string(L, "driver_installed", info.driver_installed);
    lua_pushinteger(L, info.driver_days);
    lua_setfield(L, -2, "driver_days");
    set_string(L, "dlss", info.dlss);
    set_string(L, "framegen", info.framegen);
    lua_pushboolean(L, info.dlss_card);
    lua_setfield(L, -2, "dlss_card");
    set_string(L, "dlss_card_why", info.dlss_card_why);
    lua_pushboolean(L, info.dlss5);
    lua_setfield(L, -2, "dlss5");
    set_string(L, "dlss5_why", info.dlss5_why);
    set_string(L, "fsr", info.fsr);
    lua_pushboolean(L, info.fsr_card);
    lua_setfield(L, -2, "fsr_card");
    set_string(L, "fsr_card_why", info.fsr_card_why);
    set_string(L, "upscaler", info.upscaler);
    lua_pushinteger(L, info.scene_width);
    lua_setfield(L, -2, "scene_width");
    lua_pushinteger(L, info.scene_height);
    lua_setfield(L, -2, "scene_height");
    lua_pushboolean(L, info.dlss_sized);
    lua_setfield(L, -2, "dlss_sized");
    set_string(L, "stopped", info.stopped);
    lua_pushboolean(L, info.d3d12_unsupported);
    lua_setfield(L, -2, "d3d12_unsupported");
    const HWND window = window::handle();
    MONITORINFO monitor{sizeof monitor};
    if (GetMonitorInfoW(window != nullptr ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)
                                          : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY),
                        &monitor)) {
        lua_pushinteger(L, monitor.rcMonitor.right - monitor.rcMonitor.left);
        lua_setfield(L, -2, "monitor_width");
        lua_pushinteger(L, monitor.rcMonitor.bottom - monitor.rcMonitor.top);
        lua_setfield(L, -2, "monitor_height");
        lua_pushinteger(L, monitor.rcWork.right - monitor.rcWork.left);
        lua_setfield(L, -2, "work_width");
        lua_pushinteger(L, monitor.rcWork.bottom - monitor.rcWork.top);
        lua_setfield(L, -2, "work_height");
    }
    if (long fit_w = 0, fit_h = 0; uiscale::fit_size(100, fit_w, fit_h)) {
        lua_pushinteger(L, fit_w);
        lua_setfield(L, -2, "ui_fit_width");
        lua_pushinteger(L, fit_h);
        lua_setfield(L, -2, "ui_fit_height");
    }
    return 1;
}

// graphics.hardware(): the graphics card and the CPU as they are now (hwmon.hpp: NVIDIA's NVML in the helper, Windows'
// power state; polled once a second while something asks). The call holds the readings for 5 s, so a page that shows them
// reads them each half second and they stop once it's left. `gpu` false: `gpu_why` says why (an AMD card: NVML only).
int graphics_hardware(lua_State* L)
{
    hwmon::touch();
    const hwmon::Reading r = hwmon::latest();
    const auto number = [L](const char* name, double value) {
        lua_pushnumber(L, value);
        lua_setfield(L, -2, name);
    };
    const auto boolean = [L](const char* name, bool value) {
        lua_pushboolean(L, value);
        lua_setfield(L, -2, name);
    };
    lua_createtable(L, 0, 27);
    boolean("gpu", r.gpu);
    set_string(L, "gpu_why", r.gpu_why);
    number("gpu_mhz", r.gpu_mhz);
    number("gpu_max_mhz", r.gpu_max_mhz);
    number("gpu_temp", r.gpu_temp);
    number("gpu_watts", r.gpu_watts);
    number("gpu_limit_watts", r.gpu_limit_watts);
    number("gpu_busy", r.gpu_busy);
    number("vram_used_mb", r.vram_used_mb);
    number("vram_total_mb", r.vram_total_mb);
    number("vram_game_mb", r.vram_game_mb);
    number("vram_budget_mb", r.vram_budget_mb);
    boolean("throttled", r.throttled);
    set_string(L, "throttle_text", r.throttle_text);
    boolean("cpu", r.cpu);
    number("cpu_mhz", r.cpu_mhz);
    number("cpu_max_mhz", r.cpu_max_mhz);
    number("cpu_limit_mhz", r.cpu_limit_mhz);
    if (r.cpu_load) {
        number("cpu_busy", r.cpu_busy);
        number("cpu_game", r.cpu_game);
    }
    number("ram_game_mb", r.ram_game_mb);
    number("ram_used_mb", r.ram_used_mb);
    number("ram_total_mb", r.ram_total_mb);
    boolean("power", r.power);
    boolean("ac", r.ac);
    number("battery", r.battery);
    set_string(L, "power_plan", r.power_plan);
    set_string(L, "power_mode", r.power_mode);
    return 1;
}

// graphics.reload(): applies vanacore.ini's live graphics settings now (after config.set).
// graphics.dlss_indicator(): whether NVIDIA's DLSS indicator is on (machine-wide); dlss_indicator(on) sets it,
// through Windows' administrator prompt (graphics::set_dlss_indicator).
int graphics_dlss_indicator(lua_State* L)
{
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1))
        graphics::set_dlss_indicator(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, graphics::dlss_indicator());
    return 1;
}

int graphics_reload(lua_State*)
{
    graphics::reload();
    return 0;
}

// graphics.damage_test(kind): numbers VanaCore makes over the player to show how damage numbers are placed ([graphics]
// damage.spawn, .offset.*, the colours, icon and skill name): kind "hit" (one), "round" (three hits of a weapon skill at
// once) or "barrage" (nine hits over two seconds). Returns whether the kind is known.
int graphics_damage_test(lua_State* L)
{
    const std::string kind = luaL_checkstring(L, 1);
    if (kind == "hit")
        damagetext::test(damagetext::TestKind::Hit);
    else if (kind == "round")
        damagetext::test(damagetext::TestKind::Round);
    else if (kind == "barrage")
        damagetext::test(damagetext::TestKind::Barrage);
    else {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// graphics.retry(): DLSS (or FSR) that stopped (info().stopped) tried again, as switching it off and on does.
int graphics_retry(lua_State*)
{
    graphics::retry();
    return 0;
}

// graphics.recompile(): the scene effects' shaders compiled again from the end of this frame (Client > VanaCore >
// Recompile shaders); true, or false without VanaCore's renderer
int graphics_recompile(lua_State* L)
{
    lua_pushboolean(L, graphics::recompile_effects() ? 1 : 0);
    return 1;
}

// graphics.grass_assets(): Mass Grass's catalogue, { { id =, name =, kind =, shipped =, picture = }, ... } in the window's order (the drawn
// blades first). graphics.grass_capture(name): starts capturing the plants round the player into the library; true, or false and why.
// graphics.grass_capture_status(): what the last capture did, once, or nil. graphics.grass_reload(): the catalogue read again, and
// the zones' dirt levels (data\grass_dirt.json), the grass given its settings again with them.
int graphics_grass_assets(lua_State* L)
{
    const std::vector<grass_assets::Info> list = grass_assets::list();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        lua_createtable(L, 0, 5);
        lua_pushstring(L, list[i].id.c_str());
        lua_setfield(L, -2, "id");
        lua_pushstring(L, list[i].name.c_str());
        lua_setfield(L, -2, "name");
        lua_pushstring(L, list[i].kind.c_str());
        lua_setfield(L, -2, "kind");
        lua_pushboolean(L, list[i].shipped ? 1 : 0);
        lua_setfield(L, -2, "shipped");
        lua_pushboolean(L, list[i].picture ? 1 : 0);
        lua_setfield(L, -2, "picture");
        lua_pushnumber(L, list[i].amount);
        lua_setfield(L, -2, "default");
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// graphics.grass_solo([id]): with an id, that library entry grown alone, whole (nil or "": the mix again), never saved; returns
// the one shown alone, or nil
// graphics.grass_pathpad(): the path padding the zone you're in has of its own (data\grass_dirt.json's "pathpad" for it, yalms),
// or nil when it takes [graphics] grass.pathpad (/vana's Path padding, locked while the zone has its own)
int graphics_grass_pathpad(lua_State* L)
{
    float own = 0.0f;
    if (grass_assets::pathpad_own(own))
        lua_pushnumber(L, own);
    else
        lua_pushnil(L);
    return 1;
}

int graphics_grass_solo(lua_State* L)
{
    if (lua_gettop(L) >= 1)
        grass_assets::solo(lua_isstring(L, 1) ? lua_tostring(L, 1) : "");
    const std::string id = grass_assets::solo_id();
    if (id.empty())
        lua_pushnil(L);
    else
        lua_pushstring(L, id.c_str());
    return 1;
}

// graphics.dlss_crash_test(kind): the DLSS helper fails at its next frame as kind says - 'crash' (an access violation),
// 'thread' (one on another thread), 'cpp' (an unhandled C++ exception), 'fastfail' (Windows ends it on the spot) or 'hang'
// (it stops answering) - to try what a real crash leaves (the /vana debug window's DLSS crash tests). True, or false and why
int graphics_dlss_crash_test(lua_State* L)
{
    static constexpr const char* kKinds[] = {"crash", "thread", "cpp", "fastfail", "hang"};
    const char* kind = luaL_checkstring(L, 1);
    int code = 0;
    for (int i = 0; i < 5; ++i)
        if (std::strcmp(kind, kKinds[i]) == 0)
            code = i + 1;
    if (code == 0)
        return luaL_error(L, "graphics.dlss_crash_test: '%s' isn't crash, thread, cpp, fastfail or hang", kind);
    std::string why;
    const bool ok = graphics::dlss_crash_test(code, why);
    lua_pushboolean(L, ok ? 1 : 0);
    lua_pushstring(L, why.c_str());
    return 2;
}

int graphics_grass_capture(lua_State* L)
{
    std::string message;
    const bool ok = grass_assets::capture(luaL_optstring(L, 1, ""), message);
    lua_pushboolean(L, ok ? 1 : 0);
    lua_pushstring(L, message.c_str());
    return 2;
}

int graphics_grass_capture_status(lua_State* L)
{
    const std::string s = grass_assets::take_status();
    if (s.empty())
        lua_pushnil(L);
    else
        lua_pushstring(L, s.c_str());
    return 1;
}

int graphics_grass_reload(lua_State*)
{
    grass_assets::reload();
    graphics::reload();
    return 0;
}

// camera.preview(key, value): a [camera] setting as the /vana window's slider is dragged, applied without
// vanacore.ini (the window writes it when the slider is let go)
int camera_preview(lua_State* L)
{
    camera::preview(luaL_checkstring(L, 1), static_cast<float>(luaL_checknumber(L, 2)));
    return 0;
}

// camera.free([on]): the /freecam camera on or off, the other way with nothing. true if it's on now; false, and why, if it couldn't
// come on
int camera_free(lua_State* L)
{
    const int on = camera::free_cam(lua_isnoneornil(L, 1) ? -1 : lua_toboolean(L, 1) != 0 ? 1 : 0);
    lua_pushboolean(L, on == 1);
    if (on >= 0)
        return 1;
    lua_pushstring(L, on == -1 ? "the game's camera view wasn't found" : "no character is in the world");
    return 2;
}

// camera.float([on]): /freecam float; the floaty free camera on or off (written to [camera] freecam.float), the other way with
// nothing. Returns whether it's on
int camera_float(lua_State* L)
{
    lua_pushboolean(L, camera::free_cam_float(lua_isnoneornil(L, 1) ? -1 : lua_toboolean(L, 1) != 0 ? 1 : 0) == 1);
    return 1;
}

// camera.state(): { on, have, hold, float, lag, speed } (camera.hpp's FlyState)
int camera_state(lua_State* L)
{
    const camera::FlyState state = camera::fly_state();
    lua_createtable(L, 0, 6);
    const std::pair<const char*, bool> flags[] = {{"on", state.on}, {"have", state.have}, {"hold", state.hold}, {"float", state.floaty}};
    for (const auto& [name, value] : flags) {
        lua_pushboolean(L, value);
        lua_setfield(L, -2, name);
    }
    lua_pushnumber(L, state.lag);
    lua_setfield(L, -2, "lag");
    lua_pushnumber(L, state.speed);
    lua_setfield(L, -2, "speed");
    return 1;
}

// camera.pose(): x, y, z, yaw, tilt (radians) of the free camera, or nil when it isn't on / hasn't taken its pose yet
int camera_pose(lua_State* L)
{
    float pose[5];
    if (!camera::fly_pose(pose)) {
        lua_pushnil(L);
        return 1;
    }
    for (const float value : pose)
        lua_pushnumber(L, value);
    return 5;
}

// camera.set_pose(x, y, z, yaw, tilt): the free camera put there; true if it was
int camera_set_pose(lua_State* L)
{
    float pose[5];
    for (int i = 0; i < 5; ++i)
        pose[i] = static_cast<float>(luaL_checknumber(L, i + 1));
    lua_pushboolean(L, camera::fly_set_pose(pose));
    return 1;
}

// camera.hold([on]): the free camera's keys, mouse and wheel move nothing while it's held (default on), so a pose set each frame
// stays where it was set
int camera_hold(lua_State* L)
{
    camera::fly_hold(lua_isnoneornil(L, 1) || lua_toboolean(L, 1) != 0);
    return 0;
}

// camera.speed([units a second]): the free camera's speed, set if given; returns it
int camera_speed(lua_State* L)
{
    lua_pushnumber(L, camera::fly_speed(lua_isnoneornil(L, 1) ? 0.0f : static_cast<float>(luaL_checknumber(L, 1))));
    return 1;
}

// time.clock(): seconds since an arbitrary start, to the microsecond.
int clock(lua_State* L)
{
    static LARGE_INTEGER frequency = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    lua_pushnumber(L, static_cast<lua_Number>(now.QuadPart) / static_cast<lua_Number>(frequency.QuadPart));
    return 1;
}

// system.open_url(url): opens it in the default browser (http and https only).
int open_url(lua_State* L)
{
    const char* url = luaL_checkstring(L, 1);
    const bool web = _strnicmp(url, "http://", 7) == 0 || _strnicmp(url, "https://", 8) == 0;
    lua_pushboolean(L, web && reinterpret_cast<INT_PTR>(ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL)) > 32);
    return 1;
}

// system.play_sound(path): plays a .wav file without waiting for it.
int play_sound(lua_State* L)
{
    const char* path = luaL_checkstring(L, 1);
    lua_pushboolean(L, PlaySoundA(path, nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT));
    return 1;
}

// system.clipboard(): the clipboard's text, in the game's Shift-JIS ('' when it has none).
int get_clipboard(lua_State* L)
{
    const std::string text = clipboard::get_text(window::handle());
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// system.set_clipboard(text): puts Shift-JIS text on the clipboard. True if it did.
int set_clipboard(lua_State* L)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    lua_pushboolean(L, clipboard::set_text(window::handle(), std::string(text, length)));
    return 1;
}

// system.focused(): true while the game isn't in the background (its window, or one of its process's, is in front).
int focused(lua_State* L)
{
    lua_pushboolean(L, window::handle() != nullptr && !window::in_background());
    return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.ui: text boxes and filled rectangles on the screen (a rectangle is a box drawn as its fill).

struct BoxHandle {
    int id;
};

ui::Box* to_box(lua_State* L, int index)
{
    const auto* handle = static_cast<const BoxHandle*>(luaL_checkudata(L, index, kBoxType));
    return ui::find(handle->id);
}

bool read_colour(lua_State* L, int index, float (&colour)[4])
{
    if (lua_type(L, index) == LUA_TNUMBER) {
        text::argb_colour(static_cast<std::uint32_t>(static_cast<std::int64_t>(lua_tonumber(L, index))), colour);
        return true;
    }
    return lua_type(L, index) == LUA_TSTRING && text::parse_colour(widen(lua_tostring(L, index)), colour);
}

// A text box's effects (ui.hpp): the picture drawn as a wave (`wave` the swing in pixels, `wave_length` crest to crest in
// pixels, `wave_phase` in radians) and dissolved (`dissolve` 0 to 1, `dissolve_edge` the burning edge's colour). `is_wave` is
// whether `key` is one of them; returns what's wrong, or nullptr.
const char* wave_property(lua_State* L, ui::Box& box, const char* key, int index, bool& is_wave)
{
    is_wave = !box.rect && !box.image &&
              (std::strcmp(key, "wave") == 0 || std::strcmp(key, "wave_length") == 0 || std::strcmp(key, "wave_phase") == 0 ||
               std::strcmp(key, "dissolve") == 0 || std::strcmp(key, "dissolve_edge") == 0);
    if (!is_wave)
        return nullptr;
    if (std::strcmp(key, "dissolve_edge") == 0) {
        // opaque unless the colour says otherwise: "#RRGGBB" has no alpha of its own, and the edge's starts at none (no edge)
        float edge[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        if (!read_colour(L, index, edge))
            return "expected \"#RRGGBB\", \"#RRGGBBAA\" or 0xAARRGGBB";
        std::memcpy(box.dissolve_edge, edge, sizeof edge);
        return nullptr;
    }
    if (lua_type(L, index) != LUA_TNUMBER)
        return "expected a number";
    const auto value = static_cast<float>(lua_tonumber(L, index));
    if (std::strcmp(key, "wave") == 0)
        box.wave = std::clamp(value, -1000.0f, 1000.0f);
    else if (std::strcmp(key, "wave_length") == 0)
        box.wave_length = std::clamp(value, 0.0f, 100000.0f);
    else if (std::strcmp(key, "dissolve") == 0)
        box.dissolve = std::clamp(value, 0.0f, 1.0f);
    else
        box.wave_phase = value;
    return nullptr;
}

#include "lua_api_gradient.h" // gradient_property: a text box's gradient, gradient_from and gradient_to

// Sets property `key` of `box` to the value at `index`. Returns what's wrong, or nullptr.
const char* set_property(lua_State* L, ui::Box& box, const char* key, int index)
{
    const auto number = [&](int& field, int low, int high) -> const char* {
        if (lua_type(L, index) != LUA_TNUMBER)
            return "expected a number";
        const auto value = static_cast<int>(lua_tonumber(L, index));
        field = value < low ? low : value > high ? high : value;
        return nullptr;
    };
    const auto flag = [&](bool& field) -> const char* {
        if (lua_type(L, index) != LUA_TBOOLEAN && lua_type(L, index) != LUA_TNUMBER)
            return "expected true or false";
        field = lua_type(L, index) == LUA_TBOOLEAN ? lua_toboolean(L, index) != 0 : lua_tonumber(L, index) != 0;
        return nullptr;
    };
    if (std::strcmp(key, "text") == 0) {
        if (lua_type(L, index) != LUA_TSTRING && lua_type(L, index) != LUA_TNUMBER)
            return "expected a string";
        box.text = widen(lua_tostring(L, index));
        return nullptr;
    }
    if (std::strcmp(key, "file") == 0) {
        if (!box.image)
            return "only a vana.ui.image has a file";
        if (lua_type(L, index) != LUA_TSTRING)
            return "expected a file name";
        // relative to the addon's folder, or a full path
        std::wstring file = widen(lua_tostring(L, index));
        const bool full = file.size() > 2 && (file[1] == L':' || (file[0] == L'\\' && file[1] == L'\\'));
        if (!full) {
            const std::string& folder = context(L).folder;
            const int n = MultiByteToWideChar(CP_ACP, 0, folder.c_str(), static_cast<int>(folder.size()), nullptr, 0);
            std::wstring base(static_cast<std::size_t>(n), L'\0');
            MultiByteToWideChar(CP_ACP, 0, folder.c_str(), static_cast<int>(folder.size()), base.data(), n);
            file = base + file;
        }
        box.file = file;
        return nullptr;
    }
    if (std::strcmp(key, "width") == 0 || std::strcmp(key, "height") == 0) {
        if (!box.rect && !box.image)
            return "a text box's size follows its text (only a vana.ui.rect's and a vana.ui.image's are set)";
        return number(key[0] == 'w' ? box.width : box.height, 0, 100000);
    }
    if (box.rect && std::strcmp(key, "radius") == 0)
        return number(box.radius, 0, 100000);
    if (box.image && (std::strcmp(key, "repeat_x") == 0 || std::strcmp(key, "repeat_y") == 0))
        return number(key[7] == 'x' ? box.repeat_x : box.repeat_y, 1, 1000);
    if ((box.rect || box.image) && (std::strcmp(key, "color") == 0 || std::strcmp(key, "colour") == 0))
        return read_colour(L, index, box.style.background) ? nullptr : "expected \"#RRGGBB\", \"#RRGGBBAA\" or 0xAARRGGBB";
    if (std::strcmp(key, "x") == 0)
        return number(box.x, -100000, 100000);
    if (std::strcmp(key, "y") == 0)
        return number(box.y, -100000, 100000);
    if (std::strcmp(key, "size") == 0)
        return number(box.style.size, 6, 128);
    if (std::strcmp(key, "padding") == 0)
        return number(box.style.padding, 0, 200);
    if (std::strcmp(key, "visible") == 0)
        return flag(box.visible);
    if (std::strcmp(key, "opacity") == 0) {
        if (lua_type(L, index) != LUA_TNUMBER)
            return "expected a number, 0 to 1";
        const double opacity = lua_tonumber(L, index);
        box.opacity = opacity < 0.0 ? 0.0f : opacity > 1.0 ? 1.0f : static_cast<float>(opacity);
        return nullptr;
    }
    bool is_wave = false;
    if (const char* problem = wave_property(L, box, key, index, is_wave); is_wave)
        return problem;
    bool is_gradient = false;
    if (const char* problem = gradient_property(L, box, key, index, is_gradient); is_gradient)
        return problem;
    if (std::strcmp(key, "block_mouse") == 0)
        return flag(box.block_mouse);
    if (std::strcmp(key, "layer") == 0)
        return number(box.layer, -100, 100);
    if (std::strcmp(key, "outline") == 0) {
        bool outline = box.style.stroke > 0;
        const char* problem = flag(outline);
        box.style.stroke = outline ? (box.style.stroke > 0 ? box.style.stroke : 1) : 0;
        return problem;
    }
    if (std::strcmp(key, "stroke") == 0)
        return number(box.style.stroke, 0, 8);
    if (std::strcmp(key, "stroke_color") == 0 || std::strcmp(key, "stroke_colour") == 0)
        return read_colour(L, index, box.style.stroke_color) ? nullptr : "expected \"#RRGGBB\", \"#RRGGBBAA\" or 0xAARRGGBB";
    if (std::strcmp(key, "italic") == 0)
        return flag(box.style.italic);
    if (std::strcmp(key, "right") == 0)
        return flag(box.right);
    if (std::strcmp(key, "color_codes") == 0)
        return flag(box.style.color_codes);
    if (std::strcmp(key, "bold") == 0) {
        bool bold = box.style.weight >= FW_BOLD;
        const char* problem = flag(bold);
        box.style.weight = bold ? FW_BOLD : FW_NORMAL;
        return problem;
    }
    if (std::strcmp(key, "font") == 0) {
        if (lua_type(L, index) != LUA_TSTRING)
            return "expected a font name";
        box.style.family = widen(lua_tostring(L, index));
        return nullptr;
    }
    if (std::strcmp(key, "color") == 0 || std::strcmp(key, "colour") == 0)
        return read_colour(L, index, box.style.color) ? nullptr : "expected \"#RRGGBB\", \"#RRGGBBAA\" or 0xAARRGGBB";
    if (std::strcmp(key, "background") == 0)
        return read_colour(L, index, box.style.background) ? nullptr : "expected \"#RRGGBB\", \"#RRGGBBAA\" or 0xAARRGGBB";
    if (box.rect)
        return "not a property of a rectangle (x, y, width, height, color, visible, opacity, block_mouse, layer)";
    if (box.image)
        return "not a property of an image (file, x, y, width, height, right, visible, opacity, block_mouse, layer)";
    return "not a property of a text box (text, x, y, right, visible, opacity, font, size, bold, italic, color, background, "
           "outline, stroke, stroke_color, padding, color_codes, gradient, gradient_from, gradient_to, wave, wave_length, wave_phase, "
           "dissolve, dissolve_edge, block_mouse, layer)";
}

// Applies every string key of the table at `index` to `box`. Returns what's wrong (and copies the
// key into `bad_key`), or nullptr.
const char* set_properties(lua_State* L, ui::Box& box, int index, char (&bad_key)[64])
{
    const char* problem = nullptr;
    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            problem = set_property(L, box, lua_tostring(L, -2), lua_gettop(L));
            if (problem != nullptr) {
                strncpy_s(bad_key, lua_tostring(L, -2), _TRUNCATE);
                lua_pop(L, 2);
                return problem;
            }
        }
        lua_pop(L, 1);
    }
    return nullptr;
}

// ui.text([properties]): a new text box, shown until destroyed or the addon unloads.
int ui_text(lua_State* L)
{
    if (!lua_isnoneornil(L, 1))
        luaL_checktype(L, 1, LUA_TTABLE);
    ui::Box& box = ui::create(context(L).name);
    box.x = box.y = 100;
    char bad_key[64] = {};
    const char* problem = lua_istable(L, 1) ? set_properties(L, box, 1, bad_key) : nullptr;
    if (problem != nullptr) {
        ui::destroy(box.id);
        return luaL_error(L, "vana.ui.text: %s: %s", bad_key, problem);
    }
    auto* handle = static_cast<BoxHandle*>(lua_newuserdata(L, sizeof(BoxHandle)));
    handle->id = box.id;
    luaL_getmetatable(L, kBoxType);
    lua_setmetatable(L, -2);
    return 1;
}

// ui.rect([properties]): a new filled rectangle (x, y, width, height, color, radius, visible), drawn with the
// text boxes in the order they were made, until destroyed or the addon unloads.
int ui_rect(lua_State* L)
{
    if (!lua_isnoneornil(L, 1))
        luaL_checktype(L, 1, LUA_TTABLE);
    ui::Box& box = ui::create(context(L).name);
    box.rect = true;
    box.x = box.y = 100;
    box.width = 100;
    box.height = 30;
    char bad_key[64] = {};
    const char* problem = lua_istable(L, 1) ? set_properties(L, box, 1, bad_key) : nullptr;
    if (problem != nullptr) {
        ui::destroy(box.id);
        return luaL_error(L, "vana.ui.rect: %s: %s", bad_key, problem);
    }
    auto* handle = static_cast<BoxHandle*>(lua_newuserdata(L, sizeof(BoxHandle)));
    handle->id = box.id;
    luaL_getmetatable(L, kBoxType);
    lua_setmetatable(L, -2);
    return 1;
}

// ui.image({ file = 'logo.png', x =, y =, width =, height = }): a picture (PNG, JPEG, BMP: what Windows reads), the file relative
// to the addon's folder, scaled smoothly to width x height (0 or left out: its own size; one of them: its shape kept), drawn with
// the other boxes in the order they were made, until destroyed or the addon unloads. A file that can't be read draws nothing.
// `color` tints it (its colour times the picture's, its opacity the picture's); `repeat_x` and `repeat_y` tile it that many times
// across its size.
int ui_image(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    ui::Box& box = ui::create(context(L).name);
    box.image = true;
    box.x = box.y = 100;
    box.style.background[0] = box.style.background[1] = box.style.background[2] = 255.0f; // untinted, opaque
    box.style.background[3] = 1.0f;
    char bad_key[64] = {};
    const char* problem = set_properties(L, box, 1, bad_key);
    if (problem != nullptr) {
        ui::destroy(box.id);
        return luaL_error(L, "vana.ui.image: %s: %s", bad_key, problem);
    }
    auto* handle = static_cast<BoxHandle*>(lua_newuserdata(L, sizeof(BoxHandle)));
    handle->id = box.id;
    luaL_getmetatable(L, kBoxType);
    lua_setmetatable(L, -2);
    return 1;
}

int box_destroy(lua_State* L)
{
    const auto* handle = static_cast<const BoxHandle*>(luaL_checkudata(L, 1, kBoxType));
    ui::destroy(handle->id);
    return 0;
}

int box_show(lua_State* L)
{
    if (ui::Box* box = to_box(L, 1))
        box->visible = true;
    return 0;
}

int box_hide(lua_State* L)
{
    if (ui::Box* box = to_box(L, 1))
        box->visible = false;
    return 0;
}

// box:set(properties): several properties at once.
int box_set(lua_State* L)
{
    ui::Box* box = to_box(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    if (box == nullptr)
        return luaL_error(L, "the text box was destroyed");
    char bad_key[64] = {};
    const char* problem = set_properties(L, *box, 2, bad_key);
    if (problem != nullptr)
        return luaL_error(L, "%s: %s", bad_key, problem);
    return 0;
}

void push_colour(lua_State* L, const float (&colour)[4])
{
    const std::string value = narrow(text::hex(colour));
    lua_pushlstring(L, value.data(), value.size());
}

// box.property, or box:method.
int box_index(lua_State* L)
{
    ui::Box* box = to_box(L, 1);
    const char* key = luaL_checkstring(L, 2);
    lua_pushvalue(L, 2);
    lua_rawget(L, lua_upvalueindex(1)); // methods
    if (!lua_isnil(L, -1))
        return 1;
    lua_pop(L, 1);
    if (std::strcmp(key, "destroyed") == 0) {
        lua_pushboolean(L, box == nullptr);
        return 1;
    }
    if (box == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    if (std::strcmp(key, "text") == 0) {
        const std::string value = narrow(box->text);
        lua_pushlstring(L, value.data(), value.size());
    } else if (std::strcmp(key, "x") == 0) {
        lua_pushnumber(L, box->x);
    } else if (std::strcmp(key, "y") == 0) {
        lua_pushnumber(L, box->y);
    } else if (std::strcmp(key, "width") == 0 || std::strcmp(key, "height") == 0) {
        if (box->rect) {
            lua_pushnumber(L, key[0] == 'w' ? box->width : box->height);
            return 1;
        }
        ui::refresh(*box);
        lua_pushnumber(L, key[0] == 'w' ? box->sprite.width() : box->sprite.height());
    } else if (std::strcmp(key, "radius") == 0) {
        lua_pushnumber(L, box->radius);
    } else if (std::strcmp(key, "visible") == 0) {
        lua_pushboolean(L, box->visible);
    } else if (std::strcmp(key, "opacity") == 0) {
        lua_pushnumber(L, box->opacity);
    } else if (std::strcmp(key, "block_mouse") == 0) {
        lua_pushboolean(L, box->block_mouse);
    } else if (std::strcmp(key, "layer") == 0) {
        lua_pushnumber(L, box->layer);
    } else if (std::strcmp(key, "font") == 0) {
        const std::string value = narrow(box->style.family);
        lua_pushlstring(L, value.data(), value.size());
    } else if (std::strcmp(key, "size") == 0) {
        lua_pushnumber(L, box->style.size);
    } else if (std::strcmp(key, "padding") == 0) {
        lua_pushnumber(L, box->style.padding);
    } else if (std::strcmp(key, "bold") == 0) {
        lua_pushboolean(L, box->style.weight >= FW_BOLD);
    } else if (std::strcmp(key, "outline") == 0) {
        lua_pushboolean(L, box->style.stroke > 0);
    } else if (std::strcmp(key, "stroke") == 0) {
        lua_pushnumber(L, box->style.stroke);
    } else if (std::strcmp(key, "stroke_color") == 0 || std::strcmp(key, "stroke_colour") == 0) {
        push_colour(L, box->style.stroke_color);
    } else if (std::strcmp(key, "italic") == 0) {
        lua_pushboolean(L, box->style.italic);
    } else if (std::strcmp(key, "right") == 0) {
        lua_pushboolean(L, box->right);
    } else if (std::strcmp(key, "color_codes") == 0) {
        lua_pushboolean(L, box->style.color_codes);
    } else if (std::strcmp(key, "color") == 0 || std::strcmp(key, "colour") == 0) {
        push_colour(L, box->rect ? box->style.background : box->style.color);
    } else if (std::strcmp(key, "background") == 0) {
        push_colour(L, box->style.background);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// box.property = value
int box_newindex(lua_State* L)
{
    ui::Box* box = to_box(L, 1);
    const char* key = luaL_checkstring(L, 2);
    if (box == nullptr)
        return luaL_error(L, "the text box was destroyed");
    const char* problem = set_property(L, *box, key, 3);
    if (problem != nullptr)
        return luaL_error(L, "%s: %s", key, problem);
    return 0;
}

int box_tostring(lua_State* L)
{
    const auto* handle = static_cast<const BoxHandle*>(luaL_checkudata(L, 1, kBoxType));
    const ui::Box* box = ui::find(handle->id);
    lua_pushfstring(L, box != nullptr && box->rect ? "vana.ui.rect %d" : "vana.ui.text box %d", handle->id);
    return 1;
}

// ui.screen(): the back buffer's width and height (0, 0 before the first frame).
int ui_screen(lua_State* L)
{
    unsigned width = 0, height = 0;
    ui::screen(width, height);
    lua_pushnumber(L, width);
    lua_pushnumber(L, height);
    return 2;
}

// ui.game_on_top([on]): the game's own windows (menus, shops, item details, the macro bar, ...) are drawn over this addon's
// ImGui windows where they meet them (on unless false), until it says false or unloads. [client] menus.ontop=0 turns it off.
int ui_game_on_top(lua_State* L)
{
    const Context& context = lua_api::context(L);
    raise::game_on_top(gameui::key(context.source, context.name), lua_isnoneornil(L, 1) || lua_toboolean(L, 1) != 0);
    return 0;
}

// ui.interface_hidden(): true while the game's interface is hidden (F11, [ui] hide.key)
int ui_interface_hidden(lua_State* L)
{
    lua_pushboolean(L, hideui::hidden());
    return 1;
}

// ui.set_interface_hidden(on): the interface hidden or shown as F11 does (hiding works in the world only); true if it's hidden
// after
int ui_set_interface_hidden(lua_State* L)
{
    lua_pushboolean(L, hideui::set_hidden(lua_toboolean(L, 1) != 0));
    return 1;
}

// ui.icon(name): an icon font's character for `name` ("undo", "close": fonts.google.com/icons' names)
// as text, and the font to draw it with ("Material Symbols Outlined"); nil when there's no such icon.
// ui.text({ text = text, font = font, bold = false }) draws it.
int ui_icon(lua_State* L)
{
    std::size_t length = 0;
    const char* name = luaL_checklstring(L, 1, &length);
    std::wstring family;
    const char32_t c = fonts::icon(std::string_view(name, length), &family);
    if (c == 0) {
        lua_pushnil(L);
        return 1;
    }
    std::wstring wide;
    if (c >= 0x10000)
        wide = {static_cast<wchar_t>(0xD800 + ((c - 0x10000) >> 10)), static_cast<wchar_t>(0xDC00 + ((c - 0x10000) & 0x3FF))};
    else
        wide = {static_cast<wchar_t>(c)};
    const std::string text = narrow(wide), font = narrow(family);
    lua_pushlstring(L, text.data(), text.size());
    lua_pushlstring(L, font.data(), font.size());
    return 2;
}

// ui.add_font([path]): a font file (.ttf, .otf, .ttc, .woff, .woff2) copied into VanaCore's fonts folder, where it loads at
// every start, and loaded now: its family, to draw text with. With no path, Windows' Open dialog asks for one first (the
// game's thread waits in it). nil and why when it can't be used; nil alone when the dialog is cancelled.
int ui_add_font(lua_State* L)
{
    std::wstring path = lua_isnoneornil(L, 1) ? std::wstring() : widen(luaL_checkstring(L, 1));
    if (path.empty()) {
        wchar_t picked[MAX_PATH * 2] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof dialog;
        dialog.hwndOwner = GetForegroundWindow();
        dialog.lpstrFilter = L"Fonts (*.ttf, *.otf, *.ttc, *.woff, *.woff2)\0*.ttf;*.otf;*.ttc;*.woff;*.woff2\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = picked;
        dialog.nMaxFile = static_cast<DWORD>(std::size(picked));
        dialog.lpstrTitle = L"Choose a font";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog))
            return 0;
        path = picked;
    }
    std::wstring family, why;
    const bool added = fonts::add(path, family, why);
    const std::string text = narrow(added ? family : why);
    if (!added)
        lua_pushnil(L);
    lua_pushlstring(L, text.data(), text.size());
    return added ? 1 : 2;
}

// ui.add_image([path]): a picture (.png, .jpg, .bmp, .gif) copied into VanaCore's images folder: its path from VanaCore's
// folder (images\<name>), for [client] window.image. With no path, Windows' Open dialog asks for one first (the game's thread
// waits in it). nil and why when it can't be used; nil alone when the dialog is cancelled.
int ui_add_image(lua_State* L)
{
    std::wstring path = lua_isnoneornil(L, 1) ? std::wstring() : widen(luaL_checkstring(L, 1));
    if (path.empty()) {
        wchar_t picked[MAX_PATH * 2] = {};
        OPENFILENAMEW dialog{};
        dialog.lStructSize = sizeof dialog;
        dialog.hwndOwner = GetForegroundWindow();
        dialog.lpstrFilter = L"Pictures (*.png, *.jpg, *.jpeg, *.bmp, *.gif)\0*.png;*.jpg;*.jpeg;*.bmp;*.gif\0All files (*.*)\0*.*\0";
        dialog.lpstrFile = picked;
        dialog.nMaxFile = static_cast<DWORD>(std::size(picked));
        dialog.lpstrTitle = L"Choose a picture for the windows";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog))
            return 0;
        path = picked;
    }
    std::wstring kept, why;
    const bool added = winimage::add(path, kept, why);
    const std::string text = narrow(added ? kept : why);
    if (!added)
        lua_pushnil(L);
    lua_pushlstring(L, text.data(), text.size());
    return added ? 1 : 2;
}

// ui.fonts(): the families of the fonts in VanaCore's fonts folder, in its order (icon fonts left out)
int ui_fonts(lua_State* L)
{
    const std::vector<std::wstring> list = fonts::families();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    for (std::size_t i = 0; i < list.size(); ++i) {
        const std::string text = narrow(list[i]);
        lua_pushlstring(L, text.data(), text.size());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// ui.remove_font(family): a font added to VanaCore's fonts folder taken out: unloaded now, its file moved to the Recycle
// Bin, so it is in no font list from now on. true; nil and why when it can't be (an icon font, or one not added).
int ui_remove_font(lua_State* L)
{
    std::wstring why;
    if (fonts::remove(widen(luaL_checkstring(L, 1)), why)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    const std::string text = narrow(why);
    lua_pushnil(L);
    lua_pushlstring(L, text.data(), text.size());
    return 2;
}

void open_ui(lua_State* L)
{
    luaL_newmetatable(L, kBoxType);
    lua_newtable(L); // methods, the __index closure's upvalue
    set_function(L, "destroy", &box_destroy);
    set_function(L, "show", &box_show);
    set_function(L, "hide", &box_hide);
    set_function(L, "set", &box_set);
    lua_pushcclosure(L, &box_index, 1);
    lua_setfield(L, -2, "__index");
    set_function(L, "__newindex", &box_newindex);
    set_function(L, "__tostring", &box_tostring);
    lua_pop(L, 1);

    lua_newtable(L);
    set_function(L, "text", &ui_text);
    set_function(L, "rect", &ui_rect);
    set_function(L, "screen", &ui_screen);
    set_function(L, "game_on_top", &ui_game_on_top);
    set_function(L, "interface_hidden", &ui_interface_hidden);
    set_function(L, "set_interface_hidden", &ui_set_interface_hidden);
    set_function(L, "icon", &ui_icon);
    set_function(L, "add_font", &ui_add_font);
    set_function(L, "add_image", &ui_add_image);
    set_function(L, "fonts", &ui_fonts);
    set_function(L, "remove_font", &ui_remove_font);
    set_function(L, "image", &ui_image);
    backdrop::add(L); // backdrop: frosted glass behind an ImGui window
    lua_setfield(L, -2, "ui");
    widgets::push(L); // vana.world: vana.ui boxes at a nameplate or a point in the world (docs/addons/18-widgets.md)
    lua_setfield(L, -2, "world");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.game: FFXI's own data (game.hpp). Each call reads it fresh; nil when it isn't there.

void set_number(lua_State* L, const char* name, lua_Number value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}

void set_bool(lua_State* L, const char* name, bool value)
{
    lua_pushboolean(L, value);
    lua_setfield(L, -2, name);
}

// game.entity_count(): how many entity slots there are.
int game_entity_count(lua_State* L)
{
    lua_pushnumber(L, game::entity_count());
    return 1;
}

// game.party(): { members = { [1..18] = member }, leader, party_leaders = {3}, party_counts = {3} }, or nil.
int game_party(lua_State* L)
{
    bool found = false;
    {
        game::PartyMember members[18];
        game::Alliance alliance;
        found = game::party(members, alliance);
        if (found) {
            lua_createtable(L, 0, 4);
            lua_createtable(L, 18, 0);
            for (int slot = 0; slot < 18; ++slot) {
                const game::PartyMember& member = members[slot];
                lua_createtable(L, 0, 25);
                set_bool(L, "active", member.active);
                set_number(L, "slot", member.slot);
                set_string(L, "name", member.name);
                set_number(L, "id", member.id);
                set_number(L, "target_index", member.target_index);
                set_number(L, "hp", member.hp);
                set_number(L, "mp", member.mp);
                set_number(L, "tp", member.tp);
                set_number(L, "hpp", member.hpp);
                set_number(L, "mpp", member.mpp);
                set_number(L, "zone", member.zone);
                set_number(L, "main_job", member.main_job);
                set_number(L, "main_job_level", member.main_job_level);
                set_number(L, "sub_job", member.sub_job);
                set_number(L, "sub_job_level", member.sub_job_level);
                set_number(L, "flags", member.flags);
                lua_entity::set_max(L, member.id); // hp_max, mp_max and their bounds (entityhp.hpp)
                lua_rawseti(L, -2, slot + 1);
            }
            lua_setfield(L, -2, "members");
            set_number(L, "leader", alliance.leader);
            lua_createtable(L, 3, 0);
            for (int i = 0; i < 3; ++i) {
                lua_pushnumber(L, alliance.party_leaders[i]);
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, -2, "party_leaders");
            lua_createtable(L, 3, 0);
            for (int i = 0; i < 3; ++i) {
                lua_pushnumber(L, alliance.party_counts[i]);
                lua_rawseti(L, -2, i + 1);
            }
            lua_setfield(L, -2, "party_counts");
        }
    }
    if (!found)
        lua_pushnil(L);
    return 1;
}

// game.player(): the local player's jobs, stats and buffs, or nil.
// vana.game.zoning(): true between zones (the server's zone-out came, the player isn't back): Ashita's login status is 1 then
int game_zoning(lua_State* L)
{
    lua_pushboolean(L, packets::zoning(!game::local_player_name().empty()));
    return 1;
}

// vana.game.player_zone(listed): the zone the player is in, given the party list's (`listed`): the one the server's zone-in
// named until that list has it, which it gets only once the new zone has loaded
int game_player_zone(lua_State* L)
{
    lua_pushinteger(L, packets::player_zone(static_cast<std::uint16_t>(luaL_checkinteger(L, 1))));
    return 1;
}

// vana.game.zone_weathers(zone): the weathers (0x057's ids, lowest first) zone `zone`'s model DAT has a sky and light for, or
// nil when it can't be read or has none (zoneweather.cpp)
int game_zone_weathers(lua_State* L)
{
    std::uint32_t mask = 0;
    if (!zoneweather::weathers(static_cast<int>(luaL_checkinteger(L, 1)), mask)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    int n = 0;
    for (int weather = 0; weather < 20; ++weather) {
        if ((mask & (1u << weather)) != 0) {
            lua_pushinteger(L, weather);
            lua_rawseti(L, -2, ++n);
        }
    }
    return 1;
}

// vana.game.set_target(index): entity `index` targeted as the game's own call sites do it (Ashita's ITarget::SetTarget); false
// for one with no actor (out of render range)
int game_set_target(lua_State* L)
{
    lua_pushboolean(L, ashita::set_target(static_cast<std::uint32_t>(luaL_checkinteger(L, 1))));
    return 1;
}

// vana.game.world(): { server_time (Unix seconds, from the server's last packets; 0 before any), weather (the zone's,
// Windower's weather id), chat_open (the chat line is open) }
int game_world(lua_State* L)
{
    std::uint32_t server_time = 0;
    std::uint32_t weather = 0;
    windower::state_world(server_time, weather);
    bool open = false;
    std::string typed;
    chat::input_line(open, typed);
    lua_createtable(L, 0, 3);
    set_number(L, "server_time", server_time);
    set_number(L, "weather", weather);
    set_bool(L, "chat_open", open);
    return 1;
}

int game_player(lua_State* L)
{
    game::Player player;
    if (!game::player(player)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 40);
    set_number(L, "hp_max", player.hp_max);
    set_number(L, "mp_max", player.mp_max);
    set_number(L, "main_job", player.main_job);
    set_number(L, "main_job_level", player.main_job_level);
    set_number(L, "sub_job", player.sub_job);
    set_number(L, "sub_job_level", player.sub_job_level);
    set_number(L, "exp", player.exp);
    set_number(L, "exp_needed", player.exp_needed);
    set_number(L, "attack", player.attack);
    set_number(L, "defense", player.defense);
    set_number(L, "title", player.title);
    set_number(L, "nation", player.nation);
    set_number(L, "rank", player.rank);
    set_number(L, "rank_points", player.rank_points);
    set_number(L, "homepoint", player.homepoint);
    set_number(L, "residence", player.residence);
    set_number(L, "su_level", player.su_level);
    set_number(L, "limit_points", player.limit_points);
    set_number(L, "merit_points", player.merit_points);
    set_number(L, "merit_points_max", player.merit_points_max);
    set_number(L, "assimilation_points", player.assimilation_points);
    set_bool(L, "limit_breaker", player.limit_breaker);
    set_bool(L, "exp_locked", player.exp_locked);
    set_bool(L, "limit_mode", player.limit_mode);
    set_number(L, "mount_recast", player.mount_recast);
    set_number(L, "data_loaded", player.data_loaded);
    set_number(L, "sub_map", player.sub_map);
    static constexpr const char* kStats[7] = {"str", "dex", "vit", "agi", "int", "mnd", "chr"};
    lua_createtable(L, 0, 7);
    for (int i = 0; i < 7; ++i)
        set_number(L, kStats[i], player.stats[i]);
    lua_setfield(L, -2, "stats");
    lua_createtable(L, 0, 7);
    for (int i = 0; i < 7; ++i)
        set_number(L, kStats[i], player.stat_modifiers[i]);
    lua_setfield(L, -2, "stat_modifiers");
    static constexpr const char* kResists[8] = {"fire", "ice", "wind", "earth", "lightning", "water", "light", "dark"};
    lua_createtable(L, 0, 8);
    for (int i = 0; i < 8; ++i)
        set_number(L, kResists[i], player.resists[i]);
    lua_setfield(L, -2, "resists");
    lua_createtable(L, 0, 4);
    set_number(L, "highest", player.item_level_highest);
    set_number(L, "average", player.item_level);
    set_number(L, "main", player.item_level_main);
    set_number(L, "ranged", player.item_level_ranged);
    lua_setfield(L, -2, "item_level");
    lua_createtable(L, 0, 5);
    set_number(L, "faction", player.unity_faction);
    set_number(L, "points", player.unity_points);
    set_number(L, "evaluation", player.unity_evaluation);
    set_number(L, "evaluation_partial", player.unity_evaluation_partial);
    set_number(L, "chat_color", player.unity_chat_color);
    lua_setfield(L, -2, "unity");
    lua_createtable(L, 0, 5);
    set_number(L, "job", player.mastery_job);
    set_number(L, "level", player.mastery_level);
    set_number(L, "flags", player.mastery_flags);
    set_number(L, "exp", player.mastery_exp);
    set_number(L, "exp_needed", player.mastery_exp_needed);
    lua_setfield(L, -2, "mastery");
    lua_createtable(L, 32, 0);
    for (int i = 0; i < 32; ++i) {
        lua_pushnumber(L, player.buffs[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "buffs");
    lua_createtable(L, 32, 0);
    for (int i = 0; i < 32; ++i) {
        lua_pushnumber(L, player.status_icons[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "status_icons");
    lua_createtable(L, 32, 0);
    for (int i = 0; i < 32; ++i) {
        lua_pushnumber(L, player.status_timers[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "status_timers");
    lua_pushlstring(L, reinterpret_cast<const char*>(player.homepoints), sizeof(player.homepoints));
    lua_setfield(L, -2, "homepoints");
    // gil: the inventory's slot 0
    ashita::ItemSlot gil{};
    set_number(L, "gil", ashita::container_item(0, 0, gil) ? gil.count : 0);
    return 1;
}

// The game's skill ids, named as Windower's resources name them (lower case, underscores).
constexpr const char* kSkillNames[48] = {
    nullptr, "hand_to_hand", "dagger", "sword", "great_sword", "axe", "great_axe", "scythe", "polearm", "katana",
    "great_katana", "club", "staff", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
    "automaton_melee", "automaton_archery", "automaton_magic", "archery", "marksmanship", "throwing", "guard", "evasion",
    "shield", "parrying", "divine_magic", "healing_magic", "enhancing_magic", "enfeebling_magic", "elemental_magic",
    "dark_magic", "summoning_magic", "ninjutsu", "singing", "stringed_instrument", "wind_instrument", "blue_magic", "geomancy",
    "handbell", nullptr, nullptr};
constexpr const char* kCraftNames[16] = {"fishing", "woodworking", "smithing", "goldsmithing", "clothcraft", "leathercraft",
                                         "bonecraft", "alchemy", "cooking", "synergy", "riding", "digging", nullptr, nullptr,
                                         nullptr, nullptr};

// vana.game.skills(): { [skill id] = { name, skill, capped } } for the combat and magic skills, or nil before login.
int game_skills(lua_State* L)
{
    game::Player player;
    if (!game::player(player)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 48, 0);
    for (int id = 0; id < 48; ++id) {
        if (kSkillNames[id] == nullptr)
            continue;
        lua_createtable(L, 0, 3);
        set_string(L, "name", kSkillNames[id]);
        set_number(L, "skill", player.combat_skills[id] & 0x7FFF);
        set_bool(L, "capped", (player.combat_skills[id] & 0x8000) != 0);
        lua_rawseti(L, -2, id);
    }
    return 1;
}

// vana.game.crafts(): { [craft id] = { name, skill, rank, capped } } for the crafting skills (0 fishing to 11 digging), or nil.
int game_crafts(lua_State* L)
{
    game::Player player;
    if (!game::player(player)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 12, 0);
    for (int id = 0; id < 16; ++id) {
        if (kCraftNames[id] == nullptr)
            continue;
        lua_createtable(L, 0, 4);
        set_string(L, "name", kCraftNames[id]);
        set_number(L, "skill", (player.craft_skills[id] & 0x1FE0) >> 5);
        set_number(L, "rank", player.craft_skills[id] & 0x1F);
        set_bool(L, "capped", (player.craft_skills[id] & 0x8000) != 0);
        lua_rawseti(L, -2, id);
    }
    return 1;
}

// vana.game.job_points(): { [job id] = { capacity, points, spent } }, or nil before login.
int game_job_points(lua_State* L)
{
    game::Player player;
    if (!game::player(player)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 23, 0);
    for (int job = 1; job < 24; ++job) {
        lua_createtable(L, 0, 3);
        set_number(L, "capacity", player.job_points[job].capacity);
        set_number(L, "points", player.job_points[job].points);
        set_number(L, "spent", player.job_points[job].spent);
        lua_rawseti(L, -2, job);
    }
    return 1;
}

// vana.game.jobs(): { [job id] = { level, master_level } } for every job, or nil before the game can say.
int game_jobs(lua_State* L)
{
    std::uint8_t levels[24] = {}, master[24] = {};
    if (!game::job_levels(levels, master)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 23, 0);
    for (int job = 1; job < 24; ++job) {
        lua_createtable(L, 0, 2);
        set_number(L, "level", levels[job]);
        set_number(L, "master_level", master[job]);
        lua_rawseti(L, -2, job);
    }
    return 1;
}

// vana.game.pet(): { index, id, name, hpp, mpp, tp, target_id, owner_index, owner_id } for the player's pet, or nil without
// one. The percents and TP are the server's last word on them (its pet status packet); the name and id are the pet's
// entity's, when it's in range.
int game_pet(lua_State* L)
{
    game::Pet pet;
    if (!game::pet(pet) || pet.index == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 9);
    set_number(L, "index", pet.index);
    set_number(L, "hpp", pet.hpp);
    set_number(L, "mpp", pet.mpp);
    set_number(L, "tp", pet.tp);
    set_number(L, "target_id", pet.target_id);
    set_number(L, "owner_index", pet.owner_index);
    set_number(L, "owner_id", pet.owner_id);
    game::Entity entity;
    if (game::entity(pet.index, entity)) {
        set_number(L, "id", entity.id);
        set_string(L, "name", entity.name);
    }
    return 1;
}

// vana.game.missions(): the current missions as the game keeps them, or nil before the server has sent them.
int game_missions(lua_State* L)
{
    game::Missions m;
    if (!game::missions(m) || (m.loaded & 0x80000000u) == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 12);
    set_number(L, "nation", m.nation);
    set_number(L, "nation_mission", m.nation_mission);
    set_number(L, "zilart", m.zilart);
    set_number(L, "promathia", m.promathia);
    set_number(L, "promathia_progress", m.promathia_progress);
    set_number(L, "crystalline_prophecy", m.addons & 0xF);
    set_number(L, "moogle_kupo_detat", (m.addons >> 4) & 0xF);
    set_number(L, "shantotto_ascension", (m.addons >> 8) & 0xF);
    set_number(L, "adoulin", m.adoulin);
    set_number(L, "rhapsodies", m.rhapsodies);
    set_number(L, "voracious", m.voracious);
    set_number(L, "declined", m.declined);
    return 1;
}

// vana.game.linkshell(slot): slot 1 (the linkshell) or 2 (linkshell 2): its name, message and the pearl equipped for it, or
// nil when neither a message has come for it nor a pearl is equipped. The message comes from the server at login and when
// it changes; the pearl (a linkshell, pearlsack or linkpearl marked equipped in the inventory) gives the rank, colour and id.
int game_linkshell(lua_State* L)
{
    const unsigned slot = static_cast<unsigned>(luaL_optinteger(L, 1, 1));
    if (slot < 1 || slot > 2) {
        lua_pushnil(L);
        return 1;
    }
    const game::Linkshell ls = game::linkshell(slot - 1);
    const game::Linkshell other = game::linkshell(2 - slot);
    // the pearls equipped (flags 0x13), by the name packed in their extra data from byte 9 (type at 8, colour at 6, id at 0)
    struct Pearl {
        ashita::ItemSlot item;
        std::string name;
    };
    std::vector<Pearl> pearls;
    ashita::ItemSlot item{};
    for (uint32_t index = 0; index < 81 && ashita::container_item(0, index, item); ++index) {
        if (item.id >= 513 && item.id <= 515 && item.flags == 0x13)
            pearls.push_back({item, game::linkshell_name(item.extra + 9, sizeof(item.extra) - 9)});
    }
    const Pearl* pearl = nullptr;
    for (const Pearl& p : pearls) {
        if (ls.known ? p.name == ls.name : (!other.known || p.name != other.name)) {
            pearl = &p;
            break;
        }
    }
    if (!ls.known && pearl == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 12);
    set_number(L, "slot", slot);
    set_string(L, "name", ls.known ? ls.name : pearl->name);
    if (ls.known) {
        set_string(L, "message", ls.message);
        set_string(L, "poster", ls.poster);
        set_number(L, "time", ls.time);
        set_number(L, "read_level", ls.read_level);
        set_number(L, "write_level", ls.write_level);
        set_number(L, "public_edit_level", ls.public_edit_level);
    }
    if (pearl != nullptr) {
        uint32_t id = 0;
        uint16_t color = 0;
        std::memcpy(&id, pearl->item.extra, sizeof(id));
        std::memcpy(&color, pearl->item.extra + 6, sizeof(color));
        set_number(L, "rank", pearl->item.extra[8]); // 1 linkshell, 2 pearlsack, 3 linkpearl
        set_number(L, "color", color);
        set_number(L, "id", id);
        set_number(L, "item", pearl->item.index);
        set_number(L, "item_id", pearl->item.id);
    }
    return 1;
}

// vana.game.merits(): { [merit id] = { count, next } } for every merit the server has listed since login (empty before).
int game_merits(lua_State* L)
{
    const std::vector<game::Merit> list = game::merits();
    lua_createtable(L, 0, static_cast<int>(list.size()));
    for (const game::Merit& merit : list) {
        lua_createtable(L, 0, 2);
        set_number(L, "count", merit.count);
        set_number(L, "next", merit.next);
        lua_rawseti(L, -2, merit.id);
    }
    return 1;
}

// game.target(): the target window's state, or nil. vana.game.abilities(), .spells(), .key_items(): the game's bits for what the
// player has (abilities: packet 0xAC's 224 bytes, weapon skills, job abilities, pet commands, traits; spells by id; key items by
// id), as strings; nil until they've come.
int game_bits(lua_State* L, const uint8_t* bits, size_t size)
{
    if (bits == nullptr)
        lua_pushnil(L);
    else
        lua_pushlstring(L, reinterpret_cast<const char*>(bits), size);
    return 1;
}

int game_abilities(lua_State* L)
{
    return game_bits(L, ashita::ability_bits(), 0xE0);
}

int game_spells(lua_State* L)
{
    return game_bits(L, ashita::spell_bits(), 0x80);
}

int game_key_items(lua_State* L)
{
    return game_bits(L, ashita::key_item_bits(), 0x200);
}

// vana.game.recasts(): { abilities = { [timer id] = 1/60 s left, ... }, spells = { [spell id] = 1/60 s left } } (those
// counting down), or nil before the game has them.
int game_recasts(lua_State* L)
{
    const uint8_t* abilities = ashita::ability_recasts();
    uint32_t spell_count = 0;
    const uint16_t* spells = ashita::spell_recasts(&spell_count);
    if (abilities == nullptr || spells == nullptr) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 2);
    lua_newtable(L);
    for (int slot = 0; slot < 31; ++slot) {
        uint32_t timer;
        std::memcpy(&timer, abilities + 0xF8 + slot * 4, sizeof(timer));
        const uint8_t id = abilities[slot * 8 + 3];
        if (timer == 0 || (id == 0xFF && slot != 0))
            continue;
        lua_pushinteger(L, timer);
        lua_rawseti(L, -2, id);
    }
    lua_setfield(L, -2, "abilities");
    lua_newtable(L);
    for (uint32_t id = 0; id < spell_count; ++id) {
        if (spells[id] == 0)
            continue;
        lua_pushinteger(L, spells[id]);
        lua_rawseti(L, -2, id);
    }
    lua_setfield(L, -2, "spells");
    return 1;
}

// vana.game.container(id): { capacity = n, [index] = { id, index, count, flags, price, extra } for each slot from 0 }, or
// nil before the inventory is there.
int game_container(lua_State* L)
{
    const auto container = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    ashita::ItemSlot item{};
    if (!ashita::container_item(container, 0, item)) {
        lua_pushnil(L);
        return 1;
    }
    const uint32_t capacity = ashita::container_capacity(container);
    lua_createtable(L, capacity < 81 ? static_cast<int>(capacity) : 81, 1);
    lua_pushinteger(L, capacity);
    lua_setfield(L, -2, "capacity");
    for (uint32_t index = 0; index < capacity && index < 81; ++index) {
        if (!ashita::container_item(container, index, item))
            break;
        lua_createtable(L, 0, 6);
        lua_pushinteger(L, item.id);
        lua_setfield(L, -2, "id");
        lua_pushinteger(L, item.index);
        lua_setfield(L, -2, "index");
        lua_pushinteger(L, item.count);
        lua_setfield(L, -2, "count");
        lua_pushinteger(L, item.flags);
        lua_setfield(L, -2, "flags");
        lua_pushinteger(L, item.price);
        lua_setfield(L, -2, "price");
        lua_pushlstring(L, reinterpret_cast<const char*>(item.extra), sizeof(item.extra));
        lua_setfield(L, -2, "extra");
        lua_rawseti(L, -2, static_cast<int>(index));
    }
    return 1;
}

// vana.game.equipment(): { [game slot id] = { container, index } } (index 0: nothing), or nil before the inventory is.
int game_equipment(lua_State* L)
{
    uint32_t container = 0, index = 0;
    if (!ashita::equipped(0, container, index)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 16, 0);
    for (uint32_t slot = 0; slot < 16; ++slot) {
        if (!ashita::equipped(slot, container, index))
            break;
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, container);
        lua_setfield(L, -2, "container");
        lua_pushinteger(L, index);
        lua_setfield(L, -2, "index");
        lua_rawseti(L, -2, static_cast<int>(slot));
    }
    return 1;
}

// vana.game.character(): the logged in character's name and server id, or nil before login, at the character select and
// while the game has no player (the player's party slot and entity agree: game::local_player_name). What vana.settings
// keys a character's folder on.
int game_character(lua_State* L)
{
    const std::string name = game::local_player_name();
    const std::uint32_t id = game::local_player_id();
    if (name.empty() || id == 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, name.data(), name.size());
    lua_pushnumber(L, static_cast<lua_Number>(id)); // a server id can be past lua_Integer's 31 bits
    return 2;
}

// vana.game.self(): the local player in a few numbers (what Windower's derived events compare, every frame in every
// Windower addon, so no tables): nil before login, else its server id, zone, main job, its level, sub job, its level,
// status, the main target's index (0: none), hp, mp, tp, hpp and mpp.
int game_self(lua_State* L)
{
    game::PartyMember members[18];
    game::Alliance alliance;
    game::Entity entity;
    if (!game::party(members, alliance) || !members[0].active || members[0].id == 0 || members[0].target_index == 0 ||
        !game::entity(members[0].target_index, entity)) {
        lua_pushnil(L);
        return 1;
    }
    const game::PartyMember& me = members[0];
    game::Target target;
    const unsigned target_index = game::target(target) && target.active ? target.index : 0;
    const std::uint32_t values[] = {me.id,     me.zone,    me.main_job, me.main_job_level, me.sub_job, me.sub_job_level, entity.status,
                                    target_index, me.hp,    me.mp,       me.tp,             me.hpp,     me.mpp};
    for (const std::uint32_t value : values)
        lua_pushnumber(L, static_cast<lua_Number>(value)); // a server id can be past lua_Integer's 31 bits
    return static_cast<int>(std::size(values));
}

int game_target(lua_State* L)
{
    game::Target target;
    if (!game::target(target)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 10);
    set_number(L, "index", target.index);
    set_number(L, "id", target.id);
    set_bool(L, "active", target.active);
    set_number(L, "sub_index", target.sub_index);
    set_number(L, "sub_id", target.sub_id);
    set_bool(L, "sub_active", target.sub_active);
    set_bool(L, "is_sub_target", target.is_sub_target);
    set_bool(L, "locked", target.locked);
    set_bool(L, "menu_open", target.menu_open);
    lua_createtable(L, 2, 0); // raw: the game's two entries as they are
    for (int slot = 0; slot < 2; ++slot) {
        lua_createtable(L, 0, 3);
        set_number(L, "index", target.raw_index[slot]);
        set_number(L, "id", target.raw_id[slot]);
        set_bool(L, "active", target.raw_active[slot]);
        lua_rawseti(L, -2, slot + 1);
    }
    lua_setfield(L, -2, "raw");
    return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.gamepad: the controllers Windows lists, for /vana > Client > Gamepads.

// vana.gamepad.list([refresh]) -> controllers, xinput, xinput_reading
//   controllers: { { guid, name, product, type, opened, reading, xinput }, ... } DirectInput's (guid: what the game saves
//   as padguid000; opened: the game made a device for it this session; reading: the game reads it now, through
//   DirectInput; xinput: an XInput controller)
//   xinput: { connected0, connected1, connected2, connected3 }; xinput_reading: the slots the game reads a pad from now
//   `refresh` searches again; otherwise the last search stands until a device comes or goes.
int gamepad_list(lua_State* L)
{
    const input::Gamepads pads = input::gamepads(lua_toboolean(L, 1) != 0);
    lua_createtable(L, static_cast<int>(pads.controllers.size()), 0);
    int index = 1;
    for (const input::Controller& pad : pads.controllers) {
        lua_createtable(L, 0, 7);
        set_string(L, "guid", pad.guid);
        set_string(L, "name", pad.name);
        set_string(L, "product", pad.product);
        set_number(L, "type", pad.type);
        set_bool(L, "opened", pad.opened);
        set_bool(L, "reading", pad.reading);
        set_bool(L, "xinput", pad.xinput);
        lua_rawseti(L, -2, index++);
    }
    for (const bool* slots : {pads.xinput, pads.xinput_reading}) {
        lua_createtable(L, 4, 0);
        for (int slot = 0; slot < 4; ++slot) {
            lua_pushboolean(L, slots[slot]);
            lua_rawseti(L, -2, slot + 1);
        }
    }
    return 3;
}

// vana.gamepad.game() -> { known, on, xinput }: the game's own gamepad switches as they are now (a controller's profile
// or a pick in /vana changes them while it runs; the settings file has what it started with). known false: this client's
// pad code isn't the one it was written for.
int gamepad_game(lua_State* L)
{
    const input::GameGamepad game = input::game_gamepad();
    lua_createtable(L, 0, 3);
    set_bool(L, "known", game.known);
    set_bool(L, "on", game.on);
    set_bool(L, "xinput", game.xinput);
    return 1;
}

// vana.gamepad.select(guid) -> true, or false and why: the game uses that controller now (no restart)
int gamepad_select(lua_State* L)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    std::string guid(text, length);
    std::string why;
    const bool ok = input::select_gamepad(guid, why);
    lua_pushboolean(L, ok);
    if (ok)
        return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}

// vana.gamepad.apply{ mode=, buttons=, buttons_ex=, guid= } -> true, or false and why: the pad's settings, as vanacore.ini
// has them ([ffxi] gamepad.mode ...), used by the game now. A field left out (or empty) is left as the game has it.
int gamepad_apply(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    input::GamepadSettings settings;
    const auto field = [&](const char* name, std::string& out) {
        lua_getfield(L, 1, name);
        if (lua_type(L, -1) == LUA_TSTRING) {
            size_t length = 0;
            const char* text = lua_tolstring(L, -1, &length);
            out.assign(text, length);
        }
        lua_pop(L, 1);
    };
    field("mode", settings.mode);
    field("buttons", settings.buttons);
    field("buttons_ex", settings.buttons_ex);
    field("guid", settings.guid);
    std::string why;
    const bool ok = input::apply_gamepad(settings, why);
    lua_pushboolean(L, ok);
    if (ok)
        return 1;
    lua_pushlstring(L, why.data(), why.size());
    return 2;
}

// vana.gamepad.disable(off): no controller works while `off` ([input] gamepad.disable, as it is now; not saved)
int gamepad_disable(lua_State* L)
{
    input::disable_gamepads(lua_toboolean(L, 1) != 0);
    return 0;
}

// vana.gamepad.nav(on): this addon's window has the controller for the next frame (say so each frame while it's
// open): ImGui navigates with its gamepad keys and the game reads the controller at rest
int gamepad_nav(lua_State* L)
{
    gui::want_gamepad(lua_toboolean(L, 1) != 0);
    return 0;
}
// vana.gamepad.bindings() -> { xinput = bool, confirm = n, cancel = n, menu = n, options = n }: the game's own buttons
// (padsin000), as XInput's control numbers (12 A, 13 B, 14 X, 15 Y ...) or DirectInput's button numbers, -1 unset
int gamepad_bindings(lua_State* L)
{
    const gamepad::Bindings b = gamepad::bindings();
    lua_createtable(L, 0, 5);
    set_bool(L, "xinput", b.xinput);
    set_number(L, "confirm", b.confirm);
    set_number(L, "cancel", b.cancel);
    set_number(L, "menu", b.menu);
    set_number(L, "options", b.options);
    return 1;
}
// vana.gamepad.state() -> { present = bool, idle = seconds }: a controller read lately, and how long since it was used
int gamepad_state(lua_State* L)
{
    lua_createtable(L, 0, 2);
    set_bool(L, "present", gamepad::present());
    set_number(L, "idle", gamepad::idle_seconds());
    return 1;
}
// vana.gamepad.disabled() -> true while no controller works (gamepad.disable, or gamepad.disableenumeration)
int gamepad_disabled(lua_State* L)
{
    lua_pushboolean(L, input::gamepads_disabled());
    return 1;
}

void open_gamepad(lua_State* L)
{
    lua_newtable(L);
    set_function(L, "list", &gamepad_list);
    set_function(L, "game", &gamepad_game);
    set_function(L, "select", &gamepad_select);
    set_function(L, "apply", &gamepad_apply);
    set_function(L, "disable", &gamepad_disable);
    set_function(L, "disabled", &gamepad_disabled);
    set_function(L, "nav", &gamepad_nav);
    set_function(L, "bindings", &gamepad_bindings);
    set_function(L, "state", &gamepad_state);
    lua_setfield(L, -2, "gamepad");
}

void open_game(lua_State* L)
{
    lua_newtable(L);
    lua_entity::add(L); // entity, entity_by_id, vitals
    lua_maxhp::add(L);  // native_request_max_hp, native_max_hp_answers (lua/vana.lua's request_max_hp wraps them)
    zoneload::add_lua(L); // loading
    client::add_lua(L);   // server
    set_function(L, "entity_count", &game_entity_count);
    set_function(L, "party", &game_party);
    set_function(L, "player", &game_player);
    set_function(L, "zoning", &game_zoning);
    set_function(L, "player_zone", &game_player_zone);
    set_function(L, "zone_weathers", &game_zone_weathers);
    set_function(L, "set_target", &game_set_target);
    set_function(L, "world", &game_world);
    set_function(L, "target", &game_target);
    set_function(L, "self", &game_self);
    set_function(L, "character", &game_character);
    set_function(L, "abilities", &game_abilities);
    set_function(L, "spells", &game_spells);
    set_function(L, "key_items", &game_key_items);
    set_function(L, "recasts", &game_recasts);
    set_function(L, "container", &game_container);
    set_function(L, "equipment", &game_equipment);
    set_function(L, "skills", &game_skills);
    set_function(L, "crafts", &game_crafts);
    set_function(L, "job_points", &game_job_points);
    set_function(L, "jobs", &game_jobs);
    set_function(L, "pet", &game_pet);
    set_function(L, "missions", &game_missions);
    set_function(L, "linkshell", &game_linkshell);
    set_function(L, "merits", &game_merits);
    set_function(L, "native_item_stats", &equipstats::lua_item_stats); // (lua/vana.lua: vana.gamedata.item_stats)
    lua_setfield(L, -2, "game");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.gameui (gameui.hpp, MOCKUP): the game's own interface, changed by an addon. Nothing to declare and nothing for the
// player to allow: an addon says what it changes in its README. The functions add and replace take are lua/vana.lua's.

std::string gameui_key(lua_State* L)
{
    const Context& c = context(L);
    return gameui::key(c.source, c.name);
}

// What a call came to, as Lua has it: true; or false and 'held by <addon>' (another has the one move or replace there is;
// this one's takes effect when it lets go). Asking for what VanaCore can't do is an error: the author's to fix. Raised
// with no C++ object alive (see the top of this file).
int gameui_push(lua_State* L, const char* what, const char* element, gameui::Result result, const char* holder = "")
{
    switch (result) {
    case gameui::Result::Held:
        lua_pushboolean(L, false);
        lua_pushfstring(L, "held by %s", holder);
        return 2;
    case gameui::Result::Unknown:
        return luaL_error(L, "vana.gameui.%s('%s'): VanaCore can't %s that (vana.gameui.elements() lists what it can)", what,
                          element, what);
    case gameui::Result::Label:
        return luaL_error(L, "vana.gameui.%s('%s'): the label must be 1 to 10 letters", what, element);
    case gameui::Result::Colour:
        return luaL_error(L, "vana.gameui.%s('%s'): the colour must be blue, green, yellow, red, purple, grey or '#RRGGBB'",
                          what, element);
    case gameui::Result::Unloaded:
        return luaL_error(L, "vana.gameui.%s('%s'): only a loaded addon can change the game's interface", what, element);
    default:
        lua_pushboolean(L, true);
        return 1;
    }
}

// gameui.hide(element[, hidden]): one of the game's windows not drawn (hidden false: drawn again). -> ok
int gameui_hide(lua_State* L)
{
    const char* element = luaL_checkstring(L, 1);
    const bool hidden = lua_isnone(L, 2) || lua_toboolean(L, 2) != 0;
    gameui::Result result;
    {
        result = gameui::hide(gameui_key(L), element, hidden);
    }
    return gameui_push(L, "hide", element, result);
}

// gameui.move(element, x, y): put at x, y (percents of the interface, the element's own anchor); move(element) gives it
// back to the player's setting. -> ok, why
int gameui_move(lua_State* L)
{
    const char* element = luaL_checkstring(L, 1);
    const bool back = lua_isnoneornil(L, 2);
    const double x = back ? 0 : luaL_checknumber(L, 2);
    const double y = back ? 0 : luaL_checknumber(L, 3);
    gameui::Result result;
    char holder[96] = {};
    {
        std::string by;
        result = back ? gameui::unmove(gameui_key(L), element) : gameui::move(gameui_key(L), element, x, y, by);
        std::snprintf(holder, sizeof holder, "%s", by.c_str());
    }
    return gameui_push(L, "move", element, result, holder);
}

// gameui.native_add(menu, on[, label[, colour]]): the addon's button in the menu, labelled `label` (its colour a name of
// gameui's kColours or '#RRGGBB', the game's blue left out), or taken out. -> ok
int gameui_add(lua_State* L)
{
    const char* menu = luaL_checkstring(L, 1);
    const bool on = lua_toboolean(L, 2) != 0;
    const char* label = on ? luaL_checkstring(L, 3) : "";
    const char* colour = on ? luaL_optstring(L, 4, "") : "";
    gameui::Result result;
    {
        result = gameui::add(gameui_key(L), menu, label, colour, on);
    }
    return gameui_push(L, "add", menu, result);
}

// gameui.native_replace(item, take): one of the game's menu items taken over, or given back. -> ok, why
int gameui_replace(lua_State* L)
{
    const char* item = luaL_checkstring(L, 1);
    const bool take = lua_toboolean(L, 2) != 0;
    gameui::Result result;
    char holder[96] = {};
    {
        std::string by;
        result = gameui::replace(gameui_key(L), item, take, by);
        std::snprintf(holder, sizeof holder, "%s", by.c_str());
    }
    return gameui_push(L, "replace", item, result, holder);
}

// gameui.native_image(element, file, width, height, x, y, colour, opacity): the addon's picture in place of one of the game's
// ('target_arrow'): `file` relative to the addon's folder or a full path; `width` and `height` in the interface's units (nil: the
// picture's pixels as units); `x` and `y` how far it's moved from the game's anchor (the arrow's point), in units; `colour`
// '#RRGGBB' (nil: none) and `opacity` 0 to 1. native_image(element) gives the game its own back. -> ok, why
int gameui_image(lua_State* L)
{
    const char* element = luaL_checkstring(L, 1);
    const bool back = lua_isnoneornil(L, 2);
    gameui::Result result;
    char holder[96] = {};
    bool bad_colour = false;
    {
        std::string by;
        if (back) {
            result = gameui::unimage(gameui_key(L), element);
        } else {
            gameui::Picture picture;
            picture.file = widen(luaL_checkstring(L, 2));
            const bool full = picture.file.size() > 2 && (picture.file[1] == L':' || (picture.file[0] == L'\\' && picture.file[1] == L'\\'));
            if (!full) {
                const std::string& folder = context(L).folder;
                const int n = MultiByteToWideChar(CP_ACP, 0, folder.c_str(), static_cast<int>(folder.size()), nullptr, 0);
                std::wstring base(static_cast<std::size_t>(n), L'\0');
                MultiByteToWideChar(CP_ACP, 0, folder.c_str(), static_cast<int>(folder.size()), base.data(), n);
                picture.file = base + picture.file;
            }
            picture.width = static_cast<float>(std::clamp(luaL_optnumber(L, 3, 0), 0.0, 4096.0));
            picture.height = static_cast<float>(std::clamp(luaL_optnumber(L, 4, 0), 0.0, 4096.0));
            picture.x = static_cast<float>(std::clamp(luaL_optnumber(L, 5, 0), -4096.0, 4096.0));
            picture.y = static_cast<float>(std::clamp(luaL_optnumber(L, 6, 0), -4096.0, 4096.0));
            std::uint32_t rgb = 0xFFFFFF;
            if (!lua_isnoneornil(L, 7)) {
                const char* colour = luaL_checkstring(L, 7);
                const std::size_t length = std::strlen(colour);
                const bool hex = length == 7 && colour[0] == '#' &&
                                 std::all_of(colour + 1, colour + 7, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
                if (hex)
                    rgb = static_cast<std::uint32_t>(std::strtoul(colour + 1, nullptr, 16));
                else
                    bad_colour = true;
            }
            const double opacity = std::clamp(luaL_optnumber(L, 8, 1.0), 0.0, 1.0);
            picture.argb = (static_cast<std::uint32_t>(std::lround(opacity * 255.0)) << 24) | rgb;
            result = bad_colour ? gameui::Result::Colour : gameui::image(gameui_key(L), element, picture, by);
        }
        std::snprintf(holder, sizeof holder, "%s", by.c_str());
    }
    if (bad_colour)
        return luaL_error(L, "vana.gameui.image('%s'): the colour must be '#RRGGBB'", element);
    return gameui_push(L, "image", element, result, holder);
}

// gameui.hidden(element): who has one of the game's windows hidden, as one string of names ("yunaui, hidetarget"; an
// addon's as its folder spells it; fancychat, which keeps the chat windows closed and the chat line off the screen with no
// vana.gameui, for 'chat_log' and 'chat_input'), or nil while nobody does. The player's own [client] <element>.hide is
// not in it. For the HUD Manager's Hide switches, which show an addon's hide as theirs.
int gameui_hidden(lua_State* L)
{
    const char* element = luaL_checkstring(L, 1);
    char names[192] = {};
    {
        std::string by = gameui::hidden_by(element);
        const bool fancy = (std::strcmp(element, "chat_log") == 0 && chat::chat_addon_pinned()) ||
                           (std::strcmp(element, "chat_input") == 0 && chat::input_bar_addon());
        if (fancy)
            by += by.empty() ? "fancychat" : ", fancychat";
        std::snprintf(names, sizeof names, "%s", by.c_str());
    }
    if (names[0] == '\0')
        lua_pushnil(L);
    else
        lua_pushstring(L, names);
    return 1;
}

// gameui.stats_dump(): the Equipment window's stats worked out again into the log, each step (/vana statsdump); the
// items dumped, -1 with equip_mods.json missing, -2 while it's still being read
int gameui_stats_dump(lua_State* L)
{
    lua_pushinteger(L, equipstats::dump());
    return 1;
}

// gameui.elements(): what VanaCore can change, each { id, name, actions = { 'hide', 'move', ... } }.
int gameui_elements(lua_State* L)
{
    const std::vector<gameui::Element>& list = gameui::elements();
    lua_createtable(L, static_cast<int>(list.size()), 0);
    int row = 0;
    for (const gameui::Element& element : list) {
        lua_createtable(L, 0, 3);
        set_string(L, "id", element.id);
        set_string(L, "name", element.name);
        lua_newtable(L);
        int n = 0;
        for (const unsigned action : {gameui::kHide, gameui::kMove, gameui::kAdd, gameui::kReplace, gameui::kImage}) {
            if ((element.actions & action) != 0) {
                lua_pushstring(L, gameui::action_word(action));
                lua_rawseti(L, -2, ++n);
            }
        }
        lua_setfield(L, -2, "actions");
        lua_rawseti(L, -2, ++row);
    }
    return 1;
}

// gameui.native_on(menus, event, items, swallow): a trigger on the game's menus: `menus` a name ('menuwind', 'main_menu',
// '*') or a table of them, `event` the window event (0: any), `items` nil or a table of item ids or names, `swallow` true
// for a catch (the game's own action kept from its handler). -> the trigger's id. lua/vana.lua's on, catch and off keep
// the function and call it on 'gameui_trigger'.
int gameui_on(lua_State* L)
{
    const bool swallow = lua_toboolean(L, 4) != 0;
    const char* what = swallow ? "catch" : "on";
    int id = 0;
    bool no_menu = false;
    const char* bad_item = nullptr; // a Lua string on the stack, left there
    {
        std::vector<std::string> menus;
        if (lua_istable(L, 1)) {
            for (int i = 1;; ++i) {
                lua_rawgeti(L, 1, i);
                if (lua_type(L, -1) != LUA_TSTRING) {
                    lua_pop(L, 1);
                    break;
                }
                menus.push_back(lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else if (lua_type(L, 1) == LUA_TSTRING) {
            menus.push_back(lua_tostring(L, 1));
        }
        no_menu = menus.empty();
        const int event = static_cast<int>(luaL_optinteger(L, 2, gameui::kAnyEvent));
        std::vector<int> items;
        if (!no_menu && lua_istable(L, 3)) {
            for (int i = 1; bad_item == nullptr; ++i) {
                lua_rawgeti(L, 3, i);
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    break;
                }
                if (lua_type(L, -1) == LUA_TSTRING) {
                    const char* name = lua_tostring(L, -1);
                    int found = 0;
                    for (const std::string& menu : menus)
                        if ((found = gameui::item_id(menu, name)) != 0)
                            break;
                    if (found == 0) {
                        bad_item = name; // stays on the stack for the error below
                        break;
                    }
                    items.push_back(found);
                } else {
                    items.push_back(static_cast<int>(lua_tointeger(L, -1)));
                }
                lua_pop(L, 1);
            }
        }
        if (!no_menu && bad_item == nullptr)
            id = gameui::trigger(gameui_key(L), menus, event, items, swallow);
    }
    if (no_menu)
        return luaL_error(L, "vana.gameui.%s: the first argument must name a menu ('menuwind', 'main_menu', '*') or list them", what);
    if (bad_item != nullptr)
        return luaL_error(L, "vana.gameui.%s: no item '%s' is known in that menu by name: give its id", what, bad_item);
    if (id == 0)
        return luaL_error(L, "vana.gameui.%s: only a loaded addon can listen to the game's menus", what);
    lua_pushinteger(L, id);
    return 1;
}

// gameui.native_off(id): the trigger taken out.
int gameui_off(lua_State* L)
{
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    {
        gameui::untrigger(gameui_key(L), id);
    }
    return 0;
}

// gameui.rect(element): where the element is on the screen, in the interface's pixels (vana.ui.screen's): its left, top,
// right and bottom; nil while it isn't shown. 'target_window' for now (open only with something targeted), for an addon
// drawing by it (a mark over the target's name). Another element is an error: the author's to fix.
int gameui_rect(lua_State* L)
{
    const char* element = luaL_checkstring(L, 1);
    if (std::strcmp(element, "target_window") != 0)
        return luaL_error(L, "vana.gameui.rect: %s isn't an element with a place to give (target_window is)", element);
    unsigned width = 0, height = 0;
    ui::screen(width, height);
    float left = 0, top = 0, right = 0, bottom = 0;
    if (!targetwin::rect(width, height, left, top, right, bottom))
        return 0;
    lua_pushnumber(L, left);
    lua_pushnumber(L, top);
    lua_pushnumber(L, right);
    lua_pushnumber(L, bottom);
    return 4;
}

void open_gameui(lua_State* L)
{
    lua_newtable(L);
    set_function(L, "hide", &gameui_hide);
    set_function(L, "move", &gameui_move);
    set_function(L, "rect", &gameui_rect);
    set_function(L, "native_add", &gameui_add);         // lua/vana.lua takes these out of the table and puts add and
    set_function(L, "native_replace", &gameui_replace); // remove, replace and restore, on, catch and off over them (they
    set_function(L, "native_on", &gameui_on);           // keep fn to call)
    set_function(L, "native_off", &gameui_off);
    set_function(L, "native_image", &gameui_image); // lua/vana.lua's image(element, file[, options])
    set_function(L, "elements", &gameui_elements);
    set_function(L, "hidden", &gameui_hidden);
    set_function(L, "stats_dump", &gameui_stats_dump);
    lua_gamewin::add(L); // window, windows; native_press, which lua/vana.lua wraps as press
    lua_setfield(L, -2, "gameui");
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.encoding: FFXI's text is Shift-JIS with its own row for ñ, é, ¿ and the like (gametext.hpp); Lua source files are
// usually UTF-8.

template <bool ToGame>
int recode(lua_State* L)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    {
        const std::string_view in(text, length);
        const std::string out = ToGame ? gametext::from_utf8(in) : gametext::to_utf8(in);
        lua_pushlstring(L, out.data(), out.size());
    }
    return 1;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// vana.packets: FFXI's packets, each its 4-byte header (id, size in 4-byte units, sequence) first.

// vana.packets.inject(incoming, id, data): `data` (the whole packet: VanaCore sets its header's id, size and
// sequence) into the next buffer the game receives (incoming true) or sends
int packets_inject(lua_State* L)
{
    const bool incoming = lua_toboolean(L, 1) != 0;
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(L, 2) & 0x1FF);
    size_t size = 0;
    const char* data = luaL_checklstring(L, 3, &size);
    if (size < 4)
        return luaL_error(L, "vana.packets.inject: a packet is at least its 4-byte header (%d bytes given)",
                          static_cast<int>(size));
    // An Ashita addon's packet isn't shown to the addon itself (packets::inject); empty for any other kind
    packets::inject(incoming, id, static_cast<std::uint32_t>(size), reinterpret_cast<const std::uint8_t*>(data), ashita::calling());
    return 0;
}

// vana.packets.last(incoming, id): the last packet of that id the game received (or sent) and when it came (seconds,
// each later than the one before), nil if none yet
int packets_last(lua_State* L)
{
    const bool incoming = lua_toboolean(L, 1) != 0;
    double at = 0;
    const std::string packet = packets::last(incoming, static_cast<std::uint16_t>(luaL_checkinteger(L, 2) & 0x1FF), &at);
    if (packet.empty()) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, packet.data(), packet.size());
    lua_pushnumber(L, at);
    return 2;
}

// vana.packets.queue(id, length, align, p1, p2, fill): a packet in the game's own outgoing queue (Ashita's
// QueuePacket): fill(pointer) fills it, its header's id already set. False when the game holds that id back.
int packets_queue(lua_State* L)
{
    const auto id = static_cast<std::uint16_t>(luaL_checkinteger(L, 1) & 0x1FF);
    const auto length = static_cast<std::uint16_t>(luaL_checkinteger(L, 2));
    const auto align = static_cast<std::uint16_t>(luaL_optinteger(L, 3, 0));
    const auto p1 = static_cast<std::uint32_t>(luaL_optinteger(L, 4, 0));
    const auto p2 = static_cast<std::uint32_t>(luaL_optinteger(L, 5, 0));
    luaL_checktype(L, 6, LUA_TFUNCTION);
    struct Fill {
        lua_State* L;
        bool failed = false;
        std::string error;
    } fill{L};
    const bool queued = ashita::queue_packet(
        id, length, align, p1, p2,
        [](std::uint8_t* packet, void* param) {
            auto& f = *static_cast<Fill*>(param);
            lua_pushvalue(f.L, 6);
            lua_pushlightuserdata(f.L, packet);
            if (lua_pcall(f.L, 1, 0, 0) != 0) {
                f.failed = true;
                f.error = lua_tostring(f.L, -1) != nullptr ? lua_tostring(f.L, -1) : "?";
                lua_pop(f.L, 1);
            }
        },
        &fill);
    if (fill.failed)
        return luaL_error(L, "vana.packets.queue: %s", fill.error.c_str());
    lua_pushboolean(L, queued);
    return 1;
}

// vana.plugins.raise(name, data): a plugin event (plugins' HandleEvent; addons' 'plugin_event' at the next frame)
int plugins_raise(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    size_t size = 0;
    const char* data = luaL_optlstring(L, 2, "", &size);
    addons::note_plugin_event(L, name, std::string(data, size));
    plugins::raise_event(name, std::string(data, size));
    return 0;
}

// vana.addons.loaded() and vana.plugins.loaded(): the ones loaded now, each { name, source } (source: "vanacore",
// "ashita" or "windower")
void push_loaded(lua_State* L, const std::string& name, const std::string& source, int index)
{
    lua_createtable(L, 0, 2);
    set_string(L, "name", name);
    set_string(L, "source", source);
    lua_rawseti(L, -2, index);
}

// vana.addons.loaded()'s entries also have version, author, desc, link (empty when the addon doesn't say), file, state
// (Ashita's numbers: 0 error, 1 running, 3 loading, 4 unloading, 5 reloading) and memory (bytes its Lua state holds).
int addons_loaded(lua_State* L)
{
    lua_newtable(L);
    int n = 0;
    for (const auto& addon : addons::loaded()) {
        push_loaded(L, addon.name, addon.source, ++n);
        lua_rawgeti(L, -1, n);
        set_string(L, "version", addon.version);
        set_string(L, "author", addon.author);
        set_string(L, "desc", addon.description);
        set_string(L, "link", addon.link);
        set_string(L, "file", addon.file);
        set_number(L, "state", addon.state);
        set_number(L, "memory", addon.memory);
        lua_pop(L, 1);
    }
    return 1;
}

// vana.ipc.send(text): text for the addon of this one's name in every other VanaCore instance on the PC (ipc.hpp), where
// it comes as the 'ipc' event (e.message). False if it wasn't sent (over ipc::kMaxMessage bytes, or no shared memory).
int ipc_send(lua_State* L)
{
    size_t length = 0;
    const char* text = luaL_checklstring(L, 1, &length);
    lua_pushboolean(L, ipc::send(context(L).name, std::string_view(text, length)));
    return 1;
}

// vana.layout.windows(): addons' and plugins' ImGui windows (layout.hpp), those drawn in the last frame first, then the
// ones config\layout.json has that aren't: each { name, owner ("ashita/partyfinder", "plugin/minimap"), x, y, w, h,
// scale, shown, pinned (its addon places it every frame), saved, locked, no_pointer (vana.layout.flags) }
int layout_windows(lua_State* L)
{
    const std::vector<layout::Window> windows = layout::windows();
    lua_createtable(L, static_cast<int>(windows.size()), 0);
    for (size_t i = 0; i < windows.size(); ++i) {
        const layout::Window& w = windows[i];
        lua_createtable(L, 0, 12);
        set_string(L, "name", w.name);
        set_string(L, "owner", w.owner);
        set_number(L, "x", w.x);
        set_number(L, "y", w.y);
        set_number(L, "w", w.w);
        set_number(L, "h", w.h);
        set_number(L, "scale", w.scale);
        set_bool(L, "shown", w.shown);
        set_bool(L, "pinned", w.pinned);
        set_bool(L, "saved", w.saved);
        set_bool(L, "locked", w.locked);
        set_bool(L, "no_pointer", w.no_pointer);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// vana.layout.set(name, x, y, scale): window `name` moved and/or scaled (nil: that one as it is)
int layout_set(lua_State* L)
{
    const std::string name = luaL_checkstring(L, 1);
    float values[3];
    const float* given[3] = {};
    for (int i = 0; i < 3; ++i)
        if (lua_isnumber(L, i + 2)) {
            values[i] = static_cast<float>(lua_tonumber(L, i + 2));
            given[i] = &values[i];
        }
    layout::set(name, given[0], given[1], given[2]);
    return 0;
}

// vana.layout.flags(name, locked, no_pointer): window `name` locked (the mouse can't drag or resize it) and/or with no
// pointer events (the mouse goes through it), nil: that one as it is. ImGui windows only (layout.hpp set_flags)
int layout_flags(lua_State* L)
{
    const std::string name = luaL_checkstring(L, 1);
    bool values[2];
    const bool* given[2] = {};
    for (int i = 0; i < 2; ++i)
        if (!lua_isnoneornil(L, i + 2)) {
            values[i] = lua_toboolean(L, i + 2) != 0;
            given[i] = &values[i];
        }
    layout::set_flags(name, given[0], given[1]);
    return 0;
}

// vana.layout.reset(name): what's set for it forgotten
int layout_reset(lua_State* L)
{
    layout::reset(luaL_checkstring(L, 1));
    return 0;
}

// vana.layout.highlight(name): window `name` outlined this frame and the next
int layout_highlight(lua_State* L)
{
    layout::highlight(luaL_checkstring(L, 1));
    return 0;
}

// vana.layout.outline(what): a game window's place drawn as a box this frame (outline.hpp: 'buffs' the status icons, 'chat'
// and 'chat2' the chat windows, 'input' the chat line, 'target' the Target window, 'macrobar' the macro bar, 'castbar' the cast bar,
// 'compass' the compass, 'clock' the clock under it, 'menus' the menus over the chat, 'map' the map in a box, 'regionmap' the region maps' box,
// 'equip.stats' the Equipment window's stats, 'equip.stats.check' the Check window's; hudwin.cpp's 'party', 'alliance1', 'alliance2', 'action'), shown while it isn't open, Shift + drag
// moving it (`snap`, pixels: to a grid that far apart, as /vana's HUD Manager's snap; none if left out); true if it was
// drawn, and the frame a drag is let go, its X and Y (percents, [client] <what>.x/.y) to save. `label`: a name written in
// the box's corner (the HUD Manager's Show element names), none if left out; `anchor` 'right': its right edge snaps, not its
// left (the HUD Manager's Anchor)
int layout_outline(lua_State* L)
{
    const std::string_view what = luaL_checkstring(L, 1);
    outline::snap(static_cast<int>(luaL_optinteger(L, 2, 0)), std::strcmp(luaL_optstring(L, 4, "left"), "right") == 0);
    outline::label(luaL_optstring(L, 3, ""));
    double x = -1, y = -1;
    const bool drawn = what == "buffs"    ? buffwin::outline(&x, &y)
                       : what == "chat"   ? chat::outline(0, &x, &y)
                       : what == "chat2"  ? chat::outline(1, &x, &y)
                       : what == "input"  ? chat::outline(2, &x, &y)
                       : what == "target" ? targetwin::outline(&x, &y)
                       : what == "compass" ? chat::outline(3, &x, &y)
                       : what == "clock" ? chat::outline(5, &x, &y)
                       : what == "menus" ? chat::outline(4, &x, &y)
                       : what == "macrobar" ? macrobar::outline(&x, &y)
                       : what == "castbar" ? castbar::outline(&x, &y)
                       : what == "map" ? mapwin::outline(0, &x, &y)
                       : what == "regionmap" ? mapwin::outline(1, &x, &y)
                       : what == "equip.stats" ? equipstats::outline(&x, &y)
                       : what == "equip.stats.check" ? equipstats::check_outline(&x, &y)
                       : hudwin::outline(std::string(what).c_str(), &x, &y) ? true
                                          : false;
    outline::label(""); // (not drawn: not left for the next box)
    lua_pushboolean(L, drawn);
    if (x < 0)
        return 1;
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 3;
}

// vana.layout.box(name, label, snap, picked): the HUD Manager's Addon View, window `name`'s yellow box this frame
// (addonbox.hpp: one of vana.layout.windows()'s, an ImGui window or text and shapes), `label` over its corner (nil: none),
// Shift + drag moving it, its corner to a grid `snap` pixels apart (nil: none), `picked` green with a thick edge and the
// arrow keys'. True if it was drawn, and the frame a move is let go where it went and where it was: x, y, from_x, from_y
int layout_box(lua_State* L)
{
    const addonbox::Result r = addonbox::box(luaL_checkstring(L, 1), luaL_optstring(L, 2, ""),
                                             static_cast<int>(luaL_optinteger(L, 3, 0)), lua_toboolean(L, 4) != 0);
    lua_pushboolean(L, r.drawn);
    if (!r.released)
        return 1;
    lua_pushnumber(L, r.x);
    lua_pushnumber(L, r.y);
    lua_pushnumber(L, r.from_x);
    lua_pushnumber(L, r.from_y);
    return 5;
}

// vana.layout.box_clicked(): the window whose vana.layout.box was clicked since the last call, or nil
int layout_box_clicked(lua_State* L)
{
    const std::string name = addonbox::clicked();
    if (name.empty())
        return 0;
    lua_pushstring(L, name.c_str());
    return 1;
}

// vana.layout.up(what): whether the game window of vana.layout.outline's box `what` is up now (the HUD Manager's Open only);
// nil for one that can't be told (the chat, the compass, the clock: always there)
int layout_up(lua_State* L)
{
    const char* what = luaL_checkstring(L, 1);
    const bool map = std::strcmp(what, "map") == 0, region = std::strcmp(what, "regionmap") == 0;
    const int up = map ? (mapwin::up(0) ? 1 : 0) : region ? (mapwin::up(1) ? 1 : 0) : hudwin::up(what);
    if (up < 0)
        return 0;
    lua_pushboolean(L, up != 0);
    return 1;
}

// vana.layout.clicked(): the vana.layout.outline box left-clicked without Shift since the last call ('party', 'chat'...),
// or nil (outline::clicked)
int layout_clicked(lua_State* L)
{
    const std::string id = outline::clicked();
    if (id.empty())
        return 0;
    lua_pushstring(L, id.c_str());
    return 1;
}

// vana.layout.select(what): the vana.layout.outline box `what` picked alone, as Shift + right click on it would leave it
// (nil or '': none); the arrow keys then move it, Shift + them in big steps (outline::select)
int layout_select(lua_State* L)
{
    outline::select(luaL_optstring(L, 1, ""));
    return 0;
}

// vana.layout.selected(): the ids of the boxes picked now, a list (outline::selected)
int layout_selected(lua_State* L)
{
    const std::vector<std::string> ids = outline::selected();
    lua_createtable(L, static_cast<int>(ids.size()), 0);
    for (size_t i = 0; i < ids.size(); ++i) {
        lua_pushstring(L, ids[i].c_str());
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    return 1;
}

// vana.layout.grid(pixels, color, alpha, background, background_alpha, centre_alpha): lines `pixels` apart over the whole
// game this frame, out from the screen's centre, behind ImGui's windows (/vana's HUD Manager grid); `color` '#RRGGBB',
// `alpha` 0..1; `background` (left out: none) fills the screen under the lines, `background_alpha` 0..1; `centre_alpha`
// (left out: `alpha`) is the two 3-pixel lines through the centre's
int layout_grid(lua_State* L)
{
    // '#RRGGBB' and 0..1 as an ImU32 (0xAABBGGRR)
    const auto colour = [](const char* hex, double alpha) {
        unsigned rgb = 0xFFFFFF;
        if (hex[0] == '#' && std::strlen(hex) == 7)
            rgb = static_cast<unsigned>(std::strtoul(hex + 1, nullptr, 16));
        const auto a = static_cast<unsigned>(std::lround(std::clamp(alpha, 0.0, 1.0) * 255));
        return (a << 24) | ((rgb & 0xFF) << 16) | (rgb & 0xFF00) | ((rgb >> 16) & 0xFF);
    };
    const int pixels = static_cast<int>(luaL_checkinteger(L, 1));
    const unsigned lines = colour(luaL_optstring(L, 2, "#FFFFFF"), luaL_optnumber(L, 3, 0.25));
    const unsigned fill = lua_isstring(L, 4) ? colour(lua_tostring(L, 4), luaL_optnumber(L, 5, 0.3)) : 0;
    const unsigned centre = colour(luaL_optstring(L, 2, "#FFFFFF"), luaL_optnumber(L, 6, luaL_optnumber(L, 3, 0.25)));
    outline::grid(pixels, lines, centre, fill);
    return 0;
}

// vana.plugins.loaded()'s entries also have memory: the bytes the plugin has allocated (heap blocks and address space
// reserved, plugin_memory.hpp), nil when it isn't counted.
int plugins_loaded(lua_State* L)
{
    lua_newtable(L);
    int n = 0;
    for (const auto& plugin : plugins::loaded_list()) {
        push_loaded(L, plugin.first, plugin.second, ++n);
        plugin_memory::Usage use;
        if (plugin_memory::usage(plugin.first, use)) {
            lua_rawgeti(L, -1, n);
            set_number(L, "memory", (use.heap_kb + use.virtual_kb) * 1024.0);
            lua_pop(L, 1);
        }
    }
    return 1;
}

void open_packets(lua_State* L)
{
    lua_newtable(L);
    set_function(L, "inject", &packets_inject);
    set_function(L, "last", &packets_last);
    set_function(L, "queue", &packets_queue);
    lua_setfield(L, -2, "packets");
}

void open_encoding(lua_State* L)
{
    lua_newtable(L);
    set_function(L, "to_shift_jis", &recode<true>);
    set_function(L, "from_shift_jis", &recode<false>);
    lua_setfield(L, -2, "encoding");
}

} // namespace

Context& context(lua_State* L)
{
    lua_pushlightuserdata(L, const_cast<char*>(&kContextKey));
    lua_rawget(L, LUA_REGISTRYINDEX);
    auto* context = static_cast<Context*>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return *context;
}

// vana.window.info(): the game window's place, nil before it has one: x, y, width, height (the window on the screen),
// client_width, client_height (the picture), monitor (1 the leftmost), topmost, minimized, maximized, covering (full screen),
// character (the one [window] position.<character> is for, "" before login) and monitors, left to right, each { x, y, width,
// height, main } mcp.info(): the MCP server's state (the /vana window's AI MCP panel): { running, failed, port, client, idle,
// sessions }, client the session that used it last as the player sees it ('#3 claude-code "alliance list RE"', '' before any),
// idle the seconds since its last request (-1 before any), sessions every one since the game started, oldest first: { number,
// client, name, idle, calls }.
int mcp_info(lua_State* L)
{
    const mcp::Status status = mcp::status();
    lua_createtable(L, 0, 6);
    set_bool(L, "running", status.running);
    set_bool(L, "failed", status.failed);
    set_number(L, "port", status.port);
    set_string(L, "client", status.client);
    set_number(L, "idle", status.idle_seconds);
    lua_createtable(L, static_cast<int>(status.sessions.size()), 0);
    int row = 0;
    for (const mcp::Status::Session& session : status.sessions) {
        lua_createtable(L, 0, 5);
        set_number(L, "number", session.number);
        set_string(L, "client", session.client);
        set_string(L, "name", session.name);
        set_number(L, "idle", session.idle_seconds);
        set_number(L, "calls", session.calls);
        lua_rawseti(L, -2, ++row);
    }
    lua_setfield(L, -2, "sessions");
    return 1;
}

int window_info(lua_State* L)
{
    window::Place place;
    if (!window::place(place)) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 14);
    set_number(L, "x", place.frame.left);
    set_number(L, "y", place.frame.top);
    set_number(L, "width", place.frame.right - place.frame.left);
    set_number(L, "height", place.frame.bottom - place.frame.top);
    set_number(L, "client_width", place.client_width);
    set_number(L, "client_height", place.client_height);
    set_number(L, "monitor", place.monitor);
    set_bool(L, "topmost", place.topmost);
    set_bool(L, "minimized", place.minimized);
    set_bool(L, "maximized", place.maximized);
    set_bool(L, "covering", place.covering);
    set_string(L, "character", place.character);
    const std::vector<window::Monitor> monitors = window::monitors();
    lua_createtable(L, static_cast<int>(monitors.size()), 0);
    for (size_t i = 0; i < monitors.size(); ++i) {
        const RECT& area = monitors[i].area;
        lua_createtable(L, 0, 5);
        set_number(L, "x", area.left);
        set_number(L, "y", area.top);
        set_number(L, "width", area.right - area.left);
        set_number(L, "height", area.bottom - area.top);
        set_bool(L, "main", monitors[i].main);
        lua_rawseti(L, -2, static_cast<int>(i + 1));
    }
    lua_setfield(L, -2, "monitors");
    return 1;
}

// vana.window.monitor(n): the game's window to monitor n (1 the leftmost, as info().monitors has them), its middle (full
// screen: covering it); [ffxi] resolution.game and resolution.ui go with it (window::poll). False when it can't move there.
int window_monitor(lua_State* L)
{
    lua_pushboolean(L, window::to_monitor(static_cast<int>(luaL_checkinteger(L, 1))));
    return 1;
}

// vana.window.block_mouse(on): no mouse button reaches the game while on (the Import and Export windows), and half a second
// after. True while it's blocked, ending included.
int window_block_mouse(lua_State* L)
{
    if (lua_gettop(L) > 0)
        window::block_mouse(lua_toboolean(L, 1) != 0, lua_gettop(L) > 1 ? static_cast<unsigned>(luaL_checknumber(L, 2)) : 400u);
    lua_pushboolean(L, window::mouse_blocked());
    return 1;
}

// [addons] ashita.interface: the Ashita interface Ashita addons see, 4.30 (SoulspireXI default) or 4.16
double ashita_interface(const std::wstring& ini)
{
    if (ini.empty())
        return 4.30;
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"addons", L"ashita.interface", L"4.30", value, 32, ini.c_str());
    const double version = std::wcstod(value, nullptr);
    if (std::fabs(version - 4.16) < 0.001)
        return 4.16;
    static std::wstring reported; // a value that isn't either, logged once
    if (std::fabs(version - 4.3) >= 0.001 && reported != value) {
        reported = value;
        log::write(L"Addons: [addons] ashita.interface=%ls isn't 4.16 or 4.30; Ashita addons see 4.30", value);
    }
    return 4.30;
}

// [addons] ashita.libs: the Ashita addon libraries Ashita addons load, 4.30 (the default: Ashita 4.30's, which have all 4.16's
// had and more, with ImGui 1.81's names defined over them by compat/ashita.lua, so an addon written for either version finds what
// it asks for) or 4.16 (CatsEyeXI's Ashita's own, as they were given before, for an addon the merged set doesn't suit)
double ashita_libs(const std::wstring& ini)
{
    if (ini.empty())
        return 4.30;
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"addons", L"ashita.libs", L"4.30", value, 32, ini.c_str());
    const double version = std::wcstod(value, nullptr);
    if (std::fabs(version - 4.16) < 0.001)
        return 4.16;
    static std::wstring reported; // a value that isn't either, logged once
    if (std::fabs(version - 4.3) >= 0.001 && reported != value) {
        reported = value;
        log::write(L"Addons: [addons] ashita.libs=%ls isn't 4.16 or 4.30; Ashita addons load 4.30's libraries", value);
    }
    return 4.30;
}

void open(lua_State* L, Context& context)
{
    lua_pushlightuserdata(L, const_cast<char*>(&kContextKey));
    lua_pushlightuserdata(L, &context);
    lua_rawset(L, LUA_REGISTRYINDEX);
    if (context.flavour == Flavour::Ashita) {
        context.ashita_interface = ashita_interface(context.settings_ini);
        context.ashita_libs = ashita_libs(context.settings_ini);
    }

    // Addons find modules in their own folder, then in their kind's libraries. Ashita addons' are VanaCore's own: Ashita
    // 4.30's (addons\vanacore\libs\ashita-4.30: ImGui's names, fonts, primitives and the rest that differ between the
    // versions; 4.16's own with [addons] ashita.libs=4.16), then what both versions share (addons\vanacore\libs\ashita,
    // bitreader with them), so nothing depends on the player's Ashita. 4.30's have everything 4.16's had but ImGui 1.81's
    // names 1.92 renamed or dropped, which compat/ashita.lua defines before the addon runs.
    const char* kind = context.flavour == Flavour::Windower ? "windower" : context.flavour == Flavour::Ashita ? "ashita" : "vanacore";
    const std::string libs = context.install + "addons\\" + kind + "\\libs\\";
    lua_getglobal(L, "package");
    std::string path = context.folder + "?.lua;";
    std::string cpath = context.folder + "?.dll";
    if (context.flavour == Flavour::Ashita) {
        const std::string own = context.install + "addons\\vanacore\\libs\\ashita";
        const std::string version = own + (context.ashita_libs > 4.2 ? "-4.30\\" : "-4.16\\");
        path += version + "?.lua;" + version + "?\\init.lua;";
        path += own + "\\?.lua;" + own + "\\?\\init.lua;";
        // Ashita's addons root: bludex's require('bludex\\lib\\spellbook') is addons\bludex\lib\spellbook.lua there. Ashita's
        // addons are in addons\ashita here, so what is <GetInstallPath()>addons\?.lua to them is addons\ashita\?.lua
        path += context.install + "addons\\ashita\\?.lua;";
        path += context.install + "addons\\ashita\\?\\init.lua";
        // Never the mirrored Ashita's own addons\ashita\libs, for Lua or DLLs: what's there is the player's Ashita's (its
        // LuaSec ssl.dll crashes a LuaJIT state), and VanaCore's addons load only what VanaCore carries
    } else {
        path += libs + "?.lua;" + libs + "?\\init.lua";
        cpath += ";" + libs + "?.dll";
        if (context.flavour == Flavour::Windower) {
            // then Windower's own libraries as VanaCore carries them, for an addons\windower without Windower's libs\
            // (GearSwap copied in alone: require 'files')
            const std::string carried = context.install + "addons\\vanacore\\libs\\windower\\libs\\";
            path += ";" + carried + "?.lua;" + carried + "?\\init.lua";
        }
    }
    set_string(L, "path", path);
    set_string(L, "cpath", cpath);
    // LuaSocket's C half, built in (its Lua half is the addon's kind's libraries' socket.lua, mime.lua, socket\*.lua)
    lua_getfield(L, -1, "preload");
    lua_pushcfunction(L, &luaopen_socket_core);
    lua_setfield(L, -2, "socket.core");
    lua_pushcfunction(L, &luaopen_mime_core);
    lua_setfield(L, -2, "mime.core");
    lua_pop(L, 2);

    // Folders go to Lua without a trailing backslash.
    const auto folder = [](const std::string& path) { return path.substr(0, path.size() - 1); };
    if (context.flavour != Flavour::Windower) { // Windower addons get _addon (compat/windower.lua)
        lua_newtable(L); // addon: the addon's own details (it may fill in author, version, desc)
        set_string(L, "name", context.name);
        // Ashita's addon.path ends in a backslash (addon.path .. 'img/x.png': frenslist, XIUI, xivbar, targetlines)
        set_string(L, "path", context.flavour == Flavour::Ashita ? context.folder : folder(context.folder));
        lua_setglobal(L, "addon");
    }

    lua_newtable(L); // vana
    set_string(L, "version", narrow(kVersion));
    set_function(L, "log", &log_line);
    open_memory(L);
    open_fs(L);
    open_ui(L);
    open_game(L);
    open_gameui(L);
    open_gamepad(L);
    open_packets(L);
    lua_newtable(L);
    set_function(L, "raise", &plugins_raise);
    set_function(L, "loaded", &plugins_loaded);
    lua_setfield(L, -2, "plugins");
    lua_newtable(L);
    set_function(L, "loaded", &addons_loaded);
    addonpkg::open_lua(L); // package_waiting, package_info, package_install, ...: /vana's install prompt
    lua_setfield(L, -2, "addons");
    lua_newtable(L);
    set_function(L, "send", &ipc_send);
    lua_setfield(L, -2, "ipc");
    lua_newtable(L);
    set_function(L, "windows", &layout_windows);
    set_function(L, "set", &layout_set);
    set_function(L, "reset", &layout_reset);
    set_function(L, "flags", &layout_flags);
    set_function(L, "highlight", &layout_highlight);
    set_function(L, "outline", &layout_outline);
    set_function(L, "up", &layout_up);
    set_function(L, "grid", &layout_grid);
    set_function(L, "clicked", &layout_clicked);
    set_function(L, "select", &layout_select);
    set_function(L, "selected", &layout_selected);
    set_function(L, "box", &layout_box);
    set_function(L, "box_clicked", &layout_box_clicked);
    lua_setfield(L, -2, "layout");
    open_encoding(L);
    lua_imgui::push(L); // vana.imgui: ImGui as Ashita's addons have it (compat/ashita.lua)
    lua_setfield(L, -2, "imgui");

    lua_newtable(L);
    set_function(L, "write", &chat_write);
    set_function(L, "run", &chat_run);
    set_function(L, "preview", &chat_preview);
    lua_setfield(L, -2, "chat");

    lua_newtable(L);
    set_function(L, "list", &binds_list);
    set_function(L, "base", &binds_base);
    set_function(L, "keys", &binds_keys);
    set_function(L, "add", &binds_add);
    set_function(L, "remove", &binds_remove);
    set_function(L, "capture_begin", &binds_capture_begin);
    set_function(L, "capture_end", &binds_capture_end);
    set_function(L, "capture", &binds_capture);
    lua_setfield(L, -2, "binds");

    lua_newtable(L);
    set_function(L, "get", &config_get);
    set_function(L, "set", &config_set);
    set_function(L, "game", &config_game);
    set_function(L, "reload", &config_reload);
    set_function(L, "info", &config_info);
    lua_setfield(L, -2, "config");

    lua_newtable(L);
    set_function(L, "list", &clients_list);
    set_function(L, "set_main", &clients_set_main);
    set_function(L, "copy", &clients_copy);
    set_function(L, "front", &clients_front);
    lua_setfield(L, -2, "clients");

    lua_newtable(L);
    set_function(L, "info", &graphics_info);
    set_function(L, "hardware", &graphics_hardware);
    set_function(L, "reload", &graphics_reload);
    set_function(L, "retry", &graphics_retry);
    set_function(L, "recompile", &graphics_recompile);
    set_function(L, "damage_test", &graphics_damage_test);
    set_function(L, "dlss_indicator", &graphics_dlss_indicator);
    set_function(L, "dlss_crash_test", &graphics_dlss_crash_test);
    set_function(L, "grass_assets", &graphics_grass_assets);
    set_function(L, "grass_capture", &graphics_grass_capture);
    set_function(L, "grass_capture_status", &graphics_grass_capture_status);
    set_function(L, "grass_reload", &graphics_grass_reload);
    set_function(L, "grass_solo", &graphics_grass_solo);
    set_function(L, "grass_pathpad", &graphics_grass_pathpad);
    lua_setfield(L, -2, "graphics");

    lua_newtable(L);
    set_function(L, "preview", &camera_preview);
    set_function(L, "free", &camera_free);
    set_function(L, "float", &camera_float);
    set_function(L, "state", &camera_state);
    set_function(L, "pose", &camera_pose);
    set_function(L, "set_pose", &camera_set_pose);
    set_function(L, "hold", &camera_hold);
    set_function(L, "speed", &camera_speed);
    lua_setfield(L, -2, "camera");

    lua_newtable(L);
    set_function(L, "info", &window_info);
    set_function(L, "monitor", &window_monitor);
    set_function(L, "block_mouse", &window_block_mouse);
    lua_setfield(L, -2, "window");

    lua_newtable(L);
    set_function(L, "info", &mcp_info);
    lua_setfield(L, -2, "mcp");

    lua_newtable(L);
    set_function(L, "clock", &clock);
    lua_setfield(L, -2, "time");

    lua_ashita::push(L, context.ashita_interface, context.ashita_libs); // (lua_ashita.cpp)
    lua_setfield(L, -2, "ashita");

    lua_newtable(L);
    set_function(L, "open_url", &open_url);
    set_function(L, "play_sound", &play_sound);
    set_function(L, "clipboard", &get_clipboard);
    set_function(L, "set_clipboard", &set_clipboard);
    set_function(L, "focused", &focused);
    lua_setfield(L, -2, "system");

    lua_newtable(L);
    set_string(L, "install", folder(context.install));
    set_string(L, "addons", context.install + "addons");
    set_string(L, "addon", folder(context.folder));
    // the game's own folder (FFXiMain.dll's, FTABLE.DAT and the ROMs under it), once the game is loaded
    const std::wstring game_folder = dats::ffxi_folder();
    set_string(L, "game", game_folder.empty() ? std::string() : narrow(game_folder.substr(0, game_folder.size() - 1)));
    lua_setfield(L, -2, "paths");

    lua_setglobal(L, "vana");
}

} // namespace vanacore::lua_api
