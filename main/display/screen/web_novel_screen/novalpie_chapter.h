#pragma once

#include <string>

struct cJSON;

namespace web_novel {
inline constexpr char kNovalpieUserAgent[] = "Metalio-Novalpie/1";
std::string SignNovalpieSession(const std::string& timestamp, const std::string& nonce);

// 接收同一个阅读会话的密钥和正文对象。校验失败返回空串。
std::string DecodeNovalpieChapter(const cJSON* chapter, const std::string& session_key);
} // namespace web_novel
