package org.shadowrecomp.android;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.file.Files;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.UUID;

/**
 * Copies an installation (game files, mods, settings and saves) to another storage root and
 * verifies every file before the launcher switches to it. The original is left in place.
 *
 * Nothing in the copy needs adjusting: the mod loader's profiles name mod folders, and mod
 * INI files hold paths relative to their own folder.
 */
final class StorageMigration {
    interface Progress {
        boolean cancelled();
        void copied(long bytes);
    }

    static void copy(File source, File target, Progress progress) throws IOException {
        source = source.getCanonicalFile();
        target = target.getCanonicalFile();
        if (source.equals(target) || source.getPath().startsWith(target.getPath() + File.separator) ||
                target.getPath().startsWith(source.getPath() + File.separator)) {
            throw new LocalizedIOException("error_copy_separate", null);
        }
        File[] existing = target.listFiles();
        if (existing == null || existing.length != 0) throw new LocalizedIOException("error_copy_empty", null);
        File temporary = new File(target.getParentFile(), ".storage-copy-" + UUID.randomUUID());
        try {
            copyDirectory(source, temporary, progress);
            checkCancelled(progress);
            // target is empty and no game is running; publication stays on one volume.
            Files.delete(target.toPath());
            Files.move(temporary.toPath(), target.toPath());
        } finally {
            deleteTree(temporary);
        }
    }

    private static void copyDirectory(File source, File target, Progress progress) throws IOException {
        checkCancelled(progress);
        if (Files.isSymbolicLink(source.toPath())) throw new LocalizedIOException("error_copy_link", source);
        if (!target.mkdirs() && !target.isDirectory()) throw new LocalizedIOException("error_io_create", target);
        File[] children = source.listFiles();
        if (children == null) throw new LocalizedIOException("error_io_read", source);
        for (File child : children) {
            checkCancelled(progress);
            File destination = new File(target, child.getName());
            if (Files.isSymbolicLink(child.toPath())) throw new LocalizedIOException("error_copy_link", child);
            if (child.isDirectory()) copyDirectory(child, destination, progress);
            else copyFile(child, destination, progress);
        }
    }

    private static void copyFile(File source, File target, Progress progress) throws IOException {
        long length = source.length();
        long modified = source.lastModified();
        MessageDigest written = digest();
        try (FileInputStream input = new FileInputStream(source);
             FileOutputStream output = new FileOutputStream(target)) {
            byte[] buffer = new byte[256 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                checkCancelled(progress);
                output.write(buffer, 0, count);
                written.update(buffer, 0, count);
                progress.copied(count);
            }
            output.getFD().sync();
        }
        MessageDigest readBack = digest();
        try (FileInputStream input = new FileInputStream(target)) {
            byte[] buffer = new byte[256 * 1024];
            int count;
            while ((count = input.read(buffer)) != -1) {
                checkCancelled(progress);
                readBack.update(buffer, 0, count);
            }
        }
        if (source.length() != length || source.lastModified() != modified || target.length() != length ||
                !Arrays.equals(written.digest(), readBack.digest())) throw new LocalizedIOException("error_copy_verify", source);
    }

    private static MessageDigest digest() {
        try { return MessageDigest.getInstance("SHA-256"); }
        catch (NoSuchAlgorithmException exception) { throw new AssertionError(exception); }
    }

    private static void checkCancelled(Progress progress) throws IOException {
        if (progress.cancelled()) throw new LocalizedIOException("error_copy_cancelled", null);
    }

    private static void deleteTree(File file) throws IOException {
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        Files.deleteIfExists(file.toPath());
    }
}
