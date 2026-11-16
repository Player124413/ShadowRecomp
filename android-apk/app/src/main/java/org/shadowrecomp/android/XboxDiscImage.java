package org.shadowrecomp.android;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.function.BooleanSupplier;

/**
 * Tells whether a file can be an Xbox disc image, before gigabytes of it are copied.
 *
 * This repeats the header search of the native installer (FindHeader in
 * ShadowRecomp/install/xiso_file_system.cpp) and must never be stricter than it: a file
 * rejected here is one the installer could not read either. Whether the image holds the
 * right game is still decided by the installer, which verifies every file.
 */
final class XboxDiscImage {
    private XboxDiscImage() {}

    /** The volume header was found. */
    static final int FOUND = 1;
    /** The whole file was searched and holds no volume header. */
    static final int MISSING = 0;
    /** The search was cancelled or ran out of time; nothing can be said about the file. */
    static final int UNKNOWN = -1;

    private static final byte[] MAGIC = "MICROSOFT*XBOX*MEDIA".getBytes(StandardCharsets.US_ASCII);

    /** Position of the volume header inside the game partition. */
    private static final long HEADER_OFFSET = 0x10000L;

    /** Where the game partition starts: a bare image, then XGD1, XGD2 and XGD3 full dumps. */
    private static final long[] PARTITION_OFFSETS = { 0L, 0x18300000L, 0x0FD90000L, 0x02080000L };

    /** The fallback search tries every multiple of this as the start of the partition. */
    private static final long SCAN_STEP = 0x10000L;

    /**
     * @param size        length of the file in bytes
     * @param budgetNanos how long the fallback search may take; it reads the file in
     *                    64 KiB strides, which is instant on local storage but can be very
     *                    slow on a document that is fetched over the network
     */
    static int probe(FileChannel channel, long size, long budgetNanos, BooleanSupplier cancelled)
            throws IOException {
        ByteBuffer buffer = ByteBuffer.allocate(MAGIC.length);
        for (long offset : PARTITION_OFFSETS) {
            if (hasHeaderAt(channel, offset, size, buffer)) return FOUND;
        }

        long deadline = System.nanoTime() + budgetNanos;
        int sinceCheck = 0;
        for (long offset = SCAN_STEP; offset + HEADER_OFFSET < size; offset += SCAN_STEP) {
            if (hasHeaderAt(channel, offset, size, buffer)) return FOUND;
            if (++sinceCheck >= 64) {
                sinceCheck = 0;
                if (cancelled.getAsBoolean() || System.nanoTime() - deadline > 0) return UNKNOWN;
            }
        }
        return MISSING;
    }

    private static boolean hasHeaderAt(FileChannel channel, long partitionOffset, long size,
            ByteBuffer buffer) throws IOException {
        long position = partitionOffset + HEADER_OFFSET;
        if (position + MAGIC.length > size) return false;
        buffer.clear();
        while (buffer.hasRemaining()) {
            int count = channel.read(buffer, position + buffer.position());
            if (count < 0) return false;
        }
        return Arrays.equals(buffer.array(), MAGIC);
    }
}
