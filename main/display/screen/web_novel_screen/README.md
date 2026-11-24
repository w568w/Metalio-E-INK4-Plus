# Web Novel（Novelia 与 Novalpie）

在 Metalio E-Ink4-Plus 上阅读在线小说站点（目前支持 `n.novelia.cc` 与 `novalpie.cc`），每章下载成一个 EPUB，由设备原有阅读器打开。

## 使用

1. 在设备主页打开 **Novelia** 或 **Novalpie** → **设置** → **通过 WiFi 配置账户与代理设置**。
2. 手机连接与设备相同的 Wi-Fi，扫码打开配置页。
3. 如需代理，填写 Trojan 节点并保存（可先点击「测试连接」）。代理配置由所有站点共用，保存于 SD 的 `metalio/e-ink/web_novel/proxy.json`。
4. 在手机页面登录对应站点账号：
   - **Novelia**：输入账号密码登录，登录后显示收藏夹，进入小说即可打开章节。
   - **Novalpie**：输入账号密码并完成 hCaptcha，或展开「手动导入会话」粘贴网页版 `auth_token`。
5. 阅读时打开原生阅读菜单的「目录」，可分页查看全书章节并直接跳转。

各站点的账号与缓存数据分别位于 SD 的 `metalio/e-ink/novelia/` 和 `metalio/e-ink/novalpie/`。

## 构建

使用 ESP-IDF 6.1。首次构建前把根目录 `sdkconfig` 复制到 `build/novelia/sdkconfig`，并关闭 `CONFIG_METALIO_TOUCH_UPGRADE_ON_BOOT`。

```bash
export IDF_PATH=/path/to/esp-idf
source "$IDF_PATH/export.sh"
idf.py -B build/novelia -D SDKCONFIG=build/novelia/sdkconfig set-target esp32s3
idf.py -B build/novelia -D SDKCONFIG=build/novelia/sdkconfig build
```

## 测试

无需硬件即可在 Linux 主机上运行全部自动化测试（需要 `cmake`、`g++`、`node`、`python3` 和本地 `cJSON` / `zlib` / `mbedTLS` / `ESP-IDF` 源码）：

```bash
python tests/novelia/run.py --idf /path/to/esp-idf
```

界面截图和控件坐标写到 `build/novelia-emulator-preview/screens/`。
