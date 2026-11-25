#pragma once

#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#ifndef SCHANNEL_USE_BLACKLISTS
#define SCHANNEL_USE_BLACKLISTS
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <bcrypt.h>
#include <schannel.h>
#include <security.h>
#include <wincrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace vanacore::tls {

enum class ErrorKind {
    None,
    SocketFailed,
    TcpConnectFailed,
    TlsInitFailed,
    TlsHandshakeFailed,
    ServerKeyMismatch,
    IoFailed,
};

using ConnectFn = int(WINAPI*)(SOCKET, const sockaddr*, int);

inline std::wstring format_hex(unsigned long value)
{
    wchar_t text[16];
    swprintf_s(text, L"0x%08lX", value);
    return text;
}

inline std::wstring protocol_name(DWORD protocol)
{
#ifdef SP_PROT_TLS1_3_CLIENT
    if ((protocol & SP_PROT_TLS1_3_CLIENT) != 0)
        return L"TLS 1.3";
#endif
    if ((protocol & SP_PROT_TLS1_2_CLIENT) != 0)
        return L"TLS 1.2";
    if ((protocol & SP_PROT_TLS1_1_CLIENT) != 0)
        return L"TLS 1.1";
    if ((protocol & SP_PROT_TLS1_0_CLIENT) != 0)
        return L"TLS 1.0";
    return L"TLS (" + format_hex(protocol) + L")";
}

inline bool sha256_bytes(const std::uint8_t* data, std::size_t size, std::uint8_t out[32])
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            if (BCryptHashData(hash, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0) == 0 &&
                BCryptFinishHash(hash, out, 32, 0) == 0) {
                ok = true;
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
}

// Acquires an outbound Windows Schannel credential handle supporting TLS 1.3 via SCH_CREDENTIALS (v5).
// Falls back to legacy SCHANNEL_CRED (v4) only if the OS Schannel rejects v5 with SEC_E_UNSUPPORTED_FUNCTION.
inline SECURITY_STATUS acquire_outbound_credentials(CredHandle& out_handle, bool& used_v5)
{
    used_v5 = false;
#ifdef SCH_CREDENTIALS_VERSION
    TLS_PARAMETERS tls_params{};
    // Disable legacy SSL/TLS < 1.2 while leaving TLS 1.2 and TLS 1.3 enabled so Schannel offers TLS 1.3 first.
    tls_params.grbitDisabledProtocols = SP_PROT_PCT1_CLIENT | SP_PROT_SSL2_CLIENT | SP_PROT_SSL3_CLIENT |
                                        SP_PROT_TLS1_0_CLIENT | SP_PROT_TLS1_1_CLIENT;

    SCH_CREDENTIALS creds{};
    creds.dwVersion = SCH_CREDENTIALS_VERSION;
    creds.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    creds.cTlsParameters = 1;
    creds.pTlsParameters = &tls_params;

    SECURITY_STATUS status = AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W),
                                                       SECPKG_CRED_OUTBOUND, nullptr, &creds, nullptr, nullptr,
                                                       &out_handle, nullptr);
    if (status == SEC_E_OK) {
        used_v5 = true;
        return SEC_E_OK;
    }
    if (status != SEC_E_UNSUPPORTED_FUNCTION)
        return status;
#endif
    SCHANNEL_CRED legacy{};
    legacy.dwVersion = SCHANNEL_CRED_VERSION;
    legacy.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
    legacy.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
    return AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr,
                                     &legacy, nullptr, nullptr, &out_handle, nullptr);
}

class Client {
public:
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    ~Client()
    {
        close();
    }

    void close()
    {
        if (have_context_) {
            DeleteSecurityContext(&context_);
            have_context_ = false;
        }
        if (have_credentials_) {
            FreeCredentialsHandle(&credentials_);
            have_credentials_ = false;
        }
        if (socket_ != INVALID_SOCKET) {
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        connected_ = false;
        incoming_.clear();
        decrypted_.clear();
    }

    void shutdown_socket()
    {
        if (socket_ != INVALID_SOCKET) {
            ::shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    bool connect(std::uint32_t address, std::uint16_t port, const std::wstring& host, std::wstring& error,
                 const std::uint8_t* expected_key_sha256 = nullptr, DWORD timeout_ms = 15000,
                 ConnectFn connect_fn = nullptr)
    {
        close();
        error_kind_ = ErrorKind::None;
        has_peer_key_ = false;
        negotiated_protocol_ = 0;

        socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (socket_ == INVALID_SOCKET) {
            error_kind_ = ErrorKind::SocketFailed;
            error = L"couldn't make a socket (winsock error " + std::to_wstring(WSAGetLastError()) + L")";
            return false;
        }
        if (timeout_ms > 0) {
            setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
            setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        }

        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = address;
        ConnectFn do_connect = connect_fn != nullptr ? connect_fn : &::connect;
        if (do_connect(socket_, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == SOCKET_ERROR) {
            error_kind_ = ErrorKind::TcpConnectFailed;
            error = L"TCP connection failed (winsock error " + std::to_wstring(WSAGetLastError()) + L")";
            return false;
        }
        connected_ = true;

        bool used_v5 = false;
        const SECURITY_STATUS cred_status = acquire_outbound_credentials(credentials_, used_v5);
        if (cred_status != SEC_E_OK) {
            error_kind_ = ErrorKind::TlsInitFailed;
            error = L"Windows TLS initialization failed (" + format_hex(static_cast<unsigned long>(cred_status)) + L")";
            return false;
        }
        have_credentials_ = true;
        host_ = host;

        if (!handshake({}, error)) {
            if (error_kind_ == ErrorKind::None)
                error_kind_ = ErrorKind::TlsHandshakeFailed;
            return false;
        }

        SecPkgContext_ConnectionInfo conn_info{};
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_CONNECTION_INFO, &conn_info) == SEC_E_OK)
            negotiated_protocol_ = conn_info.dwProtocol;

        PCCERT_CONTEXT cert = nullptr;
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert) == SEC_E_OK && cert != nullptr) {
            if (cert->pCertInfo != nullptr && cert->pCertInfo->SubjectPublicKeyInfo.PublicKey.pbData != nullptr &&
                cert->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData > 0) {
                has_peer_key_ = sha256_bytes(cert->pCertInfo->SubjectPublicKeyInfo.PublicKey.pbData,
                                             cert->pCertInfo->SubjectPublicKeyInfo.PublicKey.cbData,
                                             peer_key_sha256_.data());
            }
            CertFreeCertificateContext(cert);
        }

        if (expected_key_sha256 != nullptr) {
            if (!has_peer_key_ || std::memcmp(peer_key_sha256_.data(), expected_key_sha256, 32) != 0) {
                error_kind_ = ErrorKind::ServerKeyMismatch;
                error = L"server TLS public key does not match the authenticated login server";
                close();
                return false;
            }
        }

        return true;
    }

    bool connected() const { return connected_; }
    ErrorKind error_kind() const { return error_kind_; }
    DWORD negotiated_protocol() const { return negotiated_protocol_; }
    bool has_peer_key() const { return has_peer_key_; }
    const std::array<std::uint8_t, 32>& peer_key_sha256() const { return peer_key_sha256_; }

    bool write(const char* data, std::size_t size)
    {
        if (socket_ == INVALID_SOCKET || !have_context_ || sizes_.cbMaximumMessage == 0)
            return false;
        while (size > 0) {
            const std::size_t chunk = (std::min)(size, static_cast<std::size_t>(sizes_.cbMaximumMessage));
            std::vector<char> record(sizes_.cbHeader + chunk + sizes_.cbTrailer);
            std::memcpy(record.data() + sizes_.cbHeader, data, chunk);
            SecBuffer buffers[4] = {
                {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record.data()},
                {static_cast<unsigned long>(chunk), SECBUFFER_DATA, record.data() + sizes_.cbHeader},
                {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER, record.data() + sizes_.cbHeader + chunk},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
            if (EncryptMessage(&context_, 0, &description, 0) != SEC_E_OK) {
                error_kind_ = ErrorKind::IoFailed;
                return false;
            }
            const bool sent = send_all(record.data(), buffers[0].cbBuffer + buffers[1].cbBuffer + buffers[2].cbBuffer);
            SecureZeroMemory(record.data(), record.size());
            if (!sent) {
                error_kind_ = ErrorKind::IoFailed;
                return false;
            }
            data += chunk;
            size -= chunk;
        }
        return true;
    }

    bool write(const std::string& message)
    {
        return write(message.data(), message.size());
    }

    // Reads decrypted bytes into `out` (up to `max_bytes`). Returns >0 bytes, 0 on clean EOF, -1 on error.
    int read_some(char* out, int max_bytes, std::wstring& error)
    {
        if (max_bytes <= 0)
            return 0;
        if (!decrypted_.empty()) {
            const int take = (std::min)(max_bytes, static_cast<int>(decrypted_.size()));
            std::memcpy(out, decrypted_.data(), static_cast<std::size_t>(take));
            decrypted_.erase(decrypted_.begin(), decrypted_.begin() + take);
            return take;
        }
        for (;;) {
            if (!incoming_.empty()) {
                SecBuffer buffers[4] = {
                    {static_cast<unsigned long>(incoming_.size()), SECBUFFER_DATA, incoming_.data()},
                    {0, SECBUFFER_EMPTY, nullptr},
                    {0, SECBUFFER_EMPTY, nullptr},
                    {0, SECBUFFER_EMPTY, nullptr},
                };
                SecBufferDesc description{SECBUFFER_VERSION, 4, buffers};
                const SECURITY_STATUS status = DecryptMessage(&context_, &description, 0, nullptr);
                if (status == SEC_E_OK || status == SEC_I_RENEGOTIATE || status == SEC_I_CONTEXT_EXPIRED) {
                    std::vector<char> extra;
                    for (const SecBuffer& buffer : buffers) {
                        if (buffer.BufferType == SECBUFFER_DATA && buffer.cbBuffer > 0) {
                            const char* p = static_cast<const char*>(buffer.pvBuffer);
                            decrypted_.insert(decrypted_.end(), p, p + buffer.cbBuffer);
                        } else if (buffer.BufferType == SECBUFFER_EXTRA && buffer.cbBuffer > 0) {
                            const char* p = static_cast<const char*>(buffer.pvBuffer);
                            extra.assign(p, p + buffer.cbBuffer);
                        }
                    }
                    incoming_ = std::move(extra);
                    if (status == SEC_I_RENEGOTIATE) {
                        std::vector<char> input = std::move(incoming_);
                        incoming_.clear();
                        if (!handshake(std::move(input), error)) {
                            error_kind_ = ErrorKind::TlsHandshakeFailed;
                            return -1;
                        }
                    }
                    if (!decrypted_.empty()) {
                        const int take = (std::min)(max_bytes, static_cast<int>(decrypted_.size()));
                        std::memcpy(out, decrypted_.data(), static_cast<std::size_t>(take));
                        decrypted_.erase(decrypted_.begin(), decrypted_.begin() + take);
                        return take;
                    }
                    if (status == SEC_I_CONTEXT_EXPIRED)
                        return 0;
                    continue;
                }
                if (status != SEC_E_INCOMPLETE_MESSAGE) {
                    error_kind_ = ErrorKind::IoFailed;
                    error = L"TLS failed reading the reply (" + format_hex(static_cast<unsigned long>(status)) + L")";
                    return -1;
                }
            }
            if (!receive())
                return 0;
        }
    }

    template <typename Done>
    bool read_until(std::string& text, Done done, std::wstring& error)
    {
        char buf[4096];
        while (!done(text)) {
            const int got = read_some(buf, static_cast<int>(sizeof(buf)), error);
            if (got < 0)
                return false;
            if (got == 0)
                break;
            text.append(buf, static_cast<std::size_t>(got));
        }
        return done(text);
    }

private:
    bool send_all(const char* data, std::size_t size)
    {
        while (size > 0) {
            const int sent = ::send(socket_, data, static_cast<int>(size), 0);
            if (sent <= 0)
                return false;
            data += sent;
            size -= static_cast<std::size_t>(sent);
        }
        return true;
    }

    bool receive()
    {
        char chunk[16384];
        const int received = ::recv(socket_, chunk, sizeof(chunk), 0);
        if (received <= 0)
            return false;
        incoming_.insert(incoming_.end(), chunk, chunk + received);
        return true;
    }

    bool handshake(std::vector<char> input, std::wstring& error)
    {
        constexpr DWORD kFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                                 ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                                 ISC_REQ_MANUAL_CRED_VALIDATION;
        incoming_ = std::move(input);
        bool first = !have_context_;
        for (;;) {
            if (!first && incoming_.empty() && !receive()) {
                error_kind_ = ErrorKind::TlsHandshakeFailed;
                error = L"the connection closed during the TLS handshake";
                return false;
            }
            SecBuffer in[2] = {
                {static_cast<unsigned long>(incoming_.size()), SECBUFFER_TOKEN,
                 incoming_.empty() ? nullptr : incoming_.data()},
                {0, SECBUFFER_EMPTY, nullptr},
            };
            SecBufferDesc in_description{SECBUFFER_VERSION, 2, in};
            SecBuffer out[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc out_description{SECBUFFER_VERSION, 1, out};
            DWORD got = 0;
            const SECURITY_STATUS status = InitializeSecurityContextW(
                &credentials_, first ? nullptr : &context_, const_cast<wchar_t*>(host_.c_str()), kFlags, 0, 0,
                first ? nullptr : &in_description, 0, &context_, &out_description, &got, nullptr);
            if (first) {
                if (status == SEC_I_CONTINUE_NEEDED || status == SEC_E_OK)
                    have_context_ = true;
                first = false;
            }
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!receive()) {
                    error_kind_ = ErrorKind::TlsHandshakeFailed;
                    error = L"the connection closed during the TLS handshake";
                    return false;
                }
                continue;
            }
            if (out[0].cbBuffer > 0 && out[0].pvBuffer != nullptr) {
                const bool sent = send_all(static_cast<const char*>(out[0].pvBuffer), out[0].cbBuffer);
                FreeContextBuffer(out[0].pvBuffer);
                if (!sent) {
                    error_kind_ = ErrorKind::TlsHandshakeFailed;
                    error = L"couldn't send the TLS handshake";
                    return false;
                }
            }
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED) {
                error_kind_ = ErrorKind::TlsHandshakeFailed;
                error = L"the TLS handshake failed (" + format_hex(static_cast<unsigned long>(status)) + L")";
                return false;
            }
            if (in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer > 0)
                incoming_.erase(incoming_.begin(), incoming_.end() - in[1].cbBuffer);
            else
                incoming_.clear();
            if (status == SEC_E_OK)
                break;
        }
        if (QueryContextAttributesW(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_) != SEC_E_OK) {
            error_kind_ = ErrorKind::TlsHandshakeFailed;
            error = L"TLS gave no record sizes";
            return false;
        }
        return true;
    }

    SOCKET socket_ = INVALID_SOCKET;
    bool connected_ = false;
    CredHandle credentials_{};
    CtxtHandle context_{};
    bool have_credentials_ = false;
    bool have_context_ = false;
    std::wstring host_;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<char> incoming_;
    std::vector<char> decrypted_;
    ErrorKind error_kind_ = ErrorKind::None;
    DWORD negotiated_protocol_ = 0;
    bool has_peer_key_ = false;
    std::array<std::uint8_t, 32> peer_key_sha256_{};
};

} // namespace vanacore::tls
