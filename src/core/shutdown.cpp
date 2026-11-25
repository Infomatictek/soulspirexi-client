#include "shutdown.hpp"

#include <windows.h>

#include <atomic>

#include "addons.hpp"
#include "clients.hpp"
#include "gamesave.hpp"
#include "imports.hpp"
#include "log.hpp"
#include "plugins.hpp"
#include "settings.hpp"
#include "soulspire/soulspire.hpp"

namespace vanacore::shutdown {
namespace {

std::atomic<bool> g_started{false};
decltype(&ExitProcess) g_exit_process = nullptr;

struct Request {
    const wchar_t* why;
    bool as_game_thread; // the game's thread is the one stopped in the crash handler: nothing else runs Lua or plugins
};

void unload_everything(const wchar_t* why, bool as_game_thread)
{
    if (g_started.exchange(true))
        return;
    log::write(L"Shutdown: %ls: unloading addons and plugins%ls", why,
               as_game_thread ? L"" : L" (from another thread than the game's: plugins are left as they are)");
    addons::shutdown(as_game_thread);
    plugins::shutdown(as_game_thread);
    soulspire::shutdown();
    log::write(L"Shutdown: addons and plugins unloaded");
    clients::shutdown();  // its client file, while the folder is known
    settings::shutdown(); // the session copy of vanacore.ini, after everything that could write to it
}

DWORD WINAPI unload_thread(void* param)
{
    const Request* request = static_cast<const Request*>(param);
    unload_everything(request->why, request->as_game_thread);
    return 0;
}

void WINAPI exit_process(UINT code)
{
    unload_everything(L"the game is exiting", addons::is_game_thread());
    g_exit_process(code);
}

void on_module(HMODULE module)
{
    if (!imports::is_windows_module(module))
        imports::hook(module, "KERNEL32.dll", "ExitProcess", &exit_process, g_exit_process);
}

} // namespace

void run(const wchar_t* why)
{
    unload_everything(why, addons::is_game_thread());
}

void run_bounded(const wchar_t* why, unsigned wait_ms)
{
    if (g_started.load())
        return;
    static Request request;
    request = {why, addons::is_game_thread()};
    HANDLE thread = CreateThread(nullptr, 0, &unload_thread, &request, 0, nullptr);
    if (thread == nullptr)
        return;
    if (WaitForSingleObject(thread, wait_ms) != WAIT_OBJECT_0)
        log::write_from_crash(L"Shutdown: addons and plugins hadn't finished unloading after %u ms; the game ends anyway",
                              wait_ms);
    CloseHandle(thread);
}

void end_now(const wchar_t* why, unsigned wait_ms)
{
    gamesave::run();
    run_bounded(why, wait_ms);
    TerminateProcess(GetCurrentProcess(), 0);
    ExitProcess(0);
}

void install()
{
    imports::for_each_module(&on_module);
}

} // namespace vanacore::shutdown
