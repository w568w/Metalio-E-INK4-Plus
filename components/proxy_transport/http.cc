#include "proxy_transport/http.h"
#include <esp_http_client.h>
#include <esp_transport.h>
#include <cerrno>
#include <strings.h>

namespace proxy_transport {
namespace {
struct Transport {
    proxy_transport::Dialer& dialer;
    Response& response;
    bool secure;
    size_t limit;
    std::unique_ptr<proxy_transport::Stream> stream = nullptr;
    int error = 0;
    bool eof = false, headers_complete = false;

    static Transport& Get(esp_transport_handle_t handle) {
        return *static_cast<Transport*>(esp_transport_get_context_data(handle));
    }
    int Result(int count) {
        if (count < 0) {
            error = count;
            errno = -count;
            return -1;
        }
        return count;
    }
    static int Connect(esp_transport_handle_t handle, const char* host, int port, int timeout_ms) {
        auto& self = Get(handle);
        // Stream 的绝对截止时间覆盖连接、TLS 和整段响应读取，不会在每次 read 时重置。
        auto connection = self.dialer.Dial({host, static_cast<uint16_t>(port)},
                                           proxy_transport::MonotonicMs() + timeout_ms);
        if (connection && self.secure)
            connection = proxy_transport::WrapTls(std::move(connection.stream), {host, true, {}});
        self.error = connection.error;
        self.stream = std::move(connection.stream);
        if (!self.stream) {
            errno = -self.error;
            return -1;
        }
        return 0;
    }
    static int Read(esp_transport_handle_t handle, char* data, int size, int) {
        auto& self = Get(handle);
        if (self.response.error_code)
            return self.Result(-ECANCELED);
        const int count = self.stream->Read(data, size);
        // ESP transport distinguishes FIN (-1/ENOTCONN) from a read timeout (0).
        if (count == 0) {
            self.eof = true;
            errno = ENOTCONN;
            return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
        }
        return self.Result(count);
    }
    static int Write(esp_transport_handle_t handle, const char* data, int size, int) {
        auto& self = Get(handle);
        return self.Result(self.stream->Write(data, size));
    }
    static int Close(esp_transport_handle_t handle) {
        Get(handle).stream.reset();
        return 0;
    }
    static esp_err_t Event(esp_http_client_event_t* event) {
        auto& self = *static_cast<Transport*>(event->user_data);
        if (event->event_id == HTTP_EVENT_ON_HEADERS_COMPLETE)
            self.headers_complete = true;
        if (event->event_id == HTTP_EVENT_ON_HEADER)
            self.response.headers.emplace_back(event->header_key, event->header_value);
        if (event->event_id == HTTP_EVENT_ON_DATA) {
            if (size_t(event->data_len) > self.limit - self.response.body.size()) {
                self.response.error_code = -EMSGSIZE;
                return ESP_FAIL;
            }
            self.response.body.append(static_cast<const char*>(event->data), event->data_len);
        }
        return ESP_OK;
    }
};
} // namespace
std::string Response::Header(const std::string& name) const {
    for (const auto& header : headers)
        if (strcasecmp(header.first.c_str(), name.c_str()) == 0)
            return header.second;
    return {};
}
Response Request(proxy_transport::Dialer& dialer, const std::string& method, const std::string& url,
                 const std::string& body, const std::vector<std::pair<std::string, std::string>>& headers,
                 size_t limit, int timeout_ms) {
    Response response;
    if ((url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) ||
        url.find_first_of("\r\n") != std::string::npos || url.find('\0') != std::string::npos) {
        response.error_code = -EINVAL;
        return response;
    }
    if (method != "GET" && method != "POST" && method != "PUT") {
        response.error_code = -ENOTSUP;
        return response;
    }
    for (const auto& header : headers) {
        if (header.first.find_first_of("\r\n") != std::string::npos ||
            header.second.find_first_of("\r\n") != std::string::npos) {
            response.error_code = -EINVAL;
            return response;
        }
    }
    Transport context{.dialer = dialer, .response = response,
                      .secure = url.rfind("https://", 0) == 0, .limit = limit};
    auto transport = std::unique_ptr<esp_transport_item_t, decltype(&esp_transport_destroy)>(
        esp_transport_init(), esp_transport_destroy);
    if (!transport) {
        response.error_code = -ENOMEM;
        return response;
    }
    esp_transport_set_context_data(transport.get(), &context);
    esp_transport_set_func(transport.get(), Transport::Connect, Transport::Read, Transport::Write,
                           Transport::Close, nullptr, nullptr, nullptr);
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.transport = transport.get();
    config.method = method == "PUT" ? HTTP_METHOD_PUT
                    : method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET;
    config.timeout_ms = timeout_ms;
    config.disable_auto_redirect = true;
    config.event_handler = Transport::Event;
    config.user_data = &context;
    auto client = std::unique_ptr<esp_http_client, decltype(&esp_http_client_cleanup)>(
        esp_http_client_init(&config), esp_http_client_cleanup);
    if (!client) {
        response.error_code = -EIO;
        return response;
    }
    esp_http_client_set_header(client.get(), "Accept-Encoding", "identity");
    esp_http_client_set_header(client.get(), "Connection", "close");
    for (const auto& header : headers)
        esp_http_client_set_header(client.get(), header.first.c_str(), header.second.c_str());
    if (method == "POST" || method == "PUT")
        esp_http_client_set_post_field(client.get(), body.data(), body.size());
    const auto result = esp_http_client_perform(client.get());
    response.status = esp_http_client_get_status_code(client.get());
    // IDF expects a length/chunk terminator; HTTP also permits a body delimited by FIN.
    const bool closed_body = context.headers_complete && context.eof && !context.error &&
                             esp_http_client_get_content_length(client.get()) < 0 &&
                             response.Header("transfer-encoding").empty();
    if (!response.error_code && !closed_body &&
        (result != ESP_OK || !esp_http_client_is_complete_data_received(client.get())))
        response.error_code = context.error ? context.error : -EPROTO;
    return response;
}
} // namespace proxy_transport
