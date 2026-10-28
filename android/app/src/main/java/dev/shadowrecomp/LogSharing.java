package dev.shadowrecomp;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.widget.Toast;

import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

/**
 * Getting the log off the phone.
 *
 * On a PC the log is a file next to the game and the player can attach it to a message themselves. On a phone that file
 * sits in the app's private folder, where nobody can reach it without adb - which is exactly the moment when it matters,
 * because something went wrong. So the launcher and the in-game menu offer two taps: "Send the log" (the phone's share
 * sheet: a messenger, mail, Drive, anything) and "Copy the log" (the clipboard, to paste into a message).
 *
 * Which file: the newest of the run logs (files/logs/*.log), the reports of runs that ended badly (files/crashes/*.txt)
 * and the bug captures (files/bugs/<time>/log.txt). A crash report is a copy of the log of the run that died, so it is
 * the newest file and therefore the one that gets sent.
 *
 * No AndroidX: the file is handed over through the app's own tiny content provider ({@link LogProvider}), because since
 * Android 7 an app may not hand a plain file path to another app at all.
 */
public final class LogSharing {
    public static final String AUTHORITY = "dev.shadowrecomp.logs";
    public static final String LATEST = "latest";

    private LogSharing() {
    }

    /** The content URI the share sheet gets: the provider resolves it to the newest log at the moment it is read. */
    public static Uri latestUri() {
        return Uri.parse("content://" + AUTHORITY + "/" + LATEST);
    }

    private static void addNewest(List<File> out, File dir, String suffix) {
        File[] files = dir == null ? null : dir.listFiles();
        if (files == null) return;
        for (File f : files) {
            if (f.isFile() && f.getName().endsWith(suffix) && f.length() > 0) out.add(f);
        }
    }

    /** Every log on the phone, newest first. */
    static List<File> candidates(Context c) {
        List<File> out = new ArrayList<>();
        File files = c.getFilesDir();
        addNewest(out, new File(files, "logs"), ".log");
        addNewest(out, new File(files, "crashes"), ".txt");
        File bugs = new File(files, "bugs");
        File[] dirs = bugs.listFiles();
        if (dirs != null) {
            for (File d : dirs) {
                if (d.isDirectory()) addNewest(out, d, ".txt");
            }
        }
        out.sort((a, b) -> Long.compare(b.lastModified(), a.lastModified()));
        return out;
    }

    /** The file "Send the log" attaches, or null when the game has not run yet. */
    public static File latest(Context c) {
        List<File> all = candidates(c);
        return all.isEmpty() ? null : all.get(0);
    }

    /**
     * The newest report of a run that ended badly, or null. crash_report.cpp writes crash-*.txt for a real crash (its log
     * ends with the runtime's own fault line) and stop-*.txt when Android merely closed the app, so a stop file must not
     * be announced as a crash.
     */
    public static File latestCrash(Context c) {
        File dir = new File(c.getFilesDir(), "crashes");
        File[] files = dir.listFiles();
        if (files == null) return null;
        File newest = null, newestStop = null;
        for (File f : files) {
            if (!f.isFile() || f.length() == 0) continue;
            if (f.getName().startsWith("crash-") || f.getName().startsWith("fatal-") || f.getName().startsWith("java-")) {
                if (newest == null || f.lastModified() > newest.lastModified()) newest = f;
            } else if (f.getName().startsWith("stop-")) {
                if (newestStop == null || f.lastModified() > newestStop.lastModified()) newestStop = f;
            }
        }
        if (newest == null) return null;
        if (newestStop != null && newestStop.lastModified() > newest.lastModified()) return null;   // a later clean stop
        return newest;
    }

    /** One line for the launcher: what was kept, so the player knows what "Send the log" will send. */
    public static String summary(Context c) {
        File crash = latestCrash(c);
        if (crash != null) return "The game did not finish its last run. Send the log - it says why.";
        File log = latest(c);
        if (log == null) return "No log yet: the log is written while the game runs.";
        return "Newest log: " + log.getName() + " (" + Math.max(1, log.length() / 1024) + " KB)";
    }

    /** The phone's share sheet, with the newest log attached. */
    public static void share(final Activity activity) {
        final File log = latest(activity);
        if (log == null) {
            toast(activity, "There is no log yet - start the game once.");
            return;
        }
        activity.runOnUiThread(() -> {
            Intent send = new Intent(Intent.ACTION_SEND);
            send.setType("text/plain");
            send.putExtra(Intent.EXTRA_STREAM, latestUri());
            send.putExtra(Intent.EXTRA_SUBJECT, "ShadowRecomp log " + log.getName());
            send.putExtra(Intent.EXTRA_TEXT, "ShadowRecomp log: " + log.getName()
                    + "\nBuild: " + version(activity) + "\nDevice: " + DeviceInfo.report(activity));
            // The grant travels with the intent: without the ClipData some apps cannot open the stream even though the
            // permission flag is set.
            send.setClipData(ClipData.newRawUri(log.getName(), latestUri()));
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            try {
                activity.startActivity(Intent.createChooser(send, "Send the log"));
            } catch (Exception e) {
                toast(activity, "No app can receive a file here - use \"Copy the log\" instead.");
            }
        });
    }

    /** The last part of the log straight into the clipboard: the one tap that works on every phone. */
    public static void copy(final Activity activity) {
        final String text = readTail(latest(activity), 400);
        if (text.isEmpty()) {
            toast(activity, "There is no log yet - start the game once.");
            return;
        }
        activity.runOnUiThread(() -> {
            try {
                ClipboardManager cm = (ClipboardManager) activity.getSystemService(Context.CLIPBOARD_SERVICE);
                if (cm != null) cm.setPrimaryClip(ClipData.newPlainText("ShadowRecomp log", text));
                toast(activity, "Log copied - paste it into a message");
            } catch (Exception e) {
                toast(activity, "Could not copy the log: " + e.getMessage());
            }
        });
    }

    /** The last {@code maxLines} lines of a file, or "" when it does not exist. */
    static String readTail(File f, int maxLines) {
        if (f == null) return "";
        try (FileInputStream in = new FileInputStream(f)) {
            byte[] all = new byte[(int) Math.min(f.length(), 256 * 1024)];
            int n = 0;
            while (n < all.length) {
                int r = in.read(all, n, all.length - n);
                if (r < 0) break;
                n += r;
            }
            if (n == 0) return "";
            int start = 0;
            if (f.length() > all.length) {   // the window began mid-line: drop that line
                while (start < n && all[start] != '\n') start++;
                start++;
            }
            String text = new String(all, start, Math.max(0, n - start), "UTF-8");
            String[] lines = text.split("\n", -1);
            if (lines.length <= maxLines) return text;
            StringBuilder sb = new StringBuilder();
            for (int i = lines.length - maxLines; i < lines.length; i++) sb.append(lines[i]).append('\n');
            return sb.toString();
        } catch (IOException e) {
            return "";
        }
    }

    /** "1.1.42" and the build stamp, which tells an update apart from an install. */
    public static String version(Context c) {
        try {
            return c.getPackageManager().getPackageInfo(c.getPackageName(), 0).versionName;
        } catch (Exception e) {
            return "?";
        }
    }

    private static void toast(Activity a, String text) {
        try {
            Toast.makeText(a, text, Toast.LENGTH_LONG).show();
        } catch (Exception ignored) {
        }
    }
}
