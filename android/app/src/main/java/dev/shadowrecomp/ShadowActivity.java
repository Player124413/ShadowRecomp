package dev.shadowrecomp;

import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.view.Surface;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import dev.encounter.aurora.AuroraSurface;

/**
 * The game itself: SDL3's activity with Aurora's own surface (which tells the renderer when the window is really
 * there, so nothing renders into a dying surface).
 *
 * Everything is set up by the launcher: the disc image, the graphics preset and the touch-controls switch travel as
 * command-line arguments, which is the same code path the desktop build uses.
 *
 * The screen belongs to the game, so this activity runs immersive: the status bar (with its clock) and the three
 * navigation buttons (back, home, recent - the triangle, circle and square) are hidden, and swiping from an edge brings
 * them back for a few seconds. Without this they sat on top of the picture and, as the touch overlay is drawn to the
 * bottom of the screen, the navigation bar covered the controls a thumb needs.
 */
public class ShadowActivity extends SDLActivity {
    /** The arguments the launcher prepared, one per line separated by \u0001 (SDL's getArguments is String[]). */
    public static final String EXTRA_ARGS = "shadow.args";

    private static String[] sArgs = new String[0];

    /** Called by LauncherActivity before it starts this activity: SDL reads them in onCreate. */
    static void setArgs(String args) {
        sArgs = (args == null || args.isEmpty()) ? new String[0] : args.split("\u0001");
    }

    /**
     * Called from the native side (runtime/android_jni.cpp) when "Send the log" is chosen in the in-game menu.
     *
     * The name and the empty signature are the contract with that call: changing either breaks it at run time, not at
     * compile time, so keep them in step with android_share_log().
     */
    public void shareLatestLog() {
        LogSharing.share(this);
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL3", "main"};
    }

    @Override
    protected String[] getArguments() {
        // Normally the launcher's copy (a static), but the system may recreate this activity after killing the process;
        // the intent it was started with survives that, so read from it too.
        if (sArgs.length == 0 && getIntent() != null) {
            String extra = getIntent().getStringExtra(EXTRA_ARGS);
            if (extra != null && !extra.isEmpty()) setArgs(extra);
        }
        return sArgs;
    }

    @Override
    protected void onCreate(android.os.Bundle savedInstanceState) {
        JavaCrash.install(this);   // SDL's Java side is ours too: a crash there leaves a file to send
        super.onCreate(savedInstanceState);
        // A game is played, not read: never let the screen time out under a thumb.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new AuroraSurface(context);
    }

    @Override
    protected void onPause() {
        super.onPause();   // SDL3 pauses the native audio and the frame clock; Aurora also watches the surface
    }

    @Override
    protected void onResume() {
        super.onResume();
        hideSystemBars();   // coming back from the recents screen shows them again
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        // SDL's own activity touches the system UI flags, and the system restores the bars after a keyboard, a call or
        // the notification shade: so this is applied again on every focus change, not only once.
        if (hasFocus) hideSystemBars();
    }

    /**
     * Immersive, sticky full-screen: no status bar (no clock), no navigation bar (no triangle / circle / square), and a
     * swipe from an edge brings them back briefly while the game keeps drawing. API 30 has the insets controller for
     * this; older versions use the view flags, which still work on the API levels this app supports (26+).
     */
    @SuppressWarnings("deprecation")
    private void hideSystemBars() {
        Window window = getWindow();
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            // no grey scrim behind the bars while they are shown for a moment after a swipe: the picture stays the
            // picture, and the bars simply sit on it
            window.setStatusBarContrastEnforced(false);
            window.setNavigationBarContrastEnforced(false);
        }
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
        }
    }
}
