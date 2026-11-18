# Linux 模拟器

这是一个在设备上运行 RV32IMA Linux 的模拟器。从 SD 卡上选一个内核镜像启动后，用系统软键盘在终端里操作它。固件构建和刷写见根目录 README。

## 安装镜像

1. 在仓库根目录运行 `python tests/linux/prepare.py`，它从 [mini-rv32ima-images](https://github.com/cnlohr/mini-rv32ima-images) 的固定提交下载参考镜像 `linux-6.1.14-rv32nommu-cnl-1`（Buildroot，登录名 `root`，密码为空），校验 SHA256 后写到 `build/linux-assets/metalio/e-ink/linux/Image`。
2. 把 `build/linux-assets/` 下的 `metalio` 目录合并复制到 SD 卡根目录。
3. 打开主页的 Linux 应用，选取镜像启动。

镜像目录是 SD 卡上的 `metalio/e-ink/linux/`，需要 RV32IMA 无 MMU 的裸 `Image`。应用会直接启动上次用过的镜像。

## 使用

1. 输入内容后按确认键发送。
2. 「中断」发送 Ctrl-C。
3. 列表和终端都可以滑动翻页，也支持盖板键的上一页、下一页。
4. 盖板键返回结束虚拟机。

待机时虚拟机暂停，唤醒后继续。

## 限制

虚拟机内存固定 12 MiB，参考镜像在 8 MiB 下进不到 Shell，10 MiB 下跑外部命令会内存不足。终端只支持行式 Shell，`vi` 这类全屏程序用不了。

文件改动只存在虚拟机的内存里，退出后丢失；虚拟机只有串口，没有网络，也看不到 SD 卡上的其他内容。

## 检查

```sh
python tests/novelia/run.py --idf /path/to/esp-idf --linux-image build/linux-assets/metalio/e-ink/linux/Image
```

`linux_boot_test` 用与固件相同的核心和设备树启动参考镜像，验证登录、Shell 运算、`uname` 和关机，没有镜像时跳过。`linux_console_test` 检查折行、退格和翻页取值。`linux_preview` 渲染中英文界面截图到 `build/novelia-emulator-preview/linux-screens/`。

## 维护

模拟核心是 [mini-rv32ima](https://github.com/cnlohr/mini-rv32ima) 提交 `84858f58cb41899705e2ff2d6ee3b2d5c0795bfe`，`vendor/mini-rv32ima.h` 原样保留，按 MIT 许可使用，许可证见 `vendor/LICENSE`。参考镜像、内核配置和构建来源见 [mini-rv32ima-images](https://github.com/cnlohr/mini-rv32ima-images/tree/7da8c6e486b7d510426b5c2283cd338abcf04eb7)。

虚拟机的内存布局写在 `linux.dts`，内存节点排除顶端 16 KiB 留给设备树。改动后同步 `linux_vm.h` 的常量，再运行 `python tests/linux/prepare.py --dtb-only` 重新生成 `linux_dtb.h`，这一步需要主机安装 `dtc`。
