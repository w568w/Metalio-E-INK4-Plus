#!/usr/bin/env python3
"""准备 Linux 内核镜像，或从 DTS 重新生成设备树头文件。"""
import argparse
import hashlib
import io
from pathlib import Path
import subprocess
import tempfile
import urllib.request
import zipfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'main/display/screen/linux_screen'
URL = 'https://raw.githubusercontent.com/cnlohr/mini-rv32ima-images/7da8c6e486b7d510426b5c2283cd338abcf04eb7/images/linux-6.1.14-rv32nommu-cnl-1.zip'
ARCHIVE_SHA256 = 'add651195348b538c309becb39c5f8ef4f9d15ec275a2954b02016fc38091393'
IMAGE_SHA256 = '5f596134705d5aa8e7c8c406695a560d08faaff4a86bc715659e53cbebba6c7e'

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--dtb-only', action='store_true')
args = parser.parse_args()
if args.dtb_only:
    with tempfile.TemporaryDirectory() as temp:
        dtb = Path(temp) / 'linux.dtb'
        subprocess.run(['dtc', '-I', 'dts', '-O', 'dtb', '-o', str(dtb), str(SOURCE / 'linux.dts')], check=True)
        data = dtb.read_bytes()
    rows = ''.join('    ' + ', '.join(f'0x{x:02x}' for x in data[i:i+16]) + ',\n'
                   for i in range(0, len(data), 16))
    (SOURCE / 'linux_dtb.h').write_text('// 由 linux.dts 经 dtc 生成，更新命令见 README。\n'
                                          'static constexpr unsigned char kDtb[] = {\n' + rows + '};\n')
else:
    destination = ROOT / 'build/linux-assets/metalio/e-ink/linux/Image'
    if not destination.exists() or hashlib.sha256(destination.read_bytes()).hexdigest() != IMAGE_SHA256:
        with urllib.request.urlopen(URL, timeout=60) as response:
            archive = response.read()
        if hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
            raise SystemExit('Archive SHA256 mismatch')
        with zipfile.ZipFile(io.BytesIO(archive)) as package:
            image = package.read('Image')
        if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
            raise SystemExit('Image SHA256 mismatch')
        destination.parent.mkdir(parents=True, exist_ok=True)
        temporary = destination.with_suffix('.tmp')
        temporary.write_bytes(image)
        temporary.replace(destination)
    print(destination)
