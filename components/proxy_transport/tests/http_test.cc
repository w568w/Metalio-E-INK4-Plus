#include "proxy_transport/http.h"
#include <cassert>
#include <cerrno>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <psa/crypto.h>
#include <esp_event.h>
using namespace proxy_transport;
class FakeStream final : public Stream {
public:
    FakeStream(std::string bytes, std::string& request) : bytes_(std::move(bytes)), request_(request) {}
    int Read(void* p, size_t size) override {
        size_t n = std::min({size, size_t(3), bytes_.size()});
        std::memcpy(p, bytes_.data(), n); bytes_.erase(0, n); return n;
    }
    int Write(const void* p, size_t n) override {
        n = std::min<size_t>(n, 7);
        request_.append(static_cast<const char*>(p), n);
        return n;
    }
private:
    std::string bytes_;
    std::string& request_;
};
class FakeDialer final : public Dialer {
public:
    std::string bytes;
    std::string request;
    int error = 0;
    Connection Dial(const Endpoint& endpoint, int64_t) override {
        if (error) return {nullptr, error};
        assert(endpoint.host == "example.test" && endpoint.port == 80);
        request.clear();
        return {std::make_unique<FakeStream>(bytes, request), 0};
    }
};
extern "C" void app_main() {
    assert(esp_event_loop_create_default() == ESP_OK);
    assert(psa_crypto_init() == PSA_SUCCESS);
    unsigned char digest[28]; size_t length = 0;
    assert(psa_hash_compute(PSA_ALG_SHA_224, reinterpret_cast<const unsigned char*>("abc"), 3, digest, sizeof(digest), &length) == PSA_SUCCESS);
    constexpr char expected[] = "\x23\x09\x7d\x22\x34\x05\xd8\x22\x86\x42\xa4\x77\xbd\xa2\x55\xb3\x2a\xad\xbc\xe4\xbd\xa0\xb3\xf7\xe3\x6c\x9d\xa7";
    assert(length == 28 && std::memcmp(digest, expected, sizeof(digest)) == 0);
    FakeDialer dialer;
    dialer.bytes = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nSet-Cookie: first=1\r\nSet-Cookie: refresh-token=abc; Secure\r\n\r\n3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n";
    auto result = Request(dialer, "GET", "http://example.test/path?q=a");
    assert(result.error_code == 0 && result.status == 200 && result.body == "abcde");
    assert(result.headers.size() == 3 && result.headers[2].second == "refresh-token=abc; Secure");
    dialer.bytes = "HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n";
    result = Request(dialer, "PUT", "http://example.test/user/read-history/syosetu/n1", "chapter-02",
                     {{"Authorization", "Bearer test-token"}, {"Content-Type", "text/plain;charset=UTF-8"}});
    assert(result.error_code == 0 && result.status == 204);
    assert(dialer.request.rfind("PUT /user/read-history/syosetu/n1 HTTP/1.1\r\n", 0) == 0);
    assert(dialer.request.find("Authorization: Bearer test-token\r\n") != std::string::npos);
    assert(dialer.request.find("Content-Type: text/plain;charset=UTF-8\r\n") != std::string::npos);
    assert(dialer.request.substr(dialer.request.find("\r\n\r\n") + 4) == "chapter-02");
    dialer.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 8\r\n\r\nshort";
    result = Request(dialer, "GET", "http://example.test/"); assert(result.error_code == -EPROTO);
    dialer.bytes = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n12345";
    result = Request(dialer, "GET", "http://example.test/", {}, {}, 4); assert(result.error_code == -EMSGSIZE);
    dialer.bytes = "HTTP/1.1 200 OK\r\n\r\nclosed-body";
    result = Request(dialer, "GET", "http://example.test/"); assert(result.error_code == 0 && result.body == "closed-body");
    dialer.bytes = "HTTP/1.1 200 OK\r\nBroken:";
    result = Request(dialer, "GET", "http://example.test/"); assert(result.error_code == -EPROTO);
    assert(Request(dialer, "GET", "file:///invalid").error_code == -EINVAL);
    assert(Request(dialer, "DELETE", "http://example.test/").error_code == -ENOTSUP);
    assert(Request(dialer, "GET", "http://example.test/", {}, {{"X-Test", "bad\r\nvalue"}}).error_code == -EINVAL);
    dialer.error = -ECONNRESET;
    assert(Request(dialer, "GET", "http://example.test/").error_code == -ECONNRESET);
    puts("PASS: SDK HTTP chunking, headers, bounds, truncated/EOF bodies, PSA SHA-224");
    exit(0);
}
