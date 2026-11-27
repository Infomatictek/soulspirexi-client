#include "mcp.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <vanacore/version.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "chat.hpp"
#include "json.hpp"
#include "log.hpp"
#include "mcp_tools.hpp"

#pragma comment(lib, "ws2_32.lib")

namespace vanacore::mcp {
namespace {

Settings g_settings;
std::string g_token;               // [mcp] token: clients send "Authorization: Bearer <token>" (empty: none asked)
std::atomic<bool> g_running{false};
std::atomic<unsigned> g_generation{0}; // each start's: a stopped listener's thread knows it's done
SOCKET g_listener = INVALID_SOCKET;
std::atomic<int> g_connections{0};
std::atomic<bool> g_failed{false};             // enabled, but no port could be had
std::atomic<ULONGLONG> g_last_request{0};      // GetTickCount64 of the last request (0: none)
constexpr int kMaxConnections = 16;
constexpr size_t kMaxBody = 16 * 1024 * 1024;

// Each initialize is a session: numbered for the player (#1, #2, ...), with an Mcp-Session-Id the client sends back on
// its later requests, so the chat log and the /vana window say which chat is probing. A session is named by its client
// (clientInfo.title) or by the chat itself (the hello tool, which the initialize's instructions ask for).
struct Session {
    unsigned number = 0;
    std::string id;        // its Mcp-Session-Id
    std::string client;    // clientInfo.name ("claude-code", "probe")
    std::string name;      // what it calls itself ("" until hello or clientInfo.title)
    ULONGLONG last = 0;    // GetTickCount64 of its last request
    unsigned calls = 0;    // tools/call so far
};
std::mutex g_sessions_lock;
std::vector<Session> g_sessions; // oldest first
unsigned g_session_count = 0;    // the last number given
std::string g_client;            // the tag of the session that made the last request (the /vana window's Server line)
constexpr size_t kMaxSessions = 32;
thread_local std::string t_session; // the request being served: its Mcp-Session-Id ("" when it sent none)

// How the player sees a session: #3 claude-code "alliance list RE"
std::string tag(const Session& session)
{
    std::string out = "#" + std::to_string(session.number) + " " + session.client;
    if (!session.name.empty())
        out += " \"" + session.name + "\"";
    return out;
}

// (g_sessions_lock held)
Session* find_session(const std::string& id)
{
    for (Session& session : g_sessions)
        if (session.id == id)
            return &session;
    return nullptr;
}

// The session the request being served belongs to: the one its Mcp-Session-Id names, else the newest (a client that
// keeps no session, such as a script that initializes on each run). Null before any. (g_sessions_lock held)
Session* current_session()
{
    if (!t_session.empty())
        if (Session* session = find_session(t_session))
            return session;
    return g_sessions.empty() ? nullptr : &g_sessions.back();
}

// A new session's id: its number and 64 random bits, so a stale id never names a later session.
std::string new_session_id(unsigned number)
{
    static std::mt19937_64 generator{std::random_device{}() ^ static_cast<unsigned long long>(GetTickCount64())};
    char text[48];
    sprintf_s(text, "%u-%016llx", number, static_cast<unsigned long long>(generator()));
    return text;
}

// A name as the player sees it: one line, spaces collapsed, at most 48 characters.
std::string clean_name(const std::string& text)
{
    std::string out;
    for (const unsigned char c : text) {
        if (c < 32 || c == '"')
            continue;
        if (c == ' ' && (out.empty() || out.back() == ' '))
            continue;
        out += static_cast<char>(c);
        if (out.size() >= 48)
            break;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

// Work for the game's thread (on_game_thread), run at the next frame.
struct Job {
    const std::function<void()>* work = nullptr;
    HANDLE done = nullptr;
};
std::mutex g_jobs_lock;
std::deque<std::shared_ptr<Job>> g_jobs;

// Lines for the player's chat log, said at the next frame (the server's own news: listening, a client connected).
std::mutex g_notes_lock;
std::vector<std::string> g_notes;

void note(std::string text)
{
    log::write(L"MCP: %hs", text.c_str());
    const std::lock_guard lock(g_notes_lock);
    g_notes.push_back(std::move(text));
}

std::string narrow(const std::wstring& text)
{
    std::string out;
    for (const wchar_t c : text)
        out += c < 128 ? static_cast<char>(c) : '?';
    return out;
}

std::string lower(std::string text)
{
    for (char& c : text)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// ---- HTTP ------------------------------------------------------------------------------------

struct Request {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers; // names in lower case
    std::string body;

    const std::string* header(std::string_view name) const
    {
        for (const auto& [key, value] : headers)
            if (key == name)
                return &value;
        return nullptr;
    }
};

class Connection {
public:
    explicit Connection(SOCKET socket) : socket_(socket) {}
    ~Connection() { closesocket(socket_); }

    // The next request on the connection. False when it closes, times out or sends something that isn't HTTP.
    bool read(Request& request)
    {
        size_t end = std::string::npos;
        while ((end = buffer_.find("\r\n\r\n")) == std::string::npos) {
            if (buffer_.size() > 64 * 1024 || !more())
                return false;
        }
        const std::string head = buffer_.substr(0, end);
        buffer_.erase(0, end + 4);
        size_t line_end = head.find("\r\n");
        const std::string first = head.substr(0, line_end);
        const size_t space1 = first.find(' ');
        const size_t space2 = space1 == std::string::npos ? std::string::npos : first.find(' ', space1 + 1);
        if (space2 == std::string::npos)
            return false;
        request = Request{};
        request.method = first.substr(0, space1);
        request.path = first.substr(space1 + 1, space2 - space1 - 1);
        while (line_end != std::string::npos) {
            const size_t start = line_end + 2;
            line_end = head.find("\r\n", start);
            const std::string line = head.substr(start, line_end == std::string::npos ? std::string::npos : line_end - start);
            const size_t colon = line.find(':');
            if (colon == std::string::npos)
                continue;
            std::string value = line.substr(colon + 1);
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.erase(value.begin());
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
                value.pop_back();
            request.headers.emplace_back(lower(line.substr(0, colon)), value);
        }
        if (const std::string* encoding = request.header("transfer-encoding"); encoding != nullptr && lower(*encoding) == "chunked")
            return read_chunked(request.body);
        size_t length = 0;
        if (const std::string* value = request.header("content-length"))
            length = static_cast<size_t>(std::strtoull(value->c_str(), nullptr, 10));
        if (length > kMaxBody)
            return false;
        while (buffer_.size() < length)
            if (!more())
                return false;
        request.body = buffer_.substr(0, length);
        buffer_.erase(0, length);
        return true;
    }

    bool send(const std::string& data)
    {
        size_t sent = 0;
        while (sent < data.size()) {
            WSABUF part{static_cast<ULONG>(std::min<size_t>(data.size() - sent, 1 << 20)), const_cast<char*>(data.data() + sent)};
            DWORD count = 0;
            if (WSASend(socket_, &part, 1, &count, 0, nullptr, nullptr) != 0 || count == 0)
                return false;
            sent += count;
        }
        return true;
    }

private:
    // More bytes from the socket. WSARecv rather than recv: VanaCore's loader traces recv (loader.cpp).
    bool more()
    {
        char chunk[16384];
        WSABUF part{sizeof(chunk), chunk};
        DWORD count = 0, flags = 0;
        if (WSARecv(socket_, &part, 1, &count, &flags, nullptr, nullptr) != 0 || count == 0)
            return false;
        buffer_.append(chunk, count);
        return true;
    }

    bool read_chunked(std::string& body)
    {
        for (;;) {
            size_t end = std::string::npos;
            while ((end = buffer_.find("\r\n")) == std::string::npos)
                if (buffer_.size() > 1024 || !more())
                    return false;
            const size_t size = static_cast<size_t>(std::strtoull(buffer_.substr(0, end).c_str(), nullptr, 16));
            buffer_.erase(0, end + 2);
            if (body.size() + size > kMaxBody)
                return false;
            while (buffer_.size() < size + 2)
                if (!more())
                    return false;
            body.append(buffer_, 0, size);
            buffer_.erase(0, size + 2);
            if (size == 0)
                return true; // (trailers aren't used)
        }
    }

    SOCKET socket_;
    std::string buffer_;
};

const char* reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 415: return "Unsupported Media Type";
    default: return "Error";
    }
}

std::string response(int status, const std::string& body, bool keep_alive, const char* extra = "")
{
    std::string out = "HTTP/1.1 " + std::to_string(status) + " " + reason(status) + "\r\n";
    if (!body.empty())
        out += "Content-Type: application/json\r\n";
    out += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    out += extra;
    out += "\r\n";
    out += body;
    return out;
}

// Only this machine's own clients, never a web page: a page anywhere can make the browser send requests to
// 127.0.0.1, so the Host must be this server's own (a DNS rebinding's would be its name), an Origin (which
// browsers send and Claude Code doesn't) must be this machine's, and the body must be JSON (which a page can't
// send without asking first, and nothing here answers that).
int check(const Request& request)
{
    const std::string* host = request.header("host");
    if (host == nullptr)
        return 403;
    const std::string port = ":" + std::to_string(g_settings.port);
    const std::string name = lower(*host);
    const bool local_host = name == "127.0.0.1" + port || name == "localhost" + port || name == "[::1]" + port ||
                            name == "127.0.0.1" || name == "localhost";
    if (!local_host)
        return 403;
    if (const std::string* origin = request.header("origin")) {
        const std::string from = lower(*origin);
        if (from != "null" && from.rfind("http://127.0.0.1", 0) != 0 && from.rfind("http://localhost", 0) != 0 &&
            from.rfind("http://[::1]", 0) != 0)
            return 403;
    }
    if (!g_token.empty()) {
        const std::string* authorization = request.header("authorization");
        if (authorization == nullptr || *authorization != "Bearer " + g_token)
            return 401;
    }
    return 0;
}

// ---- JSON-RPC and MCP ------------------------------------------------------------------------

const char kInstructions[] =
    "A live view into a running Final Fantasy XI client, from inside its process (32-bit), through VanaCore "
    "(CatsEyeXI's client framework).\n"
    "Addresses: every address argument is an expression. Numbers are hex (0x optional; #123 is decimal); a module's "
    "name is its base (FFXiMain.dll or FFXiMain); + - * and ( ); [expr] reads a 32-bit pointer; ghidra(0x10123456) "
    "turns an address from a disassembly of the module's file (Ghidra, IDA: the file's preferred base) into where it's "
    "loaded now; entities, party, player and target are the game structures VanaCore found. Example: "
    "[[FFXiMain.dll+0x4A1234]+0x10]+4. Results name addresses as module+offset, with the file's address (VA) beside "
    "them.\n"
    "Reverse engineering: search_memory for a string or value, find_references to it, then disassemble around the "
    "reference; watch an address (a hardware breakpoint) to see which code writes, reads or runs it, with its registers "
    "and stack; value_scan to find where a value that changes in game lives; packets and chat_log see what the server "
    "sends; lua runs LuaJIT inside the game (FFI to call game functions; the vana.* API). Tools that touch the game's "
    "own state (game_state, entities, lua, run_command, screenshot) run at the next frame.";

json::Value rpc_error(const json::Value& id, int code, const std::string& message)
{
    json::Value out;
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"]["code"] = code;
    out["error"]["message"] = message;
    return out;
}

json::Value rpc_result(const json::Value& id, json::Value result)
{
    json::Value out;
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(result);
    return out;
}

bool known_version(const std::string& version)
{
    static const char* const kVersions[] = {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
    for (const char* known : kVersions)
        if (version == known)
            return true;
    return false;
}

// A tool, guarded: a hardware exception in it (it reads the game's memory) is its error, not the game's crash.
struct ToolCall {
    const std::string* name;
    const json::Value* arguments;
    json::Value result;
    std::string failure;
};

void run_tool(void* data)
{
    auto& call = *static_cast<ToolCall*>(data);
    try {
        call.result = tools::call(*call.name, *call.arguments);
    } catch (const std::exception& error) {
        call.failure = error.what();
    }
}

bool guarded(void (*function)(void*), void* data, DWORD& code)
{
    __try {
        function(data);
        return true;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One message: its response, or null for a notification (or a client's response). An initialize sets `new_session`
// to the session it opened (its id goes back as the Mcp-Session-Id header).
json::Value handle(const json::Value& message, std::string& new_session)
{
    if (!message.is_object())
        return rpc_error(nullptr, -32600, "not a JSON-RPC message");
    const json::Value* id = message.find("id");
    const std::string method = message.get_string("method");
    if (id == nullptr || id->is_null() || method.empty())
        return {}; // notifications/initialized and the like, or a response
    const json::Value* params = message.find("params");
    static const json::Value kNone = json::Value::make_object();
    const json::Value& args = params != nullptr && params->is_object() ? *params : kNone;

    if (method == "initialize") {
        json::Value result;
        const std::string asked = args.get_string("protocolVersion");
        result["protocolVersion"] = known_version(asked) ? asked : std::string("2025-06-18");
        result["capabilities"]["tools"]["listChanged"] = false;
        result["serverInfo"]["name"] = "vanacore";
        result["serverInfo"]["title"] = "VanaCore (FFXI client)";
        result["serverInfo"]["version"] = narrow(kVersion);
        Session session;
        session.client = "a client";
        if (const json::Value* info = args.find("clientInfo")) {
            session.client = clean_name(info->get_string("name", session.client));
            session.name = clean_name(info->get_string("title"));
        }
        session.last = GetTickCount64();
        std::string text;
        {
            const std::lock_guard lock(g_sessions_lock);
            session.number = ++g_session_count;
            session.id = new_session_id(session.number);
            if (g_sessions.size() >= kMaxSessions) {
                // full: the one idle longest goes
                auto oldest = g_sessions.begin();
                for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it)
                    if (it->last < oldest->last)
                        oldest = it;
                g_sessions.erase(oldest);
            }
            g_sessions.push_back(session);
            text = tag(session);
            g_client = text;
        }
        new_session = session.id;
        t_session = session.id;
        // The chat is told who it is to the player, and asked to say what it's about.
        std::string instructions = kInstructions;
        instructions += "\nTo the player, this chat is MCP session #" + std::to_string(session.number);
        if (!session.name.empty())
            instructions += " \"" + session.name + "\"";
        instructions += ". Before your first use of any other tool here, call hello with a few words for this chat's "
                        "topic (\"alliance list RE\"): their chat log and /vana window then show which chat is probing.";
        result["instructions"] = instructions;
        note(text + " connected");
        return rpc_result(*id, std::move(result));
    }
    if (method == "ping")
        return rpc_result(*id, json::Value::make_object());
    if (method == "tools/list") {
        json::Value result;
        result["tools"] = tools::list();
        return rpc_result(*id, std::move(result));
    }
    if (method == "tools/call") {
        const std::string name = args.get_string("name");
        const json::Value* arguments = args.find("arguments");
        static const json::Value kEmpty = json::Value::make_object();
        ToolCall call{&name, arguments != nullptr && arguments->is_object() ? arguments : &kEmpty, {}, {}};
        std::string who;
        {
            const std::lock_guard lock(g_sessions_lock);
            if (Session* session = current_session()) {
                ++session->calls;
                who = tag(*session);
            }
        }
        DWORD code = 0;
        if (!guarded(&run_tool, &call, code)) {
            char text[96];
            sprintf_s(text, "the tool crashed (exception 0x%08lX); the game carries on", static_cast<unsigned long>(code));
            call.failure = text;
            log::write(L"MCP: %hs crashed (0x%08lX) for %hs", name.c_str(), static_cast<unsigned long>(code), who.c_str());
        }
        if (!call.failure.empty()) {
            json::Value result;
            json::Value& item = result["content"].push(json::Value::make_object());
            item["type"] = "text";
            item["text"] = call.failure;
            result["isError"] = true;
            return rpc_result(*id, std::move(result));
        }
        return rpc_result(*id, std::move(call.result));
    }
    if (method == "resources/list") {
        json::Value result;
        result["resources"] = json::Value::make_array();
        return rpc_result(*id, std::move(result));
    }
    if (method == "prompts/list") {
        json::Value result;
        result["prompts"] = json::Value::make_array();
        return rpc_result(*id, std::move(result));
    }
    return rpc_error(*id, -32601, "no method " + method);
}

void serve(SOCKET socket)
{
    {
        Connection connection(socket);
        Request request;
        while (g_running && connection.read(request)) {
            const ULONGLONG now = GetTickCount64();
            g_last_request = now;
            const std::string* connection_header = request.header("connection");
            const bool keep_alive = connection_header == nullptr || lower(*connection_header) != "close";
            std::string reply;
            const std::string path = request.path.substr(0, request.path.find('?'));
            // The session the request names, if one: it's "used" now. An unknown or missing id isn't refused (a script
            // that initializes on each run, an id from before the server restarted): the newest session stands in.
            const std::string* session_header = request.header("mcp-session-id");
            t_session = session_header != nullptr ? *session_header : std::string();
            std::string ended; // a DELETE's session, as the player sees it
            if (const int refused = check(request); refused != 0) {
                reply = response(refused, "", false);
            } else if (path != "/mcp" && path != "/") {
                reply = response(404, "", keep_alive);
            } else if (request.method == "DELETE") {
                // the client ends its session
                {
                    const std::lock_guard lock(g_sessions_lock);
                    for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it) {
                        if (!t_session.empty() && it->id == t_session) {
                            ended = tag(*it);
                            g_sessions.erase(it);
                            break;
                        }
                    }
                }
                reply = response(ended.empty() ? 404 : 200, "", keep_alive);
            } else if (request.method != "POST") {
                // no stream of the server's own messages (GET)
                reply = response(405, "", keep_alive, "Allow: POST, DELETE\r\n");
            } else {
                {
                    const std::lock_guard lock(g_sessions_lock);
                    if (Session* session = t_session.empty() ? nullptr : find_session(t_session)) {
                        session->last = now;
                        g_client = tag(*session);
                    }
                }
                const std::string* type = request.header("content-type");
                json::Value message;
                std::string error;
                std::string new_session;
                if (type == nullptr || lower(*type).find("application/json") == std::string::npos) {
                    reply = response(415, "", keep_alive);
                } else if (!json::parse(request.body, message, error)) {
                    reply = response(400, json::dump(rpc_error(nullptr, -32700, "parse error: " + error)), keep_alive);
                } else if (message.is_array()) {
                    json::Value replies = json::Value::make_array();
                    for (const json::Value& each : message.array) {
                        json::Value answer = handle(each, new_session);
                        if (!answer.is_null())
                            replies.push(std::move(answer));
                    }
                    const std::string extra = new_session.empty() ? "" : "Mcp-Session-Id: " + new_session + "\r\n";
                    reply = replies.array.empty() ? response(202, "", keep_alive, extra.c_str())
                                                  : response(200, json::dump(replies), keep_alive, extra.c_str());
                } else {
                    const json::Value answer = handle(message, new_session);
                    const std::string extra = new_session.empty() ? "" : "Mcp-Session-Id: " + new_session + "\r\n";
                    reply = answer.is_null() ? response(202, "", keep_alive, extra.c_str())
                                             : response(200, json::dump(answer), keep_alive, extra.c_str());
                }
            }
            if (!ended.empty())
                note(ended + " left");
            if (!connection.send(reply) || reply.find("Connection: close") != std::string::npos)
                break;
        }
    }
    --g_connections;
}

void listen_thread(SOCKET listener, unsigned generation)
{
    while (g_running && g_generation == generation) {
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!g_running || g_generation != generation)
                break; // stopped: the socket was closed under it
            Sleep(100);
            continue;
        }
        if (g_connections >= kMaxConnections) {
            closesocket(client);
            continue;
        }
        const DWORD timeout = 120000; // an idle connection is let go after 2 minutes
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        const BOOL no_delay = TRUE;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
        ++g_connections;
        std::thread(serve, client).detach();
    }
}

bool g_announced = false;
std::wstring g_ini;
ULONGLONG g_ini_checked = 0;

// [mcp] available=0 (SoulspireXI default): a server that doesn't want AI tools in its players' games turns the whole
// feature off. The server never starts, whatever enabled says, and the /vana window leaves its AI MCP rows out.
// Developers and Client Sentinel explicitly opt in via [mcp] available=1 in their local test INI.
bool available(const wchar_t* ini)
{
    return GetPrivateProfileIntW(L"mcp", L"available", 0, ini) != 0;
}

// [mcp] enabled=1 and available (the default)
bool wanted(const wchar_t* ini)
{
    return available(ini) && GetPrivateProfileIntW(L"mcp", L"enabled", 0, ini) != 0;
}

// Listens with [mcp]'s port and token. False (logged) if no port could be had.
bool start()
{
    g_settings.port = GetPrivateProfileIntW(L"mcp", L"port", 47800, g_ini.c_str());
    wchar_t token[256] = {};
    GetPrivateProfileStringW(L"mcp", L"token", L"", token, 256, g_ini.c_str());
    g_token = narrow(token);
    static bool winsock = false;
    if (!winsock) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            log::write(L"MCP: winsock didn't start; no server");
            g_failed = true;
            return false;
        }
        winsock = true;
    }
    // The port asked for, or the next free one (a second game on this machine)
    const unsigned asked = g_settings.port;
    SOCKET listener = INVALID_SOCKET;
    for (unsigned port = asked; port < asked + 10; ++port) {
        listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == INVALID_SOCKET)
            break;
        const BOOL exclusive = TRUE;
        setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in at{};
        at.sin_family = AF_INET;
        at.sin_port = htons(static_cast<u_short>(port));
        at.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) == 0 && ::listen(listener, 16) == 0) {
            g_settings.port = port;
            break;
        }
        closesocket(listener);
        listener = INVALID_SOCKET;
    }
    if (listener == INVALID_SOCKET) {
        log::write(L"MCP: couldn't listen on 127.0.0.1:%u-%u; no server", asked, asked + 9);
        g_failed = true; // not tried again until [mcp] enabled is turned off and on
        return false;
    }
    g_listener = listener;
    const unsigned generation = ++g_generation;
    g_running = true;
    g_failed = false;
    g_announced = false;
    std::thread(listen_thread, listener, generation).detach();
    log::write(L"MCP: listening on http://127.0.0.1:%u/mcp%ls", g_settings.port, g_token.empty() ? L"" : L" (token required)");
    return true;
}

// [mcp] enabled=0 while the game runs: no more connections, and the open ones end at their next request.
void stop()
{
    g_running = false;
    ++g_generation;
    if (g_listener != INVALID_SOCKET)
        closesocket(g_listener);
    g_listener = INVALID_SOCKET;
    note("server stopped");
}

} // namespace

const Settings& settings()
{
    return g_settings;
}

bool on_game_thread(const std::function<void()>& work, DWORD timeout_ms)
{
    auto job = std::make_shared<Job>();
    job->work = &work;
    job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (job->done == nullptr)
        return false;
    {
        const std::lock_guard lock(g_jobs_lock);
        g_jobs.push_back(job);
    }
    bool ran = WaitForSingleObject(job->done, timeout_ms) == WAIT_OBJECT_0;
    if (!ran) {
        // Not run in time: taken back, unless the game's thread is running it right now.
        bool taken = false;
        {
            const std::lock_guard lock(g_jobs_lock);
            const auto it = std::find(g_jobs.begin(), g_jobs.end(), job);
            if (it != g_jobs.end()) {
                g_jobs.erase(it);
                taken = true;
            }
        }
        if (!taken)
            ran = WaitForSingleObject(job->done, INFINITE) == WAIT_OBJECT_0;
    }
    CloseHandle(job->done);
    return ran;
}

void install(const wchar_t* install_dir, const wchar_t* ini)
{
    g_settings.install_dir = install_dir;
    if (ini == nullptr || ini[0] == L'\0')
        return;
    g_ini = ini;
    g_ini_checked = GetTickCount64();
    if (wanted(ini))
        start();
}

bool running()
{
    return g_running;
}

Status status()
{
    Status out;
    out.running = g_running;
    out.failed = g_failed;
    out.port = g_settings.port;
    const ULONGLONG now = GetTickCount64();
    {
        const std::lock_guard lock(g_sessions_lock);
        out.client = g_client;
        for (const Session& session : g_sessions) {
            Status::Session& item = out.sessions.emplace_back();
            item.number = session.number;
            item.client = session.client;
            item.name = session.name;
            item.idle_seconds = static_cast<double>(now - session.last) / 1000.0;
            item.calls = session.calls;
        }
    }
    const ULONGLONG last = g_last_request;
    if (last != 0)
        out.idle_seconds = static_cast<double>(now - last) / 1000.0;
    return out;
}

std::string name_session(const std::string& name)
{
    const std::string clean = clean_name(name);
    if (clean.empty())
        return {}; // the tool's error
    std::string text;
    {
        const std::lock_guard lock(g_sessions_lock);
        Session* session = current_session();
        if (session == nullptr)
            return "No session: initialize first.";
        if (session->name == clean)
            return "The player already sees this chat as " + tag(*session) + ".";
        session->name = clean;
        text = tag(*session);
        g_client = text;
        note("#" + std::to_string(session->number) + " " + session->client + " is \"" + clean + "\"");
    }
    return "The player now sees this chat as " + text + ".";
}

void on_frame()
{
    // [mcp] enabled and available, changed while the game runs (the /vana window, or the file): the server follows them
    const ULONGLONG now = GetTickCount64();
    if (!g_ini.empty() && now - g_ini_checked >= 2000) {
        g_ini_checked = now;
        const bool want = wanted(g_ini.c_str());
        if (want && !g_running && !g_failed) {
            start();
        } else if (!want) {
            g_failed = false;
            if (g_running)
                stop();
        }
    }
    for (;;) {
        std::shared_ptr<Job> job;
        {
            const std::lock_guard lock(g_jobs_lock);
            if (g_jobs.empty())
                break;
            job = std::move(g_jobs.front());
            g_jobs.pop_front();
        }
        try {
            (*job->work)();
        } catch (...) {
            log::write(L"MCP: a tool's work on the game's thread failed");
        }
        SetEvent(job->done);
    }
    if (g_running)
        tools::on_frame();
    if (!chat::ready())
        return;
    if (g_running && !g_announced) {
        g_announced = true;
        chat::echo("MCP server on http://127.0.0.1:" + std::to_string(g_settings.port) + "/mcp");
    }
    std::vector<std::string> notes;
    {
        const std::lock_guard lock(g_notes_lock);
        notes.swap(g_notes);
    }
    for (const std::string& text : notes)
        chat::echo("MCP: " + text);
}

void on_packet(bool incoming, const std::uint8_t* packet, std::size_t length)
{
    if (g_running)
        tools::on_packet(incoming, packet, length);
}

void on_chat(std::string_view line, int mode)
{
    if (g_running)
        tools::on_chat(line, mode);
}

} // namespace vanacore::mcp
