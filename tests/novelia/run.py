"""Host checks for Novelia models, EPUB generation and streamed HTTP responses."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

parser = argparse.ArgumentParser()
parser.add_argument('--idf', required=True, type=Path)
parser.add_argument('--linux-image', type=Path)
args = parser.parse_args()
tests = Path(__file__).resolve().parent
repo = tests.parents[1]
app = repo / "main/display/screen/web_novel_screen"
proxy = repo / 'components/proxy_transport'

def run(*cmd):
    subprocess.run([str(arg) for arg in cmd], check=True)

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    host = repo / 'build/host-tests'
    run('cmake', '-S', repo / 'tests', '-B', host, '-DTEST_SD_ROOT=' + str(temp))
    run('cmake', '--build', host, '-j', '4')
    epub = temp / 'chapter.epub'
    run(host / 'model_test', epub)
    with zipfile.ZipFile(epub) as book:
        assert book.namelist()[0] == 'mimetype'
        assert book.testzip() is None
        for name in ['META-INF/container.xml', 'content.opf', 'chapter.xhtml']:
            ET.fromstring(book.read(name))
        assert book.read('img.png') == b'image-bytes'
    run(host / 'files_test', temp / 'config.json', epub)
    run(host / 'sd_file_test', temp)
    run(host / 'sd_data_layout_test')
    run(host / 'reader_toc_test')
    run(host / 'linux_console_test')
    image = args.linux_image or repo / 'build/linux-assets/metalio/e-ink/linux/Image'
    if image.exists():
        run(host / 'linux_boot_test', image)
    else:
        print(f'SKIP linux_boot_test: {image} (run python tests/linux/prepare.py)')
    run('node', tests / 'portal_test.js', app / 'web_novel_portal.cc')
preview = repo / 'build/novelia-emulator-preview'
run('cmake', '-S', repo / 'tools/ebook/emulator', '-B', preview, '-DAPP_UI_PREVIEW=ON')
run('cmake', '--build', preview, '--target', 'novelia_preview', 'linux_preview', '-j', '4')
run(preview / 'bin/linux_preview', repo / 'main/assets/fonts/font.fontpack', preview / 'linux-screens')
run(preview / 'bin/novelia_preview', repo / 'main/assets/fonts/font.fontpack', preview / 'screens')
host = repo / 'build/proxy-tests-http'
host.mkdir(parents=True, exist_ok=True)
with (host / 'test-build.log').open('w') as log:
    subprocess.run(['bash', '-c', '. "$1/export.sh" >/dev/null && shift && exec idf.py "$@"',
        'idf-test', str(args.idf), '--preview', '-C', str(proxy / 'tests/idf_host'), '-B', str(host),
        '-DSDKCONFIG=' + str(host / 'sdkconfig'), '-DIDF_TARGET=linux', 'build'], check=True, stdout=log, stderr=subprocess.STDOUT)
run(host / 'proxy_http_test.elf')
print('PASS: models, EPUB ZIP/XML/CRC/images, HTTP chunking/headers/limits/truncation/EOF, FatFS replacement/rollback/recovery, cache policy/cleanup/protection, form actions, external book paths and headless UI layout')
