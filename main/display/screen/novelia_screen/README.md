# Novelia

在 Metalio E-Ink4-Plus 上阅读 n.novelia.cc 的小说，目前从收藏夹选书。每章下载成一个 EPUB，由设备原有阅读器打开。

## 使用

1. 连接 Wi-Fi、插入 SD 卡并关闭 U 盘传输。
2. 在主页后续页打开 **Novelia → 设置 → 通过 WiFi 配置账户与代理设置**。
3. 手机与设备连接同一 Wi-Fi，扫码打开设备页面，填写代理节点（可选）后保存。
4. 在手机页面登录 Novelia 账号。登录后设备显示收藏夹，进入小说即可打开章节。
5. 阅读时打开原生阅读菜单的「目录」，可分页查看全书章节并直接跳转。

数据位于 SD 的 `metalio/e-ink/novelia/`。

## 构建

使用 ESP-IDF 6.1。首次构建前把根目录 `sdkconfig` 复制到 `build/novelia/sdkconfig`，并关闭 `CONFIG_METALIO_TOUCH_UPGRADE_ON_BOOT`。

```sh
uv run tools/firmware/flash_app.py --idf /path/to/esp-idf
```

bootloader 与应用要用同一个 SDK 构建。

## 检查

需要系统 cJSON、zlib、SDL2 开发库、CMake、C++17 编译器及 Node.js：

```sh
. /path/to/esp-idf/export.sh
python tests/novelia/run.py --idf /path/to/esp-idf
```

界面截图和控件坐标写到 `build/novelia-emulator-preview/screens/`。
