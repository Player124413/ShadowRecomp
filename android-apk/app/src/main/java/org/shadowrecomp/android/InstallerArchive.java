package org.shadowrecomp.android;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.util.HashSet;
import java.util.Set;
import java.util.function.BooleanSupplier;
import java.util.function.LongConsumer;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Extracts a package ZIP into a fresh, unpublished staging batch in one pass. */
final class InstallerArchive {
    static int extract(InputStream input, File batch, BooleanSupplier cancelled,
            LongConsumer progress) throws IOException {
        if (!batch.mkdirs()) throw new LocalizedIOException("error_io_create", batch);
        String root = batch.getCanonicalPath() + File.separator;
        Set<String> seen = new HashSet<>();
        int files = 0;
        try (ZipInputStream zip = new ZipInputStream(input)) {
            byte[] buffer = new byte[256 * 1024];
            ZipEntry entry;
            while ((entry = zip.getNextEntry()) != null) {
                if (cancelled.getAsBoolean()) throw new LocalizedIOException("error_zip_cancelled", null);
                String name = entry.getName().replace('\\', '/');
                if (name.startsWith("/") || name.indexOf(':') >= 0) throw new LocalizedIOException("error_zip_path", name);
                for (String part : name.split("/")) {
                    if (part.equals("..") || part.equals(".") || part.isEmpty()) throw new LocalizedIOException("error_zip_path", name);
                }
                File target = new File(batch, name).getCanonicalFile();
                if (!target.getPath().startsWith(root)) throw new LocalizedIOException("error_zip_path", name);
                if (entry.isDirectory()) continue;
                if (!seen.add(target.getPath())) throw new LocalizedIOException("error_zip_duplicate", name);
                if (!target.getParentFile().isDirectory() && !target.getParentFile().mkdirs())
                    throw new LocalizedIOException("error_io_create", target.getParent());
                try (FileOutputStream output = new FileOutputStream(target)) {
                    int count;
                    while ((count = zip.read(buffer)) != -1) {
                        if (cancelled.getAsBoolean()) throw new LocalizedIOException("error_zip_cancelled", null);
                        output.write(buffer, 0, count);
                        progress.accept(count);
                    }
                    output.getFD().sync();
                }
                files++;
            }
        }
        if (files == 0) throw new LocalizedIOException("error_zip_empty", null);
        return files;
    }
}
