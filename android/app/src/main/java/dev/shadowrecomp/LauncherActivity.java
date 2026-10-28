package dev.shadowrecomp;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

/**
 * The launcher: what a player sees first.
 *
 * It does three jobs and nothing else - import the player's own disc image, choose how the game should look and run,
 * and start it. The controls themselves are edited in the game (Touch controls > EDIT), because that is where you can
 * see them over the picture; this screen only switches them on or off.
 *
 * No AndroidX and no layout XML: plain platform widgets, so the APK stays small and the build needs no extra
 * dependencies.
 */
public class LauncherActivity extends Activity {
    private static final int PICK_DISC = 1001;
    private static final String PREFS = "shadow_launcher";

    private SharedPreferences prefs;
    private TextView status;
    private Button playButton;
    private LinearLayout presetGroup;
    private CheckBox touchToggle;
    private Button editTouchButton;
    private SeekBar renderScale;
    private TextView renderScaleLabel;
    private TextView logStatus;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        JavaCrash.install(this);   // a crash in here still leaves something to send (JavaCrash.java)
        prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        setContentView(buildUi());
        refreshDiscStatus();
        refreshLogStatus();
    }

    // ---------------------------------------------------------------------------------------------------------------
    // the screen

    private int dp(float v) {
        return (int) (v * getResources().getDisplayMetrics().density + 0.5f);
    }

    private TextView text(String s, float size, int color) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(size);
        t.setTextColor(color);
        return t;
    }

    private View buildUi() {
        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.BLACK);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        int pad = dp(20);
        root.setPadding(pad, pad, pad, pad);
        scroll.addView(root);

        root.addView(text(getString(R.string.app_name), 30, Color.WHITE));
        TextView sub = text("Shadow the Hedgehog (2005), recompiled. Bring your own USA disc image.", 14,
                Color.rgb(170, 176, 190));
        sub.setPadding(0, dp(4), 0, dp(18));
        root.addView(sub);

        // ---- the disc ----
        status = text("", 15, Color.WHITE);
        status.setPadding(0, 0, 0, dp(10));
        root.addView(status);

        Button pick = button("Import disc image\u2026", false);
        pick.setOnClickListener(v -> pickDisc());
        root.addView(pick);

        // ---- graphics preset ----
        root.addView(sectionTitle("Graphics"));
        presetGroup = new LinearLayout(this);
        presetGroup.setOrientation(LinearLayout.VERTICAL);
        String[] presets = {"auto|Auto (recommended)", "quality|Quality", "balanced|Balanced", "performance|Performance"};
        String saved = prefs.getString("preset", "auto");
        for (String p : presets) {
            String[] kv = p.split("\\|");
            RadioButton rb = new RadioButton(this);
            rb.setText(kv[1]);
            rb.setTag(kv[0]);
            rb.setTextColor(Color.WHITE);
            rb.setChecked(kv[0].equals(saved));
            presetGroup.addView(rb);
        }
        root.addView(presetGroup);

        renderScaleLabel = text("", 14, Color.rgb(170, 176, 190));
        renderScaleLabel.setPadding(0, dp(10), 0, 0);
        root.addView(renderScaleLabel);
        renderScale = new SeekBar(this);
        renderScale.setMax(3);   // 0 = auto, 1 = 50 %, 2 = 75 %, 3 = native
        renderScale.setProgress(prefs.getInt("render_scale", 0));
        renderScale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar s, int value, boolean fromUser) {
                renderScaleLabel.setText("Render resolution: " + new String[]{"Auto", "50 % (fastest)", "75 %", "Native"}[value]);
            }

            public void onStartTrackingTouch(SeekBar s) {
            }

            public void onStopTrackingTouch(SeekBar s) {
            }
        });
        renderScaleLabel.setText("Render resolution: " + new String[]{"Auto", "50 % (fastest)", "75 %", "Native"}
                [renderScale.getProgress()]);
        root.addView(renderScale);

        // ---- controls ----
        root.addView(sectionTitle("Controls"));
        touchToggle = new CheckBox(this);
        touchToggle.setText("On-screen touch controls");
        touchToggle.setTextColor(Color.WHITE);
        touchToggle.setChecked(prefs.getBoolean("touch_controls", true));
        root.addView(touchToggle);

        editTouchButton = button("Edit the touch controls\u2026", false);
        editTouchButton.setOnClickListener(v -> startGame(true));
        root.addView(editTouchButton);
        TextView editHint = text("Buttons and sticks are dragged, resized and rebound in the game, over the picture, " +
                "with a finger. A gamepad hides them while it is connected.", 13, Color.rgb(150, 156, 170));
        root.addView(editHint);

        // ---- play ----
        playButton = button("Play", true);
        LinearLayout.LayoutParams playLp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(56));
        playLp.topMargin = dp(18);
        playButton.setLayoutParams(playLp);
        playButton.setOnClickListener(v -> startGame(false));
        root.addView(playButton);

        // ---- the log ----
        // A phone has no console and no cable, so this is where "something went wrong" is answered: one tap sends the
        // newest log (a crash report, if the last run did not finish) to any app on the phone.
        root.addView(sectionTitle("Something went wrong?"));
        logStatus = text("", 14, Color.rgb(170, 176, 190));
        logStatus.setPadding(0, 0, 0, dp(6));
        root.addView(logStatus);

        Button sendLog = button("Send the log\u2026", false);
        sendLog.setOnClickListener(v -> LogSharing.share(this));
        root.addView(sendLog);

        Button copyLog = button("Copy the log", false);
        copyLog.setOnClickListener(v -> LogSharing.copy(this));
        root.addView(copyLog);

        TextView logHint = text("Opens the phone's share sheet with the log attached - a messenger, mail or Drive. " +
                "If the game crashed, the log of the run that crashed is the one sent.", 13, Color.rgb(150, 156, 170));
        root.addView(logHint);

        TextView footer = text("Settings and saves are kept on this phone and survive an update: a new build installs " +
                "over the old one, so there is nothing to import again. No game files are included: the image you import " +
                "stays in the app's private folder and is only read by the game.", 12, Color.rgb(120, 126, 140));
        footer.setPadding(0, dp(18), 0, 0);
        root.addView(footer);

        TextView version = text("Version " + LogSharing.version(this), 12, Color.rgb(110, 116, 130));
        version.setPadding(0, dp(8), 0, 0);
        root.addView(version);
        return scroll;
    }

    private TextView sectionTitle(String s) {
        TextView t = text(s, 18, Color.WHITE);
        t.setPadding(0, dp(22), 0, dp(8));
        return t;
    }

    private Button button(String label, boolean primary) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        if (primary) {
            b.setBackgroundColor(Color.rgb(79, 142, 247));
            b.setTextColor(Color.WHITE);
            b.setTextSize(18);
        }
        return b;
    }

    private void refreshDiscStatus() {
        // The editor draws over the running game (the only place a control can be judged against the picture), so it
        // needs the game booted, and the game needs the disc: both buttons wait for the import.
        final boolean have = DiscImport.haveDisc(this);
        if (editTouchButton != null) editTouchButton.setEnabled(have);
        if (have) {
            long mb = DiscImport.discFile(this).length() / (1024 * 1024);
            status.setText("Disc image ready (" + mb + " MB).");
            status.setTextColor(Color.rgb(120, 220, 150));
            playButton.setEnabled(true);
        } else {
            status.setText("No disc image yet. Import your own Shadow the Hedgehog (USA) ISO/GCM to play.");
            status.setTextColor(Color.rgb(240, 190, 120));
            playButton.setEnabled(false);
        }
    }

    // ---------------------------------------------------------------------------------------------------------------
    // importing

    private void pickDisc() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, PICK_DISC);
        } catch (Exception e) {
            Toast.makeText(this, "No file picker on this device.", Toast.LENGTH_LONG).show();
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != PICK_DISC || resultCode != RESULT_OK || data == null || data.getData() == null) return;
        Uri uri = data.getData();
        final String name = DiscImport.displayName(this, uri);
        final AlertDialog progress = progressDialog("Importing " + name + "\u2026");
        final Handler ui = new Handler(Looper.getMainLooper());
        DiscImport.importInto(getApplicationContext(), uri, new DiscImport.Progress() {
            @Override
            public void onProgress(long done, long total) {
                ui.post(() -> progress.setMessage(fmtMb(done, total)));
            }

            @Override
            public void onDone(boolean ok, String message, String path) {
                ui.post(() -> {
                    progress.dismiss();
                    if (!ok) {
                        new AlertDialog.Builder(LauncherActivity.this)
                                .setTitle("Import failed")
                                .setMessage(message)
                                .setPositiveButton("OK", null)
                                .show();
                        return;
                    }
                    refreshDiscStatus();
                    Toast.makeText(LauncherActivity.this, "Disc image imported", Toast.LENGTH_SHORT).show();
                });
            }
        });
        progress.show();
    }

    private String fmtMb(long done, long total) {
        String d = (done / (1024 * 1024)) + " MB";
        if (total <= 0) return d;
        int pct = (int) (done * 100 / total);
        return pct + " %  \u2022  " + d + " of " + (total / (1024 * 1024)) + " MB";
    }

    private AlertDialog progressDialog(String message) {
        AlertDialog.Builder b = new AlertDialog.Builder(this)
                .setMessage(message)
                .setCancelable(false);
        AlertDialog d = b.create();
        d.setCanceledOnTouchOutside(false);
        return d;
    }

    // ---------------------------------------------------------------------------------------------------------------
    // starting the game

    private String preset() {
        for (int i = 0; i < presetGroup.getChildCount(); i++) {
            RadioButton rb = (RadioButton) presetGroup.getChildAt(i);
            if (rb.isChecked()) return (String) rb.getTag();
        }
        return "auto";
    }

    private void startGame(boolean editTouch) {
        if (!DiscImport.haveDisc(this)) {
            Toast.makeText(this, "Import your disc image first.", Toast.LENGTH_SHORT).show();
            return;
        }
        int scale = renderScale.getProgress();
        boolean touch = touchToggle.isChecked();

        prefs.edit()
                .putString("preset", preset())
                .putInt("render_scale", scale)
                .putBoolean("touch_controls", touch)
                .apply();

        StringBuilder args = new StringBuilder();
        args.append("--data-dir\u0001").append(getFilesDir().getAbsolutePath());
        if (DiscImport.haveDisc(this)) {
            args.append("\u0001--disc\u0001").append(DiscImport.discFile(this).getAbsolutePath());
        }
        args.append("\u0001--perf\u0001").append(preset());
        args.append("\u0001--render-scale\u0001").append(scale);
        args.append("\u0001--touch\u0001").append(touch ? "1" : "0");
        args.append("\u0001--device-info\u0001").append(DeviceInfo.report(this));
        if (editTouch) args.append("\u0001--edit-touch");

        ShadowActivity.setArgs(args.toString());
        Intent i = new Intent(this, ShadowActivity.class);
        i.putExtra(ShadowActivity.EXTRA_ARGS, args.toString());
        startActivity(i);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshDiscStatus();
        refreshLogStatus();
    }

    private void refreshLogStatus() {
        if (logStatus == null) return;
        final boolean crashed = LogSharing.latestCrash(this) != null;
        logStatus.setText(LogSharing.summary(this));
        logStatus.setTextColor(crashed ? Color.rgb(240, 190, 120) : Color.rgb(170, 176, 190));
    }
}
