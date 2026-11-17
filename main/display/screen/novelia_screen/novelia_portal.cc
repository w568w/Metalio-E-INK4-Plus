#include "novelia_portal.h"
#include "novelia_model.h"
#include "power_policy.h"
#include <esp_log.h>
#include <memory>
#include <esp_http_server.h>
#include <esp_random.h>
#include <esp_netif.h>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <cstdio>
namespace novelia {
namespace {
httpd_handle_t server = nullptr;
constexpr uint16_t kServerPort = 8081;
// 与 AP 文件传输服务的 HTTPD_DEFAULT_CONFIG 控制端口（32768）分开，允许两个服务共存。
constexpr uint16_t kControlPort = 32771;
// 手机表单请求的内存预算；正文整体入队，不能随客户端声明长度无限分配。
constexpr size_t kMaxFormBytes = 4 * 1024;
std::unique_ptr<PowerNeedHold> network_hold;
std::string secret, status = "请先配置代理，再登录账号", visible = "{}";
std::mutex mutex;
std::function<bool(std::string)> enqueue;
constexpr const char* kPage =
    R"HTML(<!doctype html><html lang="zh"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Novelia 设备登录</title><style>body{font:17px system-ui;max-width:32rem;margin:2rem auto;padding:0 1rem}input:not([type=checkbox]),button{box-sizing:border-box;width:100%;padding:.8rem;margin:.4rem 0}label{display:block;margin-top:.6rem}section{margin:2rem 0}#status{white-space:pre-wrap}</style><h1>Novelia</h1><p id="status">正在连接设备…</p><section><h2>网络代理</h2><form id="proxy"><label><input id="proxy_enabled" type="checkbox">使用 Trojan</label><input id="server" placeholder="服务器" required><input id="port" type="number" min="1" max="65535" placeholder="端口" required><input id="sni" placeholder="SNI"><input id="proxy_password" type="password" placeholder="节点密码（留空保留原值）" autocomplete="new-password"><label><input id="skip_cert_verify" type="checkbox">跳过节点证书验证</label><button>保存代理</button></form><button id="test">测试连接</button></section><section><h2>登录 Novelia</h2><form id="login"><input id="username" autocomplete="username" placeholder="账号" required><input id="password" type="password" autocomplete="current-password" placeholder="密码" required><button>登录</button></form></section><script>
let initialized=false;const el=id=>document.getElementById(id);
async function send(value){try{const r=await fetch(location.pathname,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(value)});el('status').textContent=await r.text()}catch(e){el('status').textContent='设备连接已关闭，请重新扫码'}}
function proxyValues(){return {server:el('server').value,port:Number(el('port').value),sni:el('sni').value,proxy_password:el('proxy_password').value,proxy_enabled:el('proxy_enabled').checked,skip_cert_verify:el('skip_cert_verify').checked}}
el('proxy').onsubmit=e=>{e.preventDefault();send({...proxyValues(),action:'configure'})};
el('test').onclick=()=>send({...proxyValues(),action:'test'});
el('login').onsubmit=e=>{e.preventDefault();send({action:'login',username:el('username').value,password:el('password').value});el('password').value=''};
async function poll(){try{const r=await fetch(location.pathname+'status');if(r.status===404){el('status').textContent='登录页面已关闭，请重新扫码';clearInterval(timer);return}if(!r.ok)throw Error();const d=await r.json();el('status').textContent=d.message;if(!initialized){for(const [k,v] of Object.entries(d.config)){const e=el(k);if(!e)continue;if(e.type==='checkbox')e.checked=!!v;else e.value=v;}el('proxy_password').placeholder=d.config.has_proxy_password?'节点密码（已保存，留空保留）':'请输入节点密码';initialized=true}}catch(e){el('status').textContent='设备暂时无法连接，正在重试…'}}const timer=setInterval(poll,1000);poll();
</script></html>)HTML";
esp_err_t Get(httpd_req_t* req) {
    const std::string path = req->uri;
    if (path != secret && path != secret + "status") {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    if (path == secret) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        return httpd_resp_send(req, kPage, HTTPD_RESP_USE_STRLEN);
    }
    auto value = Object();
    {
        std::lock_guard<std::mutex> lock(mutex);
        Set(value.get(), "message", status);
        auto config = Parse(visible);
        cJSON_AddItemToObject(value.get(), "config", config.release());
    }
    auto body = Encode(value.get());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.data(), body.size());
}
esp_err_t Post(httpd_req_t* req) {
    if (std::string(req->uri) != secret || req->content_len == 0 || req->content_len > kMaxFormBytes) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid request");
        return ESP_OK;
    }
    std::string body(req->content_len, '\0');
    size_t offset = 0;
    while (offset < body.size()) {
        int n = httpd_req_recv(req, &body[offset], body.size() - offset);
        if (n <= 0)
            return ESP_FAIL;
        offset += n;
    }
    PowerPolicy::GetInstance().NotifyUserActivity();
    PortalStatus("正在处理，请稍候…");
    if (!enqueue(std::move(body))) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "设备忙，请稍后重试");
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_sendstr(req, "正在处理，请稍候…");
}
} // namespace
void PortalStatus(const std::string& message, const std::string& config) {
    std::lock_guard<std::mutex> lock(mutex);
    status = message;
    if (!config.empty())
        visible = config;
}
std::string StartPortal(std::function<bool(std::string)> submit) {
    StopPortal();
    auto* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip{};
    if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK || ip.ip.addr == 0)
        return {};
    uint8_t random[16];
    esp_fill_random(random, sizeof(random));
    constexpr char hex[] = "0123456789abcdef";
    secret = "/";
    for (uint8_t b : random) {
        secret += hex[b >> 4];
        secret += hex[b & 15];
    }
    secret += "/";
    enqueue = std::move(submit);
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = kServerPort;
    cfg.ctrl_port = kControlPort;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    network_hold = std::make_unique<PowerNeedHold>(PowerNeed::ForegroundNetwork);
    if (httpd_start(&server, &cfg) != ESP_OK) {
        server = nullptr;
        network_hold.reset();
        return {};
    }
    ESP_LOGI("NoveliaPortal", "phone portal started; holding foreground network");
    httpd_uri_t get{};
    get.uri = "/*";
    get.method = HTTP_GET;
    get.handler = Get;
    httpd_uri_t post{};
    post.uri = "/*";
    post.method = HTTP_POST;
    post.handler = Post;
    if (httpd_register_uri_handler(server, &get) != ESP_OK ||
        httpd_register_uri_handler(server, &post) != ESP_OK) {
        StopPortal();
        return {};
    }
    char address[32];
    snprintf(address, sizeof(address), IPSTR, IP2STR(&ip.ip));
    return std::string("http://") + address + ":" + std::to_string(kServerPort) + secret;
}
void StopPortal() {
    if (server) {
        httpd_stop(server);
        server = nullptr;
        ESP_LOGI("NoveliaPortal", "phone portal stopped");
    }
    network_hold.reset();
}
} // namespace novelia
