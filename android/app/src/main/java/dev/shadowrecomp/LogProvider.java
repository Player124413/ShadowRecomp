package dev.shadowrecomp;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.File;
import java.io.FileNotFoundException;

/**
 * The one thing a log needs to leave an Android app: a {@code content://} URI.
 *
 * Handing a file path to another app (a {@code file://} URI) has thrown FileUriExposedException since Android 7, and the
 * usual answer - AndroidX's FileProvider - would add a dependency to a build that deliberately has none. This provider
 * does only what the share sheet needs, for one URI, read-only, and the app never exports it: the permission is granted
 * per-intent, to the app the player picks.
 */
public class LogProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    /** Only "latest" exists: the newest log at the moment it is read (see LogSharing.latest). */
    private File resolve(Uri uri) {
        java.util.List<String> path = uri.getPathSegments();
        if (path.size() != 1 || !LogSharing.LATEST.equals(path.get(0)) || getContext() == null) return null;
        return LogSharing.latest(getContext());
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("the log is read-only");
        File f = resolve(uri);
        if (f == null) throw new FileNotFoundException("no log yet");
        return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
    }

    /** The name and the size the receiving app shows and copies: without a DISPLAY_NAME some apps save "latest" as the
     *  file name and the log loses the timestamp that identifies the run. */
    @Override
    public Cursor query(Uri uri, String[] projection, String selection, String[] args, String sortOrder) {
        File f = resolve(uri);
        if (f == null) return null;
        String[] cols = projection != null && projection.length > 0
                ? projection : new String[]{OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE};
        MatrixCursor c = new MatrixCursor(cols, 1);
        Object[] row = new Object[cols.length];
        for (int i = 0; i < cols.length; i++) {
            if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) row[i] = f.getName();
            else if (OpenableColumns.SIZE.equals(cols[i])) row[i] = f.length();
            else row[i] = null;
        }
        c.addRow(row);
        return c;
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        return null;   // read-only
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        return 0;
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        return 0;
    }
}
