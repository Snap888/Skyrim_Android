package com.tj.skyrim;

import android.content.Context;
import android.content.SharedPreferences;

import java.io.File;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

/**
 * Selects a complete DXVK + game-runtime pair before SDL loads any native
 * library. A variant is selectable only when both ELF objects are packaged.
 * Never attempts to unload or replace a loaded Vulkan implementation.
 */
public final class DxvkRuntimeSelector {
    public static final String PREFS = "launcher";
    public static final String PREF_VERSION_INDEX = "dxvkVersion";

    public static final class Variant {
        public final String id;
        public final String label;
        public final String dxvkLibrary;
        public final String mainLibrary;

        private Variant(String id, String label, String dxvkLibrary, String mainLibrary) {
            this.id = id;
            this.label = label;
            this.dxvkLibrary = dxvkLibrary;
            this.mainLibrary = mainLibrary;
        }
    }

    // The baseline is the Android-adapted native-1.9.2b implementation.
    private static final Variant BASELINE = new Variant(
            "native-1.9.2b", "Android DXVK Native 1.9.2b (current)",
            "dxvk_d3d9", "main");

    // Reserved SONAME pair for the separately ported upstream Native 2.6.2.
    // It is deliberately not advertised until both libraries exist in the APK.
    private static final Variant NATIVE_262 = new Variant(
            "native-2.6.2", "DXVK Native 2.6.2",
            "dxvk_d3d9_262", "main_262");

    private DxvkRuntimeSelector() {}

    public static List<Variant> available(Context context) {
        if (context == null) return Collections.singletonList(BASELINE);
        File dir = new File(context.getApplicationInfo().nativeLibraryDir);
        List<Variant> variants = new ArrayList<>();
        // Always retain a safe baseline option. Its presence is validated at load time.
        variants.add(BASELINE);
        if (hasPair(dir, NATIVE_262)) variants.add(NATIVE_262);
        return variants;
    }

    public static Variant resolve(Context context) {
        List<Variant> variants = available(context);
        SharedPreferences prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        int selected = prefs.getInt(PREF_VERSION_INDEX, 0);
        if (selected < 0 || selected >= variants.size()) return BASELINE;
        Variant variant = variants.get(selected);
        File dir = new File(context.getApplicationInfo().nativeLibraryDir);
        return hasPair(dir, variant) ? variant : BASELINE;
    }

    public static String[] labels(Context context) {
        List<Variant> variants = available(context);
        String[] labels = new String[variants.size()];
        for (int i = 0; i < variants.size(); i++) labels[i] = variants.get(i).label;
        return labels;
    }

    private static boolean hasPair(File dir, Variant variant) {
        return new File(dir, "lib" + variant.dxvkLibrary + ".so").isFile()
                && new File(dir, "lib" + variant.mainLibrary + ".so").isFile();
    }
}
