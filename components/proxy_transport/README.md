# proxy_transport

ESP-IDF 的 TCP 隧道组件。`Dialer` 按目标地址建立一条字节流，`Stream` 是这条流。

TLS 和代理协议都实现为包在字节流外面的包装层，因此直连、Trojan、目标网站的 TLS 可以自由组合。

## 用法

```cpp
#include "proxy_transport/transport.h"
using namespace proxy_transport;

// 经 Trojan 节点，再与目标网站建立 TLS
TrojanOptions node;
node.server = {"proxy.example", 443};
node.password = password;
node.tls.server_name = "certificate.example"; // 证书名与 TCP 地址分开配置
auto dialer = MakeTrojanDialer(node, MakeDirectDialer());

auto tunnel = dialer->Dial({"n.novelia.cc", 443}, MonotonicMs() + 30000);
if (!tunnel) return tunnel.error; // 负 errno
auto site = WrapTls(std::move(tunnel.stream), {"n.novelia.cc"});
if (!site) return site.error;
// site.stream->Read / Write 传输 HTTP 字节
```

直连更短：`MakeDirectDialer()->Dial({"example.com", 443}, MonotonicMs() + 30000)`。

要让 `esp_http_client` 走同一条隧道，用 `proxy_transport/http.h`：

```cpp
#include "proxy_transport/http.h"

auto response = Request(dialer, "GET", "https://n.novelia.cc/api/...");
// response.status / headers / body；失败时 response.error_code 给出负 errno
```

`Request` 通过 `esp_http_client` 的自定义 transport 使用传入的 `Dialer`。`limit` 限制响应体字节数（默认 2 MiB，`kMaxBodyBytes`），`timeout_ms` 覆盖连接、握手和整段响应。

## 行为约定

- 一个 `Stream` 独占一条连接，只在一个工作任务里读写，析构即关闭。不要在 LVGL 任务里调用。
- `Read` / `Write` 返回实际传输的字节数，允许短读短写；`Read` 返回 0 表示对端关闭，失败返回负 errno。写入整块数据用 `WriteAll`。
- `Dial` 的 `deadline_ms` 是绝对单调时刻，覆盖连接、TLS 握手和随后的每一次读写。平台 DNS 查询不受它约束。
- 证书默认校验；`ca_pem` 为空时使用 ESP 证书包，主机测试必须显式给 PEM。`verify_certificate = false` 只作用于该层，内层 TLS 照常校验。
- Trojan 的目标可以是域名、IPv4 或 IPv6，域名交给节点解析。协议头与首段应用数据合并进第一条 TLS 记录。协议没有服务端应答，拨通不代表密码被接受，连通性需要用一次真实请求确认。
- 传输开始后持续到完成、出错或截止时间到期；取消在操作边界生效。

## 依赖

使用方在 `REQUIRES` 里加入 `proxy_transport`。组件本身 `REQUIRES lwip mbedtls`，`PRIV_REQUIRES esp_http_client tcp_transport`——`http.cc` 会随组件一起编译链接。

Trojan 用 PSA 计算 SHA-224，来自 `CONFIG_MBEDTLS_SHA256_C=y`（IDF 6.1 的 `mbedtls/esp_config.h` 据此定义 `PSA_WANT_ALG_SHA_224`）。固件和主机工程都要打开这个选项。

## 新增一种协议

实现 `Dialer`，在 `Dial` 里返回一个 `Connection`。TLS、HTTP 和上层协议都不用改。`MakeTrojanDialer` 的 `upstream` 参数就是这样用的：测试里换成不连真实网络的实现。

## 主机测试

需要 C++17、Python 3、`openssl` 命令行，以及 Mbed TLS 3.6 的源码与主机构建产物。3.6 的 PSA 代码放在独立子模块里，头文件路径有两条：

```sh
c++ -std=c++17 -Wall -Wextra -Werror \
  -Iinclude -I$MBEDTLS/include -I$MBEDTLS/tf-psa-crypto/include \
  transport.cc tls_stream.cc trojan.cc tests/client.cc \
  -L$MBEDTLS_BUILD/library -lmbedtls -lmbedx509 -lmbedcrypto \
  -o /tmp/proxy-test
python3 tests/integration.py /tmp/proxy-test
```

`integration.py` 在本机起一对临时 TLS 服务与代理，覆盖域名和 IPv4 寻址、SNI、证书拒绝、外层跳过验证、嵌套 TLS、第一次写入超过 16 KiB 的数据，以及超时。

`esp_http_client` 那一层的测试是独立的 IDF 工程：

```sh
idf.py -C tests/idf_host -B /tmp/proxy-http -DIDF_TARGET=linux build
```

协议依据：[Trojan protocol](https://trojan-gfw.github.io/trojan/protocol)。
