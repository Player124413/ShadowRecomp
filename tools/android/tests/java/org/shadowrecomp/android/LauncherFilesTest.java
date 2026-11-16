package org.shadowrecomp.android;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
import java.util.Random;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Test of the launcher's file helpers that have no Android dependencies: unpacking a game
 * ZIP into the staging area (InstallerArchive) and copying an installation to another
 * storage location (StorageMigration). Runs on any JVM.
 */
public class LauncherFilesTest {
    static int failures = 0, checks = 0;
    static void check(boolean ok, String what) { checks++; if (!ok) { failures++; System.out.println("FAIL " + what); } }

    static Path scratch;

    static File fresh(String name) throws IOException {
        return Files.createTempDirectory(scratch, name).toFile();
    }

    static byte[] bytes(int size, long seed) {
        byte[] data = new byte[size];
        new Random(seed).nextBytes(data);
        return data;
    }

    /** Pairs of entry name and content; a null content makes a directory entry. */
    static byte[] zip(Object... namesAndContents) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        try (ZipOutputStream zip = new ZipOutputStream(out)) {
            for (int i = 0; i < namesAndContents.length; i += 2) {
                zip.putNextEntry(new ZipEntry((String) namesAndContents[i]));
                if (namesAndContents[i + 1] != null) zip.write((byte[]) namesAndContents[i + 1]);
                zip.closeEntry();
            }
        }
        return out.toByteArray();
    }

    /** The resource key the operation failed with, or "" when it succeeded. */
    static String extractError(byte[] archive, File batch) {
        try {
            InstallerArchive.extract(new ByteArrayInputStream(archive), batch, () -> false, count -> {});
            return "";
        } catch (LocalizedIOException exception) {
            return exception.resource;
        } catch (IOException exception) {
            return exception.toString();
        }
    }

    static String copyError(File source, File target, StorageMigration.Progress progress) {
        try {
            StorageMigration.copy(source, target, progress);
            return "";
        } catch (LocalizedIOException exception) {
            return exception.resource;
        } catch (IOException exception) {
            return exception.toString();
        }
    }

    static final StorageMigration.Progress QUIET = new StorageMigration.Progress() {
        public boolean cancelled() { return false; }
        public void copied(long bytes) {}
    };

    static int countEntries(File directory) {
        int count = 0;
        File[] children = directory.listFiles();
        if (children != null) for (File child : children) count += 1 + (child.isDirectory() ? countEntries(child) : 0);
        return count;
    }

    static boolean leftovers(File directory, String prefix) {
        File[] children = directory.listFiles();
        if (children != null) for (File child : children) if (child.getName().startsWith(prefix)) return true;
        return false;
    }

    // ------------------------------------------------------------------------------------

    static void testArchive() throws Exception {
        byte[] xbe = bytes(300_000, 1);  // larger than the 256 KiB copy buffer
        byte[] data = bytes(5_000, 2);

        // A normal archive, with a wrapper folder and a directory entry.
        {
            File parent = fresh("zip-ok");
            File batch = new File(parent, "import-1.part");
            long[] reported = { 0 };
            int files = InstallerArchive.extract(new ByteArrayInputStream(zip(
                    "Shadow/", null,
                    "Shadow/default.xbe", xbe,
                    "Shadow/data/stage.dat", data,
                    "Shadow/empty.bin", new byte[0])),
                batch, () -> false, count -> reported[0] += count);
            check(files == 3, "extract counts files, not directories (got " + files + ")");
            check(Arrays.equals(Files.readAllBytes(new File(batch, "Shadow/default.xbe").toPath()), xbe), "large entry content");
            check(Arrays.equals(Files.readAllBytes(new File(batch, "Shadow/data/stage.dat").toPath()), data), "nested entry content");
            check(new File(batch, "Shadow/empty.bin").length() == 0 && new File(batch, "Shadow/empty.bin").isFile(), "empty entry is created");
            check(reported[0] == xbe.length + data.length, "progress reports every byte (got " + reported[0] + ")");
        }

        // Windows-style separators are understood.
        {
            File batch = new File(fresh("zip-backslash"), "b");
            check(extractError(zip("Shadow\\data\\stage.dat", data), batch).isEmpty(), "backslash separators are accepted");
            check(new File(batch, "Shadow/data/stage.dat").isFile(), "backslash separators create folders");
        }

        // Names that would leave the batch are refused, and nothing is written outside it.
        String[] escapes = {
            "../evil.txt", "Shadow/../../evil.txt", "..\\evil.txt", "Shadow\\..\\..\\evil.txt",
            "/evil.txt", "\\evil.txt", "C:/evil.txt", "C:\\evil.txt", "Shadow//evil.txt", "./evil.txt", "Shadow/./evil.txt", "..",
        };
        for (String name : escapes) {
            File parent = fresh("zip-escape");
            File batch = new File(parent, "b");
            String error = extractError(zip("ok.txt", data, name, data), batch);
            check(error.equals("error_zip_path"), "entry '" + name + "' is refused (got '" + error + "')");
            check(!new File(parent, "evil.txt").exists() && !new File(scratch.toFile(), "evil.txt").exists()
                && !new File("/evil.txt").exists(), "entry '" + name + "' wrote nothing outside the batch");
        }

        // Two entries that end up as the same file.
        {
            String error = extractError(zip("a/b.txt", data, "a\\b.txt", xbe), new File(fresh("zip-dup"), "b"));
            check(error.equals("error_zip_duplicate"), "duplicate entries are refused (got '" + error + "')");
        }

        // Nothing usable inside.
        check(extractError(zip(), new File(fresh("zip-empty"), "b")).equals("error_zip_empty"), "an empty archive is refused");
        check(extractError(zip("only/", null, "only/dirs/", null), new File(fresh("zip-dirs"), "b")).equals("error_zip_empty"),
            "an archive of directories is refused");
        check(extractError(bytes(4096, 3), new File(fresh("zip-garbage"), "b")).equals("error_zip_empty"), "a file that is no ZIP is refused");
        check(extractError(new byte[0], new File(fresh("zip-zero"), "b")).equals("error_zip_empty"), "a zero-length file is refused");

        // A truncated archive fails instead of passing for a complete one.
        {
            byte[] whole = zip("Shadow/default.xbe", xbe, "Shadow/data/stage.dat", data);
            String error = extractError(Arrays.copyOf(whole, whole.length / 2), new File(fresh("zip-cut"), "b"));
            check(!error.isEmpty(), "a truncated archive is refused");
        }

        // The batch must be new: extracting over an existing folder could mix two sources.
        {
            File batch = fresh("zip-exists");
            check(extractError(zip("a.txt", data), batch).equals("error_io_create"), "an existing batch folder is refused");
        }

        // Cancelling stops between the pieces of an entry.
        {
            File batch = new File(fresh("zip-cancel"), "b");
            int[] pieces = { 0 };
            String error;
            try {
                InstallerArchive.extract(new ByteArrayInputStream(zip("big.bin", bytes(3_000_000, 4), "next.bin", data)),
                    batch, () -> pieces[0] >= 2, count -> pieces[0]++);
                error = "";
            } catch (LocalizedIOException exception) { error = exception.resource; }
            check(error.equals("error_zip_cancelled"), "cancelling is reported (got '" + error + "')");
            check(!new File(batch, "next.bin").exists(), "nothing is extracted after cancelling");
        }
    }

    // ------------------------------------------------------------------------------------

    static File makeInstallation(String name) throws IOException {
        File root = fresh(name);
        Files.createDirectories(new File(root, "game/data").toPath());
        Files.createDirectories(new File(root, "mods/CoolMod").toPath());
        Files.createDirectories(new File(root, "mods/.modloader/profiles").toPath());
        Files.createDirectories(new File(root, ".config/ShadowRecomp/save").toPath());
        Files.createDirectories(new File(root, "to_install").toPath()); // empty folder
        Files.write(new File(root, "game/default.xbe").toPath(), bytes(700_000, 10));
        Files.write(new File(root, "game/data/stage.dat").toPath(), bytes(12_345, 11));
        Files.write(new File(root, "game/data/empty.bin").toPath(), new byte[0]);
        // Not UTF-8, and it mentions the installation's own path: it must still arrive unchanged.
        ByteArrayOutputStream ini = new ByteArrayOutputStream();
        ini.write("Name=Cool\nAuthor=".getBytes(StandardCharsets.US_ASCII));
        ini.write(new byte[] { (byte) 0xE9, (byte) 0x82, (byte) 0xFF, (byte) 0xC3 });
        ini.write(("\nPath=" + root.getCanonicalPath() + File.separator + "mods\r\n").getBytes(StandardCharsets.UTF_8));
        Files.write(new File(root, "mods/CoolMod/mod.ini").toPath(), ini.toByteArray());
        Files.write(new File(root, "mods/.modloader/profiles/Default.json").toPath(),
            "{\"EnabledMods\":[\"CoolMod\"]}".getBytes(StandardCharsets.UTF_8));
        Files.write(new File(root, ".config/ShadowRecomp/config.toml").toPath(), "[Video]\n".getBytes(StandardCharsets.UTF_8));
        Files.write(new File(root, ".config/ShadowRecomp/save/Save.dat").toPath(), bytes(4_096, 12));
        return root;
    }

    static boolean sameTree(File a, File b) throws IOException {
        String[] namesA = a.list(), namesB = b.list();
        if (namesA == null || namesB == null) return false;
        Arrays.sort(namesA);
        Arrays.sort(namesB);
        if (!Arrays.equals(namesA, namesB)) return false;
        for (String name : namesA) {
            File childA = new File(a, name), childB = new File(b, name);
            if (childA.isDirectory() != childB.isDirectory()) return false;
            if (childA.isDirectory()) { if (!sameTree(childA, childB)) return false; }
            else if (!Arrays.equals(Files.readAllBytes(childA.toPath()), Files.readAllBytes(childB.toPath()))) return false;
        }
        return true;
    }

    static void testMigration() throws Exception {
        // A complete copy.
        {
            File source = makeInstallation("copy-src");
            File parent = fresh("copy-dst");
            File target = new File(parent, "ShadowRecomp");
            check(target.mkdir(), "setup");
            int entries = countEntries(source);
            long[] copied = { 0 };
            String error = copyError(source, target, new StorageMigration.Progress() {
                public boolean cancelled() { return false; }
                public void copied(long bytes) { copied[0] += bytes; }
            });
            check(error.isEmpty(), "a copy into an empty folder succeeds (got '" + error + "')");
            check(sameTree(source, target), "the copy is identical, byte for byte");
            check(countEntries(source) == entries && countEntries(target) == entries, "the original is left in place");
            check(new File(target, "to_install").isDirectory(), "empty folders are copied");
            check(copied[0] == 700_000 + 12_345 + 4_096
                + new File(source, "mods/CoolMod/mod.ini").length()
                + new File(source, "mods/.modloader/profiles/Default.json").length()
                + new File(source, ".config/ShadowRecomp/config.toml").length(), "progress reports every byte (got " + copied[0] + ")");
            check(!leftovers(parent, ".storage-copy-"), "no temporary folder is left behind");
        }

        // The destination has to be an existing, empty folder.
        {
            File source = makeInstallation("full-src");
            File target = fresh("full-dst");
            Files.write(new File(target, "keep.txt").toPath(), new byte[] { 1 });
            check(copyError(source, target, QUIET).equals("error_copy_empty"), "a folder with content is refused");
            check(countEntries(target) == 1 && new File(target, "keep.txt").length() == 1, "its content is untouched");
            check(copyError(source, new File(fresh("missing-dst"), "nothing-here"), QUIET).equals("error_copy_empty"),
                "a folder that does not exist is refused");
        }

        // Source and destination must not contain each other.
        {
            File source = makeInstallation("nest-src");
            File inside = new File(source, "to_install");
            check(copyError(source, source, QUIET).equals("error_copy_separate"), "copying a folder onto itself is refused");
            check(copyError(source, inside, QUIET).equals("error_copy_separate"), "copying into its own subfolder is refused");
            check(copyError(inside, source, QUIET).equals("error_copy_separate"), "copying onto its own parent is refused");
            check(new File(source, "game/default.xbe").length() == 700_000, "the installation is untouched");
        }

        // Folders that only share a name prefix are separate.
        {
            File parent = fresh("prefix");
            File source = new File(parent, "Shadow");
            File target = new File(parent, "ShadowRecomp");
            check(source.mkdir() && target.mkdir(), "setup");
            Files.write(new File(source, "a.bin").toPath(), bytes(100, 20));
            check(copyError(source, target, QUIET).isEmpty() && new File(target, "a.bin").length() == 100,
                "folders sharing a name prefix are not mistaken for nested ones");
        }

        // Cancelling: the destination stays the empty folder it was.
        {
            File source = makeInstallation("cancel-src");
            File parent = fresh("cancel-dst");
            File target = new File(parent, "ShadowRecomp");
            check(target.mkdir(), "setup");
            long[] copied = { 0 };
            String error = copyError(source, target, new StorageMigration.Progress() {
                public boolean cancelled() { return copied[0] > 300_000; }
                public void copied(long bytes) { copied[0] += bytes; }
            });
            check(error.equals("error_copy_cancelled"), "cancelling is reported (got '" + error + "')");
            check(target.isDirectory() && countEntries(target) == 0, "a cancelled copy leaves the destination empty");
            check(!leftovers(parent, ".storage-copy-"), "a cancelled copy leaves no temporary folder");
            check(new File(source, "game/default.xbe").length() == 700_000, "a cancelled copy leaves the original alone");
        }

        // Links are refused rather than followed out of the installation.
        {
            File source = makeInstallation("link-src");
            File outside = fresh("link-outside");
            Files.write(new File(outside, "secret.txt").toPath(), new byte[] { 1, 2, 3 });
            boolean linked = true;
            try { Files.createSymbolicLink(new File(source, "mods/link").toPath(), outside.toPath()); }
            catch (UnsupportedOperationException | IOException exception) { linked = false; }
            if (linked) {
                File parent = fresh("link-dst");
                File target = new File(parent, "ShadowRecomp");
                check(target.mkdir(), "setup");
                String error = copyError(source, target, QUIET);
                check(error.equals("error_copy_link"), "a symbolic link is refused (got '" + error + "')");
                check(target.isDirectory() && countEntries(target) == 0, "a refused copy leaves the destination empty");
                check(!leftovers(parent, ".storage-copy-"), "a refused copy leaves no temporary folder");
            } else {
                System.out.println("note: this file system has no symbolic links; that case was skipped");
            }
        }
    }

    static void deleteTree(File file) {
        if (!Files.isSymbolicLink(file.toPath())) {
            File[] children = file.listFiles();
            if (children != null) for (File child : children) deleteTree(child);
        }
        file.delete();
    }

    public static void main(String[] args) throws Exception {
        scratch = Files.createTempDirectory("launcher-files-test");
        try {
            testArchive();
            testMigration();
        } finally {
            deleteTree(scratch.toFile());
        }
        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
