package dev.shadowrecomp;

import android.content.Context;
import android.content.Intent;
import android.view.Surface;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

import dev.encounter.aurora.AuroraSurface;

/**
 * The game itself: SDL3's activity with Aurora's own surface (which tells the renderer when the window is really
 * there, so nothing renders into a dying surface).
 *
 * Everything is set up by the launcher: the disc image, the graphics preset and the touch-controls switch travel as
 * command-line arguments, which is the same code path the desktop build uses.
 */
public class ShadowActivity extends SDLActivity {
    /** The arguments the launcher prepared, one per line separated by \u0001 (SDL's getArguments is String[]). */
    public static final String EXTRA_ARGS = "shadow.args";

    private static String[] sArgs = new String[0];

    /** Called by LauncherActivity before it starts this activity: SDL reads them in onCreate. */
    static void setArgs(String args) {
        sArgs = (args == null || args.isEmpty()) ? new String[0] : args.split("\u0001");
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
        super.onCreate(savedInstanceState);
        // A game is played, not read: never let the screen time out under a thumb.
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new AuroraSurface(context);
    }

    @Override
    protected void onPause() {
        super.onPause();   // SDL3 pauses the native audio and the frame clock; Aurora also watches the surface
    }
}
