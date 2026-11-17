"""Host checks for shared SD file replacement and reader data paths."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--mbedtls', required=True, type=Path)
parser.add_argument('--mbedtls-build', required=True, type=Path)
args = parser.parse_args()
tests = Path(__file__).resolve().parent
repo = tests.parent
storage = repo / 'main/boards/common/storage'

def run(*cmd):
    subprocess.run([str(arg) for arg in cmd], check=True)

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    compile = [os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-Wno-missing-field-initializers', '-I' + str(storage), '-I' + str(repo / 'main'),
        '-I' + str(repo / 'tools/ebook/emulator/src/esp_compat'),
        '-I' + str(args.mbedtls / 'include'), '-DSD_MOUNT_POINT="' + str(temp) + '"',
        '-DSD_APP_ROOT="' + str(temp / 'metalio/e-ink') + '"']
    libraries = ['-L' + str(args.mbedtls_build / 'library'), '-lmbedcrypto']
    run(*compile, storage / 'sd_file.cc', tests / 'sd_file_test.cc', *libraries,
        *['-Wl,--wrap=' + name for name in ['rename', 'unlink', 'fwrite', 'fflush', 'fsync', 'fclose']],
        '-o', temp / 'replace')
    run(temp / 'replace', temp)
    run(*compile, '-Wno-unused-variable', '-Wno-unused-parameter', '-ffunction-sections',
        '-fdata-sections', '-I' + str(tests / 'storage_stubs'),
        storage / 'sd_data_layout.cc', storage / 'sd_file.cc', tests / 'sd_data_layout_test.cc',
        *libraries, '-Wl,--gc-sections', '-o', temp / 'layout')
    run(temp / 'layout')
print('PASS: shared storage replacement, recovery, relocation and application book paths')
