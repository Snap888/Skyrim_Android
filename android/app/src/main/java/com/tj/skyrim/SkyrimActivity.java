package com.tj.skyrim;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.hardware.display.DisplayManager;
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

public class SkyrimActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "dxvk_d3d9", "main" };
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
        addOverlayMenuButton();
        if (BuildConfig.DUAL) startHudScreen();
    }

    @Override
    protected void onDestroy() {
        if (hudScreen != null) { hudScreen.dismiss(); hudScreen = null; }
        if (!mBrokenLibraries) nativeSetVirtualPad(0, 0, 0, 0, 0, 0, 0);
        super.onDestroy();
    }

    // ---- In-game LSFG controls ------------------------------------------------
    private static native int[] nativeGetLsfgSettings();
    private static native boolean nativeSetLsfgSettings(int multiplier, int flowPercent, int performance);

    private int dp(float value) {
        return (int) (value * getResources().getDisplayMetrics().density + 0.5f);
    }

    private static native void nativeSetTouchEditor(boolean enabled);
    private native void nativeSetLegacyTouchEnabled(boolean enabled);
    private native void nativeSetVirtualPad(int buttons, int lx, int ly, int rx, int ry, int lt, int rt);
    private GamepadOverlayView gamepadOverlay;
    private boolean gamepadOverlayEnabled = true;

    private void addGamepadOverlay() {
        if (mLayout == null) return;
        // The old D3D9 touch renderer remains compiled for rollback, but is
        // switched off so only this Android/Kotlin overlay is visible.
        nativeSetLegacyTouchEnabled(false);
        gamepadOverlay = new GamepadOverlayView(this, (buttons, lx, ly, rx, ry, lt, rt) ->
                nativeSetVirtualPad(buttons, lx, ly, rx, ry, lt, rt));
        gamepadOverlay.setContentDescription("Virtual Xbox gamepad controls");
        gamepadOverlay.setLayerType(android.view.View.LAYER_TYPE_HARDWARE, null);
        android.widget.RelativeLayout.LayoutParams p = new android.widget.RelativeLayout.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.MATCH_PARENT);
        mLayout.addView(gamepadOverlay, p);
        gamepadOverlay.bringToFront();
        gamepadOverlay.setVisibility(gamepadOverlayEnabled ? android.view.View.VISIBLE : android.view.View.GONE);
    }

    private void setGamepadOverlayEnabled(boolean enabled) {
        gamepadOverlayEnabled = enabled;
        if (gamepadOverlay != null) {
            gamepadOverlay.setVisibility(enabled ? android.view.View.VISIBLE : android.view.View.GONE);
        }
        if (!enabled && !mBrokenLibraries) nativeSetVirtualPad(0, 0, 0, 0, 0, 0, 0);
        android.widget.Toast.makeText(this,
                enabled ? "Gamepad overlay enabled" : "Gamepad overlay disabled",
                android.widget.Toast.LENGTH_SHORT).show();
    }

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
        String[] items = { "LSFG settings", "Android keyboard", "Gamepad overlay: " + (gamepadOverlayEnabled ? "ON" : "OFF"), "Legacy touch controls editor" };
        new AlertDialog.Builder(this)
                .setTitle("Skyrim overlay")
                .setItems(items, (dialog, which) -> {
                    if (which == 0) showLsfgSettings();
                    else if (which == 1) {
                        // SDLActivity routes this request to Android's InputMethodManager.
                        showTextInput(0, 0, 1, 1);
                    } else {
                        touchEditorEnabled = !touchEditorEnabled;
                        nativeSetTouchEditor(touchEditorEnabled);
                        android.widget.Toast.makeText(this,
                                touchEditorEnabled ? "Touch editor enabled" : "Touch editor disabled",
                                android.widget.Toast.LENGTH_SHORT).show();
                    }
                }).show();
    }

    private void showLsfgSettings() {
        int[] current = nativeGetLsfgSettings();
        if (current == null || current.length < 4 || current[0] == 0) {
            new AlertDialog.Builder(this)
                    .setTitle("LSFG")
                    .setMessage("LSFG backend is not active. Check port/lsfg.conf and Lossless.dll, then restart the game.")
                    .setPositiveButton("OK", null)
                    .show();
            return;
        }

        LinearLayout panel = new LinearLayout(this);
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setPadding(dp(20), dp(8), dp(20), dp(4));

        TextView status = new TextView(this);
        status.setText("Changes apply during gameplay and are saved for the next launch. x-0 disables frame generation; x-2/x-3/x-4 enable it.");
        status.setTextSize(13);
        panel.addView(status);

        TextView multiplierLabel = new TextView(this);
        multiplierLabel.setText("Frame generation");
        multiplierLabel.setPadding(0, dp(16), 0, dp(4));
        panel.addView(multiplierLabel);
        Spinner multiplier = new Spinner(this);
        multiplier.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item,
                new String[] { "x-0 (Off)", "x-2", "x-3", "x-4" }));
        multiplier.setSelection(current[1] <= 1 ? 0 : Math.max(1, Math.min(3, current[1] - 1)));
        panel.addView(multiplier);

        TextView flowLabel = new TextView(this);
        flowLabel.setPadding(0, dp(16), 0, 0);
        panel.addView(flowLabel);
        SeekBar flow = new SeekBar(this);
        flow.setMax(60);
        flow.setProgress(Math.max(0, Math.min(60, current[2] - 40)));
        panel.addView(flow);
        Runnable updateFlowLabel = () -> flowLabel.setText("Flow quality: " + (flow.getProgress() + 40) + "%");
        updateFlowLabel.run();
        flow.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {
                updateFlowLabel.run();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });

        TextView modeLabel = new TextView(this);
        modeLabel.setText("GPU mode");
        modeLabel.setPadding(0, dp(16), 0, dp(4));
        panel.addView(modeLabel);
        Spinner mode = new Spinner(this);
        mode.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item,
                new String[] { "Performance", "Balanced", "Low GPU load" }));
        mode.setSelection(Math.max(0, Math.min(2, current[3])));
        panel.addView(mode);

        new AlertDialog.Builder(this)
                .setTitle("LSFG dynamic settings")
                .setView(panel)
                .setNegativeButton("Cancel", null)
                .setPositiveButton("Apply", (dialog, which) -> {
                    int selectedFrameMode = multiplier.getSelectedItemPosition();
                    int mult = selectedFrameMode == 0 ? 1 : selectedFrameMode + 1;
                    int quality = flow.getProgress() + 40;
                    int perf = mode.getSelectedItemPosition();
                    if (!nativeSetLsfgSettings(mult, quality, perf)) {
                        android.widget.Toast.makeText(this, "Could not apply LSFG settings",
                                android.widget.Toast.LENGTH_SHORT).show();
                    } else {
                        android.widget.Toast.makeText(this, "LSFG settings applied",
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
