package com.tj.skyrim;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.hardware.display.DisplayManager;
import android.hardware.input.InputManager;
import android.view.InputDevice;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.app.AlertDialog;
import java.io.File;
import java.io.FileWriter;
import java.util.Date;
import java.util.List;
import org.libsdl.app.SDLActivity;

public class SkyrimActivity extends SDLActivity implements InputManager.InputDeviceListener {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "dxvk_d3d9", "snap_frame_engine", "snap_ai_engine", "snap_motion_v2", "main" };
    }

    /* argv[1] = game folder, argv[2] = save folder, from the launcher. */
    @Override
    protected String[] getArguments() {
        String game = getIntent().getStringExtra("game");
        return game == null ? new String[0] : new String[] { game };
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        saveLastExit();
        super.onCreate(savedInstanceState);
        addGamepadOverlay();
        inputManager = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (inputManager != null) inputManager.registerInputDeviceListener(this, new Handler(Looper.getMainLooper()));
        refreshPhysicalGamepadState();
        addOverlayMenuButton();
        if (BuildConfig.DUAL) startHudScreen();
    }

    @Override
    protected void onDestroy() {
        if (inputManager != null) inputManager.unregisterInputDeviceListener(this);
        if (hudScreen != null) { hudScreen.dismiss(); hudScreen = null; }
        if (!mBrokenLibraries) nativeSetVirtualPad(0, 0, 0, 0, 0, 0, 0);
        super.onDestroy();
    }

    // ---- In-game SnapFrame controls ---------------------------------------------
    private static native int[] nativeGetSnapFrameSettings();
    private static native boolean nativeSetSnapFrameSettings(int enabled, int flowPercent, int presentMode);

    private int dp(float value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static native void nativeSetTouchEditor(boolean enabled);
    private native void nativeSetLegacyTouchEnabled(boolean enabled);
    private native void nativeSetVirtualPad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt);
    private native void nativeQuickAction(boolean load);
    private GamepadOverlayView gamepadOverlay;
    private boolean gamepadOverlayEnabled = true;
    private boolean physicalGamepadConnected = false;
    private boolean forceOverlayVisible = false;
    private InputManager inputManager;
    private final java.util.Set<String> gamepadDisabledControls = new java.util.HashSet<>();
    private final java.util.Set<String> gamepadToggleControls = new java.util.HashSet<>();

    private void addGamepadOverlay() {
        if (mLayout == null) return;
        // The old D3D9 touch renderer remains compiled for rollback, but is
        // switched off so only this Android/Kotlin overlay is visible.
        nativeSetLegacyTouchEnabled(false);
        gamepadOverlay = new GamepadOverlayView(this, (buttons, lx, ly, rx, ry, lt, rt) -> {
                    nativeSetVirtualPad(buttons, lx, ly, rx, ry, lt, rt);
                    return kotlin.Unit.INSTANCE;
                }, action -> {
                    nativeQuickAction("QUICK LOAD".equals(action));
                    return kotlin.Unit.INSTANCE;
                });
        gamepadOverlay.setContentDescription("Virtual Xbox gamepad controls");
        gamepadOverlay.setLayerType(android.view.View.LAYER_TYPE_HARDWARE, null);
        android.widget.RelativeLayout.LayoutParams p = new android.widget.RelativeLayout.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT);
        mLayout.addView(gamepadOverlay, p);
        android.content.SharedPreferences prefs = getSharedPreferences("gamepad_overlay", MODE_PRIVATE);
        gamepadOverlayEnabled = prefs.getBoolean("overlay_enabled", true);
        applyGamepadProfile(prefs, prefs.getString("active_profile", "Default"));
        gamepadOverlay.bringToFront();
        gamepadOverlay.setVisibility(gamepadOverlayEnabled ? android.view.View.VISIBLE : android.view.View.GONE);
    }

    private void setGamepadOverlayEnabled(boolean enabled) {
        gamepadOverlayEnabled = enabled;
        forceOverlayVisible = enabled;
        getSharedPreferences("gamepad_overlay", MODE_PRIVATE).edit().putBoolean("overlay_enabled", enabled).apply();
        if (gamepadOverlay != null) {
            gamepadOverlay.setVisibility(enabled && (!physicalGamepadConnected || forceOverlayVisible) ? android.view.View.VISIBLE : android.view.View.GONE);
        }
        if (!enabled && !mBrokenLibraries) nativeSetVirtualPad(0, 0, 0, 0, 0, 0, 0);
        android.widget.Toast.makeText(this,
                enabled ? "Gamepad overlay enabled" : "Gamepad overlay disabled",
                android.widget.Toast.LENGTH_SHORT).show();
    }

    private boolean hasPhysicalGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice device = InputDevice.getDevice(id);
            if (device != null && (device.getSources() & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD) return true;
        }
        return false;
    }

    private void refreshPhysicalGamepadState() {
        physicalGamepadConnected = hasPhysicalGamepad();
        if (gamepadOverlay != null) {
            boolean visible = gamepadOverlayEnabled && (!physicalGamepadConnected || forceOverlayVisible);
            gamepadOverlay.setVisibility(visible ? android.view.View.VISIBLE : android.view.View.GONE);
        }
    }

    @Override public void onInputDeviceAdded(int deviceId) { refreshPhysicalGamepadState(); }
    @Override public void onInputDeviceRemoved(int deviceId) { refreshPhysicalGamepadState(); }
    @Override public void onInputDeviceChanged(int deviceId) { refreshPhysicalGamepadState(); }

    private boolean touchEditorEnabled;

    private void addOverlayMenuButton() {
        if (mLayout == null) return;
        Button button = new Button(this);
        button.setText("☰");
        button.setTextSize(12);
        button.setAllCaps(false);
        button.setTextColor(Color.WHITE);
        button.setPadding(dp(10), 0, dp(10), 0);
        GradientDrawable background = new GradientDrawable();
        background.setColor(Color.argb(220, 24, 31, 43));
        background.setCornerRadius(dp(10));
        background.setStroke(dp(1), Color.argb(220, 100, 180, 255));
        button.setBackground(background);
        button.setElevation(dp(8));
        android.widget.RelativeLayout.LayoutParams params =
                new android.widget.RelativeLayout.LayoutParams(dp(48), dp(42));
        params.addRule(android.widget.RelativeLayout.ALIGN_PARENT_TOP);
        params.addRule(android.widget.RelativeLayout.ALIGN_PARENT_LEFT);
        params.setMargins(dp(12), dp(12), 0, 0);
        mLayout.addView(button, params);
        button.setContentDescription("In-game menu");
        button.setOnClickListener(v -> showOverlayMenu());
    }

    private void showOverlayMenu() {
        String[] items = { "Snap AI / Frame Generation", "Android keyboard", "Gamepad overlay: " + (gamepadOverlayEnabled ? "ON" : "OFF"), "Gamepad editor", "Legacy touch controls editor" };
        new AlertDialog.Builder(this)
                .setTitle("Skyrim overlay")
                .setItems(items, (dialog, which) -> {
                    if (which == 0) showSnapFrameSettings();
                    else if (which == 1) {
                        // SDLActivity routes this request to Android's InputMethodManager.
                        showTextInput(0, 0, 1, 1);
                    } else if (which == 2) {
                        setGamepadOverlayEnabled(!gamepadOverlayEnabled);
                    } else if (which == 3) {
                        showGamepadControlsEditor();
                    } else {
                        touchEditorEnabled = !touchEditorEnabled;
                        nativeSetTouchEditor(touchEditorEnabled);
                        android.widget.Toast.makeText(this,
                                touchEditorEnabled ? "Touch editor enabled" : "Touch editor disabled",
                                android.widget.Toast.LENGTH_SHORT).show();
                    }
                }).show();
    }

    private int editorColor(String value) { return Color.parseColor(value); }

    private android.graphics.drawable.Drawable editorBackground(String color, int radius, String stroke) {
        GradientDrawable d = new GradientDrawable();
        d.setColor(editorColor(color));
        d.setCornerRadius(dp(radius));
        if (stroke != null) d.setStroke(dp(1), editorColor(stroke));
        return d;
    }

    private TextView editorText(String value, int size, boolean bold) {
        TextView t = new TextView(this);
        t.setText(value); t.setTextSize(size); t.setTextColor(Color.rgb(220, 235, 235));
        if (bold) t.setTypeface(null, android.graphics.Typeface.BOLD);
        return t;
    }

    private void showGamepadControlsEditor() {
        if (gamepadOverlay == null) return;
        android.content.SharedPreferences prefs = getSharedPreferences("gamepad_overlay", MODE_PRIVATE);
        java.util.Set<String> stored = prefs.getStringSet("profiles", java.util.Collections.emptySet());
        java.util.TreeSet<String> profiles = new java.util.TreeSet<>(stored == null
                ? java.util.Collections.emptySet() : new java.util.HashSet<>(stored));
        profiles.add("Default");
        String active = prefs.getString("active_profile", "Default");
        if (!profiles.contains(active)) active = "Default";

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(18), dp(14), dp(18), dp(12));
        root.setBackgroundColor(Color.rgb(18, 25, 31));

        TextView heading = editorText("CONTROLLER CONFIGURATION", 17, true);
        heading.setTextColor(Color.rgb(53, 185, 182));
        root.addView(heading);
        TextView hint = editorText("Select a profile, configure buttons and arrange the on-screen layout.", 12, false);
        hint.setPadding(0, dp(5), 0, dp(10)); root.addView(hint);

        LinearLayout profileRow = new LinearLayout(this);
        profileRow.setGravity(android.view.Gravity.CENTER_VERTICAL);
        TextView profileLabel = editorText("PROFILE", 12, true);
        profileRow.addView(profileLabel, new LinearLayout.LayoutParams(dp(76), dp(48)));
        Spinner profileSpinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item,
                profiles.toArray(new String[0]));
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        profileSpinner.setAdapter(adapter);
        profileSpinner.setSelection(Math.max(0, java.util.Arrays.asList(profiles.toArray(new String[0])).indexOf(active)));
        profileRow.addView(profileSpinner, new LinearLayout.LayoutParams(0, dp(48), 1));
        Button addProfile = new Button(this); addProfile.setText("+");
        profileRow.addView(addProfile, new LinearLayout.LayoutParams(dp(52), dp(48)));
        root.addView(profileRow);

        LinearLayout actions = new LinearLayout(this);
        actions.setGravity(android.view.Gravity.CENTER_VERTICAL);
        Button arrange = new Button(this);
        arrange.setText(gamepadOverlay.isLayoutEditMode() ? "Finish layout" : "Arrange");
        Button appearance = new Button(this); appearance.setText("Size / shape");
        Button close = new Button(this); close.setText("Done");
        actions.addView(arrange, new LinearLayout.LayoutParams(0, dp(48), 1));
        actions.addView(appearance, new LinearLayout.LayoutParams(0, dp(48), 1));
        actions.addView(close, new LinearLayout.LayoutParams(0, dp(48), 1));
        root.addView(actions);

        TextView section = editorText("BUTTON MAPPING", 13, true);
        section.setTextColor(Color.rgb(53, 185, 182));
        section.setPadding(0, dp(14), 0, dp(6)); root.addView(section);

        android.widget.ScrollView scroll = new android.widget.ScrollView(this);
        LinearLayout list = new LinearLayout(this); list.setOrientation(LinearLayout.VERTICAL);
        for (String name : gamepadOverlay.controlNames()) {
            LinearLayout row = new LinearLayout(this);
            row.setGravity(android.view.Gravity.CENTER_VERTICAL);
            row.setPadding(dp(8), dp(4), dp(8), dp(4));
            row.setBackground(editorBackground("#202c35", 9, "#28565a"));
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                    android.view.ViewGroup.LayoutParams.MATCH_PARENT, dp(54));
            rowLp.bottomMargin = dp(5);

            TextView label = editorText(name, 14, true);
            row.addView(label, new LinearLayout.LayoutParams(0, dp(42), 1));
            android.widget.Switch enabled = new android.widget.Switch(this);
            enabled.setText("ON"); enabled.setTextSize(11);
            enabled.setChecked(!gamepadDisabledControls.contains(name));
            row.addView(enabled);
            android.widget.Switch toggle = new android.widget.Switch(this);
            toggle.setText("TOGGLE"); toggle.setTextSize(10);
            toggle.setChecked(gamepadToggleControls.contains(name));
            toggle.setEnabled(enabled.isChecked() && !name.startsWith("QUICK "));
            row.addView(toggle);
            enabled.setOnCheckedChangeListener((button, checked) -> {
                if (checked) gamepadDisabledControls.remove(name);
                else { gamepadDisabledControls.add(name); gamepadToggleControls.remove(name); toggle.setChecked(false); }
                toggle.setEnabled(checked && !name.startsWith("QUICK "));
                saveGamepadControlSettings(prefs);
                gamepadOverlay.configureControl(name, checked, checked && toggle.isChecked());
            });
            toggle.setOnCheckedChangeListener((button, checked) -> {
                if (checked) gamepadToggleControls.add(name); else gamepadToggleControls.remove(name);
                saveGamepadControlSettings(prefs);
                gamepadOverlay.configureControl(name, enabled.isChecked(), checked);
            });
            list.addView(row, rowLp);
        }
        scroll.addView(list);
        root.addView(scroll, new LinearLayout.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));

        AlertDialog dialog = new AlertDialog.Builder(this).setView(root).create();
        profileSpinner.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            boolean first = true;
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, android.view.View view, int position, long id) {
                if (first) { first = false; return; }
                String target = (String) profileSpinner.getItemAtPosition(position);
                if (!target.equals(prefs.getString("active_profile", "Default"))) {
                    if (!prefs.edit().putString("active_profile", target).commit()) {
                        android.widget.Toast.makeText(SkyrimActivity.this, "Could not activate profile", android.widget.Toast.LENGTH_LONG).show();
                        return;
                    }
                    applyGamepadProfile(prefs, target);
                    gamepadOverlay.reloadProfile();
                    dialog.dismiss();
                    showGamepadControlsEditor();
                }
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        addProfile.setOnClickListener(v -> { dialog.dismiss(); showCreateGamepadProfile(prefs); });
        arrange.setOnClickListener(v -> {
            boolean editing = !gamepadOverlay.isLayoutEditMode();
            gamepadOverlay.setLayoutEditMode(editing);
            if (editing) {
                dialog.dismiss();
                android.widget.Toast.makeText(this, "Drag controls on the game screen, then reopen Gamepad editor to finish", android.widget.Toast.LENGTH_LONG).show();
            } else {
                arrange.setText("Arrange controls");
                android.widget.Toast.makeText(this, "Layout saved", android.widget.Toast.LENGTH_SHORT).show();
            }
        });
        appearance.setOnClickListener(v -> showGamepadAppearanceEditor());
        close.setOnClickListener(v -> dialog.dismiss());
        dialog.show();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(editorBackground("#12191f", 14, "#238b8d"));
            dialog.getWindow().setLayout(Math.min(getResources().getDisplayMetrics().widthPixels - dp(24), dp(560)),
                    Math.min(getResources().getDisplayMetrics().heightPixels - dp(48), dp(680)));
        }
    }

    private void showGamepadAppearanceEditor() {
        if (gamepadOverlay == null) return;
        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(18), dp(12), dp(18), dp(8));

        TextView controlLabel = editorText("CONTROL", 12, true);
        panel.addView(controlLabel);
        Spinner control = new Spinner(this);
        String[] names = gamepadOverlay.controlNames().toArray(new String[0]);
        control.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, names));
        panel.addView(control);

        TextView sizeLabel = editorText("SIZE: 100%", 13, true);
        sizeLabel.setPadding(0, dp(18), 0, 0);
        panel.addView(sizeLabel);
        SeekBar size = new SeekBar(this);
        size.setMax(120); // 60% .. 180%, in one-percent steps
        panel.addView(size);

        TextView shapeLabel = editorText("BUTTON SHAPE", 12, true);
        shapeLabel.setPadding(0, dp(14), 0, dp(4));
        panel.addView(shapeLabel);
        String[] shapeNames = { "Circle", "Square", "Rectangle — sharp corners", "Rectangle — rounded corners" };
        String[] shapeValues = { "circle", "square", "rect", "rounded_rect" };
        Spinner shape = new Spinner(this);
        shape.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, shapeNames));
        panel.addView(shape);

        final boolean[] updating = { false };
        Runnable loadSelected = () -> {
            String name = (String) control.getSelectedItem();
            if (name == null) return;
            updating[0] = true;
            float currentSize = gamepadOverlay.controlSize(name);
            int progress = Math.max(0, Math.min(120, Math.round((currentSize - 0.6f) * 100f)));
            size.setProgress(progress);
            sizeLabel.setText("SIZE: " + Math.round((0.6f + progress / 100f) * 100f) + "%");
            String currentShape = gamepadOverlay.controlShape(name);
            int selected = 0;
            for (int i = 0; i < shapeValues.length; i++) if (shapeValues[i].equals(currentShape)) selected = i;
            shape.setSelection(selected);
            updating[0] = false;
        };
        control.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, android.view.View view, int position, long id) {
                loadSelected.run();
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        size.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                float value = 0.6f + progress / 100f;
                sizeLabel.setText("SIZE: " + Math.round(value * 100f) + "%");
                if (!updating[0]) {
                    String name = (String) control.getSelectedItem();
                    if (name != null) gamepadOverlay.configureAppearance(name, value, shapeValues[shape.getSelectedItemPosition()]);
                }
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        shape.setOnItemSelectedListener(new android.widget.AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(android.widget.AdapterView<?> parent, android.view.View view, int position, long id) {
                if (!updating[0]) {
                    String name = (String) control.getSelectedItem();
                    if (name != null) gamepadOverlay.configureAppearance(name, 0.6f + size.getProgress() / 100f, shapeValues[position]);
                }
            }
            @Override public void onNothingSelected(android.widget.AdapterView<?> parent) {}
        });
        loadSelected.run();
        new AlertDialog.Builder(this).setTitle("Control appearance").setView(panel)
                .setPositiveButton("Done", null).show();
    }

    private void saveGamepadControlSettings(android.content.SharedPreferences prefs) {
        String profile = prefs.getString("active_profile", "Default");
        prefs.edit()
                .putStringSet(profile + "_disabled_controls", new java.util.HashSet<>(gamepadDisabledControls))
                .putStringSet(profile + "_toggle_controls", new java.util.HashSet<>(gamepadToggleControls))
                .commit();
    }

    private void showCreateGamepadProfile(android.content.SharedPreferences prefs) {
        android.widget.EditText field = new android.widget.EditText(this);
        field.setSingleLine(true); field.setHint("Profile name");
        field.setInputType(android.text.InputType.TYPE_CLASS_TEXT | android.text.InputType.TYPE_TEXT_FLAG_CAP_SENTENCES);
        new AlertDialog.Builder(this).setTitle("Create controller profile").setView(field)
                .setPositiveButton("Create", (d, w) -> {
                    String name = field.getText().toString().trim().replaceAll("[^A-Za-z0-9 _-]", "");
                    java.util.Set<String> current = prefs.getStringSet("profiles", java.util.Collections.emptySet());
                    java.util.HashSet<String> profiles = new java.util.HashSet<>(current == null
                            ? java.util.Collections.emptySet() : current);
                    if (name.isEmpty() || name.length() > 32 || name.equalsIgnoreCase("Default")
                            || profiles.contains(name)) {
                        android.widget.Toast.makeText(this, "Invalid or existing profile name", android.widget.Toast.LENGTH_SHORT).show();
                        showGamepadControlsEditor(); return;
                    }
                    String source = prefs.getString("active_profile", "Default");
                    java.util.HashMap<String, ?> all = new java.util.HashMap<>(prefs.getAll());
                    android.content.SharedPreferences.Editor edit = prefs.edit();
                    for (java.util.Map.Entry<String, ?> entry : all.entrySet()) {
                        String key = entry.getKey();
                        if (key.startsWith(source + "_x_") || key.startsWith(source + "_y_")
                                || key.startsWith(source + "_size_") || key.startsWith(source + "_shape_")) {
                            if (entry.getValue() instanceof Float)
                                edit.putFloat(name + key.substring(source.length()), (Float) entry.getValue());
                            else if (entry.getValue() instanceof String)
                                edit.putString(name + key.substring(source.length()), (String) entry.getValue());
                        }
                    }
                    java.util.Set<String> disabled = prefs.getStringSet(source + "_disabled_controls",
                            prefs.getStringSet("disabled_controls", java.util.Collections.emptySet()));
                    java.util.Set<String> toggles = prefs.getStringSet(source + "_toggle_controls",
                            prefs.getStringSet("toggle_controls", java.util.Collections.emptySet()));
                    profiles.add(name);
                    boolean saved = edit.putStringSet(name + "_disabled_controls", new java.util.HashSet<>(
                                    disabled == null ? java.util.Collections.emptySet() : disabled))
                            .putStringSet(name + "_toggle_controls", new java.util.HashSet<>(
                                    toggles == null ? java.util.Collections.emptySet() : toggles))
                            .putStringSet("profiles", profiles).putString("active_profile", name).commit();
                    if (!saved) {
                        android.widget.Toast.makeText(this, "Could not save profile", android.widget.Toast.LENGTH_LONG).show();
                        showGamepadControlsEditor(); return;
                    }
                    applyGamepadProfile(prefs, name);
                    gamepadOverlay.reloadProfile();
                    showGamepadControlsEditor();
                }).setNegativeButton("Cancel", (d, w) -> showGamepadControlsEditor()).show();
    }

    private void applyGamepadProfile(android.content.SharedPreferences prefs, String profile) {
        gamepadDisabledControls.clear(); gamepadToggleControls.clear();
        java.util.Set<String> disabled = prefs.getStringSet(profile + "_disabled_controls",
                profile.equals("Default") ? prefs.getStringSet("disabled_controls", java.util.Collections.emptySet())
                        : java.util.Collections.emptySet());
        java.util.Set<String> toggles = prefs.getStringSet(profile + "_toggle_controls",
                profile.equals("Default") ? prefs.getStringSet("toggle_controls", java.util.Collections.emptySet())
                        : java.util.Collections.emptySet());
        if (disabled != null) gamepadDisabledControls.addAll(new java.util.HashSet<>(disabled));
        if (toggles != null) gamepadToggleControls.addAll(new java.util.HashSet<>(toggles));
        for (String control : gamepadOverlay.controlNames())
            gamepadOverlay.configureControl(control, !gamepadDisabledControls.contains(control),
                    gamepadToggleControls.contains(control));
    }

    private void showSnapFrameSettings() {
        int[] current = nativeGetSnapFrameSettings();
        if (current == null || current.length < 4 || current[0] == 0) {
            new AlertDialog.Builder(this)
                    .setTitle("Snap AI / Frame Generation")
                    .setMessage("SnapFrame backend is not active. Check port/snapframe.conf and the SnapFrame initialization log.")
                    .setPositiveButton("OK", null)
                    .show();
            return;
        }

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(8), dp(20), dp(4));

        TextView status = new TextView(this);
        status.setText("Frame Generation changes apply at the next safe render callback. Flow Scale changes reset temporal history before rebuilding its GPU resources.");
        status.setTextSize(13);
        panel.addView(status);

        TextView presentModeLabel = new TextView(this);
        presentModeLabel.setText("Vulkan Present Mode");
        presentModeLabel.setTextSize(14);
        presentModeLabel.setPadding(0, dp(16), 0, dp(4));
        panel.addView(presentModeLabel);

        String[] presentModeNames = { "Auto", "FIFO", "MAILBOX", "IMMEDIATE" };
        Spinner presentMode = new Spinner(this);
        presentMode.setAdapter(new ArrayAdapter<>(this,
                android.R.layout.simple_spinner_dropdown_item, presentModeNames));
        presentMode.setSelection(Math.max(0, Math.min(3, current[3])));
        panel.addView(presentMode);

        TextView presentModeHint = new TextView(this);
        presentModeHint.setText("Changing mode safely recreates the shared Vulkan swapchain at the next present boundary. A brief frame-time spike may occur; unsupported MAILBOX / IMMEDIATE falls back to FIFO.");
        presentModeHint.setTextSize(11);
        panel.addView(presentModeHint);

        CheckBox frameGeneration = new CheckBox(this);
        frameGeneration.setText("Frame Generation");
        frameGeneration.setChecked(current[1] != 0);
        frameGeneration.setPadding(0, dp(12), 0, dp(4));
        panel.addView(frameGeneration);

        TextView flowLabel = new TextView(this);
        flowLabel.setPadding(0, dp(16), 0, dp(4));
        panel.addView(flowLabel);

        SeekBar flowScale = new SeekBar(this);
        flowScale.setMax(75);
        flowScale.setProgress(Math.max(0, Math.min(75, current[2] - 25)));
        panel.addView(flowScale);
        Runnable updateFlowLabel = () -> flowLabel.setText("Flow Scale: " + (flowScale.getProgress() + 25) + "%");
        updateFlowLabel.run();
        flowScale.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {
                updateFlowLabel.run();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });

        TextView aiStatus = new TextView(this);
        aiStatus.setText("Snap AI Engine: adaptive GPU control is not available yet. It will be enabled after the DXVK telemetry adapter is connected.");
        aiStatus.setTextSize(12);
        aiStatus.setPadding(0, dp(16), 0, dp(4));
        panel.addView(aiStatus);

        new AlertDialog.Builder(this)
                .setTitle("Snap AI / Frame Generation")
                .setView(panel)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Apply", (dialog, which) -> {
                    int enabled = frameGeneration.isChecked() ? 1 : 0;
                    int scale = flowScale.getProgress() + 25;
                    int mode = presentMode.getSelectedItemPosition();
                    if (!nativeSetSnapFrameSettings(enabled, scale, mode)) {
                        android.widget.Toast.makeText(this, "Could not apply SnapFrame settings",
                                android.widget.Toast.LENGTH_SHORT).show();
                    } else {
                        android.widget.Toast.makeText(this, "SnapFrame settings applied; present mode switches at the next safe present boundary",
                                android.widget.Toast.LENGTH_SHORT).show();
                    }
                })
                .show();
    }

    // ---- Dual build: the HUD on the second display (runtime/hud_screen.cpp) ----
    static native void nativeHudSurface(android.view.Surface surface);

    private Presentation hudScreen;
    private DisplayManager displays;

    private void startHudScreen() {
        displays = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        displays.registerDisplayListener(new DisplayManager.DisplayListener() {
            @Override public void onDisplayAdded(int id) { showHudScreen(); }
            @Override public void onDisplayRemoved(int id) {
                if (hudScreen != null && hudScreen.getDisplay().getDisplayId() == id) {
                    hudScreen.dismiss();
                    hudScreen = null;
                }
            }
            @Override public void onDisplayChanged(int id) {}
        }, new Handler(Looper.getMainLooper()));
        showHudScreen();
    }

    private Display secondDisplay() {
        Display[] p = displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        for (Display d : p) if (d.getDisplayId() != Display.DEFAULT_DISPLAY) return d;
        for (Display d : displays.getDisplays()) if (d.getDisplayId() != Display.DEFAULT_DISPLAY) return d;
        return null;
    }

    private void showHudScreen() {
        if (hudScreen != null) return;
        Display d = secondDisplay();
        if (d == null) return;
        try {
            hudScreen = new Presentation(this, d);
            SurfaceView view = new SurfaceView(hudScreen.getContext());
            view.setBackgroundColor(Color.TRANSPARENT);
            view.getHolder().addCallback(new SurfaceHolder.Callback() {
                @Override public void surfaceCreated(SurfaceHolder h) {}
                @Override public void surfaceChanged(SurfaceHolder h, int format, int w, int hgt) {
                    if (mBrokenLibraries) return;
                    nativeHudSurface(h.getSurface());
                }
                @Override public void surfaceDestroyed(SurfaceHolder h) {
                    if (mBrokenLibraries) return;
                    nativeHudSurface(null);
                }
            });
            hudScreen.setContentView(view);
            // the game keeps the input focus
            hudScreen.getWindow().addFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                    | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE | WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
            hudScreen.show();
        } catch (Exception e) {
            hudScreen = null;
        }
    }

    /* Why the previous run ended (crash, low memory kill, ...), kept in
     * files/last_exit.txt since log.txt is replaced on every launch. */
    private void saveLastExit() {
        if (Build.VERSION.SDK_INT < 30) return;
        try {
            ActivityManager am = (ActivityManager) getSystemService(ACTIVITY_SERVICE);
            List<ApplicationExitInfo> l = am.getHistoricalProcessExitReasons(null, 0, 3);
            String game = getIntent().getStringExtra("game");
            File dir = game != null ? new File(game, "port") : null;
            if (dir != null) dir.mkdirs();
            if (l.isEmpty() || dir == null) return;
            try (FileWriter w = new FileWriter(new File(dir, "last_exit.txt"))) {
                for (ApplicationExitInfo e : l) {
                    w.write(new Date(e.getTimestamp()) + ": reason " + reasonName(e.getReason())
                            + " status " + e.getStatus() + " importance " + e.getImportance()
                            + " pss " + e.getPss() / 1024 + "MB rss " + e.getRss() / 1024 + "MB"
                            + " desc " + e.getDescription() + "\n");
                }
                ActivityManager.MemoryInfo mi = new ActivityManager.MemoryInfo();
                am.getMemoryInfo(mi);
                w.write("device RAM " + mi.totalMem / (1 << 20) + "MB, available now " + mi.availMem / (1 << 20)
                        + "MB, low-memory threshold " + mi.threshold / (1 << 20) + "MB\n");
            }
        } catch (Exception ignored) {
        }
    }

    private static String reasonName(int r) {
        switch (r) {
        case ApplicationExitInfo.REASON_LOW_MEMORY: return "LOW_MEMORY";
        case ApplicationExitInfo.REASON_CRASH: return "CRASH_JAVA";
        case ApplicationExitInfo.REASON_CRASH_NATIVE: return "CRASH_NATIVE";
        case ApplicationExitInfo.REASON_SIGNALED: return "SIGNALED";
        case ApplicationExitInfo.REASON_ANR: return "ANR";
        case ApplicationExitInfo.REASON_EXIT_SELF: return "EXIT_SELF";
        case ApplicationExitInfo.REASON_USER_REQUESTED: return "USER_REQUESTED";
        case ApplicationExitInfo.REASON_EXCESSIVE_RESOURCE_USAGE: return "EXCESSIVE_RESOURCE_USAGE";
        case ApplicationExitInfo.REASON_OTHER: return "OTHER";
        default: return "code " + r;
        }
    }
}

