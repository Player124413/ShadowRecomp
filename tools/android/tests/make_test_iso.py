#!/usr/bin/env python3
"""Writes a small Xbox disc image (XDVDFS) for tests.

    usage: make_test_iso.py <output.iso> [--partition-offset BYTES] [--xbe-name NAME]

The image has the layout of a real one: a volume descriptor in sector 32, a root
directory stored as a binary search tree, a sub-directory and a few files. Its
default.xbe is a placeholder with a valid header and no code, so the image is only good
for exercising the tools that read disc images (tools/android/fetch-xbe.py, the
build-android workflow's pipeline test).

--partition-offset puts the file system that far into the file, the way full disc dumps
do (0x18300000 for an original Xbox disc); the space before it is a hole.
"""
import argparse
import struct
import sys
from pathlib import Path

SECTOR = 2048
VOLUME_SECTOR = 32
MAGIC = b'MICROSOFT*XBOX*MEDIA'
ATTRIBUTE_DIRECTORY = 0x10
ATTRIBUTE_ARCHIVE = 0x20


def placeholder_xbe(title='ShadowRecomp pipeline test'):
    """An XBE header with a certificate and nothing else."""
    base = 0x10000
    certificate_offset = 0x178
    size = 0x1000

    header = bytearray(size)
    header[0:4] = b'XBEH'
    struct.pack_into('<I', header, 0x104, base)                         # base address
    struct.pack_into('<I', header, 0x108, size)                         # size of headers
    struct.pack_into('<I', header, 0x10C, size)                         # size of image
    struct.pack_into('<I', header, 0x110, certificate_offset)           # size of image header
    struct.pack_into('<I', header, 0x118, base + certificate_offset)    # certificate address

    certificate = bytearray(0x1D0)
    struct.pack_into('<I', certificate, 0x00, len(certificate))
    struct.pack_into('<I', certificate, 0x08, 0xFFFF0000)               # title id
    name = title.encode('utf-16-le')[:78]
    certificate[0x0C:0x0C + len(name)] = name
    struct.pack_into('<I', certificate, 0xA0, 0x7)                      # all regions
    header[certificate_offset:certificate_offset + len(certificate)] = certificate

    # Filler that is not all zeroes, so that a truncated copy hashes differently.
    for index in range(certificate_offset + len(certificate), size):
        header[index] = (index * 131 + 17) & 0xFF
    return bytes(header)


def directory_table(entries):
    """Serializes (name, sector, size, attributes) entries as an XDVDFS directory.

    Entries form a binary search tree ordered by upper-cased name. Each one starts on a
    4-byte boundary, names its children by their offset in 4-byte units, and never
    crosses a sector boundary.
    """
    ordered = sorted(entries, key=lambda entry: entry[0].upper())
    table = bytearray()

    def place(low, high):
        """Writes ordered[low:high] as a subtree; returns the offset of its root."""
        if low >= high:
            return 0
        middle = (low + high) // 2
        name, sector, size, attributes = ordered[middle]
        encoded = name.encode('ascii')
        length = (14 + len(encoded) + 3) & ~3

        if len(table) // SECTOR != (len(table) + length - 1) // SECTOR:
            table.extend(b'\xff' * (SECTOR - len(table) % SECTOR))

        offset = len(table)
        table.extend(b'\xff' * length)
        left = place(low, middle)
        right = place(middle + 1, high)
        struct.pack_into('<HHIIBB', table, offset, left // 4, right // 4, sector, size, attributes, len(encoded))
        table[offset + 14:offset + 14 + len(encoded)] = encoded
        return offset

    place(0, len(ordered))
    table.extend(b'\xff' * (-len(table) % SECTOR))
    return bytes(table)


def build_image(xbe, xbe_name='default.xbe'):
    """Returns the bytes of the file system (which starts at the partition offset)."""
    files = {
        xbe_name: xbe,
        'Shadow.one': b'ONE ' * 700,
        'bink.bik': bytes(range(256)) * 9,
        'ZZZ last entry.txt': b'the last name in the root directory\n',
        'AAA first entry.txt': b'the first name in the root directory\n',
    }
    nested = {
        'STG0100.dat': b'stage data ' * 300,
        'default.xbe.bak': b'a decoy in a sub-directory, which is not the executable',
    }

    def sectors(data):
        return (len(data) + SECTOR - 1) // SECTOR

    # Sizes are known up front (a directory's size only depends on its names), so the
    # layout can be decided before anything is written.
    nested_size = len(directory_table([(name, 0, 0, 0) for name in nested]))
    root_names = list(files) + ['stg0100', 'empty']
    root_size = len(directory_table([(name, 0, 0, 0) for name in root_names]))

    next_sector = VOLUME_SECTOR + 1
    root_sector = next_sector
    next_sector += root_size // SECTOR
    nested_sector = next_sector
    next_sector += nested_size // SECTOR

    placement = {}
    for group in (files, nested):
        for name, data in group.items():
            placement[name] = next_sector
            next_sector += max(1, sectors(data))

    image = bytearray(next_sector * SECTOR)

    descriptor = bytearray(SECTOR)
    descriptor[0:len(MAGIC)] = MAGIC
    struct.pack_into('<II', descriptor, len(MAGIC), root_sector, root_size)
    descriptor[SECTOR - len(MAGIC):] = MAGIC
    image[VOLUME_SECTOR * SECTOR:(VOLUME_SECTOR + 1) * SECTOR] = descriptor

    root = [(name, placement[name], len(data), ATTRIBUTE_ARCHIVE) for name, data in files.items()]
    root.append(('stg0100', nested_sector, nested_size, ATTRIBUTE_DIRECTORY))
    root.append(('empty', 0, 0, ATTRIBUTE_DIRECTORY))
    root_table = directory_table(root)
    assert len(root_table) == root_size
    image[root_sector * SECTOR:root_sector * SECTOR + root_size] = root_table

    nested_table = directory_table([(name, placement[name], len(data), ATTRIBUTE_ARCHIVE) for name, data in nested.items()])
    assert len(nested_table) == nested_size
    image[nested_sector * SECTOR:nested_sector * SECTOR + nested_size] = nested_table

    for group in (files, nested):
        for name, data in group.items():
            start = placement[name] * SECTOR
            image[start:start + len(data)] = data

    return bytes(image)


def write_image(path, partition_offset=0, xbe=None, xbe_name='default.xbe'):
    """Writes the image and returns the default.xbe that is in it."""
    xbe = placeholder_xbe() if xbe is None else xbe
    image = build_image(xbe, xbe_name)
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, 'wb') as output:
        output.seek(partition_offset)
        output.write(image)
    return xbe


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output')
    parser.add_argument('--partition-offset', type=lambda text: int(text, 0), default=0)
    parser.add_argument('--xbe-name', default='default.xbe')
    arguments = parser.parse_args()

    if arguments.partition_offset % SECTOR:
        sys.exit('make_test_iso: the partition offset must be a multiple of 2048')

    write_image(arguments.output, arguments.partition_offset, xbe_name=arguments.xbe_name)
    print(f'{arguments.output}: test disc image, file system at offset 0x{arguments.partition_offset:X}')


if __name__ == '__main__':
    main()
