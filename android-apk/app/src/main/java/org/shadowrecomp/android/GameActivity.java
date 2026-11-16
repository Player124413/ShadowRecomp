package org.shadowrecomp.android;

import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.SystemClock;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import android.view.Display;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowManager;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.IOException;

/**
 * The game itself. SDL's own Java glue (org.libsdl.app, taken unmodified from the SDL
 * submodule so it always matches the native SDL build) does the heavy lifting; this
 * subclass only adds what the port needs on top of it:
 *
 * <ul>
 *   <li>publishes the storage layout to the native side through environment variables
 *       before libmain.so is loaded, because the game resolves its paths in static
 *       initializers, long before JNI is usable;</li>
 *   <li>keeps the session alive when Back is pressed (the task is minimized instead of
 *       the activity being finished);</li>
 *   <li>shows the progress of the native installer and localized native messages.</li>
 * </ul>
 */
public class GameActivity extends SDLActivity {
    private static final String TAG = "ShadowRecomp";

    /**
     * Intent extra with {@link SystemClock#uptimeMillis()} at the moment Launch game was pressed.
     * The native side measures the startup from there (see the "[startup]" lines of log.txt).
     */
    static final String EXTRA_LAUNCH_UPTIME = "org.shadowrecomp.android.LAUNCH_UPTIME";

    private static boolean gameStorageLocked;

    private AlertDialog installDialog;
    private ProgressBar installBar;
    private TextView installLabel;

    /**
     * True once a game session has resolved its storage in this process. SDL cannot boot a
     * second session in the same process, so the launcher refuses to change the storage
     * root or import game files until the app has been fully closed.
     */
    static boolean isGameStorageLocked() {
        return gameStorageLocked || (mSingleton != null && !mBrokenLibraries);
    }

    @Override
    protected String[] getLibraries() {
        // SDL3 and SDL3_mixer are linked statically into libmain.so.
        return new String[] { "main" };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Must happen before super.onCreate(): that is where libmain.so gets loaded and
        // its static initializers read these variables.
        publishEnvironment();

        super.onCreate(savedInstanceState);

        // Landscape gameplay would otherwise leave a black bar over the display cutout.
        if (Build.VERSION.SDK_INT >= 28 /* Android 9 (P) */) {
            WindowManager.LayoutParams params = getWindow().getAttributes();
            params.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            getWindow().setAttributes(params);
        }

        requestHighestRefreshRate();
    }

    private void publishEnvironment() {
        String dataRoot = "";
        try {
            File root = AppStorage.activeGameRoot(this);
            AppStorage.validateRoot(this, root);
            dataRoot = root.getCanonicalPath();
            gameStorageLocked = true;
        } catch (IOException exception) {
            // The native side reports the problem with a localized message box and exits.
            Log.e(TAG, "Game storage is unavailable", exception);
        }

        File media = AppStorage.mediaBase(this);
        File cache = getCacheDir();
        setEnvironment("SHADOWRECOMP_DATA_ROOT", dataRoot);
        setEnvironment("SHADOWRECOMP_INTERNAL_DIR", getFilesDir().getAbsolutePath());
        setEnvironment("SHADOWRECOMP_EXTERNAL_DIR", AppStorage.transferRoot(this).getAbsolutePath());
        setEnvironment("SHADOWRECOMP_MEDIA_DIR", media != null ? media.getAbsolutePath() : "");
        setEnvironment("SHADOWRECOMP_CACHE_DIR", cache != null ? cache.getAbsolutePath() : "");
        setEnvironment("SHADOWRECOMP_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir);
        setEnvironment("SHADOWRECOMP_APP_VERSION", UpdateManager.currentVersion(this));
        setEnvironment("SHADOWRECOMP_APK_STAMP", apkStamp());

        // SystemClock.uptimeMillis() and the native CLOCK_MONOTONIC are the same clock. Left
        // empty when the activity was not started by the launcher's button; the native side
        // then counts from the moment the game library was loaded.
        Intent intent = getIntent();
        long launchUptime = intent != null ? intent.getLongExtra(EXTRA_LAUNCH_UPTIME, 0) : 0;
        setEnvironment("SHADOWRECOMP_LAUNCH_UPTIME_MS", launchUptime > 0 ? Long.toString(launchUptime) : "");
    }

    /**
     * Identifies this installation of the APK: the value changes with every install and
     * update. The native side keeps it next to the Vulkan drivers it extracts from the APK,
     * so that it only has to compare them with the APK again when the APK was replaced.
     */
    private String apkStamp() {
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            return info.lastUpdateTime + "-" + info.getLongVersionCode() + "-" + new File(getApplicationInfo().sourceDir).length();
        } catch (PackageManager.NameNotFoundException | RuntimeException exception) {
            // Without a stamp the native side compares the drivers on every launch.
            Log.w(TAG, "Could not identify the installed APK", exception);
            return "";
        }
    }

    private static void setEnvironment(String name, String value) {
        try {
            Os.setenv(name, value != null ? value : "", true);
        } catch (ErrnoException exception) {
            Log.e(TAG, "Could not set " + name, exception);
        }
    }

    // High refresh rate phones keep apps at 60 Hz unless they ask for more, which caps the
    // game at 60 FPS through vsync even when its FPS option is higher. Prefer the fastest
    // mode at the current resolution; the in-game limiter still paces lower FPS settings.
    @SuppressWarnings("deprecation")
    private void requestHighestRefreshRate() {
        try {
            Display display = getWindowManager().getDefaultDisplay();
            Display.Mode current = display.getMode();
            Display.Mode best = current;
            for (Display.Mode mode : display.getSupportedModes()) {
                if (mode.getPhysicalWidth() == current.getPhysicalWidth() &&
                    mode.getPhysicalHeight() == current.getPhysicalHeight() &&
                    mode.getRefreshRate() > best.getRefreshRate()) {
                    best = mode;
                }
            }
            WindowManager.LayoutParams params = getWindow().getAttributes();
            params.preferredDisplayModeId = best.getModeId();
            getWindow().setAttributes(params);
            Log.v(TAG, "Preferred display mode " + best.getModeId() + ": " +
                best.getPhysicalWidth() + "x" + best.getPhysicalHeight() + " @ " + best.getRefreshRate() + " Hz");
        } catch (Exception exception) {
            Log.w(TAG, "Could not request a higher refresh rate", exception);
        }
    }

    // ------------------------------------------------------------------
    // Back handling: a game session must survive Back exactly like Home.
    // ------------------------------------------------------------------

    @Override
    public void onBackPressed() {
        minimizeTask();
    }

    @Override
    public void superOnBackPressed() {
        minimizeTask();
    }

    private void minimizeTask() {
        if (!isFinishing()) {
            boolean moved = moveTaskToBack(true);
            Log.v(TAG, "moveTaskToBack(true): " + moved);
        }
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // SDL's focused surface consumes key events before Activity.onBackPressed() is
        // reached, so the system Back key/gesture is intercepted here. Gamepad buttons
        // (and the BACK fallback Android synthesizes for an unhandled B button), mouse
        // back buttons and Back while the on-screen keyboard is open keep SDL's handling.
        if (!mBrokenLibraries && event.getKeyCode() == KeyEvent.KEYCODE_BACK && isSystemBack(event)) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                minimizeTask();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private static boolean isSystemBack(KeyEvent event) {
        int source = event.getSource();
        if ((event.getFlags() & KeyEvent.FLAG_FALLBACK) != 0) return false;
        if ((source & InputDevice.SOURCE_MOUSE) == InputDevice.SOURCE_MOUSE) return false;
        if ((source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) return false;
        if ((source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return false;
        return mTextEdit == null || mTextEdit.getVisibility() != View.VISIBLE;
    }

    // ------------------------------------------------------------------
    // Called from native code (os/android/jni_android.cpp) on the SDL thread.
    // ------------------------------------------------------------------

    /** Native messages follow Android's language, including ones the game has no locale for. */
    public String getLocalizedMessage(String resourceName, String detail) {
        int resource = getResources().getIdentifier(resourceName, "string", getPackageName());
        return resource == 0 ? "" : getString(resource, detail != null ? detail : "");
    }

    /**
     * Shows or updates the progress of the native game installer.
     *
     * @param fileName file currently being verified and copied, may be empty
     * @param permille progress in the range 0..1000
     */
    public void setInstallProgress(final String fileName, final int permille) {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (isFinishing()) return;
                if (installDialog == null) {
                    int padding = Math.round(20 * getResources().getDisplayMetrics().density);
                    LinearLayout layout = new LinearLayout(GameActivity.this);
                    layout.setOrientation(LinearLayout.VERTICAL);
                    layout.setPadding(padding, padding, padding, padding / 2);
                    installBar = new ProgressBar(GameActivity.this, null, android.R.attr.progressBarStyleHorizontal);
                    installBar.setMax(1000);
                    layout.addView(installBar, new LinearLayout.LayoutParams(
                        LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
                    installLabel = new TextView(GameActivity.this);
                    installLabel.setGravity(Gravity.START);
                    installLabel.setSingleLine(true);
                    installLabel.setPadding(0, padding / 2, 0, 0);
                    layout.addView(installLabel);
                    installDialog = new AlertDialog.Builder(GameActivity.this)
                        .setTitle(R.string.native_install_title)
                        .setMessage(R.string.native_install_message)
                        .setView(layout)
                        .setCancelable(false)
                        .create();
                    installDialog.setCanceledOnTouchOutside(false);
                    installDialog.show();
                }
                installBar.setProgress(Math.max(0, Math.min(1000, permille)));
                installLabel.setText(fileName != null ? fileName : "");
            }
        });
    }

    /** Dismisses the installer progress dialog, if it is showing. */
    public void finishInstallProgress() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                if (installDialog != null) {
                    installDialog.dismiss();
                    installDialog = null;
                    installBar = null;
                    installLabel = null;
                }
            }
        });
    }

    @Override
    protected void onDestroy() {
        if (installDialog != null) {
            installDialog.dismiss();
            installDialog = null;
        }
        super.onDestroy();
    }
}
