package org.shadowrecomp.android;

import android.app.Activity;
import android.content.ClipData;
import android.content.Intent;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.view.Gravity;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.AtomicMoveNotSupportedException;
import java.nio.file.Files;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.Set;

/**
 * Small Android front-end for the game's mod loader (ShadowRecomp/mod/mod_loader.cpp).
 *
 * The loader reads the same files SA Mod Manager writes on a PC:
 *
 * <pre>
 *   mods/&lt;folder&gt;/mod.ini                         one mod per folder
 *   mods/.modloader/profiles/profiles.json        which profile is active
 *   mods/.modloader/profiles/&lt;profile&gt;.json       EnabledMods (load order) and EnabledCodes
 * </pre>
 *
 * The manager never interprets or copies mod payloads. It lists the mod folders, lets
 * the user enable them and arrange their order, and rewrites only the two lists of the
 * active profile, preserving everything else SA Mod Manager stored in it.
 */
public final class ModManagerActivity extends Activity {
    private static final String PROFILES_DIRECTORY = "mods/.modloader/profiles";
    private static final String PROFILE_LIST = "profiles.json";
    private static final String DEFAULT_PROFILE = "Default.json";
    private static final int MAX_MODS = 1000;
    private static final long MAX_TEXT_FILE = 4L * 1024 * 1024;

    // Built-in codes understood by the native loader: the hidden boolean values of the
    // [Codes] section in ShadowRecomp/user/config_def.h. Names must match exactly.
    // Codes found in the profile but absent here are preserved and still toggleable.
    private static final String[][] CODE_DEFS = {
        { "QuickBoot", "Quick boot (skip intro logos)" },
        { "InfiniteLives", "Infinite lives" },
        { "InfiniteAmmo", "Infinite ammo" },
        { "AllStagesUnlocked", "All stages unlocked" },
        { "EasyLastStoryUnlock", "Easier Last Story unlock" },
        { "FillChaosControlGaugeOnInput", "Fill Chaos Control gauge on input" },
        { "FillChaosBlastGaugeOnInput", "Fill Chaos Blast gauge on input" },
        { "VacuumAnyEnemy", "Vacuum any enemy" },
        { "VacuumGivePointsOnFireSuck", "Vacuum gives points on fire suck" },
        { "XboxEarlyTargetBehavior", "Xbox early target behavior" },
        { "HoldLTForceJumpDash", "Hold LT to force jump dash" },
        { "ReloadedMod", "Reloaded mod" },
        { "ReloadedModOriginalExpertRingDropBehavior", "Reloaded: original Expert ring drop behavior" },
        { "ReloadedOriginalElecCodeEnemySpeed", "Reloaded: original enemy speed" },
        { "ExtendedMultiplayer", "Extended multiplayer" },
        { "PlayAsBattleCharacters", "Play as battle characters" },
        { "ShadowAndroidsNoEmbeddedWeapons", "Shadow Androids without embedded weapons" },
        { "SelectModeExpertStages", "Select Mode: Expert stages" },
        { "SelectModeEnableLastStoryDevilDoomSlots", "Select Mode: Last Story and Devil Doom slots" },
        { "SkipLoadingAnyEvents", "Skip loading any events" },
        { "SingleFrameInvulnRecovery", "Single-frame invulnerability recovery" },
        { "AllPartnersCanHomingAttack", "All partners can homing attack" },
        { "UseOfficialTitleOnTitleBar", "Use the official title" },
    };

    private final List<ModEntry> mods = new ArrayList<>();
    private final LinkedHashSet<String> enabledCodes = new LinkedHashSet<>();
    private LinearLayout modList;
    private TextView status;

    private static final class ModEntry {
        final String folder;
        final String title;
        final String details;
        boolean enabled;

        ModEntry(String folder, String title, String details) {
            this.folder = folder;
            this.title = title;
            this.details = details;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(16), dp(16), dp(16), dp(12));

        TextView title = new TextView(this);
        title.setText(R.string.mod_manager_title);
        title.setTextSize(24);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        page.addView(title, matchWrap());

        TextView hint = new TextView(this);
        hint.setText(R.string.mod_manager_hint);
        hint.setPadding(0, dp(8), 0, dp(10));
        page.addView(hint, matchWrap());

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);

        Button openFiles = new Button(this);
        openFiles.setText(R.string.mod_manager_open_files);
        openFiles.setOnClickListener(view -> openGameFiles());
        actions.addView(openFiles, weightedWrap());

        Button refresh = new Button(this);
        refresh.setText(R.string.mod_manager_refresh);
        refresh.setOnClickListener(view -> reload());
        actions.addView(refresh, weightedWrap());
        page.addView(actions, matchWrap());

        ScrollView scroll = new ScrollView(this);
        modList = new LinearLayout(this);
        modList.setOrientation(LinearLayout.VERTICAL);
        scroll.addView(modList, matchWrap());
        page.addView(scroll, new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1.0f));

        status = new TextView(this);
        status.setPadding(0, dp(8), 0, dp(4));
        page.addView(status, matchWrap());

        Button save = new Button(this);
        save.setText(R.string.mod_manager_save);
        save.setOnClickListener(view -> saveSelection());
        page.addView(save, matchWrap());

        setContentView(page);
        ImmersiveWindow.apply(this);
        reload();
    }

    // ------------------------------------------------------------------
    // Loading
    // ------------------------------------------------------------------

    private File gameRoot() {
        return AppStorage.activeGameRoot(this);
    }

    private File profilesDirectory() {
        return new File(gameRoot(), PROFILES_DIRECTORY);
    }

    /** The profile the native loader will read: the one profiles.json points at, else Default.json. */
    private File activeProfileFile() {
        File directory = profilesDirectory();
        JSONObject list = readJson(new File(directory, PROFILE_LIST));
        if (list != null) {
            JSONArray profiles = list.optJSONArray("ProfilesList");
            int index = list.optInt("ProfileIndex", 0);
            if (profiles != null && index >= 0 && index < profiles.length()) {
                JSONObject profile = profiles.optJSONObject(index);
                String fileName = profile != null ? profile.optString("Filename", "") : "";
                if (isPlainFileName(fileName)) return new File(directory, fileName);
            }
        }
        return new File(directory, DEFAULT_PROFILE);
    }

    private static boolean isPlainFileName(String name) {
        return !name.isEmpty() && name.indexOf('/') < 0 && name.indexOf('\\') < 0 && !name.equals("..") && !name.equals(".");
    }

    private void reload() {
        mods.clear();
        enabledCodes.clear();

        Map<String, ModEntry> discovered = new LinkedHashMap<>();
        File[] folders = new File(gameRoot(), "mods").listFiles(File::isDirectory);
        if (folders != null) {
            Arrays.sort(folders, (left, right) -> left.getName().compareToIgnoreCase(right.getName()));
            for (File folder : folders) {
                if (discovered.size() >= MAX_MODS) break;
                if (folder.getName().startsWith(".")) continue;
                File ini = new File(folder, "mod.ini");
                if (!ini.isFile()) continue;
                Map<String, String> values = readModIni(ini);
                String name = values.get("name");
                String title = name != null && !name.isEmpty() ? name : folder.getName();
                StringBuilder details = new StringBuilder(folder.getName());
                String version = values.get("version");
                String author = values.get("author");
                if (version != null && !version.isEmpty()) details.append("  ·  v").append(version);
                if (author != null && !author.isEmpty()) details.append("  ·  ").append(author);
                discovered.put(folder.getName(), new ModEntry(folder.getName(), title, details.toString()));
            }
        }

        // Enabled mods come first, in the profile's load order; the rest follow by name.
        JSONObject profile = readJson(activeProfileFile());
        if (profile != null) {
            JSONArray enabledMods = profile.optJSONArray("EnabledMods");
            if (enabledMods != null) {
                for (int index = 0; index < enabledMods.length(); index++) {
                    ModEntry entry = discovered.remove(enabledMods.optString(index, ""));
                    if (entry != null) {
                        entry.enabled = true;
                        mods.add(entry);
                    }
                }
            }
            JSONArray codes = profile.optJSONArray("EnabledCodes");
            if (codes != null) {
                for (int index = 0; index < codes.length(); index++) {
                    String code = codes.optString(index, "");
                    if (!code.isEmpty()) enabledCodes.add(code);
                }
            }
        }
        mods.addAll(discovered.values());

        status.setText(getString(R.string.mod_manager_found, mods.size()));
        rebuildRows();
    }

    /** mod.ini keys before the first section, lower-cased; values are trimmed. */
    private static Map<String, String> readModIni(File file) {
        Map<String, String> values = new LinkedHashMap<>();
        if (file.length() > MAX_TEXT_FILE) return values;
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(
                new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                line = line.trim();
                if (line.startsWith("\uFEFF")) line = line.substring(1).trim();
                if (line.isEmpty() || line.startsWith(";") || line.startsWith("#")) continue;
                if (line.startsWith("[")) {
                    if (line.toLowerCase(Locale.ROOT).startsWith("[main]") || line.equals("[]")) continue;
                    break;
                }
                int separator = line.indexOf('=');
                if (separator <= 0) continue;
                String key = line.substring(0, separator).trim().toLowerCase(Locale.ROOT);
                String value = line.substring(separator + 1).trim();
                if (value.length() >= 2 && value.startsWith("\"") && value.endsWith("\"")) {
                    value = value.substring(1, value.length() - 1);
                }
                if (!values.containsKey(key)) values.put(key, value);
            }
        } catch (IOException ignored) {
            // An unreadable mod.ini still lists the mod under its folder name.
        }
        return values;
    }

    private static JSONObject readJson(File file) {
        if (!file.isFile() || file.length() > MAX_TEXT_FILE) return null;
        try {
            String text = new String(Files.readAllBytes(file.toPath()), StandardCharsets.UTF_8);
            if (text.startsWith("\uFEFF")) text = text.substring(1);
            return new JSONObject(text);
        } catch (IOException | JSONException exception) {
            return null;
        }
    }

    // ------------------------------------------------------------------
    // UI
    // ------------------------------------------------------------------

    private void rebuildRows() {
        modList.removeAllViews();
        if (mods.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText(R.string.mod_manager_empty);
            empty.setPadding(0, dp(24), 0, dp(24));
            empty.setGravity(Gravity.CENTER);
            modList.addView(empty, matchWrap());
            addCodesSection();
            return;
        }

        for (int index = 0; index < mods.size(); index++) {
            final ModEntry entry = mods.get(index);
            LinearLayout row = new LinearLayout(this);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setGravity(Gravity.CENTER_VERTICAL);

            CheckBox enabled = new CheckBox(this);
            enabled.setText(entry.title + "\n" + entry.details);
            enabled.setChecked(entry.enabled);
            enabled.setOnCheckedChangeListener((button, checked) -> entry.enabled = checked);
            row.addView(enabled, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f));

            final int rowIndex = index;
            Button up = new Button(this);
            up.setText("↑");
            up.setEnabled(index > 0);
            up.setOnClickListener(view -> move(rowIndex, -1));
            row.addView(up, squareButton());

            Button down = new Button(this);
            down.setText("↓");
            down.setEnabled(index + 1 < mods.size());
            down.setOnClickListener(view -> move(rowIndex, 1));
            row.addView(down, squareButton());

            modList.addView(row, matchWrap());
        }

        addCodesSection();
    }

    private void addCodesSection() {
        TextView header = new TextView(this);
        header.setText(R.string.mod_manager_codes);
        header.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        header.setPadding(0, dp(18), 0, dp(2));
        modList.addView(header, matchWrap());

        TextView hint = new TextView(this);
        hint.setText(R.string.mod_manager_codes_hint);
        hint.setPadding(0, 0, 0, dp(6));
        modList.addView(hint, matchWrap());

        Set<String> known = new HashSet<>();
        for (String[] def : CODE_DEFS) {
            known.add(def[0]);
            addCodeRow(def[0], def[1], enabledCodes.contains(def[0]));
        }
        for (String name : new ArrayList<>(enabledCodes)) {
            if (!known.contains(name)) addCodeRow(name, name, true);
        }
    }

    private void addCodeRow(final String name, String label, boolean checked) {
        CheckBox box = new CheckBox(this);
        box.setText(label);
        box.setChecked(checked);
        box.setOnCheckedChangeListener((button, isChecked) -> {
            if (isChecked) {
                enabledCodes.add(name);
            } else {
                enabledCodes.remove(name);
            }
        });
        modList.addView(box, matchWrap());
    }

    private void move(int index, int offset) {
        int target = index + offset;
        if (target < 0 || target >= mods.size()) {
            return;
        }
        Collections.swap(mods, index, target);
        rebuildRows();
    }

    // ------------------------------------------------------------------
    // Saving
    // ------------------------------------------------------------------

    private void saveSelection() {
        File directory = profilesDirectory();
        if (!directory.isDirectory() && !directory.mkdirs()) {
            showError(getString(R.string.mod_manager_error_directory, directory));
            return;
        }

        try {
            File profileFile = activeProfileFile();
            JSONObject profile = readJson(profileFile);
            if (profile == null) profile = new JSONObject();

            JSONArray enabledMods = new JSONArray();
            int enabledCount = 0;
            for (ModEntry entry : mods) {
                if (entry.enabled) {
                    enabledMods.put(entry.folder);
                    enabledCount++;
                }
            }
            JSONArray codes = new JSONArray();
            for (String code : enabledCodes) codes.put(code);
            profile.put("EnabledMods", enabledMods);
            profile.put("EnabledCodes", codes);
            writeAtomic(profileFile, profile.toString(2));

            String message = getString(R.string.mod_manager_saved, enabledCount);
            status.setText(message + "\n" + getString(R.string.mod_manager_restart));
            Toast.makeText(this, message, Toast.LENGTH_SHORT).show();
        } catch (IOException | JSONException exception) {
            showError(getString(R.string.mod_manager_error_save, FileErrorMessages.describe(this, exception)));
        }
    }

    private static void writeAtomic(File target, String contents) throws IOException {
        File temporary = new File(target.getParentFile(), target.getName() + ".tmp");
        try (FileOutputStream output = new FileOutputStream(temporary);
             OutputStreamWriter writer = new OutputStreamWriter(output, StandardCharsets.UTF_8)) {
            writer.write(contents);
            writer.flush();
            output.getFD().sync();
        }

        try {
            Files.move(temporary.toPath(), target.toPath(),
                StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING);
        } catch (AtomicMoveNotSupportedException exception) {
            Files.move(temporary.toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING);
        } finally {
            Files.deleteIfExists(temporary.toPath());
        }
    }

    private void openGameFiles() {
        try {
            Uri directory = DocumentsContract.buildDocumentUri(
                getPackageName() + ".documents", "game:");
            // VIEW opens the full Files activity (copy/rename/delete), unlike
            // OPEN_DOCUMENT_TREE which is only a destination picker and is redirected
            // to device storage by several Android 16 DocumentsUI builds.
            Intent intent = new Intent(Intent.ACTION_VIEW);
            intent.setDataAndType(directory, "vnd.android.document/directory");
            intent.setClipData(ClipData.newRawUri("Shadow Recompiled files", directory));
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                Intent.FLAG_ACTIVITY_NEW_DOCUMENT);
            startActivity(intent);
        } catch (Exception exception) {
            showError(getString(R.string.mod_manager_error_files));
        }
    }

    private void showError(String message) {
        status.setText(message);
        Toast.makeText(this, message, Toast.LENGTH_LONG).show();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private static LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    private static LinearLayout.LayoutParams weightedWrap() {
        return new LinearLayout.LayoutParams(
            0, LinearLayout.LayoutParams.WRAP_CONTENT, 1.0f);
    }

    private LinearLayout.LayoutParams squareButton() {
        return new LinearLayout.LayoutParams(dp(52), LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) ImmersiveWindow.apply(this);
    }
}
