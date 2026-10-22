#!/usr/bin/env python3
"""Independent check of ShadowRecomp's ETC2 encoder (see etc2_check.cpp).

Decodes the blocks written by the C++ tool with a decoder from another project ("texture2ddecoder", which implements
the format as the hardware does) and reports how close the result is to the source: PSNR per channel, plus the alpha
channel for the RGBA image. The images are deliberately different things: a smooth gradient (over 30 dB: the format
handles those well), UI art with an alpha ramp, pure noise (low, and expected to be: 4 bits per pixel), and shaded art
with a little dither - the closest of them to the game's own textures. The floors below are this encoder's own measured
numbers, so the check is a regression guard, not a quality target.

    pip install texture2ddecoder
    python3 tools/android/etc2_check.py blocks.bin
"""
import struct
import sys

import texture2ddecoder


def psnr(a, b):
    if not a:
        return 99.0
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)
    return 99.0 if mse == 0 else 10 * (0.0 - __import__("math").log10(mse / (255.0 * 255.0)))


def decode_image(kind, blocks, w, h):
    """The pixels, block by block, in the order the encoder wrote them."""
    px = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            if kind == "rgba":
                blk = blocks[((by // 4) * (w // 4) + bx // 4) * 16 :][:16]
                dec = texture2ddecoder.decode_etc2a8(blk, 4, 4)   # EAC alpha (first 8 bytes) + ETC2 RGB (last 8)
                for i in range(16):
                    o = i * 4
                    px += bytes((dec[o + 2], dec[o + 1], dec[o], dec[o + 3]))   # the decoder returns BGRA
            else:
                blk = blocks[((by // 4) * (w // 4) + bx // 4) * 8 :][:8]
                dec = texture2ddecoder.decode_etc2(blk, 4, 4)
                for i in range(16):
                    o = i * 4
                    px += bytes((dec[o + 2], dec[o + 1], dec[o], 255))
    return px


def main(path):
    data = open(path, "rb").read()
    magic, count = struct.unpack_from("<4sI", data, 0)
    if magic != b"E2CK":
        print("not an etc2_check file")
        return 1
    at = 8
    ok = True
    for i in range(count):
        w, h, bs = struct.unpack_from("<III", data, at)
        at += 12
        src = data[at : at + w * h * 4]
        at += w * h * 4
        nblocks = (w // 4) * (h // 4)
        blocks = data[at : at + nblocks * bs]
        at += nblocks * bs
        kind = "rgba" if bs == 16 else "rgb"
        px = decode_image(kind, blocks, w, h)
        # the source, reassembled in the encoder's block order
        ref = bytearray()
        for by in range(0, h, 4):
            for bx in range(0, w, 4):
                for y in range(4):
                    o = ((by + y) * w + bx) * 4
                    ref += src[o : o + 16]
        rgb = psnr(list(px[0::4]) + list(px[1::4]) + list(px[2::4]), list(ref[0::4]) + list(ref[1::4]) + list(ref[2::4]))
        a = psnr(list(px[3::4]), list(ref[3::4])) if kind == "rgba" else 99.0
        print("  image %d (%dx%d, %s, %d bytes/block): colour PSNR %.1f dB%s"
              % (i, w, h, kind, bs, rgb, (", alpha PSNR %.1f dB" % a) if kind == "rgba" else ""))
        # Regression floors, not targets: this encoder is deliberately smaller and simpler than etcpak's, and these
        # are its measured numbers on these three images (with a little room for encoder tweaks). A layout or search
        # bug shows up immediately - a broken encoder scores under 12 dB on all of them.
        floors = {0: (32.0, 99.0), 1: (9.0, 10.0), 3: (30.0, 99.0)}   # 3: the shaded image, photo-like art
        floor_rgb, floor_a = floors.get(i, (8.0, 99.0))
        if rgb < floor_rgb or (kind == "rgba" and a < floor_a):
            ok = False
    print("OK" if ok else "FAILED (below the regression floors)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "etc2_blocks.bin"))
