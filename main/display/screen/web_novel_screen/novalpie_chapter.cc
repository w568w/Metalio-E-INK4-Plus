#include "novalpie_chapter.h"
#include "web_novel_model.h"

#include <charconv>
#include <cstdint>
#include <mbedtls/base64.h>
#include <mbedtls/md.h>
#include <psa/crypto.h>

namespace web_novel {
namespace {
std::vector<uint8_t> Base64(const std::string& text) {
    std::vector<uint8_t> bytes(text.size() / 4 * 3 + 3);
    size_t size = 0;
    if (mbedtls_base64_decode(bytes.data(), bytes.size(), &size,
                              reinterpret_cast<const uint8_t*>(text.data()), text.size()) != 0)
        return {};
    bytes.resize(size);
    return bytes;
}
std::string HexDigest(psa_algorithm_t algorithm, const std::string& input) {
    uint8_t digest[32];
    size_t size = 0;
    if (psa_hash_compute(algorithm, reinterpret_cast<const uint8_t*>(input.data()), input.size(),
                         digest, sizeof(digest), &size) != PSA_SUCCESS)
        return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        result += hex[digest[i] >> 4];
        result += hex[digest[i] & 15];
    }
    return result;
}
} // namespace

std::string SignNovalpieSession(const std::string& timestamp, const std::string& nonce) {
    // 网页阅读模块的会话签名；签名使用同一条请求中的 User-Agent。
    constexpr char seed[] = "X9f2m8Q5zL1p4R7t0Y3u6W2s5V8x1B4n7M0k3J6h9G2d5F8c1A4b7E0r3T6y9U2i";
    constexpr char alphabet[] = "M9N8B7V6C5X4Z3L2K1J0HGFDSAPOIUYTREWQmnbvcxzlkjhgfdsaqwertyuiop+/";
    constexpr char standard[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    uint64_t seconds = 0;
    const auto end = timestamp.data() + timestamp.size();
    const auto parsed = std::from_chars(timestamp.data(), end, seconds);
    if (parsed.ec != std::errc{} || parsed.ptr != end || psa_crypto_init() != PSA_SUCCESS)
        return {};
    const uint32_t value = static_cast<uint32_t>(seconds);
    char rotated[8];
    const auto hex = std::to_chars(rotated, rotated + sizeof(rotated), (value << 3) | (value >> 29), 16);

    const auto fingerprint = HexDigest(PSA_ALG_MD5, std::string(kNovalpieUserAgent) + timestamp + nonce);
    const auto message = HexDigest(PSA_ALG_SHA_256, fingerprint + seed + std::string(rotated, hex.ptr));
    const auto key = HexDigest(PSA_ALG_MD5, seed);
    if (fingerprint.empty() || message.empty() || key.empty())
        return {};

    const auto* sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    uint8_t mac[20];
    if (!sha1 ||
        mbedtls_md_hmac(sha1, reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                        reinterpret_cast<const uint8_t*>(message.data()), message.size(), mac) != 0)
        return {};

    uint8_t encoded[29];
    size_t length = 0;
    if (mbedtls_base64_encode(encoded, sizeof(encoded), &length, mac, sizeof(mac)) != 0)
        return {};
    std::string result(reinterpret_cast<const char*>(encoded), length);
    for (char& c : result)
        if (const char* match = std::char_traits<char>::find(standard, 64, c))
            c = alphabet[match - standard];
    return result;
}

std::string DecodeNovalpieChapter(const cJSON* chapter, const std::string& session_key) {
    if (!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(chapter, "encrypted")))
        return Text(chapter, "content");
    auto session = Base64(session_key);
    auto iv = Base64(Text(chapter, "iv"));
    auto tag = Base64(Text(chapter, "tag"));
    auto body = Base64(Text(chapter, "content"));
    if (session.size() != 32 || iv.size() != 12 || tag.size() != 16 || body.empty() ||
        psa_crypto_init() != PSA_SUCCESS)
        return {};

    // 网页对 Base64 解码后的会话密钥做 SHA-256，再用 AES-GCM 验证并解密正文。
    uint8_t key[32];
    size_t size = 0;
    if (psa_hash_compute(PSA_ALG_SHA_256, session.data(), session.size(), key, sizeof(key), &size) != PSA_SUCCESS)
        return {};
    auto attributes = psa_key_attributes_init();
    psa_set_key_type(&attributes, PSA_KEY_TYPE_AES);
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attributes, PSA_ALG_GCM);
    mbedtls_svc_key_id_t handle{};
    if (psa_import_key(&attributes, key, size, &handle) != PSA_SUCCESS)
        return {};

    std::string plain(body.size(), '\0');
    body.insert(body.end(), tag.begin(), tag.end());
    const auto status = psa_aead_decrypt(handle, PSA_ALG_GCM, iv.data(), iv.size(), nullptr, 0,
                                         body.data(), body.size(), reinterpret_cast<uint8_t*>(plain.data()),
                                         plain.size(), &size);
    psa_destroy_key(handle);
    if (status != PSA_SUCCESS)
        return {};
    plain.resize(size);
    return plain;
}
} // namespace web_novel
