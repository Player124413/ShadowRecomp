package dev.shadowrecomp;

import android.content.ContentResolver;
import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;

/**
 * Copies the player's own disc image into the app's folder.
 *
 * The game never ships game files: the player picks their own Shadow the Hedgehog (USA) image with the system file
 * picker, this copies it once (with a progress callback) and the native side then reads it in place - nothing is
 * unpacked, so a 1.4 GB image costs 1.4 GB of storage and no more.
 */
final class DiscImport {
    /** Always the first USA release; the runtime refuses anything else (it would boot to garbage). */
    static final String GAME_ID = "GUPE8P";
    private static final int BOOT_MAGIC_OFFSET = 0x1C;
    private static final long BOOT_MAGIC = 0xC2339F3DL;
    private static final int HEADER_BYTES = 0x440;

    interface Progress {
        void onProgress(long done, long total);

        void onDone(boolean ok, String message, String path);
    }

    private DiscImport() {
    }

    /** Where the imported image lives: files dir, so no storage permission is ever needed. */
    static File discFile(Context context) {
        return new File(context.getFilesDir(), "shadow.iso");
    }

    static boolean haveDisc(Context context) {
        File f = discFile(context);
        return f.isFile() && f.length() > HEADER_BYTES;
    }

    /** A readable name for the picker's result, for the status line. */
    static String displayName(Context context, Uri uri) {
        try (Cursor c = context.getContentResolver().query(uri, null, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                int idx = c.getColumnIndex(OpenableColumns.DISPLAY_NAME);
                if (idx >= 0) return c.getString(idx);
            }
        } catch (Exception ignored) {
        }
        String last = uri.getLastPathSegment();
        return last != null ? last : uri.toString();
    }

    /**
     * Copies {@code uri} to the disc slot on a background thread and reports back on that thread.
     * {@code progress.onDone} is called exactly once.
     */
    static void importInto(Context context, Uri uri, Progress progress) {
        final ContentResolver resolver = context.getContentResolver();
        final File out = discFile(context);
        final File tmp = new File(context.getFilesDir(), "shadow.import.tmp");
        Thread t = new Thread(() -> {
            long total = -1;
            try (Cursor c = resolver.query(uri, null, null, null, null)) {
                if (c != null && c.moveToFirst()) {
                    int idx = c.getColumnIndex(OpenableColumns.SIZE);
                    if (idx >= 0 && !c.isNull(idx)) total = c.getLong(idx);
                }
            } catch (Exception ignored) {
            }
            byte[] header = new byte[HEADER_BYTES];
            int headerRead = 0;
            long copied = 0;
            try (InputStream in = resolver.openInputStream(uri);
                 FileOutputStream fos = new FileOutputStream(tmp)) {
                if (in == null) {
                    progress.onDone(false, "The picker returned nothing to read.", null);
                    return;
                }
                byte[] buf = new byte[1 << 20];
                int n;
                long lastReport = 0;
                while ((n = in.read(buf)) > 0) {
                    if (headerRead < HEADER_BYTES) {
                        int take = Math.min(n, HEADER_BYTES - headerRead);
                        System.arraycopy(buf, 0, header, headerRead, take);
                        headerRead += take;
                    }
                    fos.write(buf, 0, n);
                    copied += n;
                    if (copied - lastReport > (32L << 20)) {
                        lastReport = copied;
                        progress.onProgress(copied, total);
                    }
                }
                fos.getFD().sync();
            } catch (Exception e) {
                tmp.delete();
                progress.onDone(false, "Could not copy the file: " + e.getMessage(), null);
                return;
            }

            String problem = checkHeader(header, headerRead);
            if (problem != null) {
                tmp.delete();
                progress.onDone(false, problem, null);
                return;
            }
            if (!tmp.renameTo(out)) {
                out.delete();
                if (!tmp.renameTo(out)) {
                    tmp.delete();
                    progress.onDone(false, "Could not put the image in the app's folder (is it full?).", null);
                    return;
                }
            }
            progress.onDone(true, "Imported", out.getAbsolutePath());
        }, "disc-import");
        t.setPriority(Thread.MIN_PRIORITY + 2);   // a big copy must not starve the UI thread
        t.start();
    }

    /** null when the image is the USA first release, else a sentence for the player. */
    static String checkHeader(byte[] header, int read) {
        if (read < HEADER_BYTES) return "That file is too small to be a GameCube disc image.";
        long magic = ((long) (header[BOOT_MAGIC_OFFSET] & 0xFF) << 24)
                | ((long) (header[BOOT_MAGIC_OFFSET + 1] & 0xFF) << 16)
                | ((long) (header[BOOT_MAGIC_OFFSET + 2] & 0xFF) << 8)
                | (header[BOOT_MAGIC_OFFSET + 3] & 0xFF);
        if (magic != BOOT_MAGIC) return "That is not a GameCube disc image (ISO or GCM).";
        StringBuilder id = new StringBuilder(6);
        for (int i = 0; i < 6; i++) id.append((char) (header[i] & 0xFF));
        if (!GAME_ID.equals(id.toString())) {
            return "That is " + id + ", but this build needs Shadow the Hedgehog (USA): " + GAME_ID + ".";
        }
        if (header[7] != 0) return "That is a later USA revision; only the first release works.";
        return null;
    }
}
