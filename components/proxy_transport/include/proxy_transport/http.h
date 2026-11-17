#pragma once
#include "proxy_transport/transport.h"
#include <string>
#include <utility>
#include <vector>
namespace proxy_transport {
constexpr size_t kMaxBodyBytes = 2 * 1024 * 1024;
struct Response {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int error_code = 0; // 0 表示传输成功，失败为负 errno；HTTP 状态码单独保存在 status 中。
    std::string Header(const std::string& name) const;
};
// ESP-IDF GET/POST/PUT；timeout_ms 覆盖连接、握手和完整响应，limit 为响应体字节上限。
Response Request(proxy_transport::Dialer& dialer, const std::string& method, const std::string& url,
                 const std::string& body = {},
                 const std::vector<std::pair<std::string, std::string>>& headers = {},
                 size_t limit = kMaxBodyBytes, int timeout_ms = 30000);
} // namespace proxy_transport
