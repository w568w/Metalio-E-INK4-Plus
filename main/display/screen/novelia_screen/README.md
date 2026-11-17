# Novelia

在 Metalio E-Ink4-Plus 上浏览 Novelia 收藏夹，下载并使用设备原有阅读器阅读章节。支持 Sakura、GPT、有道及日文原文。

## 使用

1. 连接 Wi-Fi、插入 SD 卡并关闭 U 盘传输。
2. 在系统主页后续页打开 **Novelia → 设置 → 通过 WiFi 配置账户与代理设置**。
3. 手机与设备连接同一 Wi-Fi，扫码打开设备页面。需要代理时填写 Trojan 节点，「测试连接」仅测试当前表单，不保存；确认成功后点击「保存代理」。密码留空表示保留已有节点密码。
4. 在手机页面输入 Novelia 账号和密码。登录成功后设备显示收藏夹；依次进入小说和目录，选择翻译，再打开章节。

节点地址和 TLS SNI 分别配置；「跳过节点证书验证」只影响 Trojan 外层，目标网站 HTTPS 始终验证证书。手机页面在取消、离页、待机或十分钟后关闭，成功登录后也会关闭。

盖板上一页／下一页切换列表页；已访问的收藏页在当前会话直接复用，重启后也优先使用 SD 快照。「刷新」重新拉取当前页。加载提示固定在标题栏，不改变列表高度；长书名会自动缩小字号。阅读页沿用原有字体、排版、触摸和音量键。章末继续翻页会下载下一章，章首向前翻会进入上一章末页。短按 HOME 先关闭阅读浮层，再返回 Novelia 目录；单章阅读页隐藏目录入口。长按 HOME 返回系统主页。

「继续阅读」打开上次成功显示的章节，并使用阅读器保存的章内位置。下一章预取默认开启，可在设置中关闭。所选翻译缺失时会提示可用版本，由用户切换。

设置中的「本地缓存」可统计占用并删除所有账号的收藏、目录快照及已下载章节。清理后保留登录、代理设置和阅读位置，章节在下次阅读时重新下载。

数据位于 SD 的 `metalio/e-ink/novelia/`。账号、代理及翻译偏好保存在 `config.json`；账号密码不保存，登录 token 和代理密码保存在本机配置中。收藏和目录快照按账号区分；章节 EPUB 与阅读记录独立保存。章内进度、书签与分页索引沿用系统统一存储，外部书籍使用独立命名空间。退出登录保留缓存，重新登录后可以继续使用。

网络失败时显示收藏及目录的离线快照；已下载的章节可以离线阅读。插图采用 JPEG／PNG，单张下载上限 2 MiB，超限、格式不支持或下载失败显示文字占位。

## 构建与检查

使用 ESP-IDF 6.1。首次使用先将根目录 `sdkconfig` 复制到 `build/novelia/sdkconfig`，并关闭 `CONFIG_METALIO_TOUCH_UPGRADE_ON_BOOT`。

增量构建、应用分区刷写与启动日志采集：

```sh
uv run scripts/flash_app.py --idf /path/to/esp-idf
```

脚本使用 `build/novelia/sdkconfig`，产物为 `build/novelia/Metalio-E-Ink4-Plus.bin`。首次刷入时，bootloader 与应用必须使用同一 SDK 构建。

主机检查需要系统 cJSON、zlib、SDL2 开发库、CMake、C++17 编译器、Node.js 及已编译的 Mbed TLS 3.6：

```sh
. /path/to/esp-idf/export.sh
python tests/novelia/run.py \
  --idf /path/to/esp-idf \
  --mbedtls /path/to/mbedtls \
  --mbedtls-build /path/to/mbedtls-build
```

检查覆盖响应解析、EPUB ZIP/XML/CRC/图片、HTTP 分帧与截断、FatFS 替换与恢复、离线缓存、手机表单动作与断线重试、缓存统计和删除失败，以及使用设备字库的无窗口界面渲染。界面截图与控件边界记录输出到 `build/novelia-emulator-preview/screens/`。
