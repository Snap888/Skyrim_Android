package com.tj.skyrim;

import android.app.ActivityManager;
import android.app.ApplicationExitInfo;
import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowManager;
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
        if (BuildConfig.DUAL) startHudScreen();
    }

    @Override
    protected void onDestroy() {
        if (hudScreen != null) { hudScreen.dismiss(); hudScreen = null; }
        super.onDestroy();
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
