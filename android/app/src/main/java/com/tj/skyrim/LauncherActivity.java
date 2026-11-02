package com.tj.skyrim;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.Manifest;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.graphics.Point;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.system.Os;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.AdapterView;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/* Start screen, in the spirit of the PC SkyrimLauncher: pick the folder that
 * holds the Skyrim install, set the graphics options, press Play. The options
 * are written to SkyrimPrefs.ini (where the game reads them) before the game
 * starts; the folder is passed to the native side as argv[1]. */
public class LauncherActivity extends Activity {
    private static final int REQ_FOLDER = 1, REQ_PERM = 2, REQ_DLL = 3, REQ_GPU_DRIVER = 4;
    private SharedPreferences prefs;
    private TextView folderText, folderStatus;
    private Button playButton;
    private Button pickDllButton;
    private TextView dllText, lsfgQualityValue;
    private Spinner gpuDriverSpinner;
    private TurnipDriverManager gpuDrivers;
    private List<TurnipDriverManager.Driver> installedGpuDrivers = new ArrayList<>();
    private SeekBar lsfgQuality;
    private boolean pendingPick, pendingPlay;  /* continue once the storage prompt is answered */
    private boolean askedAllFiles;
    private final Map<String, Spinner> spinners = new LinkedHashMap<>();
    private final Map<String, CheckBox> checks = new LinkedHashMap<>();

    /* option key -> labels, and the INI values each label stands for */
    private static final String[] PRESETS = {"Low", "Medium", "High", "Ultra"};
    private static final String[] PRESET_FILES = {"low.ini", "medium.ini", "high.ini", "VeryHigh.ini"};
    private static final int[] MODE_HEIGHTS = {1080, 900, 720, 648, 576, 540, 480, 432, 360, 288, 270, 216, 180, 144, 108};

    // Aspect ratio of the picture. Very wide phones (20:9 and up) show the game
    // at 16:9 or so with black bars instead of stretching its UI to the edges.
    private static final String[] ASPECTS = {"16:9", "16:10", "4:3", "21:9", "Screen"};
    private static final int[][] ASPECT_V = {{16, 9}, {16, 10}, {4, 3}, {21, 9}, {0, 0}};

    private int[] screen() {
        Point sz = new Point();
        getWindowManager().getDefaultDisplay().getRealSize(sz);
        return new int[]{Math.max(sz.x, sz.y), Math.min(sz.x, sz.y)};
    }

    // Must match bridge_scaled_mode in runtime/d3d9_bridge.cpp: the game only
    // accepts a size from the display mode list.
    private int[][] modes(int aspect) {
        int[] s = screen();
        int w = s[0], h = s[1];
        int an = ASPECT_V[aspect][0], ad = ASPECT_V[aspect][1];
        if (an == 0) { an = w; ad = h; }
        int top = (int) Math.min(h, (long) w * ad / an);
        java.util.ArrayList<int[]> l = new java.util.ArrayList<>();
        l.add(new int[]{(int) ((long) top * an / ad) & ~1, top});
        for (int mh : MODE_HEIGHTS)
            if (mh < top) l.add(new int[]{(int) ((long) mh * an / ad) & ~1, mh});
        return l.toArray(new int[0][]);
    }

    private static String[] modeNames(int[][] m) {
        String[] r = new String[m.length];
        for (int i = 0; i < m.length; i++) r[i] = m[i][0] + " x " + m[i][1];
        return r;
    }

    private int defaultMode(int[][] m) {
        for (int i = 0; i < m.length; i++) if (m[i][1] <= 540) return i;
        return m.length - 1;
    }
    private static final String[] AA = {"Off", "2x", "4x", "8x"};
    private static final int[] AA_V = {1, 2, 4, 8};
    private static final String[] ANISO = {"Off", "2x", "4x", "8x", "16x"};
    private static final int[] ANISO_V = {1, 2, 4, 8, 16};
    private static final String[] TEXQ = {"Low", "Medium", "High"};
    private static final int[] TEXQ_V = {2, 1, 0};  // iTexMipMapSkip
    private static final String[] SHADOW = {"Low", "Medium", "High", "Ultra"};
    private static final int[] SHADOW_V = {512, 1024, 2048, 4096};
    private static final String[] DECALS = {"None", "Low", "Medium", "High"};
    private static final int[] DECALS_V = {0, 100, 250, 1000};
    private static final String[] BLUR = {"Low", "Medium", "High"};
    private static final String[] FADE = {"Short", "Medium", "Far", "Very far"};
    private static final String[] FADE_V = {"2.5", "4.5", "6.5", "9.0"};
    private static final String[] HUD_SIZE = {"Small", "Normal", "Large"};
    private static final String[] HUD_ALIGN = {"Left", "Center", "Right"};
    private static final String[] HUD_VERTICAL = {"Top", "Bottom"};
    private static final String[] LSFG_MULTIPLIER = {"x2", "x3", "x4"};
    private static final String[] LSFG_MODE = {"Performance", "Stability", "Low"};
    private static final String[] GAME_FPS_LIMIT = {"Unlimited", "30 FPS", "40 FPS", "45 FPS", "50 FPS", "60 FPS", "90 FPS", "120 FPS"};

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        prefs = getSharedPreferences("launcher", MODE_PRIVATE);
        gpuDrivers = new TurnipDriverManager(this);
        buildUi();
        refreshFolder();
    }

    @Override
    protected void onResume() {
        super.onResume();
        refreshFolder();
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private TextView text(LinearLayout parent, String s, int sp, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(sp);
        t.setTextColor(Color.rgb(230, 225, 210));
        if (bold) t.setTypeface(null, android.graphics.Typeface.BOLD);
        t.setPadding(0, dp(6), 0, dp(2));
        parent.addView(t);
        return t;
    }

    private Spinner spinner(LinearLayout parent, String key, String label, String[] items, int def) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        TextView t = new TextView(this);
        t.setText(label);
        t.setTextSize(16);
        t.setTextColor(Color.rgb(230, 225, 210));
        row.addView(t, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        Spinner s = new Spinner(this);
        ArrayAdapter<String> a = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, items);
        a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        s.setAdapter(a);
        s.setSelection(Math.min(prefs.getInt(key, def), items.length - 1));
        row.addView(s, new LinearLayout.LayoutParams(dp(180), ViewGroup.LayoutParams.WRAP_CONTENT));
        parent.addView(row);
        spinners.put(key, s);
        return s;
    }

    private CheckBox check(LinearLayout parent, String key, String label, boolean def) {
        CheckBox c = new CheckBox(this);
        c.setText(label);
        c.setTextSize(16);
        c.setTextColor(Color.rgb(230, 225, 210));
        c.setChecked(prefs.getBoolean(key, def));
        parent.addView(c);
        checks.put(key, c);
        return c;
    }

    private void buildUi() {
        ScrollView sv = new ScrollView(this);
        sv.setBackgroundColor(Color.rgb(24, 22, 20));
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(24), dp(16), dp(24), dp(24));
        sv.addView(root);

        text(root, "Skyrim", 28, true);

        text(root, "Game folder", 20, true);
        folderText = text(root, "", 14, false);
        folderStatus = text(root, "", 14, false);
        Button pick = new Button(this);
        pick.setText("Choose Skyrim folder");
        pick.setOnClickListener(v -> {
            /* the game reads the folder with plain file calls: ask for file
             * access first (only shown once), then pick */
            if (hasStorage()) openPicker();
            else { pendingPick = true; requestStorage(); }
        });
        root.addView(pick);

        text(root, "Options", 20, true);
        Spinner preset = spinner(root, "preset", "Detail", PRESETS, 0);
        Spinner aspect = spinner(root, "aspect", "Aspect ratio", ASPECTS, 0);
        int[][] m = modes(aspect.getSelectedItemPosition());
        Spinner res = spinner(root, "res", "Resolution", modeNames(m), defaultMode(m));
        aspect.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            boolean first = true;
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                if (first) { first = false; return; }
                // keep the same height where the new list has it
                int oldH = modes(prefs.getInt("aspectShown", 0))[Math.max(0, res.getSelectedItemPosition())][1];
                int[][] nm = modes(pos);
                ArrayAdapter<String> a = new ArrayAdapter<>(LauncherActivity.this,
                        android.R.layout.simple_spinner_item, modeNames(nm));
                a.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
                res.setAdapter(a);
                int pick = defaultMode(nm);
                for (int i = 0; i < nm.length; i++) if (nm[i][1] == oldH) pick = i;
                res.setSelection(pick);
                prefs.edit().putInt("aspectShown", pos).apply();
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });
        prefs.edit().putInt("aspectShown", aspect.getSelectedItemPosition()).apply();
        spinner(root, "aa", "Antialiasing", AA, 0);
        spinner(root, "aniso", "Anisotropic filtering", ANISO, 0);
        spinner(root, "texq", "Texture quality", TEXQ, 1);
        spinner(root, "shadow", "Shadow detail", SHADOW, 0);
        spinner(root, "decals", "Decal quantity", DECALS, 0);
        spinner(root, "blur", "Radial blur quality", BLUR, 0);
        spinner(root, "fade", "Object fade distance", FADE, 0);
        check(root, "fxaa", "FXAA", false);
        check(root, "vsync", "Vertical sync", true);
        check(root, "touch", "Touch controls", true);
        check(root, "nintendo", "Nintendo button layout (A/B, X/Y swapped)", false);

        text(root, "LSFG FPS HUD", 20, true);
        check(root, "hudEnabled", "Show FPS HUD", true);
        spinner(root, "hudSize", "Font size", HUD_SIZE, 1);
        spinner(root, "hudAlign", "Horizontal position", HUD_ALIGN, 0);
        spinner(root, "hudVertical", "Vertical position", HUD_VERTICAL, 0);
        check(root, "hudGame", "Game FPS", true);
        check(root, "hudGenerated", "FG submit FPS", true);
        check(root, "hudOutput", "Output submit FPS", true);
        check(root, "hudDriverInfo", "Show Vulkan driver information", false);

        text(root, "Lossless Scaling Frame Generation", 20, true);
        check(root, "lsfgEnabled", "Enable LSFG frame generation", false);
        text(root, "Lossless.dll", 16, false);
        dllText = text(root, prefs.getString("lsfgDllName", "No DLL selected"), 14, false);
        pickDllButton = new Button(this);
        pickDllButton.setText("Choose Lossless.dll");
        pickDllButton.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, REQ_DLL);
        });
        root.addView(pickDllButton);
        spinner(root, "lsfgMultiplier", "Generated frames", LSFG_MULTIPLIER, 0);
        text(root, "Generation quality", 16, false);
        lsfgQuality = new SeekBar(this);
        lsfgQuality.setMax(9);
        lsfgQuality.setProgress(Math.max(0, Math.min(9, prefs.getInt("lsfgQuality", 6))));
        root.addView(lsfgQuality);
        lsfgQualityValue = text(root, "", 14, false);
        updateLsfgQualityLabel();
        lsfgQuality.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int progress, boolean fromUser) {
                updateLsfgQualityLabel();
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        spinner(root, "lsfgMode", "Generation mode", LSFG_MODE, 1);
        text(root, "Performance: reduced compute load. Stability: full selected quality. Low: half-resolution flow.", 13, false);

        text(root, "Vulkan GPU driver", 20, true);
        text(root, "System uses the Android device driver. Import a compatible Turnip ZIP to use a custom Adreno driver.", 13, false);
        gpuDriverSpinner = new Spinner(this);
        root.addView(gpuDriverSpinner);
        refreshGpuDriverChoices(null);
        Button importGpuDriver = new Button(this);
        importGpuDriver.setText("Import Turnip driver ZIP");
        importGpuDriver.setOnClickListener(v -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("application/zip");
            startActivityForResult(intent, REQ_GPU_DRIVER);
        });
        root.addView(importGpuDriver);

        text(root, "Game frame rate limit (DXVK)", 20, true);
        spinner(root, "gameFpsLimit", "Maximum game FPS", GAME_FPS_LIMIT, 0);
        text(root, "Limits the original game's frame rate before LSFG. Generated frames are not capped by this setting. Use Unlimited to disable the limiter.", 13, false);

        text(root, "DXVK Performance A/B test", 20, true);
        check(root, "dxvkPerfEnabled", "Write separate DXVK benchmark log", false);
        spinner(root, "dxvkPerfRun", "Test pass", new String[] {"A — LSFG OFF", "B — LSFG ON"}, 0);
        text(root, "The CSV log is saved in the selected Skyrim folder under port/dxvk_perf_off.csv or port/dxvk_perf_on.csv. The selected pass also forces LSFG OFF/ON for this run.", 13, false);

        text(root, "Experimental runtime tuning", 20, true);
        check(root, "runtimeSyncPipelines", "Synchronous shader pipeline compilation (noasync.txt)", false);
        text(root, "Compiles cached pipelines on the thread that registers the shader. First use may cause a long frame; keep OFF for normal play.", 13, false);
        check(root, "runtimeThreadLogs", "Log DXVK worker thread scheduling", false);
        text(root, "Logs DXVK shader-cache worker TID, CPU core and timing. Does not change Skyrim/Havok thread affinity.", 13, false);
        check(root, "runtimeSurfaceLogs", "Log Android Surface lifecycle and dimensions", true);
        text(root, "Records surface create/change/destroy, actual SurfaceView dimensions, display dimensions and refresh rate.", 13, false);
        check(root, "runtimeTuningLogs", "Write runtime tuning configuration and diagnostics", true);
        text(root, "Configuration and DXVK log are stored in the selected game folder under port/. Android Surface events are also visible in logcat (tag SDL).", 13, false);
        check(root, "lsfgGpuProfile", "LSFG GPU stage profiling (CSV)", false);
        text(root, "Writes per-stage Vulkan GPU timings to port/dxvk_lsfg_gpu.csv. Diagnostic mode adds timestamp queries and file I/O; keep OFF during normal gameplay.", 13, false);

        preset.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            boolean first = true;
            @Override public void onItemSelected(AdapterView<?> p, View v, int pos, long id) {
                if (first) { first = false; return; }  // initial selection is not a user change
                applyPreset(pos);
            }
            @Override public void onNothingSelected(AdapterView<?> p) {}
        });

        playButton = new Button(this);
        playButton.setText("Play");
        playButton.setTextSize(22);
        playButton.setOnClickListener(v -> play());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(64));
        lp.topMargin = dp(16);
        root.addView(playButton, lp);
        setContentView(sv);
    }

    private void updateLsfgQualityLabel() {
        if (lsfgQualityValue != null && lsfgQuality != null)
            lsfgQualityValue.setText(String.format(java.util.Locale.US, "%.1f", (lsfgQuality.getProgress() + 1) / 10.0f));
    }

    /* Choosing a preset sets the detail options to what the PC launcher's preset does. */
    private void applyPreset(int p) {
        int[] tex = {1, 1, 2, 2}, shadow = {0, 1, 2, 3}, decals = {0, 1, 2, 3}, blur = {0, 1, 2, 2},
              fade = {0, 1, 2, 3}, aa = {0, 0, 2, 3}, aniso = {0, 2, 3, 4};
        boolean[] fxaa = {false, false, false, false};
        // texture index: 0 low,1 medium,2 high (TEXQ order)
        spinners.get("texq").setSelection(tex[p]);
        spinners.get("shadow").setSelection(shadow[p]);
        spinners.get("decals").setSelection(decals[p]);
        spinners.get("blur").setSelection(blur[p]);
        spinners.get("fade").setSelection(fade[p]);
        spinners.get("aa").setSelection(aa[p]);
        spinners.get("aniso").setSelection(aniso[p]);
        checks.get("fxaa").setChecked(fxaa[p]);
    }

    /* Storage: the app targets API 29 with legacy storage, so the normal
     * "allow access to files" prompt gives the game plain file access. Only
     * if this Android ignores legacy storage and the folder still can't be
     * read is all-files access asked for (once, automatically). */
    private boolean hasStorage() {
        return checkSelfPermission(Manifest.permission.READ_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED
                || (Build.VERSION.SDK_INT >= 30 && Environment.isExternalStorageManager());
    }

    private void requestStorage() {
        requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQ_PERM);
    }

    @Override
    public void onRequestPermissionsResult(int req, String[] perms, int[] res) {
        super.onRequestPermissionsResult(req, perms, res);
        if (req != REQ_PERM) return;
        boolean ok = hasStorage();
        if (ok && pendingPick) openPicker();
        if (ok && pendingPlay) play();
        pendingPick = pendingPlay = false;
        refreshFolder();
    }

    /* folder exists but its files can't be read: legacy storage was refused */
    private boolean unreadable(List<String> bad) {
        for (String b : bad) if (b.startsWith("cannot read")) return true;
        return false;
    }

    private boolean hasAllFiles() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    private void requestAllFiles() {
        try {
            Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName()));
            startActivity(i);
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void openPicker() {
        startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQ_FOLDER);
    }

    @Override
    protected void onActivityResult(int req, int res, Intent data) {
        super.onActivityResult(req, res, data);
        if (req == REQ_GPU_DRIVER) {
            if (res == RESULT_OK && data != null && data.getData() != null) {
                try {
                    TurnipDriverManager.Driver driver = gpuDrivers.importZip(data.getData());
                    refreshGpuDriverChoices(driver.id);
                    folderStatus.setText("Imported " + driver.name + " " + driver.version
                            + ". Restart the game after changing the GPU driver.");
                } catch (Exception e) {
                    folderStatus.setText("Could not import GPU driver: " + e.getMessage());
                }
            }
            return;
        }
        if (req == REQ_DLL) {
            if (res == RESULT_OK && data != null && data.getData() != null) {
                try {
                    copyLosslessDll(data.getData());
                } catch (Exception e) {
                    folderStatus.setText("Could not import Lossless.dll: " + e.getMessage());
                }
            }
            return;
        }
        if (req != REQ_FOLDER || res != RESULT_OK || data == null || data.getData() == null) return;
        String path = treeToPath(data.getData());
        if (path == null) {
            folderStatus.setText("That location is not a plain folder on this device; choose one on internal storage or the SD card.");
            return;
        }
        prefs.edit().putString("folder", path).apply();
        refreshFolder();
        if (!askedAllFiles && !hasAllFiles() && unreadable(checkFolder(path))) {
            askedAllFiles = true;
            requestAllFiles();
        }
    }

    /* content://...tree/primary:Games/Skyrim -> /storage/emulated/0/Games/Skyrim */
    private void copyLosslessDll(Uri uri) throws Exception {
        File dir = new File(getFilesDir(), "lsfg");
        if (!dir.exists() && !dir.mkdirs()) throw new java.io.IOException("cannot create app LSFG folder");
        File target = new File(dir, "Lossless.dll");
        File temp = new File(dir, "Lossless.dll.tmp");
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new java.io.IOException("selected file cannot be opened");
            int first = in.read();
            int second = in.read();
            if (first != 'M' || second != 'Z')
                throw new java.io.IOException("selected file is not a Windows PE DLL (MZ header missing)");
            try (FileOutputStream out = new FileOutputStream(temp)) {
                out.write(first);
                out.write(second);
                byte[] buffer = new byte[1 << 16];
                for (int n; (n = in.read(buffer)) > 0; ) out.write(buffer, 0, n);
                out.getFD().sync();
            }
        }
        if (target.exists() && !target.delete()) {
            temp.delete();
            throw new java.io.IOException("cannot replace previous DLL");
        }
        if (!temp.renameTo(target)) {
            temp.delete();
            throw new java.io.IOException("cannot move imported DLL into app storage");
        }
        File cache = new File(getNoBackupFilesDir(), "lsfg/cache");
        deleteTree(cache);
        prefs.edit().putString("lsfgDllName", "Lossless.dll (app storage)").apply();
        dllText.setText("Lossless.dll (app storage)");
        folderStatus.setText("Lossless.dll imported into private app storage");
    }

    private static void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }

    private static String treeToPath(Uri tree) {
        String id;
        try { id = DocumentsContract.getTreeDocumentId(tree); } catch (Exception e) { return null; }
        int c = id.indexOf(':');
        if (c < 0) return null;
        String vol = id.substring(0, c), rel = id.substring(c + 1);
        String base = vol.equalsIgnoreCase("primary") ? Environment.getExternalStorageDirectory().getPath()
                : "/storage/" + vol;
        return rel.isEmpty() ? base : base + "/" + rel;
    }

    private static File findCi(File dir, String name) {
        File f = new File(dir, name);
        if (f.exists()) return f;
        String[] l = dir.list();
        if (l != null) for (String s : l) if (s.equalsIgnoreCase(name)) return new File(dir, s);
        return null;
    }

    // SHA-1 of the Steam TESV.exe (1.9.32.0.8) the port was recompiled from:
    // other versions have different code at different addresses and would crash.
    private static final String SHA_TESV_STEAM = "f39aecebf6e332616c4c518617073f6e8250e0f8";

    private void checkSha(File f, String want, String name, List<String> bad) {
        String got = sha1(f);
        if (got == null) bad.add("cannot read " + name);
        else if (!got.equals(want)) bad.add(name + " is the wrong version (SHA-1 " + got.substring(0, 8)
                + ", expected " + want.substring(0, 8) + ")");
    }

    /* SHA-1 as hex, cached by path, size and modification time; null if unreadable. */
    private String sha1(File f) {
        String key = "sha1:" + f.getAbsolutePath(), stamp = f.length() + ":" + f.lastModified();
        String cached = prefs.getString(key, null);
        if (cached != null && cached.startsWith(stamp + "=")) return cached.substring(stamp.length() + 1);
        try (java.io.InputStream in = new java.io.FileInputStream(f)) {
            java.security.MessageDigest md = java.security.MessageDigest.getInstance("SHA-1");
            byte[] buf = new byte[1 << 16];
            for (int n; (n = in.read(buf)) > 0; ) md.update(buf, 0, n);
            StringBuilder sb = new StringBuilder();
            for (byte b : md.digest()) sb.append(String.format("%02x", b));
            String hex = sb.toString();
            prefs.edit().putString(key, stamp + "=" + hex).apply();
            return hex;
        } catch (Exception e) {
            return null;
        }
    }

    /* Returns the list of problems with the folder (empty when it is ready). */
    private List<String> checkFolder(String path) {
        List<String> bad = new ArrayList<>();
        if (path == null) { bad.add("no folder chosen"); return bad; }
        File d = new File(path);
        if (!d.isDirectory()) { bad.add("folder not found"); return bad; }
        File data = findCi(d, "Data");
        File esm = data != null ? findCi(data, "Skyrim.esm") : null;
        if (data == null) bad.add("Data folder missing");
        else if (esm == null) bad.add(data.canRead() ? "Data/Skyrim.esm missing" : "cannot read the Data folder");
        else if (!esm.canRead()) bad.add("cannot read Data/Skyrim.esm");
        // Only the original exe is checked: Steamless output differs between Steamless versions.
        File packed = findCi(d, "TESV.exe");
        if (packed != null) checkSha(packed, SHA_TESV_STEAM, "TESV.exe", bad);
        if (findCi(d, "TESV_unpacked.exe") == null && findCi(d, "TESV.exe.unpacked.exe") == null)
            bad.add(packed != null ? "TESV_unpacked.exe missing (unpack TESV.exe with Steamless)" : "TESV_unpacked.exe missing");
        for (String n : new String[] {"d3dx9_42.dll", "X3DAudio1_7.dll", "binkw32.dll"})
            if (findCi(d, n) == null) bad.add(n + " missing");
        return bad;
    }

    private void refreshFolder() {
        String path = prefs.getString("folder", null);
        folderText.setText(path == null ? "No folder chosen yet. Copy your Skyrim folder (with the 3 extra files) "
                + "anywhere outside Android/data, e.g. Internal storage/Skyrim, then choose it here." : path);
        List<String> bad = checkFolder(path);
        folderStatus.setText(bad.isEmpty() ? "Ready" : "Not ready: " + String.join(", ", bad));
        folderStatus.setTextColor(bad.isEmpty() ? Color.rgb(140, 200, 120) : Color.rgb(230, 150, 110));
        playButton.setEnabled(bad.isEmpty());
    }

    private void refreshGpuDriverChoices(String selectId) {
        if (gpuDriverSpinner == null) return;
        try {
            installedGpuDrivers = gpuDrivers.list();
            List<String> labels = new ArrayList<>();
            labels.add("System (Android driver)");
            for (TurnipDriverManager.Driver driver : installedGpuDrivers) labels.add(driver.toString());
            ArrayAdapter<String> adapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item, labels);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            gpuDriverSpinner.setAdapter(adapter);
            String wanted = selectId != null ? selectId : prefs.getString("gpuDriverId", "");
            int selection = 0;
            for (int i = 0; i < installedGpuDrivers.size(); i++)
                if (installedGpuDrivers.get(i).id.equals(wanted)) selection = i + 1;
            gpuDriverSpinner.setSelection(selection);
        } catch (Exception e) {
            folderStatus.setText("Could not list GPU drivers: " + e.getMessage());
        }
    }

    private TurnipDriverManager.Driver selectedGpuDriver() {
        int index = gpuDriverSpinner != null ? gpuDriverSpinner.getSelectedItemPosition() : 0;
        return index > 0 && index <= installedGpuDrivers.size() ? installedGpuDrivers.get(index - 1) : null;
    }

    private void writeGpuDriverConfig(File game) throws Exception {
        File port = new File(game, "port");
        if (!port.exists() && !port.mkdirs()) throw new java.io.IOException("cannot create port config folder");
        TurnipDriverManager.Driver driver = selectedGpuDriver();
        File config = new File(port, "gpu-driver.conf");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(config))) {
            if (driver == null) {
                w.write("enabled=0\n");
            } else {
                w.write("enabled=1\n");
                w.write("driver_path=" + gpuDrivers.driverDirectory(driver.id) + "\n");
                w.write("library_name=" + driver.library + "\n");
                w.write("driver_name=" + driver.name.replace('\n', ' ') + "\n");
            }
        }
    }

    private void saveChoices() {
        SharedPreferences.Editor e = prefs.edit();
        for (Map.Entry<String, Spinner> s : spinners.entrySet()) e.putInt(s.getKey(), s.getValue().getSelectedItemPosition());
        for (Map.Entry<String, CheckBox> c : checks.entrySet()) e.putBoolean(c.getKey(), c.getValue().isChecked());
        if (lsfgQuality != null) e.putInt("lsfgQuality", lsfgQuality.getProgress());
        TurnipDriverManager.Driver gpuDriver = selectedGpuDriver();
        e.putString("gpuDriverId", gpuDriver != null ? gpuDriver.id : "");
        e.apply();
    }

    private int sel(String k) { return spinners.get(k).getSelectedItemPosition(); }

    private void play() {
        if (!hasStorage()) { pendingPlay = true; requestStorage(); return; }
        saveChoices();
        boolean perfEnabled = checks.get("dxvkPerfEnabled").isChecked();
        boolean perfLsfgOn = sel("dxvkPerfRun") == 1;
        boolean effectiveLsfgEnabled = perfEnabled ? perfLsfgOn : checks.get("lsfgEnabled").isChecked();
        if (effectiveLsfgEnabled && !new File(getFilesDir(), "lsfg/Lossless.dll").isFile()) {
            folderStatus.setText("LSFG is enabled for this run: choose a valid Lossless.dll first.");
            return;
        }
        String folder = prefs.getString("folder", null);
        File save = new File(folder, "port/save");
        File iniDir = new File(save, "Documents/My Games/Skyrim");
        iniDir.mkdirs();
        try {
            writePrefs(new File(folder), new File(iniDir, "SkyrimPrefs.ini"));
            writeDxvkHudConfig(new File(folder));
            writeLsfgConfig(new File(folder), effectiveLsfgEnabled);
            writeGpuDriverConfig(new File(folder));
            configureDxvkPerfEnvironment(new File(folder), perfEnabled, perfLsfgOn);
            configureRuntimeTuning(new File(folder));
            configureLsfgGpuProfiler(new File(folder));
            // the native side builds the same mode list from this aspect (see modes())
            int[] av = ASPECT_V[sel("aspect")];
            try (Writer w = new OutputStreamWriter(new FileOutputStream(new File(save, "display.cfg")))) {
                w.write(av[0] + " " + av[1] + "\n");
            }
            try (Writer w = new OutputStreamWriter(new FileOutputStream(new File(save, "touch.cfg")))) {
                w.write((checks.get("touch").isChecked() ? "1" : "0") + " " + (checks.get("nintendo").isChecked() ? "1" : "0") + "\n");
            }
        } catch (Exception ex) {
            folderStatus.setText("Could not write the settings: " + ex);
            return;
        }
        Intent i = new Intent(this, SkyrimActivity.class);
        i.putExtra("game", folder);
        startActivity(i);
        finish();
    }

    /* Runtime tuning flags are inherited by SDL/DXVK in this process. The
     * noasync marker is also written to port/save for easy inspection. */
    private void configureRuntimeTuning(File game) throws Exception {
        File port = new File(game, "port");
        File save = new File(port, "save");
        if (!port.exists() && !port.mkdirs())
            throw new java.io.IOException("cannot create port tuning folder");
        if (!save.exists() && !save.mkdirs())
            throw new java.io.IOException("cannot create port/save tuning folder");

        boolean syncPipelines = checks.get("runtimeSyncPipelines").isChecked();
        boolean threadLogs = checks.get("runtimeThreadLogs").isChecked();
        boolean surfaceLogs = checks.get("runtimeSurfaceLogs").isChecked();
        boolean tuningLogs = checks.get("runtimeTuningLogs").isChecked();

        File marker = new File(save, "noasync.txt");
        if (syncPipelines) {
            if (!marker.exists() && !marker.createNewFile())
                throw new java.io.IOException("cannot create " + marker);
        } else if (marker.exists() && !marker.delete()) {
            throw new java.io.IOException("cannot remove " + marker);
        }

        setRuntimeEnv("DXVK_NO_ASYNC", syncPipelines);
        setRuntimeEnv("DXVK_THREAD_DIAGNOSTICS", threadLogs);
        setRuntimeEnv("DXVK_SURFACE_DIAGNOSTICS", surfaceLogs);
        setRuntimeEnv("DXVK_RUNTIME_DIAGNOSTICS", tuningLogs);

        File config = new File(port, "runtime_tuning.conf");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(config))) {
            w.write("sync_pipeline_compile=" + (syncPipelines ? "1" : "0") + "\n");
            w.write("dxvk_thread_diagnostics=" + (threadLogs ? "1" : "0") + "\n");
            w.write("android_surface_diagnostics=" + (surfaceLogs ? "1" : "0") + "\n");
            w.write("runtime_diagnostics=" + (tuningLogs ? "1" : "0") + "\n");
            w.write("cpu_affinity=not_implemented_game_threads\n");
            w.write("worker_spin_wait=not_implemented_game_workers\n");
        }
        android.util.Log.i("SkyrimRuntime", "Runtime tuning: syncPipelines=" + syncPipelines
                + ", threadLogs=" + threadLogs + ", surfaceLogs=" + surfaceLogs
                + ", tuningLogs=" + tuningLogs);
    }

    private void setRuntimeEnv(String name, boolean enabled) throws Exception {
        if (enabled) Os.setenv(name, "1", true);
        else Os.unsetenv(name);
    }

    /* Configure LSFG GPU timestamp profiling before SkyrimActivity starts.
     * The native profiler caches its environment setting on first use. */
    private void configureLsfgGpuProfiler(File game) throws Exception {
        File port = new File(game, "port");
        if (!port.exists() && !port.mkdirs())
            throw new java.io.IOException("cannot create port GPU profiler folder");

        boolean enabled = checks.get("lsfgGpuProfile").isChecked();
        File log = new File(port, "dxvk_lsfg_gpu.csv");
        if (enabled) {
            if (log.exists() && !log.delete())
                throw new java.io.IOException("cannot reset previous LSFG GPU log: " + log);
            if (!log.createNewFile())
                throw new java.io.IOException("cannot create LSFG GPU log: " + log);
            Os.setenv("DXVK_LSFG_GPU_PROFILE_LOG", log.getAbsolutePath(), true);
            Os.setenv("DXVK_LSFG_GPU_PROFILE", "1", true);
        } else {
            Os.unsetenv("DXVK_LSFG_GPU_PROFILE");
            Os.unsetenv("DXVK_LSFG_GPU_PROFILE_LOG");
        }

        File config = new File(port, "lsfg_gpu_profile.conf");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(config))) {
            w.write("enabled=" + (enabled ? "1" : "0") + "\n");
            w.write("csv_path=" + (enabled ? log.getAbsolutePath() : "disabled") + "\n");
        }
        android.util.Log.i("SkyrimRuntime", "LSFG GPU profiler: enabled=" + enabled
                + (enabled ? ", csv=" + log.getAbsolutePath() : ""));
    }

    /* Configure benchmark variables in this app process before SDL loads DXVK.
     * The CSV is intentionally stored beside the other port diagnostics. */
    private void configureDxvkPerfEnvironment(File game, boolean enabled, boolean lsfgOn) throws Exception {
        if (!enabled) {
            Os.unsetenv("DXVK_PERF_LOG");
            Os.unsetenv("DXVK_PERF_RUN");
            return;
        }
        File port = new File(game, "port");
        if (!port.exists() && !port.mkdirs()) throw new java.io.IOException("cannot create port log folder");
        String run = lsfgOn ? "lsfg_on" : "lsfg_off";
        File log = new File(port, lsfgOn ? "dxvk_perf_on.csv" : "dxvk_perf_off.csv");
        if (log.exists() && !log.delete()) throw new java.io.IOException("cannot reset previous benchmark log: " + log);
        if (!log.createNewFile()) throw new java.io.IOException("cannot create benchmark log: " + log);
        if (!log.delete()) throw new java.io.IOException("cannot prepare benchmark log: " + log);
        Os.setenv("DXVK_PERF_RUN", run, true);
        Os.setenv("DXVK_PERF_LOG", log.getAbsolutePath(), true);
    }

    /* DXVK reads dxvk.conf beside the game executable. Preserve unrelated
     * user options and replace only the HUD line managed by this launcher. */
    private void writeDxvkHudConfig(File game) throws Exception {
        File config = new File(game, "dxvk.conf");
        List<String> lines = new ArrayList<>();
        if (config.isFile()) {
            try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(config)))) {
                String line;
                while ((line = r.readLine()) != null) {
                    String normalized = line.trim().toLowerCase(java.util.Locale.ROOT);
                    if (normalized.startsWith("dxvk.hud") || normalized.startsWith("d3d9.maxframerate")) continue;
                    lines.add(line);
                }
            }
        }

        if (checks.get("hudEnabled").isChecked()) {
            String[] scales = {"0.75", "1.0", "1.25"};
            String[] aligns = {"left", "center", "right"};
            String[] vertical = {"top", "bottom"};
            String hud = "fps,scale=" + scales[Math.min(sel("hudSize"), 2)]
                    + ",align=" + aligns[Math.min(sel("hudAlign"), 2)]
                    + ",vertical=" + vertical[Math.min(sel("hudVertical"), 1)]
                    + ",game=" + (checks.get("hudGame").isChecked() ? "1" : "0")
                    + ",fg=" + (checks.get("hudGenerated").isChecked() ? "1" : "0")
                    + ",output=" + (checks.get("hudOutput").isChecked() ? "1" : "0")
                    + (checks.get("hudDriverInfo").isChecked() ? ",devinfo" : "");
            lines.add("dxvk.hud = " + hud);
        } else {
            lines.add("dxvk.hud = disabled");
        }

        int[] fpsValues = {0, 30, 40, 45, 50, 60, 90, 120};
        int gameFpsLimit = fpsValues[Math.max(0, Math.min(sel("gameFpsLimit"), fpsValues.length - 1))];
        lines.add("d3d9.maxFrameRate = " + gameFpsLimit);

        try (Writer w = new OutputStreamWriter(new FileOutputStream(config))) {
            for (String line : lines) w.write(line + "\n");
        }
    }

    /* LSFG reads only user-supplied Lossless.dll; shader/cache files are kept
     * in app-private storage and never copied into the game folder. */
    private void writeLsfgConfig(File game, boolean enabled) throws Exception {
        File port = new File(game, "port");
        if (!port.exists() && !port.mkdirs()) throw new java.io.IOException("cannot create port config folder");
        File dll = new File(getFilesDir(), "lsfg/Lossless.dll");
        File privateRoot = new File(getNoBackupFilesDir(), "lsfg");
        File cache = new File(privateRoot, "cache");
        File dxvkCache = new File(privateRoot, "dxvk");
        if (!cache.exists() && !cache.mkdirs()) throw new java.io.IOException("cannot create private LSFG cache");
        if (!dxvkCache.exists() && !dxvkCache.mkdirs()) throw new java.io.IOException("cannot create private DXVK cache");
        int multiplier = sel("lsfgMultiplier") + 2;
        float quality = (lsfgQuality.getProgress() + 1) / 10.0f;
        int mode = sel("lsfgMode"); // 0 performance, 1 stability, 2 low GPU load
        File config = new File(port, "lsfg.conf");
        try (Writer w = new OutputStreamWriter(new FileOutputStream(config))) {
            w.write("enabled=" + (enabled ? "1" : "0") + "\n");
            w.write("multiplier=" + multiplier + "\n");
            w.write(String.format(java.util.Locale.US, "flow_scale=%.1f\n", quality));
            w.write("performance=" + mode + "\n");
            w.write("dll_path=" + dll.getAbsolutePath() + "\n");
            w.write("shader_cache=" + cache.getAbsolutePath() + "\n");
            w.write("state_cache=" + dxvkCache.getAbsolutePath() + "\n");
        }
    }

    /* SkyrimPrefs.ini: keep what is there, start from the chosen preset file
     * in the game folder, then apply the individual options. */
    private void writePrefs(File game, File out) throws Exception {
        LinkedHashMap<String, LinkedHashMap<String, String>> ini = new LinkedHashMap<>();
        readIni(out, ini);
        File preset = findCi(game, PRESET_FILES[sel("preset")]);
        if (preset != null) readIni(preset, ini);
        int[] mode = modes(sel("aspect"))[sel("res")];
        put(ini, "Display", "iSize W", Integer.toString(mode[0]));
        put(ini, "Display", "iSize H", Integer.toString(mode[1]));
        put(ini, "Display", "bFull Screen", "1");
        put(ini, "Display", "iMultiSample", Integer.toString(AA_V[sel("aa")]));
        put(ini, "Display", "iMaxAnisotropy", Integer.toString(ANISO_V[sel("aniso")]));
        put(ini, "Display", "iTexMipMapSkip", Integer.toString(TEXQ_V[sel("texq")]));
        int sh = SHADOW_V[sel("shadow")];
        put(ini, "Display", "iShadowMapResolution", Integer.toString(sh));
        put(ini, "Display", "iShadowMapResolutionPrimary", Integer.toString(sh));
        put(ini, "Display", "iShadowMapResolutionSecondary", Integer.toString(Math.max(512, sh / 2)));
        int dec = DECALS_V[sel("decals")];
        put(ini, "Decals", "bDecals", dec > 0 ? "1" : "0");
        put(ini, "Decals", "bSkinnedDecals", dec > 0 ? "1" : "0");
        put(ini, "Decals", "uMaxDecals", Integer.toString(dec));
        put(ini, "Decals", "uMaxSkinDecals", Integer.toString(dec / 10));
        put(ini, "Display", "iMaxDecalsPerFrame", Integer.toString(dec / 10));
        put(ini, "Display", "iMaxSkinDecalsPerFrame", Integer.toString(dec / 40));
        put(ini, "Imagespace", "iRadialBlurLevel", Integer.toString(sel("blur")));
        String f = FADE_V[sel("fade")];
        put(ini, "LOD", "fLODFadeOutMultObjects", f);
        put(ini, "LOD", "fLODFadeOutMultItems", f);
        put(ini, "LOD", "fLODFadeOutMultActors", f);
        put(ini, "Display", "bFXAAEnabled", checks.get("fxaa").isChecked() ? "1" : "0");
        put(ini, "Display", "iPresentInterval", checks.get("vsync").isChecked() ? "1" : "0");
        put(ini, "MAIN", "bGamepadEnable", "1");
        put(ini, "Controls", "bGamePadRumble", "1");
        try (Writer wr = new OutputStreamWriter(new FileOutputStream(out))) {
            for (Map.Entry<String, LinkedHashMap<String, String>> s : ini.entrySet()) {
                wr.write("[" + s.getKey() + "]\r\n");
                for (Map.Entry<String, String> kv : s.getValue().entrySet()) wr.write(kv.getKey() + "=" + kv.getValue() + "\r\n");
                wr.write("\r\n");
            }
        }
    }

    private static void put(LinkedHashMap<String, LinkedHashMap<String, String>> ini, String sec, String k, String v) {
        String sk = sec;
        for (String s : ini.keySet()) if (s.equalsIgnoreCase(sec)) sk = s;
        LinkedHashMap<String, String> m = ini.get(sk);
        if (m == null) ini.put(sk, m = new LinkedHashMap<>());
        String kk = k;
        for (String s : m.keySet()) if (s.equalsIgnoreCase(k)) kk = s;
        m.put(kk, v);
    }

    private static void readIni(File f, LinkedHashMap<String, LinkedHashMap<String, String>> ini) {
        if (!f.exists()) return;
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f)))) {
            String sec = "General", line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith(";") || line.startsWith("#")) continue;
                if (line.startsWith("[")) {
                    int e = line.indexOf(']');
                    sec = (e > 0 ? line.substring(1, e) : line.substring(1)).trim();
                    continue;
                }
                int eq = line.indexOf('=');
                if (eq > 0) put(ini, sec, line.substring(0, eq).trim(), line.substring(eq + 1).trim());
            }
        } catch (Exception ignored) {
        }
    }
}
