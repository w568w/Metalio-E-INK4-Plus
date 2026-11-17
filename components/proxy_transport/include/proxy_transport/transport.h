#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace proxy_transport {

struct Endpoint {
    std::string host;
    uint16_t port = 0;
};

// Blocking, single-owner byte stream. Positive = transferred bytes, zero = EOF,
// negative = -errno. Each operation may transfer fewer bytes than requested.
// Destruction closes the connection. Use from a worker, not the UI task.
class Stream {
public:
    virtual ~Stream() = default;
    virtual int Read(void* buffer, size_t size) = 0;
    virtual int Write(const void* buffer, size_t size) = 0;
};

struct Connection {
    std::unique_ptr<Stream> stream;
    int error = 0; // -errno on failure
    explicit operator bool() const {
        return stream != nullptr;
    }
};

// deadline_ms is an absolute monotonic deadline shared by all connection layers.
// DNS resolution uses the platform resolver and is not deadline-bounded.
class Dialer {
public:
    virtual ~Dialer() = default;
    virtual Connection Dial(const Endpoint& destination, int64_t deadline_ms) = 0;
};

int64_t MonotonicMs();
int WriteAll(Stream& stream, const void* data, size_t size);
std::unique_ptr<Dialer> MakeDirectDialer();

struct TlsOptions {
    std::string server_name; // 必填的 SNI 和证书主机名，与 TCP 地址独立配置
    bool verify_certificate = true;
    std::string ca_pem; // 空值使用 ESP 证书包；非空 PEM 可指定私有 CA（主机验证时必填）
};

// Owns the underlying stream, including on handshake failure. Can wrap a tunnel.
Connection WrapTls(std::unique_ptr<Stream> stream, const TlsOptions& options);

struct TrojanOptions {
    Endpoint server;
    std::string password;
    TlsOptions tls;
};

// upstream is explicitly supplied: direct TCP in production, a fake in tests.
// options.tls.server_name 为空时使用 options.server.host。
// One returned stream represents one TCP CONNECT tunnel (no UDP or multiplexing).
std::unique_ptr<Dialer> MakeTrojanDialer(TrojanOptions options, std::unique_ptr<Dialer> upstream);

} // namespace proxy_transport
