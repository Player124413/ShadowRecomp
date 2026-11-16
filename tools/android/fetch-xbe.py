#!/usr/bin/env python3
"""Gets default.xbe, the one game file a build needs, out of wherever the game is.

    usage: fetch-xbe.py <source> [--output FILE] [--no-verify]

<source> is one of

    - a link (http, https) to anything below except a folder,
    - an Xbox disc image (.iso): a bare file system image or a full disc dump,
    - an archive (.zip, .7z, .rar) holding a disc image or the game's files,
    - a folder with the game's files,
    - default.xbe itself.

The file is written to ShadowRecompLib/private/default.xbe unless --output says
otherwise. It is first checked against the dumps of the game this project knows
(ShadowRecomp/install/hashes/game.cpp), because the recompiler would happily translate
any other executable into a build that does not work; --no-verify skips the check.

Links: a direct download link works as it is. Share links of Google Drive, Dropbox and
Yandex Disk are turned into direct ones. Other file hosts that put a web page in front of
the file are not supported.

Needs curl for links and 7z (unrar for .rar) for archives other than plain .zip files.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.parse
import urllib.request
import zipfile
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
DEFAULT_OUTPUT = REPOSITORY / 'ShadowRecompLib' / 'private' / 'default.xbe'
KNOWN_HASHES = REPOSITORY / 'ShadowRecomp' / 'install' / 'hashes' / 'game.cpp'

XBE_NAME = 'default.xbe'
XBE_MAGIC = b'XBEH'
# The executable is a few megabytes; anything far outside that is not it.
XBE_SIZE_LIMIT = 64 * 1024 * 1024
# The installer refuses disc images above 10 GB (install/xiso_file_system.cpp); an
# archive of one can be a little larger.
DOWNLOAD_SIZE_LIMIT = 16 * 1024 * 1024 * 1024
MINIMUM_FREE_SPACE = 512 * 1024 * 1024
NESTING_LIMIT = 3

USER_AGENT = 'Mozilla/5.0 (X11; Linux x86_64) fetch-xbe (ShadowRecomp)'
DOWNLOAD_ATTEMPTS = 5
RETRY_PAUSE = 5     # seconds


class Failure(Exception):
    """A problem with the source that the user has to resolve."""


def say(message):
    print(message, flush=True)


def megabytes(size):
    if size >= 1024 ** 3:
        return f'{size / 1024 ** 3:.2f} GiB'
    if size >= 1024 ** 2:
        return f'{size / 1024 ** 2:.1f} MiB'
    return f'{size / 1024:.1f} KiB' if size >= 1024 else f'{size} bytes'


# ---------------------------------------------------------------------------------------
# XXH3, the hash the installer identifies game files with.
# ---------------------------------------------------------------------------------------

_MASK = (1 << 64) - 1
_PRIME32_1 = 0x9E3779B1
_PRIME32_2 = 0x85EBCA77
_PRIME32_3 = 0xC2B2AE3D
_PRIME64_1 = 0x9E3779B185EBCA87
_PRIME64_2 = 0xC2B2AE3D27D4EB4F
_PRIME64_3 = 0x165667B19E3779F9
_PRIME64_4 = 0x85EBCA77C2B2AE63
_PRIME64_5 = 0x27D4EB2F165667C5
_SECRET = bytes.fromhex(
    'b8fe6c3923a44bbe7c01812cf721ad1cded46de9839097db7240a4a4b7b3671f'
    'cb79e64eccc0e578825ad07dccff7221b8084674f743248ee03590e6813a264c'
    '3c2852bb91c300cb88d0658b1b532ea371644897a20df94e3819ef46a9deacd8'
    'a8fa763fe39c343ff9dcbbc7c70b4f1d8a51e04bcdb45931c89f7ec9d9787364'
    'eac5ac8334d3ebc3c581a0fffa1363eb170ddd51b7f0da49d316552629d4689e'
    '2b16be587d47a1fc8ff8b8d17ad031ce45cb3a8f95160428afd7fbcabb4b407e')


def xxh3_64(data):
    """XXH3_64bits() of `data`: default secret, no seed.

    Inputs of 240 bytes or less take other code paths in xxHash, which nothing here needs.
    """
    length = len(data)
    if length <= 240:
        raise ValueError('xxh3_64: the input must be longer than 240 bytes')

    stripe = 64
    stripes_per_block = (len(_SECRET) - stripe) // 8
    keys = [struct.unpack_from('<8Q', _SECRET, 8 * index) for index in range(stripes_per_block)]
    scramble_key = struct.unpack_from('<8Q', _SECRET, len(_SECRET) - stripe)
    acc = [_PRIME32_3, _PRIME64_1, _PRIME64_2, _PRIME64_3, _PRIME64_4, _PRIME32_2, _PRIME64_5, _PRIME32_1]

    def accumulate(offset, key):
        lanes = struct.unpack_from('<8Q', data, offset)
        for index in range(8):
            value = lanes[index]
            mixed = value ^ key[index]
            acc[index ^ 1] = (acc[index ^ 1] + value) & _MASK
            acc[index] = (acc[index] + (mixed & 0xFFFFFFFF) * (mixed >> 32)) & _MASK

    # Every stripe except the one that ends the input. The accumulators are scrambled
    # after each full block of stripes.
    for number in range((length - 1) // stripe):
        accumulate(number * stripe, keys[number % stripes_per_block])
        if number % stripes_per_block == stripes_per_block - 1:
            for index in range(8):
                value = acc[index]
                value ^= value >> 47
                value ^= scramble_key[index]
                acc[index] = (value * _PRIME32_1) & _MASK

    accumulate(length - stripe, struct.unpack_from('<8Q', _SECRET, len(_SECRET) - stripe - 7))

    result = (length * _PRIME64_1) & _MASK
    for index in range(4):
        low, high = struct.unpack_from('<2Q', _SECRET, 11 + 16 * index)
        product = (acc[2 * index] ^ low) * (acc[2 * index + 1] ^ high)
        result = (result + ((product & _MASK) ^ (product >> 64))) & _MASK

    result ^= result >> 37
    result = (result * 0x165667919E3779F9) & _MASK
    return result ^ (result >> 32)


def known_hashes(path, name=XBE_NAME):
    """The hashes the installer accepts for `name`, from the tables fshasher generated."""
    text = Path(path).read_text(encoding='utf-8')
    hashes_block = re.search(r'GameHashes\[\]\s*=\s*\{(.*?)\};', text, re.S)
    files_block = re.search(r'GameFiles\[\]\s*=\s*\{(.*?)\n\};', text, re.S)
    if not hashes_block or not files_block:
        raise Failure(f'{path} does not have the GameHashes and GameFiles tables.')

    hashes = [int(value) for value in re.findall(r'(\d+)ULL', hashes_block.group(1))]
    files = re.findall(r'\{\s*"([^"]+)"\s*,\s*(\d+)\s*\}', files_block.group(1))
    if sum(int(count) for _, count in files) != len(hashes):
        raise Failure(f'{path}: the file table does not add up to the hash table.')

    position = 0
    for file_name, count in files:
        count = int(count)
        if file_name.lower() == name.lower():
            return hashes[position:position + count]
        position += count
    raise Failure(f'{path} does not list {name}.')


# ---------------------------------------------------------------------------------------
# XBE
# ---------------------------------------------------------------------------------------

def describe_xbe(data):
    """Title and title ID from the certificate, or None if the header makes no sense."""
    try:
        base, = struct.unpack_from('<I', data, 0x104)
        certificate_address, = struct.unpack_from('<I', data, 0x118)
        offset = certificate_address - base
        if offset < 0x178 or offset + 0xB0 > len(data):
            return None
        title_id, = struct.unpack_from('<I', data, offset + 0x08)
        raw_title = data[offset + 0x0C:offset + 0x0C + 80].decode('utf-16-le', errors='replace')
        title = raw_title.split('\0', 1)[0].strip()
        if not title or not title.isprintable():
            return None
        return f'"{title}" (title ID {title_id:08X})'
    except struct.error:
        return None


def check_xbe(data, origin):
    if len(data) > XBE_SIZE_LIMIT:
        raise Failure(f'{origin} is {megabytes(len(data))}, far too large for the game\'s executable.')
    if data[:4] != XBE_MAGIC:
        raise Failure(f'{origin} is not an Xbox executable (it does not start with "XBEH").')


# ---------------------------------------------------------------------------------------
# Xbox disc images (XDVDFS). Follows ShadowRecomp/install/xiso_file_system.cpp, which is
# what reads the same image when the game is installed.
# ---------------------------------------------------------------------------------------

SECTOR = 2048
HEADER_OFFSET = 0x10000
HEADER_MAGIC = b'MICROSOFT*XBOX*MEDIA'
# Where the game partition starts: a bare image, then XGD1, XGD2 and XGD3 disc dumps.
PARTITION_OFFSETS = (0, 0x18300000, 0x0FD90000, 0x02080000)
SCAN_STEP = 0x10000
ATTRIBUTE_DIRECTORY = 0x10
DIRECTORY_SIZE_LIMIT = 16 * 1024 * 1024


def read_at(stream, offset, size):
    stream.seek(offset)
    data = stream.read(size)
    return data if len(data) == size else None


def find_partition(stream, size):
    """Offset of the game partition in a disc image, or None if it is not one."""
    def has_header(offset):
        return read_at(stream, offset + HEADER_OFFSET, len(HEADER_MAGIC)) == HEADER_MAGIC

    for offset in PARTITION_OFFSETS:
        if has_header(offset):
            return offset

    offset = SCAN_STEP
    while offset + HEADER_OFFSET < size:
        if has_header(offset):
            return offset
        offset += SCAN_STEP
    return None


def directory_entries(table):
    """(name, sector, size, attributes) of every entry in a directory table."""
    entries = []
    pending = [0]
    visited = set()
    while pending:
        offset = pending.pop()
        if offset in visited or offset + 14 > len(table):
            continue
        visited.add(offset)

        left, right, sector, size, attributes, name_length = struct.unpack_from('<HHIIBB', table, offset)
        if left == 0xFFFF:          # padding
            continue
        if offset + 14 + name_length > len(table):
            raise Failure('The disc image has a damaged directory.')
        if left:
            pending.append(left * 4)
        if right:
            pending.append(right * 4)

        name = table[offset + 14:offset + 14 + name_length].decode('latin-1')
        if name and name not in ('.', '..') and '/' not in name and '\\' not in name:
            entries.append((name, sector, size, attributes))
    return entries


def xbe_from_image(path):
    """default.xbe from the root directory of a disc image, or None if `path` is not one."""
    size = os.path.getsize(path)
    with open(path, 'rb') as stream:
        partition = find_partition(stream, size)
        if partition is None:
            return None

        root = read_at(stream, partition + HEADER_OFFSET + len(HEADER_MAGIC), 8)
        root_sector, root_size = struct.unpack('<II', root) if root else (0, 0)
        if root_size == 0 or root_size > DIRECTORY_SIZE_LIMIT:
            raise Failure('The disc image has no root directory.')

        table = read_at(stream, partition + root_sector * SECTOR, root_size)
        if table is None:
            raise Failure('The disc image is cut short: its root directory lies beyond the end of the file. '
                          'The download or the copy is probably incomplete.')

        for name, sector, file_size, attributes in directory_entries(table):
            if name.lower() != XBE_NAME or attributes & ATTRIBUTE_DIRECTORY:
                continue
            if file_size > XBE_SIZE_LIMIT:
                raise Failure(f'{XBE_NAME} in the disc image is {megabytes(file_size)}, far too large for the game\'s executable.')
            data = read_at(stream, partition + sector * SECTOR, file_size)
            if data is None:
                raise Failure(f'The disc image is cut short: {XBE_NAME} lies beyond the end of the file. '
                              'The download or the copy is probably incomplete.')
            say(f'Disc image: Xbox file system at offset 0x{partition:X}, {XBE_NAME} is {megabytes(file_size)}.')
            return data

    raise Failure(f'The disc image has no {XBE_NAME} in its root directory, so it is not a game disc.')


# ---------------------------------------------------------------------------------------
# Archives and folders
# ---------------------------------------------------------------------------------------

def kind_of(path):
    with open(path, 'rb') as stream:
        head = stream.read(4096)

    if head[:4] == XBE_MAGIC:
        return 'xbe'
    if head[:4] in (b'PK\x03\x04', b'PK\x05\x06', b'PK\x07\x08'):
        return 'zip'
    if head[:6] == b"7z\xbc\xaf'\x1c":
        return '7z'
    if head[:4] == b'Rar!':
        return 'rar'
    if head[:2] == b'\x1f\x8b' or head[:6] == b'\xfd7zXZ\x00' or head[:3] == b'BZh' or head[257:262] == b'ustar':
        return 'compressed'     # .gz, .xz, .bz2, .tar

    text = head.lstrip(b'\xef\xbb\xbf \t\r\n').lower()
    if text.startswith((b'<!doctype html', b'<html', b'<head', b'<?xml')) or b'<title' in text[:2048]:
        return 'html'
    return 'data'


def run_extractor(command, what):
    """Runs an unpacker. False: it is not installed. Failure: it could not unpack."""
    try:
        result = subprocess.run(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except FileNotFoundError:
        return False
    if result.returncode != 0:
        output = result.stdout.decode('utf-8', errors='replace')
        if 'password' in output.lower():
            raise Failure(f'{what.capitalize()} is protected by a password, which is not supported.')
        lines = [line.strip() for line in output.splitlines() if line.strip()]
        detail = ' '.join(lines[-6:]) or f'exit code {result.returncode}'
        raise Failure(f'Could not unpack {what} with {command[0]}: {detail}')
    return True


def unpack_archive(path, kind, destination):
    """Unpacks all of an archive into `destination` with the tools of the system."""
    destination.mkdir(parents=True, exist_ok=True)
    what = 'the archive'

    if kind == 'rar' and run_extractor(['unrar', 'x', '-y', '-o+', '-p-', '-idq', str(path), str(destination) + os.sep], what):
        return

    try:
        # -p with a dummy password: an encrypted archive fails instead of asking for one.
        for program in ('7z', '7zz', '7za'):
            if run_extractor([program, 'x', '-y', '-bd', '-pfetch-xbe', f'-o{destination}', str(path)], what):
                return
    except Failure:
        if kind != 'rar':
            raise
        # The 7z of most Linux distributions is built without RAR support.

    if kind == 'rar':
        raise Failure('The file is a RAR archive, which needs unrar (or a 7z that reads RAR) to be installed. '
                      'Repack it as .zip or .7z, or link to the .iso itself.')
    raise Failure('The file is an archive that needs 7z to unpack, and 7z is not installed.')


def xbe_from_zip(path, work, depth):
    """Looks into a .zip without unpacking all of it. None means: unpack it another way."""
    try:
        with zipfile.ZipFile(path) as archive:
            members = [member for member in archive.infolist() if not member.is_dir()]

            executables = [member for member in members if member.filename.replace('\\', '/').rsplit('/', 1)[-1].lower() == XBE_NAME]
            if executables:
                member = min(executables, key=lambda entry: entry.filename.replace('\\', '/').count('/'))
                if member.file_size > XBE_SIZE_LIMIT:
                    raise Failure(f'{member.filename} in the archive is far too large for the game\'s executable.')
                say(f'Archive: taking {member.filename}.')
                return archive.read(member)

            # No game files in sight: the largest member is the disc image, if there is one.
            if not members:
                raise Failure('The archive is empty.')
            member = max(members, key=lambda entry: entry.file_size)
            if member.file_size > DOWNLOAD_SIZE_LIMIT:
                raise Failure(f'{member.filename} in the archive is too large to be a disc image.')
            require_space(work, member.file_size)

            say(f'Archive: unpacking {member.filename} ({megabytes(member.file_size)}).')
            target = work / f'member-{depth}'
            with archive.open(member) as source, open(target, 'wb') as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
    except zipfile.BadZipFile:
        raise Failure('The .zip archive is damaged. The download or the copy is probably incomplete.') from None
    except (NotImplementedError, RuntimeError):
        # A compression method Python lacks (Deflate64, which Windows uses for files
        # over 2 GB) or a password: let 7z deal with it.
        return None

    try:
        return locate(target, work, depth + 1, origin=member.filename)
    finally:
        target.unlink(missing_ok=True)


def xbe_from_folder(folder, work, depth):
    files = [Path(root) / name for root, _, names in os.walk(folder) for name in names]

    executables = [path for path in files if path.name.lower() == XBE_NAME]
    if executables:
        path = min(executables, key=lambda entry: len(entry.relative_to(folder).parts))
        say(f'Taking {path.relative_to(folder)}.')
        if path.stat().st_size > XBE_SIZE_LIMIT:
            raise Failure(f'{path.relative_to(folder)} is far too large for the game\'s executable.')
        return path.read_bytes()

    # Otherwise the game is one level further down, in a disc image or another archive:
    # that is one of the largest files.
    candidates = sorted((path for path in files if path.stat().st_size > HEADER_OFFSET + SECTOR),
                        key=lambda path: path.stat().st_size, reverse=True)
    first_failure = None
    for path in candidates[:3]:
        try:
            return locate(path, work, depth + 1, origin=str(path.relative_to(folder)))
        except Failure as failure:
            first_failure = first_failure or failure
    raise first_failure or Failure(f'Found neither {XBE_NAME} nor a disc image.')


def locate(path, work, depth=0, origin=None):
    """Returns the bytes of default.xbe found in `path`, whatever `path` is."""
    path = Path(path)
    origin = origin or path.name
    if depth > NESTING_LIMIT:
        raise Failure('Archives are nested too deeply.')

    if path.is_dir():
        return xbe_from_folder(path, work, depth)

    kind = kind_of(path)
    if kind == 'xbe':
        if path.stat().st_size > XBE_SIZE_LIMIT:
            raise Failure(f'{origin} is far too large for the game\'s executable.')
        return path.read_bytes()

    if kind == 'html':
        raise Failure('Got a web page instead of a file. The link has to download the file directly; '
                      'file hosts that show a page first are not supported (Google Drive, Dropbox and '
                      'Yandex Disk share links are).')

    if kind == 'zip':
        data = xbe_from_zip(path, work, depth)
        if data is not None:
            return data

    if kind in ('zip', '7z', 'rar', 'compressed'):
        require_space(work, path.stat().st_size)
        unpacked = work / f'unpacked-{depth}'
        try:
            unpack_archive(path, kind, unpacked)
            return xbe_from_folder(unpacked, work, depth + 1)
        finally:
            shutil.rmtree(unpacked, ignore_errors=True)

    data = xbe_from_image(path)
    if data is None:
        raise Failure(f'{origin} is neither an Xbox disc image nor an archive. '
                      'Expected an .iso of the game, a .zip/.7z with it, or default.xbe.')
    return data


def require_space(folder, needed):
    free = shutil.disk_usage(folder).free
    if free < needed + MINIMUM_FREE_SPACE:
        raise Failure(f'Not enough disk space: {megabytes(free)} free, {megabytes(needed)} needed.')


# ---------------------------------------------------------------------------------------
# Links
# ---------------------------------------------------------------------------------------

def fetch_json(url):
    request = urllib.request.Request(url, headers={'User-Agent': USER_AGENT, 'Accept': 'application/json'})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def resolve_link(url, fetch_json=fetch_json):
    """Turns the share links of a few file hosts into links that download the file."""
    parts = urllib.parse.urlsplit(url)
    host = (parts.hostname or '').lower()
    query = urllib.parse.parse_qs(parts.query)

    if host in ('drive.google.com', 'docs.google.com', 'drive.usercontent.google.com'):
        match = re.search(r'/(?:file/)?d/([\w-]{10,})', parts.path)
        file_id = match.group(1) if match else (query.get('id') or [''])[0]
        if '/folders/' in parts.path or not file_id:
            raise Failure('This Google Drive link is not a link to a single file. Share the file itself '
                          '("Anyone with the link") and use that link.')
        return ('https://drive.usercontent.google.com/download?export=download&confirm=t&id='
                + urllib.parse.quote(file_id))

    if host == 'dropbox.com' or host.endswith('.dropbox.com'):
        query['dl'] = ['1']
        return urllib.parse.urlunsplit(parts._replace(query=urllib.parse.urlencode(query, doseq=True)))

    if host in ('yadi.sk', 'disk.yandex.ru', 'disk.yandex.com', 'disk.yandex.by', 'disk.yandex.kz',
                'disk.yandex.com.tr', 'disk.360.yandex.ru', 'disk.360.yandex.com'):
        api = ('https://cloud-api.yandex.net/v1/disk/public/resources/download?public_key='
               + urllib.parse.quote(url, safe=''))
        try:
            link = fetch_json(api).get('href')
        except Exception as error:      # urllib raises a family of unrelated exceptions
            raise Failure(f'Yandex Disk did not give a download link ({error}). '
                          'Check that the link is public and still valid.') from None
        if not link:
            raise Failure('Yandex Disk did not give a download link. Check that the link is public.')
        return link

    return url


def download(url, destination):
    """Downloads with curl, resuming after an interruption if the server allows it."""
    if shutil.which('curl') is None:
        raise Failure('curl is needed to download the link, and it is not installed.')

    host = urllib.parse.urlsplit(url).hostname or 'the server'
    say(f'Downloading from {host}.')

    command = ['curl', '--fail', '--location', '--silent', '--show-error', '--globoff',
               '--connect-timeout', '30', '--speed-limit', '1024', '--speed-time', '120',
               '--user-agent', USER_AGENT, '--output', str(destination)]
    resume = False
    for attempt in range(1, DOWNLOAD_ATTEMPTS + 1):
        process = subprocess.Popen(command + (['--continue-at', '-'] if resume else []) + [url],
                                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        last_report = time.monotonic()
        while process.poll() is None:
            time.sleep(0.2)
            size = destination.stat().st_size if destination.exists() else 0
            if size > DOWNLOAD_SIZE_LIMIT or shutil.disk_usage(destination.parent).free < MINIMUM_FREE_SPACE:
                process.kill()
                process.wait()
                if size > DOWNLOAD_SIZE_LIMIT:
                    raise Failure(f'The file behind the link is larger than {megabytes(DOWNLOAD_SIZE_LIMIT)}; that is not a game disc.')
                raise Failure('Ran out of disk space while downloading.')
            if time.monotonic() - last_report >= 30:
                last_report = time.monotonic()
                say(f'  {megabytes(size)} so far')

        error = process.stderr.read().decode('utf-8', errors='replace').strip()
        process.stderr.close()
        if process.returncode == 0:
            size = destination.stat().st_size if destination.exists() else 0
            if size == 0:
                raise Failure('The link returned an empty file.')
            say(f'Downloaded {megabytes(size)}.')
            return

        # 33: the server cannot resume. Start over once, without asking it to.
        if process.returncode == 33 and resume:
            destination.unlink(missing_ok=True)
            resume = False
            continue

        # Exit codes of curl for failures that a retry can fix: could not connect, partial
        # file, timeout, connection reset, HTTP/2 stream error.
        if process.returncode in (7, 18, 28, 35, 52, 55, 56, 92) and attempt < DOWNLOAD_ATTEMPTS:
            say(f'  interrupted ({error or "curl exit code " + str(process.returncode)}), trying again')
            resume = destination.exists() and destination.stat().st_size > 0
            time.sleep(RETRY_PAUSE)
            continue

        if process.returncode == 22:
            raise Failure(f'The link did not work: {error.replace("curl: (22) ", "")}. '
                          'Check that it is still valid and needs no sign-in.')
        raise Failure(f'The download failed: {error or "curl exit code " + str(process.returncode)}.')

    raise Failure('The download kept being interrupted.')


# ---------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', help='link, disc image, archive, folder or default.xbe')
    parser.add_argument('--output', default=str(DEFAULT_OUTPUT), help='where to write default.xbe')
    parser.add_argument('--no-verify', action='store_true',
                        help='accept an executable that is not a known dump of the game')
    parser.add_argument('--hashes', default=str(KNOWN_HASHES), help=argparse.SUPPRESS)
    parser.add_argument('--work-dir', default=None, help='where to keep the download while it is processed')
    arguments = parser.parse_args()

    work = Path(tempfile.mkdtemp(prefix='fetch-xbe-', dir=arguments.work_dir))
    try:
        source = arguments.source
        if re.match(r'^https?://', source, re.I):
            target = work / 'download'
            download(resolve_link(source), target)
            data = locate(target, work, origin='The downloaded file')
            target.unlink(missing_ok=True)
        else:
            path = Path(source)
            if not path.exists():
                raise Failure(f'{source} does not exist.')
            data = locate(path, work)

        check_xbe(data, XBE_NAME)
        description = describe_xbe(data)
        digest = xxh3_64(data) if len(data) > 240 else None
        say(f'{XBE_NAME}: {megabytes(len(data))}'
            + (f', {description}' if description else '')
            + (f', XXH3 {digest:016X}' if digest is not None else ''))

        if not arguments.no_verify:
            accepted = known_hashes(arguments.hashes)
            if digest not in accepted:
                raise Failure(
                    f'This {XBE_NAME} is not one of the {len(accepted)} dumps of Shadow the Hedgehog that '
                    f'the project supports' + (f': it is {description}' if description else '') + '. '
                    'The disc image has to be an unmodified copy of the original Xbox version of the game.')
            say('It is a known dump of the game.')

        output = Path(arguments.output)
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(output.name + '.part')
        temporary.write_bytes(data)
        os.replace(temporary, output)
        say(f'Wrote {output}')
    except Failure as failure:
        message = str(failure)
        if os.environ.get('GITHUB_ACTIONS') == 'true':
            print(f'::error title=Game files::{message}', flush=True)
        sys.exit(f'fetch-xbe: {message}')
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == '__main__':
    main()
