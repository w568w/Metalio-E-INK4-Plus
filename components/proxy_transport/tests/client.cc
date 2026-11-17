#include "proxy_transport/transport.h"
#include <cassert>
#include <cerrno>
#include <fstream>
#include <iterator>
#include <iostream>

using namespace proxy_transport;

class ShortWriter : public Stream {
public:
    int Read(void*, size_t) override { return 0; }
    int Write(const void* p, size_t n) override {
        if (fail) return -EPIPE;
        size_t count = std::min<size_t>(n, 2);
        data.append(static_cast<const char*>(p), count);
        return int(count);
    }
    bool fail = false;
    std::string data;
};

int main(int argc, char** argv) {
    assert(argc == 5);
    ShortWriter writer;
    assert(WriteAll(writer, "abcde", 5) == 0 && writer.data == "abcde");
    writer.fail = true;
    assert(WriteAll(writer, "x", 1) == -EPIPE);
    auto expired = MakeDirectDialer()->Dial({"127.0.0.1", uint16_t(std::stoi(argv[1]))}, MonotonicMs() - 1);
    assert(!expired && expired.error == -ETIMEDOUT);
    std::ifstream file(argv[3]);
    std::string ca((std::istreambuf_iterator<char>(file)), {});
    TrojanOptions options;
    options.server = {"127.0.0.1", uint16_t(std::stoi(argv[1]))};
    options.password = "test-password";
    options.tls = {"localhost", true, ca};
    auto dialer = MakeTrojanDialer(options, MakeDirectDialer());
    const std::string mode = argv[4];
    if (mode == "bad-certificate") {
        options.tls.server_name = "wrong.example";
        auto rejected = MakeTrojanDialer(options, MakeDirectDialer())->Dial(
            {"localhost", uint16_t(std::stoi(argv[2]))}, MonotonicMs() + 5000);
        assert(!rejected);
        return 0;
    }
    if (mode == "skip-certificate") {
        options.tls.server_name = "wrong.example";
        options.tls.verify_certificate = false;
        dialer = MakeTrojanDialer(options, MakeDirectDialer());
    }
    auto tunnel = dialer->Dial({mode == "ipv4" ? "127.0.0.1" : "localhost",
        uint16_t(std::stoi(argv[2]))}, MonotonicMs() + 5000);
    if (!tunnel) { std::cerr << "trojan " << tunnel.error << '\n'; return 1; }
    if (mode == "large-first-write") {
        std::string payload(4096, 'x');
        assert(tunnel.stream->Write(payload.data(), payload.size()) == int(payload.size()));
        char reply[2];
        assert(tunnel.stream->Read(reply, sizeof(reply)) == 2);
        assert(std::string(reply, 2) == "OK");
        return 0;
    }
    auto https = WrapTls(std::move(tunnel.stream), {"localhost", true, ca});
    if (!https) { std::cerr << "inner TLS " << https.error << '\n'; return 1; }
    std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    assert(WriteAll(*https.stream, request.data(), request.size()) == 0);
    std::string response;
    char buffer[17]; // exercise fragmented application reads
    for (;;) {
        int n = https.stream->Read(buffer, sizeof(buffer));
        assert(n >= 0);
        if (!n) break;
        response.append(buffer, n);
    }
    assert(response.find("200 OK") != std::string::npos);
    assert(response.find("nested TLS works") != std::string::npos);
}
