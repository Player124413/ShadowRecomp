package dev.shadowrecomp;

import android.content.Context;

import java.io.File;
import java.io.PrintWriter;
import java.io.StringWriter;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * A Java-side crash (a bug in the launcher's UI, an OutOfMemoryError while reading the disc image) is a dialog that
 * disappears with the process, and nothing is left to read. The native crashes already leave a log - runtime/rt_sys.cpp
 * writes the fault into it and crash_report.cpp keeps it - so this does the same for the Java side: one file in
 * files/crashes/, which "Send the log" picks up because it is the newest file there.
 *
 * The previous handler is called afterwards, so the system still shows its own dialog and the app still dies the way it
 * normally would: this only writes down what happened.
 */
public final class JavaCrash {
    private JavaCrash() {
    }

    public static void install(final Context context) {
        final Context app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try {
                write(app, thread, error);
            } catch (Throwable ignored) {
                // nothing useful can be done from here: never let the reporter hide the real crash
            }
            if (previous != null) previous.uncaughtException(thread, error);
        });
    }

    private static void write(Context context, Thread thread, Throwable error) {
        File dir = new File(context.getFilesDir(), "crashes");
        if (!dir.isDirectory() && !dir.mkdirs()) return;
        String stamp = new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.US).format(new Date());
        File out = new File(dir, "java-" + stamp + ".txt");
        StringWriter trace = new StringWriter();
        PrintWriter w = new PrintWriter(trace);
        w.println("ShadowRecomp's Android side stopped: " + error);
        w.println("build: " + LogSharing.version(context));
        w.println("device: " + DeviceInfo.report(context));
        w.println("thread: " + thread.getName());
        w.println();
        error.printStackTrace(w);
        w.flush();
        try (PrintWriter file = new PrintWriter(out, "UTF-8")) {
            file.print(trace);
        } catch (Exception ignored) {
        }
    }
}
