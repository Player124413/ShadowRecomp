package org.shadowrecomp.android;

import java.io.File;
import java.io.RandomAccessFile;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;

/**
 * Test of the launcher's disc image check; runs on any JVM. The images are sparse files of
 * several gigabytes that take no space on the usual file systems.
 */
public class XboxDiscImageTest {
    static int failures = 0, checks = 0;
    static void check(boolean ok, String what) { checks++; if (!ok) { failures++; System.out.println("FAIL " + what); } }

    static File make(long size, long... magicAt) throws Exception {
        File file = File.createTempFile("xdi", ".bin");
        file.deleteOnExit();
        try (RandomAccessFile raf = new RandomAccessFile(file, "rw")) {
            for (long at : magicAt) { raf.seek(at); raf.write("MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII)); }
            raf.setLength(size); // sparse; also cuts a magic that reaches past the end
        }
        return file;
    }

    static int probe(File file, long budget, boolean cancel) throws Exception {
        try (RandomAccessFile raf = new RandomAccessFile(file, "r"); FileChannel channel = raf.getChannel()) {
            return XboxDiscImage.probe(channel, raf.length(), budget, () -> cancel);
        }
    }

    public static void main(String[] args) throws Exception {
        final long LONG = 60_000_000_000L;
        final long GiB = 1L << 30;

        // Bare image (extract-xiso output): header at 0x10000.
        check(probe(make(4 * GiB, 0x10000L), LONG, false) == XboxDiscImage.FOUND, "bare image");
        // Full XGD1 dump (redump): partition at 0x18300000.
        check(probe(make(7 * GiB + 12345, 0x18300000L + 0x10000L), LONG, false) == XboxDiscImage.FOUND, "XGD1 dump");
        check(probe(make(7 * GiB, 0x0FD90000L + 0x10000L), LONG, false) == XboxDiscImage.FOUND, "XGD2 dump");
        check(probe(make(7 * GiB, 0x02080000L + 0x10000L), LONG, false) == XboxDiscImage.FOUND, "XGD3 dump");
        // Unusual partition start on a 64 KiB boundary: found by the scan.
        check(probe(make(2 * GiB, 0x250000L + 0x10000L), LONG, false) == XboxDiscImage.FOUND, "scan");
        // Also when cancelled or out of time, as long as a known offset matches.
        check(probe(make(4 * GiB, 0x10000L), 0, true) == XboxDiscImage.FOUND, "known offset beats cancel");

        // Not an Xbox image: sparse zeros (stands in for a GameCube/PS2 image or an archive).
        check(probe(make(1 * GiB + 400000000L), LONG, false) == XboxDiscImage.MISSING, "no header");
        // The magic somewhere that is not a header position.
        check(probe(make(1 * GiB, 0x10001L), LONG, false) == XboxDiscImage.MISSING, "unaligned magic");
        check(probe(make(1 * GiB, 0x8000L), LONG, false) == XboxDiscImage.MISSING, "magic at half step");
        check(probe(make(1 * GiB, 0L), LONG, false) == XboxDiscImage.MISSING, "magic at offset 0");
        // Tiny and empty files.
        check(probe(make(0), LONG, false) == XboxDiscImage.MISSING, "empty");
        check(probe(make(19), LONG, false) == XboxDiscImage.MISSING, "19 bytes");
        check(probe(make(0x10000L + 19, 0x10000L), LONG, false) == XboxDiscImage.MISSING, "truncated header");
        check(probe(make(0x10000L + 20, 0x10000L), LONG, false) == XboxDiscImage.FOUND, "header at the very end");
        // Scan boundary: the native loop requires offset + 0x10000 < fileSize, so a header
        // in the last 20 bytes at a scanned position is still found...
        check(probe(make(0x30000L + 20, 0x30000L), LONG, false) == XboxDiscImage.FOUND, "scan, header at the end");
        // ...and a file ending exactly at the scan position has no room for one.
        check(probe(make(0x30000L), LONG, false) == XboxDiscImage.MISSING, "scan, no room");

        // Cancelled / out of budget during the scan: no verdict.
        check(probe(make(8 * GiB), LONG, true) == XboxDiscImage.UNKNOWN, "cancelled");
        check(probe(make(8 * GiB), -1, false) == XboxDiscImage.UNKNOWN, "no budget");
        // A small file finishes before the first budget check.
        check(probe(make(0x200000L), -1, true) == XboxDiscImage.MISSING, "small file ignores budget");

        // Timing of the worst case on local storage: a full scan of an 8 GiB file.
        long start = System.nanoTime();
        check(probe(make(8 * GiB), LONG, false) == XboxDiscImage.MISSING, "8 GiB scan");
        System.out.println("full scan of 8 GiB: " + (System.nanoTime() - start) / 1_000_000 + " ms");

        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
