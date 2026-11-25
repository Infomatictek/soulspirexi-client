#include "login.hpp"

#include "../shared/vanacore/tls_client.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vanacore::login {
namespace {

constexpr int kLoginCommand = 0x10;
constexpr DWORD kTimeoutMs = 15000;

// The server's result codes (a login's).
enum : int {
    kResultFail = 0x00, // no such account, or it's banned
    kResultSuccess = 0x01,
    kResultWrongPassword = 0x02,
    kResultAlreadyLoggedIn = 0x0A,
    kResultVersionUnsupported = 0x0B,
};

std::string utf8(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length, nullptr, nullptr);
    return out;
}

std::wstring wide(const std::string& text)
{
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::wstring hex(unsigned long value)
{
    wchar_t text[16];
    swprintf_s(text, L"0x%08lX", value);
    return text;
}

// --- JSON: just what the login's request and reply need ------------------------------------------

std::string json_string(const std::string& text)
{
    std::string out = "\"";
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                sprintf_s(escaped, "\\u%04x", static_cast<unsigned char>(c));
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    return out + "\"";
}

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    std::string text;
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    const Value* member(std::string_view name) const
    {
        for (const auto& [key, value] : members)
            if (key == name)
                return &value;
        return nullptr;
    }
};

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    bool parse(Value& out)
    {
        if (!value(out, 0))
            return false;
        space();
        return at_ == text_.size();
    }

private:
    void space()
    {
        while (at_ < text_.size() && (text_[at_] == ' ' || text_[at_] == '\t' || text_[at_] == '\n' || text_[at_] == '\r'))
            ++at_;
    }

    bool literal(std::string_view word)
    {
        if (text_.substr(at_, word.size()) != word)
            return false;
        at_ += word.size();
        return true;
    }

    static void append_utf8(std::string& out, unsigned long code)
    {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    bool hex4(unsigned long& code)
    {
        if (at_ + 4 > text_.size())
            return false;
        code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text_[at_++];
            code <<= 4;
            if (c >= '0' && c <= '9')
                code |= static_cast<unsigned long>(c - '0');
            else if (c >= 'a' && c <= 'f')
                code |= static_cast<unsigned long>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                code |= static_cast<unsigned long>(c - 'A' + 10);
            else
                return false;
        }
        return true;
    }

    bool string(std::string& out)
    {
        if (at_ >= text_.size() || text_[at_] != '"')
            return false;
        ++at_;
        while (at_ < text_.size()) {
            const char c = text_[at_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (at_ >= text_.size())
                return false;
            const char escaped = text_[at_++];
            switch (escaped) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned long code = 0;
                if (!hex4(code))
                    return false;
                // a surrogate pair's second half
                if (code >= 0xD800 && code <= 0xDBFF && literal("\\u")) {
                    unsigned long low = 0;
                    if (!hex4(low))
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                append_utf8(out, code);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool value(Value& out, int depth)
    {
        if (depth > 32)
            return false;
        space();
        if (at_ >= text_.size())
            return false;
        const char c = text_[at_];
        if (c == '{') {
            ++at_;
            out.type = Value::Type::Object;
            space();
            if (at_ < text_.size() && text_[at_] == '}') {
                ++at_;
                return true;
            }
            for (;;) {
                space();
                std::string key;
                if (!string(key))
                    return false;
                space();
                if (at_ >= text_.size() || text_[at_++] != ':')
                    return false;
                Value member;
                if (!value(member, depth + 1))
                    return false;
                out.members.emplace_back(std::move(key), std::move(member));
                space();
                if (at_ >= text_.size())
                    return false;
                if (text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (text_[at_++] != '}')
                    return false;
                return true;
            }
        }
        if (c == '[') {
            ++at_;
            out.type = Value::Type::Array;
            space();
            if (at_ < text_.size() && text_[at_] == ']') {
                ++at_;
                return true;
            }
            for (;;) {
                Value item;
                if (!value(item, depth + 1))
                    return false;
                out.items.push_back(std::move(item));
                space();
                if (at_ >= text_.size())
                    return false;
                if (text_[at_] == ',') {
                    ++at_;
                    continue;
                }
                if (text_[at_++] != ']')
                    return false;
                return true;
            }
        }
        if (c == '"') {
            out.type = Value::Type::String;
            return string(out.text);
        }
        if (literal("true")) {
            out.type = Value::Type::Bool;
            out.number = 1;
            return true;
        }
        if (literal("false")) {
            out.type = Value::Type::Bool;
            return true;
        }
        if (literal("null")) {
            out.type = Value::Type::Null;
            return true;
        }
        // a number
        const size_t start = at_;
        while (at_ < text_.size() && std::strchr("+-0123456789.eE", text_[at_]) != nullptr)
            ++at_;
        if (at_ == start)
            return false;
        out.type = Value::Type::Number;
        out.number = std::strtod(std::string(text_.substr(start, at_ - start)).c_str(), nullptr);
        return true;
    }

    std::string_view text_;
    size_t at_ = 0;
};

// True once `text` holds a whole JSON object: its braces close, outside strings.
bool whole_object(std::string_view text)
{
    int depth = 0;
    bool in_string = false, escaped = false, opened = false;
    for (const char c : text) {
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                in_string = false;
            continue;
        }
        if (c == '"')
            in_string = true;
        else if (c == '{')
            ++depth, opened = true;
        else if (c == '}' && --depth == 0 && opened)
            return true;
    }
    return false;
}

// --- TLS transport is shared with VanaCore.Core (src/shared/vanacore/tls_client.hpp) -------------

ErrorCategory map_tls_error(vanacore::tls::ErrorKind kind)
{
    switch (kind) {
    case vanacore::tls::ErrorKind::SocketFailed:
    case vanacore::tls::ErrorKind::TcpConnectFailed:
        return ErrorCategory::TcpConnectFailed;
    case vanacore::tls::ErrorKind::TlsInitFailed:
        return ErrorCategory::TlsInitFailed;
    case vanacore::tls::ErrorKind::TlsHandshakeFailed:
        return ErrorCategory::TlsHandshakeFailed;
    case vanacore::tls::ErrorKind::ServerKeyMismatch:
        return ErrorCategory::ServerKeyMismatch;
    case vanacore::tls::ErrorKind::IoFailed:
    case vanacore::tls::ErrorKind::None:
    default:
        return ErrorCategory::ProtocolError;
    }
}

struct Winsock {
    bool ready = false;
    Winsock()
    {
        WSADATA data{};
        ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~Winsock()
    {
        if (ready)
            WSACleanup();
    }
};

} // namespace

bool resolve(const std::wstring& server, std::uint32_t& address, std::wstring& message)
{
    Winsock winsock;
    if (!winsock.ready) {
        message = L"Windows' networking (winsock) wouldn't start.";
        return false;
    }
    addrinfoW hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* found = nullptr;
    const int error = GetAddrInfoW(server.c_str(), nullptr, &hints, &found);
    if (error != 0 || found == nullptr) {
        message = L"Couldn't find the server " + server + L" (winsock error " + std::to_wstring(error) +
                  L"). Check your internet connection.";
        return false;
    }
    address = reinterpret_cast<const sockaddr_in*>(found->ai_addr)->sin_addr.s_addr;
    FreeAddrInfoW(found);
    return true;
}

bool parse_version(const std::wstring& text, Version& version)
{
    Version parsed = {0, 0, 0};
    size_t part = 0, at = 0;
    for (;;) {
        const size_t dot = text.find(L'.', at);
        const std::wstring number = text.substr(at, dot == std::wstring::npos ? std::wstring::npos : dot - at);
        if (part == parsed.size() || number.empty() || number.size() > 3 ||
            number.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;
        parsed[part] = std::stoi(number);
        if (parsed[part++] > 255)
            return false;
        if (dot == std::wstring::npos)
            break;
        at = dot + 1;
    }
    if (part < 2)
        return false;
    version = parsed;
    return true;
}

Result log_in(const std::wstring& server, std::uint32_t address, std::uint16_t port, const std::wstring& user,
              const std::wstring& password, const Version& version)
{
    Result result;
    Winsock winsock;
    if (!winsock.ready) {
        result.message = L"Windows' networking (winsock) wouldn't start.";
        return result;
    }
    const std::wstring where = server + L":" + std::to_wstring(port);

    vanacore::tls::Client tls;
    std::wstring error;
    if (!tls.connect(address, port, server, error, nullptr, kTimeoutMs)) {
        result.error_category = map_tls_error(tls.error_kind());
        result.message = L"Couldn't reach the login server at " + where + L": " + error + L".";
        return result;
    }
    if (tls.negotiated_protocol() != 0)
        result.tls_protocol = vanacore::tls::protocol_name(tls.negotiated_protocol());
    if (tls.has_peer_key()) {
        result.has_server_key = true;
        std::memcpy(result.server_key_sha256, tls.peer_key_sha256().data(), sizeof(result.server_key_sha256));
    }

    std::string request = "{\"command\":" + std::to_string(kLoginCommand) + ",\"username\":" + json_string(utf8(user)) +
                          ",\"password\":" + json_string(utf8(password)) + ",\"otp\":\"\",\"new_password\":\"\"" +
                          // Trusted computer fields (xiloader 2.1+ / 3.0.x protocol)
                          ",\"trust_token\":\"\",\"trust_this_computer\":false" +
                          ",\"version\":[" + std::to_string(version[0]) + "," + std::to_string(version[1]) + "," +
                          std::to_string(version[2]) + "]}";
    const bool sent = tls.write(request);
    SecureZeroMemory(request.data(), request.size());
    if (!sent) {
        result.error_category = ErrorCategory::ProtocolError;
        result.message = L"Couldn't send the login to " + where + L".";
        return result;
    }

    std::string reply;
    if (!tls.read_until(reply, [](const std::string& text) { return whole_object(text); }, error)) {
        result.error_category = ErrorCategory::ProtocolError;
        result.message = reply.empty() ? L"The login server at " + where + L" didn't reply" +
                                             (error.empty() ? L"" : L" (" + error + L")") + L"."
                                       : L"The login server's reply made no sense.";
        return result;
    }
    Value json;
    if (!Parser(reply).parse(json) || json.type != Value::Type::Object) {
        result.error_category = ErrorCategory::ProtocolError;
        result.message = L"The login server's reply made no sense.";
        return result;
    }

    // A message of the server's own (an unsupported version, maintenance, ...)
    if (const Value* message = json.member("error_message");
        message != nullptr && message->type == Value::Type::String && !message->text.empty()) {
        result.outcome = Outcome::Refused;
        result.error_category = ErrorCategory::AuthRefused;
        result.message = wide(message->text);
        return result;
    }
    const Value* code = json.member("result");
    if (code == nullptr || code->type != Value::Type::Number) {
        result.error_category = ErrorCategory::ProtocolError;
        result.message = L"The login server's reply had no result.";
        return result;
    }
    switch (static_cast<int>(code->number)) {
    case kResultSuccess: {
        const Value* id = json.member("account_id");
        const Value* hash = json.member("session_hash");
        if (id == nullptr || id->type != Value::Type::Number || hash == nullptr || hash->type != Value::Type::Array ||
            hash->items.size() != sizeof(result.session_hash)) {
            result.error_category = ErrorCategory::ProtocolError;
            result.message = L"The login server accepted the login but sent no account or session.";
            return result;
        }
        result.account_id = static_cast<std::uint32_t>(id->number);
        for (size_t i = 0; i < hash->items.size(); ++i)
            result.session_hash[i] = static_cast<std::uint8_t>(static_cast<int>(hash->items[i].number) & 0xFF);
        result.outcome = Outcome::LoggedIn;
        result.error_category = ErrorCategory::None;
        return result;
    }
    case kResultWrongPassword:
        result.outcome = Outcome::Refused;
        result.error_category = ErrorCategory::AuthRefused;
        result.message = L"Wrong username or password.";
        return result;
    case kResultFail:
        result.outcome = Outcome::Refused;
        result.error_category = ErrorCategory::AuthRefused;
        result.message = L"This account can't log in (it doesn't exist, or it's suspended).";
        return result;
    case kResultAlreadyLoggedIn:
        result.outcome = Outcome::Refused;
        result.error_category = ErrorCategory::AuthRefused;
        result.message = L"This account is already logged in.";
        return result;
    case kResultVersionUnsupported:
        result.outcome = Outcome::Refused;
        result.error_category = ErrorCategory::AuthRefused;
        result.message = L"The server doesn't support this SoulspireXI Client login protocol version.";
        return result;
    default:
        result.outcome = Outcome::Refused;
        result.error_category = ErrorCategory::AuthRefused;
        result.message = L"The login server refused the login (code " + std::to_wstring(static_cast<int>(code->number)) +
                         L").";
        return result;
    }
}

} // namespace vanacore::login
