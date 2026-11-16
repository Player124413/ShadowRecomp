#!/usr/bin/env python3
"""Tests for tools/android/fetch-xbe.py, which gets default.xbe out of a disc image, an
archive, a folder or a link.

Disc images come from make_test_iso.py. Links are served by a web server in this
process, including one that drops the connection half way through.
"""
import http.server
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SCRIPT = HERE.parent / 'fetch-xbe.py'

sys.dont_write_bytecode = True
sys.path.insert(0, str(HERE))
import make_test_iso  # noqa: E402

spec = importlib.util.spec_from_file_location('fetch_xbe', SCRIPT)
fetch_xbe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch_xbe)

failures = 0
checks = 0
current = ''


def scenario(name):
    global current
    current = name


def check(condition, what):
    global failures, checks
    checks += 1
    if not condition:
        failures += 1
        print(f'FAIL [{current}] {what}')


def fails_with(text, function, *arguments):
    """True if the call raises a Failure whose message contains `text`."""
    try:
        function(*arguments)
    except fetch_xbe.Failure as failure:
        if text in str(failure):
            return True
        print(f'     (the message was: {failure})')
    return False


ROOT = Path(tempfile.mkdtemp(prefix='fetch-xbe-test-'))
WORK = ROOT / 'work'
WORK.mkdir()
XBE = make_test_iso.placeholder_xbe()


def run(*arguments, verify=False):
    """Runs the script; returns (exit code, output)."""
    command = [sys.executable, str(SCRIPT), *map(str, arguments), '--work-dir', str(WORK)]
    if not verify:
        command.append('--no-verify')
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120,
                            env={**os.environ, 'GITHUB_ACTIONS': 'false'})
    return result.returncode, result.stdout


def fetched(source, name='out.xbe'):
    """Runs the script on `source`; returns what it wrote, or its output if it failed."""
    output = ROOT / 'outputs' / name
    output.unlink(missing_ok=True)
    code, text = run(source, '--output', output)
    return output.read_bytes() if code == 0 and output.exists() else text


def refused(text, source):
    """True if the script fails on `source` with a message that contains `text` (or one
    of them, if `text` is a tuple)."""
    result = fetched(source, 'refused.xbe')
    if isinstance(result, str) and any(part in result for part in ([text] if isinstance(text, str) else text)):
        return True
    print(f'     (the result was: {result[:300]!r})')
    return False


def image(name, **options):
    path = ROOT / name
    make_test_iso.write_image(path, **options)
    return path


# ---------------------------------------------------------------------------------------
scenario('xxh3')
# Answers of the reference implementation (xxHash 0.8) for bytes (i * 7 + 3) & 0xFF.


def pattern(length):
    return bytes((index * 7 + 3) & 0xFF for index in range(length))


for length, expected in ((241, 0x8BEADD3A8874FE17), (1024, 0x9B81661C641C72B1), (1025, 0x806C2072ED713576),
                         (65537, 0xF7460AF97425B2C4), (300000, 0x8EA7F53679B9303F)):
    check(fetch_xbe.xxh3_64(pattern(length)) == expected, f'{length} bytes')
check(fetch_xbe.xxh3_64(bytes(range(256)) * 4) == 0xA870F92984398D22, 'four times 0..255')
try:
    fetch_xbe.xxh3_64(b'x' * 240)
    check(False, 'inputs it does not cover are refused')
except ValueError:
    check(True, 'inputs it does not cover are refused')

# ---------------------------------------------------------------------------------------
scenario('known dumps')
real = fetch_xbe.known_hashes(fetch_xbe.KNOWN_HASHES)
check(len(real) >= 1 and len(set(real)) == len(real) and all(real), 'the installer\'s table lists default.xbe')

table = ROOT / 'table.cpp'
table.write_text('''const uint64_t GameHashes[] = {
    11ULL,
    22ULL,
    33ULL,
    44ULL,
    55ULL,
};
const std::pair<const char *, uint32_t> GameFiles[] = {
    { "a/first.one", 1 },
    { "default.xbe", 3 },
    { "z/last.one", 1 },
};
const size_t GameFilesSize = std::size(GameFiles);
''')
check(fetch_xbe.known_hashes(table) == [22, 33, 44], 'the hashes of a file are the ones at its position')
check(fetch_xbe.known_hashes(table, 'Z/LAST.one') == [55], 'names are matched without regard to case')
check(fails_with('does not list', fetch_xbe.known_hashes, table, 'missing.bin'), 'a file that is not listed')
broken = ROOT / 'broken.cpp'
broken.write_text(table.read_text().replace('{ "z/last.one", 1 }', '{ "z/last.one", 4 }'))
check(fails_with('does not add up', fetch_xbe.known_hashes, broken), 'tables that do not match each other')

# ---------------------------------------------------------------------------------------
scenario('disc images')
check(fetched(image('bare.iso')) == XBE, 'a bare file system image')
for offset in (0x18300000, 0x0FD90000, 0x02080000):
    check(fetched(image(f'dump-{offset:x}.iso', partition_offset=offset)) == XBE, f'a disc dump, partition at 0x{offset:X}')
check(fetched(image('odd.iso', partition_offset=0x70000)) == XBE, 'a partition at an unusual offset')
check(fetched(image('upper.iso', xbe_name='DEFAULT.XBE')) == XBE, 'DEFAULT.XBE in upper case')

large = bytes(XBE) + bytes(range(256)) * 9000
check(fetched(image('large.iso', xbe=large)) == large, 'an executable of several megabytes')

code, text = run(ROOT / 'bare.iso', '--output', ROOT / 'outputs' / 'nested' / 'deeper' / 'default.xbe')
check(code == 0 and (ROOT / 'outputs' / 'nested' / 'deeper' / 'default.xbe').read_bytes() == XBE, 'the output folder is created')
check('pipeline test' in text and 'FFFF0000' in text, 'the title of the executable is reported')
check(not list((ROOT / 'outputs').rglob('*.part')), 'no partial output is left behind')

# ---------------------------------------------------------------------------------------
scenario('damaged images')
data = (ROOT / 'bare.iso').read_bytes()
root_sector, = struct.unpack_from('<I', data, 0x10000 + 20)
entries = fetch_xbe.directory_entries(data[root_sector * 2048:root_sector * 2048 + 2048])
xbe_sector = next(sector for name, sector, _, _ in entries if name == 'default.xbe')
check(len(entries) == 7, 'the test image has the entries it was built with')

cut = ROOT / 'cut.iso'
cut.write_bytes(data[:xbe_sector * 2048 + 100])
check(refused('cut short', cut), 'an image that ends inside the executable')

cut.write_bytes(data[:root_sector * 2048 + 10])
check(refused('cut short', cut), 'an image that ends inside the root directory')

no_game = ROOT / 'other.iso'
make_test_iso.write_image(no_game, xbe_name='game.xbe')
check(refused('not a game disc', no_game), 'an image without default.xbe')

noise = ROOT / 'noise.bin'
noise.write_bytes(bytes(range(256)) * 2000)
check(refused('neither an Xbox disc image nor an archive', noise), 'a file that is no image at all')

empty_root = bytearray(data)
struct.pack_into('<II', empty_root, 0x10000 + 20, 0, 0)
(ROOT / 'noroot.iso').write_bytes(empty_root)
check(refused('no root directory', ROOT / 'noroot.iso'), 'an image without a root directory')

# Make a child of the root entry its own child. The search must still end, with whatever
# entries it reached before going in circles.
loop = bytearray(data)
left, right = struct.unpack_from('<HH', loop, root_sector * 2048)
child = left or right
struct.pack_into('<HH', loop, root_sector * 2048 + child * 4, child, child)
(ROOT / 'loop.iso').write_bytes(loop)
result = fetched(ROOT / 'loop.iso')
check(result == XBE or (isinstance(result, str) and 'not a game disc' in result), 'a directory that loops does not hang')

not_xbe = ROOT / 'notxbe.iso'
make_test_iso.write_image(not_xbe, xbe=b'MZ' + bytes(5000))
check(refused('not an Xbox executable', not_xbe), 'a default.xbe that is something else')

# ---------------------------------------------------------------------------------------
scenario('archives')


def zipped(name, members, compression=zipfile.ZIP_DEFLATED):
    path = ROOT / name
    with zipfile.ZipFile(path, 'w', compression) as archive:
        for member, content in members.items():
            if isinstance(content, Path):
                archive.write(content, member)
            else:
                archive.writestr(member, content)
    return path


check(fetched(zipped('image.zip', {'Shadow the Hedgehog (USA).iso': ROOT / 'bare.iso', 'readme.txt': 'hi'})) == XBE,
      'a .zip with a disc image')
check(fetched(zipped('stored.zip', {'game.iso': ROOT / 'bare.iso'}, zipfile.ZIP_STORED)) == XBE, 'a .zip without compression')
check(fetched(zipped('dump.zip', {'dump.iso': ROOT / 'dump-2080000.iso'})) == XBE, 'a .zip with a disc dump')
check(fetched(zipped('files.zip', {'Shadow/default.xbe': XBE, 'Shadow/Shadow.one': b'x' * 100000,
                                   'Shadow/backup/default.xbe': b'XBEH decoy further down'})) == XBE,
      'a .zip with the game\'s files takes the default.xbe nearest the top')
check(fetched(zipped('upper.zip', {'GAME\\DEFAULT.XBE': XBE})) == XBE, 'backslashes and upper case in a .zip')
check(fetched(zipped('nested.zip', {'inner.zip': ROOT / 'image.zip'}, zipfile.ZIP_STORED)) == XBE, 'a .zip in a .zip')
check(refused('empty', zipped('empty.zip', {})), 'an empty .zip')

damaged = ROOT / 'damaged.zip'
damaged.write_bytes((ROOT / 'image.zip').read_bytes()[:(ROOT / 'image.zip').stat().st_size // 2])
check(refused('damaged', damaged), 'a .zip that was cut short')

deep = ROOT / 'bare.iso'
for level in range(6):
    deep = zipped(f'deep{level}.zip', {'inner.bin': deep}, zipfile.ZIP_STORED)
check(refused('nested too deeply', deep), 'archives nested without end')

seven_zip = shutil.which('7z') or shutil.which('7zz') or shutil.which('7za')
if seven_zip:
    path = ROOT / 'image.7z'
    subprocess.run([seven_zip, 'a', '-bd', '-y', str(path), str(ROOT / 'bare.iso')], check=True, stdout=subprocess.DEVNULL)
    check(fetched(path) == XBE, 'a .7z with a disc image')

    path = ROOT / 'files.7z'
    folder = ROOT / 'tree' / 'Shadow The Hedgehog'
    folder.mkdir(parents=True)
    (folder / 'Default.xbe').write_bytes(XBE)
    (folder / 'Shadow.one').write_bytes(b'x' * 100000)
    subprocess.run([seven_zip, 'a', '-bd', '-y', str(path), str(ROOT / 'tree')], check=True, stdout=subprocess.DEVNULL)
    check(fetched(path) == XBE, 'a .7z with the game\'s files')

    path = ROOT / 'secret.7z'
    subprocess.run([seven_zip, 'a', '-bd', '-y', '-psesame', '-mhe=on', str(path), str(ROOT / 'bare.iso')], check=True, stdout=subprocess.DEVNULL)
    check(refused('password', path), 'a .7z with a password is refused, not waited for')

    path = ROOT / 'secret.zip'
    subprocess.run([seven_zip, 'a', '-bd', '-y', '-tzip', '-psesame', str(path), str(ROOT / 'bare.iso')], check=True, stdout=subprocess.DEVNULL)
    check(refused('password', path), 'a .zip with a password')

    path = ROOT / 'bare.iso.gz'
    subprocess.run([seven_zip, 'a', '-bd', '-y', '-tgzip', str(path), str(ROOT / 'bare.iso')], check=True, stdout=subprocess.DEVNULL)
    check(fetched(path) == XBE, 'a .gz with a disc image')
else:
    print('7z is not installed: the tests of .7z archives are skipped')
    seven = ROOT / 'fake.7z'
    seven.write_bytes(b"7z\xbc\xaf'\x1c" + bytes(100000))
    check(refused('7z is not installed', seven), 'a .7z without 7z installed')

rar = ROOT / 'fake.rar'
rar.write_bytes(b'Rar!\x1a\x07\x01\x00' + bytes(100000))
check(refused(('RAR', 'Could not unpack'), rar), 'a .rar that cannot be unpacked')

# ---------------------------------------------------------------------------------------
scenario('folders')
folder = ROOT / 'game'
(folder / 'event').mkdir(parents=True)
(folder / 'default.xbe').write_bytes(XBE)
(folder / 'event' / 'default.xbe').write_bytes(b'XBEH not this one')
check(fetched(folder) == XBE, 'a folder with the game\'s files')
check(fetched(folder / 'default.xbe') == XBE, 'default.xbe itself')

folder = ROOT / 'downloads'
folder.mkdir()
shutil.copy(ROOT / 'bare.iso', folder / 'Shadow.iso')
(folder / 'notes.txt').write_text('small')
check(fetched(folder) == XBE, 'a folder with a disc image in it')

folder = ROOT / 'nothing'
folder.mkdir()
check(refused('Found neither', folder), 'an empty folder')
check(refused('does not exist', ROOT / 'missing.iso'), 'a source that does not exist')

# ---------------------------------------------------------------------------------------
scenario('verification')
accepted = ROOT / 'accepted.cpp'
accepted.write_text(table.read_text().replace('22ULL', f'{fetch_xbe.xxh3_64(XBE)}ULL'))
output = ROOT / 'outputs' / 'verified.xbe'
code, text = run(ROOT / 'bare.iso', '--output', output, '--hashes', accepted, verify=True)
check(code == 0 and output.read_bytes() == XBE and 'known dump' in text, 'a dump that is listed is accepted')

output = ROOT / 'outputs' / 'rejected.xbe'
code, text = run(ROOT / 'bare.iso', '--output', output, '--hashes', table, verify=True)
check(code != 0 and not output.exists(), 'a dump that is not listed is refused and nothing is written')
check('is not one of the 3 dumps' in text and 'pipeline test' in text, 'and the message says what it is instead')

code, text = run(ROOT / 'bare.iso', '--output', output, verify=True)
check(code != 0 and not output.exists(), 'the placeholder is not a dump the installer knows')

code, text = run(ROOT / 'bare.iso', '--output', output)
check(code == 0 and output.read_bytes() == XBE, '--no-verify accepts it')

# ---------------------------------------------------------------------------------------
scenario('share links')
resolve = fetch_xbe.resolve_link
drive = 'https://drive.usercontent.google.com/download?export=download&confirm=t&id=1AbC_dEf-GhIjKlMnOpQrStUvWxYz012345'
for link in ('https://drive.google.com/file/d/1AbC_dEf-GhIjKlMnOpQrStUvWxYz012345/view?usp=sharing',
             'https://drive.google.com/file/d/1AbC_dEf-GhIjKlMnOpQrStUvWxYz012345/view?usp=drive_link',
             'https://drive.google.com/open?id=1AbC_dEf-GhIjKlMnOpQrStUvWxYz012345',
             'https://drive.google.com/uc?export=download&id=1AbC_dEf-GhIjKlMnOpQrStUvWxYz012345',
             'https://docs.google.com/uc?id=1AbC_dEf-GhIjKlMnOpQrStUvWxYz012345',
             drive):
    check(resolve(link) == drive, f'Google Drive: {link.split("?")[0][-40:]}')
check(fails_with('not a link to a single file', resolve, 'https://drive.google.com/drive/folders/1AbC_dEf-GhIjKlMnOpQrStUvWxYz012345'),
      'a Google Drive folder')
check(resolve('https://www.dropbox.com/s/abc123/Shadow.iso?dl=0') == 'https://www.dropbox.com/s/abc123/Shadow.iso?dl=1', 'Dropbox')
check(resolve('https://www.dropbox.com/scl/fi/abc/Shadow.iso?rlkey=k1&st=x&dl=0') ==
      'https://www.dropbox.com/scl/fi/abc/Shadow.iso?rlkey=k1&st=x&dl=1', 'Dropbox, new style of link')
check(resolve('https://www.dropbox.com/s/abc123/Shadow.iso') == 'https://www.dropbox.com/s/abc123/Shadow.iso?dl=1', 'Dropbox without dl')

asked = []


def yandex(url):
    asked.append(url)
    return {'href': 'https://downloader.disk.yandex.ru/disk/abc/file.iso?token=1', 'method': 'GET'}


check(resolve('https://disk.yandex.ru/d/AbCdEf123456', yandex) == 'https://downloader.disk.yandex.ru/disk/abc/file.iso?token=1', 'Yandex Disk')
check(asked == ['https://cloud-api.yandex.net/v1/disk/public/resources/download?public_key=https%3A%2F%2Fdisk.yandex.ru%2Fd%2FAbCdEf123456'],
      'the public link is passed to the Yandex Disk API')
check(resolve('https://yadi.sk/d/AbCdEf123456', yandex).startswith('https://downloader.disk.yandex.ru/'), 'yadi.sk')
check(fails_with('did not give a download link', resolve, 'https://disk.yandex.ru/d/x', lambda url: {}), 'Yandex Disk without a link')


def unreachable(url):
    raise OSError('HTTP Error 404: Not Found')


check(fails_with('404', resolve, 'https://disk.yandex.ru/d/x', unreachable), 'a Yandex Disk link that does not exist')
plain = 'https://archive.org/download/item/Shadow%20the%20Hedgehog%20(USA).iso'
check(resolve(plain) == plain, 'other links are left alone')

# ---------------------------------------------------------------------------------------
scenario('links')
SERVED = {
    '/game.iso': (ROOT / 'bare.iso').read_bytes(),
    '/game.zip': (ROOT / 'image.zip').read_bytes(),
    '/page': b'<!DOCTYPE html>\n<html><head><title>Download</title></head><body>Click here</body></html>',
    '/error.xml': b'<?xml version="1.0" encoding="UTF-8"?>\n<Error><Code>AccessDenied</Code></Error>',
    '/empty': b'',
    '/flaky.iso': (ROOT / 'large.iso').read_bytes(),
    '/norange.iso': (ROOT / 'large.iso').read_bytes(),
}
requests = []


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *arguments):
        pass

    def do_GET(self):
        path = self.path.split('?')[0]
        byte_range = self.headers.get('Range')
        requests.append((path, byte_range))

        if path == '/moved':
            self.send_response(302)
            self.send_header('Location', '/game.iso')
            self.send_header('Content-Length', '0')
            self.end_headers()
            return
        if path not in SERVED:
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return

        body = SERVED[path]
        first = path in ('/flaky.iso', '/norange.iso') and sum(1 for seen, _ in requests if seen == path) == 1
        start = 0
        if byte_range and path != '/norange.iso':
            start = int(byte_range.split('=')[1].split('-')[0])
            self.send_response(206)
            self.send_header('Content-Range', f'bytes {start}-{len(body) - 1}/{len(body)}')
        else:
            self.send_response(200)
        self.send_header('Content-Length', str(len(body) - start))
        self.end_headers()

        try:
            if first:
                # Promise the whole file, send a part of it and hang up.
                self.wfile.write(body[:len(body) // 3])
                self.wfile.flush()
                self.close_connection = True
                self.connection.shutdown(2)
            else:
                self.wfile.write(body[start:])
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True    # curl gave up on the response, as some tests expect


if shutil.which('curl'):
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_address[1]}'
    fetch_xbe.RETRY_PAUSE = 0
    fetch_xbe.say = lambda message: None

    check(fetched(f'{base}/game.iso') == XBE, 'a link to a disc image')
    check(fetched(f'{base}/game.zip?download=1') == XBE, 'a link to a .zip')
    check(fetched(f'{base}/moved') == XBE, 'a link that redirects')
    check(refused('did not work', f'{base}/gone.iso') and refused('404', f'{base}/gone.iso'), 'a dead link')
    check(refused('web page', f'{base}/page'), 'a link to a web page')
    check(refused('web page', f'{base}/error.xml'), 'a link to an error document')
    check(refused('empty file', f'{base}/empty'), 'a link to nothing')
    check(fails_with('The download', fetch_xbe.download, 'http://127.0.0.1:1/game.iso', ROOT / 'nowhere.download'),
          'a server that is not there')

    target = ROOT / 'flaky.download'
    fetch_xbe.download(f'{base}/flaky.iso', target)
    check(target.read_bytes() == SERVED['/flaky.iso'], 'a dropped connection is resumed')
    resumed = [byte_range for path, byte_range in requests if path == '/flaky.iso']
    check(len(resumed) == 2 and resumed[0] is None and resumed[1] == f'bytes={len(SERVED["/flaky.iso"]) // 3}-',
          'the second request asks for the rest only')

    target = ROOT / 'norange.download'
    fetch_xbe.download(f'{base}/norange.iso', target)
    check(target.read_bytes() == SERVED['/norange.iso'], 'a server that cannot resume is asked for the whole file again')
    check(len([1 for path, _ in requests if path == '/norange.iso']) == 3, 'after one attempt to resume')

    server.shutdown()
else:
    print('curl is not installed: the tests of links are skipped')

# ---------------------------------------------------------------------------------------
scenario('cleanup')
check(not list(WORK.iterdir()), 'nothing is left in the work folder')

shutil.rmtree(ROOT, ignore_errors=True)
print(f'{checks} checks, {failures} failures')
sys.exit(1 if failures else 0)
