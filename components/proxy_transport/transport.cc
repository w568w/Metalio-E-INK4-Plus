#include "proxy_transport/transport.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <ctime>
#include <fcntl.h>
#include <netdb.h>
#ifdef ESP_PLATFORM
#include <esp_timer.h>
#if !CONFIG_IDF_TARGET_LINUX
#include <lwip/sockets.h>
#else
#include <poll.h>
#include <arpa/inet.h>
#endif
#else
#include <poll.h>
#endif
#include <sys/socket.h>
#include <unistd.h>

namespace proxy_transport {
int64_t MonotonicMs() {
#if defined(ESP_PLATFORM) && !CONFIG_IDF_TARGET_LINUX
    return esp_timer_get_time() / 1000;
#else
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
#endif
}

int WriteAll(Stream& stream, const void* data, size_t size) {
    auto* bytes = static_cast<const uint8_t*>(data);
    while (size) {
        int n = stream.Write(bytes, size);
        if (n <= 0)
            return n < 0 ? n : -EPIPE;
        if (size_t(n) > size)
            return -EIO;
        bytes += n;
        size -= n;
    }
    return 0;
}

namespace {
class SocketStream final : public Stream {
public:
    SocketStream(int fd, int64_t deadline) : fd_(fd), deadline_(deadline) {}
    ~SocketStream() override {
        close(fd_);
    }
    int Wait(short events) {
        for (;;) {
            int64_t left = deadline_ - MonotonicMs();
            if (left <= 0)
                return -ETIMEDOUT;
            pollfd p{fd_, events, 0};
            int n = poll(&p, 1, int(std::min<int64_t>(left, INT_MAX)));
            if (n > 0)
                return 0; // recv/send/SO_ERROR reports hangup and socket errors
            if (n == 0)
                return -ETIMEDOUT;
            if (errno != EINTR)
                return -errno;
        }
    }
    int Read(void* data, size_t size) override {
        return Transfer(data, size, false);
    }
    int Write(const void* data, size_t size) override {
        return Transfer(const_cast<void*>(data), size, true);
    }

private:
    int Transfer(void* data, size_t size, bool writing) {
        if (!size)
            return 0;
        for (;;) {
            int error = Wait(writing ? POLLOUT : POLLIN);
            if (error)
                return error;
            const size_t count = std::min<size_t>(size, INT_MAX);
#ifdef MSG_NOSIGNAL
            constexpr int flags = MSG_NOSIGNAL;
#else
            constexpr int flags = 0;
#endif
            int n = writing ? send(fd_, data, count, flags) : recv(fd_, data, count, 0);
            if (n >= 0)
                return n;
            if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                return -errno;
        }
    }
    int fd_;
    int64_t deadline_;
};

class DirectDialer final : public Dialer {
public:
    Connection Dial(const Endpoint& target, int64_t deadline) override {
        if (target.host.empty() || target.host.find('\0') != std::string::npos || !target.port)
            return {nullptr, -EINVAL};
        if (deadline <= MonotonicMs())
            return {nullptr, -ETIMEDOUT};
        addrinfo hints{}, *addresses = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        const std::string service = std::to_string(target.port);
        if (getaddrinfo(target.host.c_str(), service.c_str(), &hints, &addresses))
            return {nullptr, -EHOSTUNREACH};
        Connection result{nullptr, -EHOSTUNREACH};
        for (auto* address = addresses; address; address = address->ai_next) {
            if (deadline <= MonotonicMs()) {
                result.error = -ETIMEDOUT;
                break;
            }
            int fd = socket(address->ai_family, SOCK_STREAM, address->ai_protocol);
            if (fd < 0) {
                result.error = -errno;
                continue;
            }
            auto stream = std::make_unique<SocketStream>(fd, deadline);
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
                result.error = -errno;
                continue;
            }
            if (connect(fd, address->ai_addr, address->ai_addrlen) < 0) {
                if (errno != EINPROGRESS) {
                    result.error = -errno;
                    continue;
                }
                result.error = stream->Wait(POLLOUT);
                if (result.error)
                    continue;
                int error = 0;
                socklen_t length = sizeof(error);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
                    error = errno;
                if (error) {
                    result.error = -error;
                    continue;
                }
            }
            result = {std::move(stream), 0};
            break;
        }
        freeaddrinfo(addresses);
        return result;
    }
};
} // namespace
std::unique_ptr<Dialer> MakeDirectDialer() {
    return std::make_unique<DirectDialer>();
}
} // namespace proxy_transport
