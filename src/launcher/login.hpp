#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace vanacore::login {

// A bootloader version, as the login server checks it (major.minor).
using Version = std::array<int, 3>;

// The version VanaCore reports: LandSandBoat xiloader 3.0.0 protocol (TLS 1.3 auth + TLS 1.3 lobby).
// Override via [server] loginversion= only if connecting to a legacy server.
inline constexpr Version kVersion = {3, 0, 0};

// "3.0.0" (or "3.0", its patch 0) as a version; false if it isn't one (each part 0-255).
bool parse_version(const std::wstring& text, Version& version);

enum class Outcome {
    LoggedIn,
    Refused,       // the server said no: wrong username or password, banned, already logged in, ...
    ServerProblem, // it couldn't be reached, or its reply made no sense
};

enum class ErrorCategory {
    None,
    DnsFailed,
    TcpConnectFailed,
    TlsInitFailed,
    TlsHandshakeFailed,
    ServerKeyMismatch,
    ProtocolError,
    AuthRefused,
};

struct Result {
    Outcome outcome = Outcome::ServerProblem;
    ErrorCategory error_category = ErrorCategory::None;
    std::wstring message; // for the player when it isn't LoggedIn
    std::wstring tls_protocol;
    std::uint32_t account_id = 0;
    std::uint8_t session_hash[16] = {};
    bool has_server_key = false;
    std::uint8_t server_key_sha256[32] = {};
};

// Resolves `server` (a host name or an IPv4 address) into `address` (network byte order); false,
// with the reason in `message`, if it can't.
bool resolve(const std::wstring& server, std::uint32_t& address, std::wstring& message);

// Logs in to the CatsEyeXI login server at `address`:`port` (TLS) as xiloader 2.1.2 does: one
// JSON request, one JSON reply with the account's id and the session's hash. `server` is the name
// the certificate would carry (not checked: the server's certificate is self-signed).
Result log_in(const std::wstring& server, std::uint32_t address, std::uint16_t port, const std::wstring& user,
              const std::wstring& password, const Version& version = kVersion);

} // namespace vanacore::login
