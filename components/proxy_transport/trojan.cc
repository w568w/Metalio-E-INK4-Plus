#include "proxy_transport/transport.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include <array>
#include <algorithm>
#include <cerrno>
#include <utility>
#include <sys/socket.h>
#ifdef ESP_PLATFORM
#if !CONFIG_IDF_TARGET_LINUX
#include <lwip/sockets.h>
#else
#include <poll.h>
#include <arpa/inet.h>
#endif
#else
#include <arpa/inet.h>
#endif
#include <psa/crypto.h>
#include <mbedtls/ssl.h>

namespace proxy_transport {
namespace {
// The protocol preface and first application bytes share the first TLS record.
class TrojanStream final : public Stream {
public:
    TrojanStream(std::unique_ptr<Stream> stream, std::string preface)
        : stream_(std::move(stream)), preface_(std::move(preface)) {}
    int Read(void* data, size_t size) override {
        if (failed_)
            return failed_;
        if (!size)
            return 0;
        if (!preface_.empty()) {
            failed_ = WriteAll(*stream_, preface_.data(), preface_.size());
            if (failed_)
                return failed_;
            preface_.clear(); // server-first protocols need CONNECT before reading
        }
        return stream_->Read(data, size);
    }
    int Write(const void* data, size_t size) override {
        if (failed_)
            return failed_;
        if (!size)
            return 0;
        if (preface_.empty())
            return stream_->Write(data, size);
        // 将协议头和首段数据合并，大小限制取自 Mbed TLS 的单条出站记录上限。
        const size_t count = std::min<size_t>(size, MBEDTLS_SSL_OUT_CONTENT_LEN - preface_.size());
        preface_.append(static_cast<const char*>(data), count);
        failed_ = WriteAll(*stream_, preface_.data(), preface_.size());
        if (failed_)
            return failed_;
        preface_.clear();
        return static_cast<int>(count);
    }

private:
    std::unique_ptr<Stream> stream_;
    std::string preface_;
    int failed_ = 0;
};
class TrojanDialer final : public Dialer {
public:
    TrojanDialer(TrojanOptions options, std::unique_ptr<Dialer> upstream)
        : options_(std::move(options)), upstream_(std::move(upstream)) {}

    Connection Dial(const Endpoint& destination, int64_t deadline) override {
        if (!upstream_ || options_.password.empty() || destination.host.empty() ||
            destination.host.find('\0') != std::string::npos || !destination.port)
            return {nullptr, -EINVAL};
        std::array<uint8_t, 16> address{};
        std::string request;
        if (psa_crypto_init() != PSA_SUCCESS)
            return {nullptr, -EIO};
        std::array<uint8_t, 28> hash{};
        size_t hash_size = 0;
        if (psa_hash_compute(PSA_ALG_SHA_224, reinterpret_cast<const uint8_t*>(options_.password.data()),
                             options_.password.size(), hash.data(), hash.size(), &hash_size) != PSA_SUCCESS ||
            hash_size != hash.size())
            return {nullptr, -ENOTSUP};
        constexpr char hex[] = "0123456789abcdef";
        for (uint8_t byte : hash) {
            request += hex[byte >> 4];
            request += hex[byte & 15];
        }
        request += "\r\n\x01"; // CONNECT; Trojan has no server acknowledgement
        if (inet_pton(AF_INET, destination.host.c_str(), address.data()) == 1) {
            request += '\x01';
            request.append(reinterpret_cast<char*>(address.data()), 4);
        } else if (inet_pton(AF_INET6, destination.host.c_str(), address.data()) == 1) {
            request += '\x04';
            request.append(reinterpret_cast<char*>(address.data()), 16);
        } else {
            if (destination.host.size() > 255)
                return {nullptr, -EINVAL};
            request += '\x03';
            request += char(destination.host.size());
            request += destination.host;
        }
        request += char(destination.port >> 8);
        request += char(destination.port & 255);
        request += "\r\n";

        auto connection = upstream_->Dial(options_.server, deadline);
        if (!connection)
            return connection;
        TlsOptions tls = options_.tls;
        if (tls.server_name.empty())
            tls.server_name = options_.server.host;
        connection = WrapTls(std::move(connection.stream), tls);
        if (!connection)
            return connection;
        return {std::make_unique<TrojanStream>(std::move(connection.stream), std::move(request)), 0};
    }

private:
    TrojanOptions options_;
    std::unique_ptr<Dialer> upstream_;
};
} // namespace
std::unique_ptr<Dialer> MakeTrojanDialer(TrojanOptions options, std::unique_ptr<Dialer> upstream) {
    return std::make_unique<TrojanDialer>(std::move(options), std::move(upstream));
}
} // namespace proxy_transport
