"""Host checks for Novelia models, EPUB generation and streamed HTTP responses."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--idf', required=True, type=Path)
parser.add_argument('--mbedtls', required=True, type=Path)
parser.add_argument('--mbedtls-build', required=True, type=Path)
args = parser.parse_args()
tests = Path(__file__).resolve().parent
repo = tests.parents[1]
app = repo / "main/display/screen/novelia_screen"
proxy = repo / 'components/proxy_transport'
storage = repo / 'main/boards/common/storage'

def run(*cmd):
    subprocess.run([str(arg) for arg in cmd], check=True)

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    includes = ['-I' + str(app), '-I' + str(repo / 'main'), '-I/usr/include/cjson', '-I' + str(proxy / 'include'),
                '-I' + str(args.mbedtls / 'include')]
    compile = [os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
               '-Wno-missing-field-initializers', *includes]
    run(*compile, app / 'novelia_model.cc', app / 'novelia_epub.cc',
        tests / 'model_test.cc', '-lcjson', '-lz', '-o', temp / 'model')
    epub = temp / 'chapter.epub'
    run(temp / 'model', epub)
    with zipfile.ZipFile(epub) as book:
        assert book.namelist()[0] == 'mimetype'
        assert book.testzip() is None
        for name in ['META-INF/container.xml', 'content.opf', 'chapter.xhtml']:
            ET.fromstring(book.read(name))
        assert book.read('img.png') == b'image-bytes'
    host_paths = ['-I' + str(storage),
                  '-I' + str(repo / 'tools/ebook/emulator/src/esp_compat'),
                  '-DSD_MOUNT_POINT="' + str(temp) + '"',
                  '-DSD_APP_ROOT="' + str(temp / 'metalio/e-ink') + '"']
    run(*compile, *host_paths, '-ffunction-sections', '-fdata-sections', app / 'novelia_service.cc',
        app / 'novelia_model.cc', app / 'novelia_epub.cc', app / 'novelia_download.cc',
        app / 'novelia_cache.cc', storage / 'sd_file.cc', tests / 'files_test.cc',
        proxy / 'transport.cc', proxy / 'tls_stream.cc', proxy / 'trojan.cc',
        '-L' + str(args.mbedtls_build / 'library'), '-lmbedtls', '-lmbedx509', '-lmbedcrypto', '-lcjson', '-lz',
        '-Wl,--gc-sections', '-Wl,--wrap=rename', '-Wl,--wrap=remove', '-o', temp / 'files')
    run(temp / 'files', temp / 'config.json', epub)
    run(sys.executable, repo / 'tests/run_storage_tests.py', '--mbedtls', args.mbedtls,
        '--mbedtls-build', args.mbedtls_build)
    run('node', tests / 'portal_test.js', app / 'novelia_portal.cc')
preview = repo / 'build/novelia-emulator-preview'
run('cmake', '-S', repo / 'tools/ebook/emulator', '-B', preview, '-DNOVELIA_UI_PREVIEW=ON')
run('cmake', '--build', preview, '--target', 'novelia_preview', '-j', '4')
run(preview / 'bin/novelia_preview', repo / 'main/assets/fonts/font.fontpack', preview / 'screens')
host = repo / 'build/proxy-tests-http'
host.mkdir(parents=True, exist_ok=True)
with (host / 'test-build.log').open('w') as log:
    subprocess.run(['bash', '-c', '. "$1/export.sh" >/dev/null && shift && exec idf.py "$@"',
        'idf-test', str(args.idf), '--preview', '-C', str(proxy / 'tests/idf_host'), '-B', str(host),
        '-DSDKCONFIG=' + str(host / 'sdkconfig'), '-DIDF_TARGET=linux', 'build'], check=True, stdout=log, stderr=subprocess.STDOUT)
run(host / 'proxy_http_test.elf')
print('PASS: models, EPUB ZIP/XML/CRC/images, HTTP chunking/headers/limits/truncation/EOF, FatFS replacement/rollback/recovery, cache policy/cleanup/protection, form actions, external book paths and headless UI layout')
