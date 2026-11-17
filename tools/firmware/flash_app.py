# /// script
# requires-python = ">=3.10"
# dependencies = ["esptool==5.6.0", "pyserial>=3.5"]
# ///
"""Build and flash one Metalio app, then report actual startup milestones."""
from __future__ import annotations

import argparse
import contextlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

ROOT = Path(__file__).resolve().parents[2]
STOP = False


def request_stop(*_):
    global STOP
    STOP = True


def find_port(requested):
    if requested:
        return requested
    devices = [p for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]
    if len(devices) != 1:
        raise RuntimeError("没找到唯一的 Espressif USB 设备，请用 --port 指定")
    return devices[0].device


def stop_previous_monitor(state_dir):
    # 上一次采集还在跑就先收掉；没收掉的话后面的串口占用检查会报出来。
    record = state_dir / "monitor.pid"
    if not record.exists():
        return
    with contextlib.suppress(ValueError, ProcessLookupError, PermissionError):
        os.kill(int(record.read_text()), signal.SIGTERM)
    record.unlink(missing_ok=True)


def connect_serial(port, dtr=False):
    connection = serial.Serial()
    connection.port = port
    connection.baudrate = 115200
    connection.timeout = 0.2
    connection.exclusive = True
    connection.rts = False
    connection.dtr = dtr
    connection.open()
    return connection


def build_app(args, directory):
    command = ["idf.py", "-B", str(directory), "-DSDKCONFIG=" + str(directory / "sdkconfig"), "app"]
    idf = args.idf or os.environ.get("IDF_PATH")
    if idf:
        command = ["bash", "-c", '. "$1/export.sh" >/dev/null && shift && exec idf.py "$@"',
                   "idf-build", str(Path(idf).resolve()), *command[1:]]
    elif not shutil.which("idf.py"):
        raise RuntimeError("请激活 ESP-IDF，或指定 --idf /path/to/esp-idf")
    print("[构建] 增量构建 app", flush=True)
    with (directory / "flash-build.log").open("w") as log:
        result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise RuntimeError("构建失败，见 " + str(directory / "flash-build.log"))


def observe(connection, port, args, directory, timings, pipeline_start):
    started = time.monotonic()
    deadline = started + args.boot_timeout
    markers = {"app": b"main_task: Calling app_main()", "home": b"HomeScreen: home page", "storage": b"SD ok at /sdcard"}
    pending = set(markers)
    ready = False
    recent = b""
    try:
        with (directory / "flash-boot.log").open("wb") as log:
            while not STOP:
                now = time.monotonic()
                if ready and now >= deadline:
                    break
                if not ready and now >= deadline:
                    raise RuntimeError("启动等待超时，未见到 " + "、".join(sorted(pending)) + "，见 "
                                       + str(directory / "flash-boot.log"))
                try:
                    data = connection.read(8192)
                except (serial.SerialException, OSError):
                    connection.close()
                    while not STOP and time.monotonic() < deadline:
                        try:
                            connection = connect_serial(port, dtr=True)
                            break
                        except (serial.SerialException, OSError):
                            time.sleep(0.1)
                    continue
                if not data:
                    continue
                log.write(data)
                log.flush()
                recent = (recent + data)[-32768:]
                if any(message in recent for message in [b"Guru Meditation", b"assert failed", b"abort() was called"]):
                    raise RuntimeError("设备启动异常，见 " + str(directory / "flash-boot.log"))
                for name in list(pending):
                    if markers[name] in recent:
                        timings["boot_" + name] = round(time.monotonic() - started, 2)
                        pending.remove(name)
                        print(f"[启动] {name}: {timings['boot_' + name]} 秒", flush=True)
                if not ready and args.ready not in pending:
                    ready = True
                    timings["total_to_ready"] = round(time.monotonic() - pipeline_start, 2)
                    (directory / "flash-timings.json").write_text(json.dumps(timings, indent=2) + "\n")
                    print("[完成] " + json.dumps(timings, ensure_ascii=False), flush=True)
                    if not args.observe:
                        break
                    deadline = time.monotonic() + args.observe
                    print(f"[观察] 继续采集 {args.observe} 秒日志，设备可正常使用", flush=True)
    finally:
        with contextlib.suppress(OSError, serial.SerialException):
            connection.dtr = False
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/novelia")
    parser.add_argument("--idf", help="ESP-IDF source directory")
    parser.add_argument("--no-build", action="store_true", help="flash the existing build")
    parser.add_argument("--port")
    parser.add_argument("--ready", choices=["app", "home", "storage"], default="home")
    parser.add_argument("--boot-timeout", type=float, default=90)
    parser.add_argument("--observe", type=float, default=0, help="continue logging after startup, in seconds")
    args = parser.parse_args()
    os.chdir(ROOT)
    directory = args.build_dir.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    state_dir = directory / "flash-state"
    state_dir.mkdir(exist_ok=True)
    started = time.monotonic()
    timings = {}
    (directory / "flash-timings.json").unlink(missing_ok=True)
    if not args.no_build:
        build_app(args, directory)
    timings["build"] = round(time.monotonic() - started, 2)
    settings = (directory / "sdkconfig").read_text()
    if "CONFIG_METALIO_TOUCH_UPGRADE_ON_BOOT=y" in settings:
        raise RuntimeError("当前构建启用了触摸固件升级，请先在试刷配置中关闭")
    manifest = json.loads((directory / "flasher_args.json").read_text())
    if manifest["extra_esptool_args"]["chip"] != "esp32s31":
        raise RuntimeError("此脚本用于 Metalio ESP32-S31")
    app = manifest["app"]
    app_file = directory / app["file"]
    stop_previous_monitor(state_dir)
    port = find_port(args.port)
    if shutil.which("fuser"):
        users = subprocess.run(["fuser", port], capture_output=True, text=True).stdout.strip()
        if users:
            raise RuntimeError(f"串口被其他进程占用，请先关闭监视器：PID {users}")
    record = state_dir / "monitor.pid"
    try:
        phase = time.monotonic()
        command = [sys.executable, "-m", "esptool", "--chip", "esp32s31", "--port", port,
                   "--before", "usb-reset", "--after", "hard-reset", "write-flash",
                   "--skip-flashed", "--flash-mode", "keep", "--flash-freq", "keep",
                   "--flash-size", "keep", app["offset"], str(app_file)]
        print("[刷写] 应用分区；esptool 压缩传输并校验", flush=True)
        with (directory / "flash-write.log").open("w") as log:
            result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise RuntimeError("刷写失败，见 " + str(directory / "flash-write.log"))
        timings["flash"] = round(time.monotonic() - phase, 2)
        # 刷写完 CLI 会复位芯片，USB JTAG 可能重新枚举。
        deadline = time.monotonic() + args.boot_timeout
        while True:
            try:
                connection = connect_serial(port, dtr=True)
                break
            except (serial.SerialException, OSError):
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.1)
        record.write_text(str(os.getpid()))
        observe(connection, port, args, directory, timings, started)
    finally:
        record.unlink(missing_ok=True)


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    try:
        main()
    except (RuntimeError, OSError) as error:
        print("[失败] " + str(error), file=sys.stderr)
        sys.exit(1)
