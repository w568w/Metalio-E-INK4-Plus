#include "proxy_transport/transport.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <mbedtls/ssl.h>
#include <mbedtls/version.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>
#ifdef ESP_PLATFORM
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <esp_random.h>
#else
#include <sys/random.h>
#endif

namespace proxy_transport {
namespace {
class TlsStream final : public Stream {
public:
    explicit TlsStream(std::unique_ptr<Stream> stream) : stream_(std::move(stream)) {
        mbedtls_ssl_init(&ssl_);
        mbedtls_ssl_config_init(&config_);
        mbedtls_x509_crt_init(&ca_);
    }
    ~TlsStream() override {
        // Closing the owned transport is sufficient; don't block destruction on close_notify.
        mbedtls_ssl_free(&ssl_);
        mbedtls_ssl_config_free(&config_);
        mbedtls_x509_crt_free(&ca_);
    }
    int Handshake(const TlsOptions& options) {
        if (!stream_ || options.server_name.empty() || options.server_name.find('\0') != std::string::npos)
            return -EINVAL;
        if (psa_crypto_init() != PSA_SUCCESS)
            return -EIO;
        if (mbedtls_ssl_config_defaults(&config_, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                        MBEDTLS_SSL_PRESET_DEFAULT))
            return -EPROTO;
        // ESP-IDF 使用 Mbed TLS 4 的 PSA RNG；独立主机验证的 3.6 需要显式 RNG 回调。
#if MBEDTLS_VERSION_MAJOR < 4
        mbedtls_ssl_conf_rng(&config_, Random, nullptr);
#endif
        mbedtls_ssl_conf_authmode(&config_, options.verify_certificate ? MBEDTLS_SSL_VERIFY_REQUIRED
                                                                       : MBEDTLS_SSL_VERIFY_NONE);
        if (options.verify_certificate) {
            if (!options.ca_pem.empty()) {
                if (mbedtls_x509_crt_parse(&ca_, reinterpret_cast<const uint8_t*>(options.ca_pem.c_str()),
                                           options.ca_pem.size() + 1))
                    return -EINVAL;
                mbedtls_ssl_conf_ca_chain(&config_, &ca_, nullptr);
            } else {
#ifdef ESP_PLATFORM
                if (esp_crt_bundle_attach(&config_) != ESP_OK)
                    return -EIO;
#else
                return -EINVAL; // Host tests explicitly supply their test CA.
#endif
            }
        }
        if (mbedtls_ssl_setup(&ssl_, &config_) ||
            mbedtls_ssl_set_hostname(&ssl_, options.server_name.c_str()))
            return -EPROTO;
        mbedtls_ssl_set_bio(&ssl_, this, Send, Receive, nullptr);
        int result;
        do {
            result = mbedtls_ssl_handshake(&ssl_);
        } while (Retry(result));
#ifdef ESP_PLATFORM
        if (result)
            ESP_LOGW("ProxyTLS", "handshake %s failed tls=%d io=%d verify=0x%lx", options.server_name.c_str(),
                     result, io_error_, (unsigned long)mbedtls_ssl_get_verify_result(&ssl_));
#endif
        return result ? Failure() : 0;
    }
    int Read(void* data, size_t size) override {
        if (!size)
            return 0;
        int result;
        do {
            result = mbedtls_ssl_read(&ssl_, static_cast<uint8_t*>(data), std::min<size_t>(size, INT_MAX));
        } while (Retry(result));
        if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;
        return result < 0 ? Failure() : result;
    }
    int Write(const void* data, size_t size) override {
        if (!size)
            return 0;
        int result;
        do {
            result =
                mbedtls_ssl_write(&ssl_, static_cast<const uint8_t*>(data), std::min<size_t>(size, INT_MAX));
        } while (Retry(result));
        return result < 0 ? Failure() : result;
    }

private:
    static bool Retry(int result) {
        return result == MBEDTLS_ERR_SSL_WANT_READ || result == MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    int Failure() const {
        return io_error_ ? io_error_ : -EPROTO;
    }
    static int Send(void* context, const uint8_t* data, size_t size) {
        auto& self = *static_cast<TlsStream*>(context);
        int n = self.stream_->Write(data, size);
        if (n > 0)
            return n;
        self.io_error_ = n < 0 ? n : -EPIPE;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
    static int Receive(void* context, uint8_t* data, size_t size) {
        auto& self = *static_cast<TlsStream*>(context);
        int n = self.stream_->Read(data, size);
        if (n >= 0)
            return n;
        self.io_error_ = n;
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    }
#if MBEDTLS_VERSION_MAJOR < 4
    static int Random(void*, uint8_t* data, size_t size) {
#ifdef ESP_PLATFORM
        esp_fill_random(data, size);
#else
        while (size) {
            ssize_t n = getrandom(data, size, 0);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return -1;
            data += n;
            size -= n;
        }
#endif
        return 0;
    }
#endif
    std::unique_ptr<Stream> stream_;
    mbedtls_ssl_context ssl_;
    mbedtls_ssl_config config_;
    mbedtls_x509_crt ca_;
    int io_error_ = 0;
};
} // namespace
Connection WrapTls(std::unique_ptr<Stream> stream, const TlsOptions& options) {
    auto tls = std::make_unique<TlsStream>(std::move(stream));
    int error = tls->Handshake(options);
    if (error)
        return {nullptr, error};
    return {std::move(tls), 0};
}
} // namespace proxy_transport
