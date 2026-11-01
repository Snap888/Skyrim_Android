package com.tj.skyrim;

import android.content.Context;
import android.net.Uri;
import android.os.Build;
import org.json.JSONObject;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Imports user-provided Android Vulkan driver packages into private app storage. */
final class TurnipDriverManager {
    private static final long MAX_UNPACKED = 256L * 1024L * 1024L;
    private static final int MAX_ENTRIES = 256;
    private final Context context;
    private final File root;

    TurnipDriverManager(Context context) {
        this.context = context.getApplicationContext();
        root = new File(context.getFilesDir(), "gpu-drivers");
    }

    static final class Driver {
        final String id, name, version, library;
        Driver(String id, String name, String version, String library) {
            this.id = id; this.name = name; this.version = version; this.library = library;
        }
        @Override public String toString() { return name + " (" + version + ")"; }
    }

    List<Driver> list() throws Exception {
        List<Driver> result = new ArrayList<>();
        File[] dirs = root.listFiles();
        if (dirs == null) return result;
        for (File dir : dirs) {
            if (!dir.isDirectory() || dir.getName().startsWith(".")) continue;
            try { result.add(readDriver(dir)); } catch (Exception ignored) { }
        }
        return result;
    }

    Driver importZip(Uri uri) throws Exception {
        if (!root.exists() && !root.mkdirs()) throw new java.io.IOException("cannot create private driver directory");
        File staging = new File(root, ".import-" + System.nanoTime());
        if (!staging.mkdirs()) throw new java.io.IOException("cannot create staging directory");
        long total = 0;
        int count = 0;
        try (InputStream raw = context.getContentResolver().openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(raw)) {
            if (raw == null) throw new java.io.IOException("selected ZIP cannot be opened");
            ZipEntry entry;
            byte[] buffer = new byte[65536];
            while ((entry = zip.getNextEntry()) != null) {
                if (++count > MAX_ENTRIES) throw new java.io.IOException("ZIP contains too many entries");
                String name = entry.getName().replace('\\\\', '/');
                if (name.startsWith("/") || name.contains("../") || name.equals("..")
                        || name.startsWith("../") || name.contains("/..")) {
                    throw new java.io.IOException("unsafe ZIP entry path: " + name);
                }
                File out = new File(staging, name);
                String base = staging.getCanonicalPath() + File.separator;
                if (!out.getCanonicalPath().startsWith(base)) throw new java.io.IOException("ZIP path escapes destination");
                if (entry.isDirectory()) {
                    if (!out.mkdirs() && !out.isDirectory()) throw new java.io.IOException("cannot create " + name);
                } else {
                    File parent = out.getParentFile();
                    if (parent != null && !parent.mkdirs() && !parent.isDirectory())
                        throw new java.io.IOException("cannot create ZIP directory");
                    try (FileOutputStream file = new FileOutputStream(out)) {
                        for (int n; (n = zip.read(buffer)) > 0; ) {
                            total += n;
                            if (total > MAX_UNPACKED) throw new java.io.IOException("ZIP expands beyond 256 MiB");
                            file.write(buffer, 0, n);
                        }
                        file.getFD().sync();
                    }
                }
                zip.closeEntry();
            }
            Driver driver = readDriver(staging);
            String id = safeId(driver.name + "-" + driver.version);
            File destination = new File(root, id);
            if (destination.exists()) deleteTree(destination);
            if (!staging.renameTo(destination)) throw new java.io.IOException("cannot install driver into private storage");
            return readDriver(destination);
        } catch (Exception e) {
            deleteTree(staging);
            throw e;
        }
    }

    private Driver readDriver(File dir) throws Exception {
        File meta = findFile(dir, "meta.json");
        if (meta == null) throw new java.io.IOException("meta.json is missing");
        byte[] bytes = new byte[(int)Math.min(meta.length(), 65536)];
        try (FileInputStream in = new FileInputStream(meta)) {
            int n = in.read(bytes);
            if (n <= 0) throw new java.io.IOException("meta.json is empty");
        }
        JSONObject json = new JSONObject(new String(bytes, java.nio.charset.StandardCharsets.UTF_8));
        String name = json.optString("name", "").trim();
        String version = json.optString("driverVersion", json.optString("packageVersion", "")).trim();
        String library = json.optString("libraryName", "").trim();
        int minApi = json.optInt("minApi", 1);
        if (name.isEmpty() || version.isEmpty() || library.isEmpty())
            throw new java.io.IOException("meta.json must define name, driverVersion and libraryName");
        if (minApi > Build.VERSION.SDK_INT) throw new java.io.IOException("driver requires Android API " + minApi);
        if (library.contains("/") || library.contains("\\\\") || !library.endsWith(".so"))
            throw new java.io.IOException("invalid libraryName in meta.json");
        File lib = findFile(dir, library);
        if (lib == null || !lib.isFile()) throw new java.io.IOException(library + " is missing");
        try (RandomAccessFile elf = new RandomAccessFile(lib, "r")) {
            if (elf.length() < 20 || elf.readInt() != 0x7f454c46) throw new java.io.IOException("driver library is not ELF");
            elf.seek(18);
            int machine = elf.readUnsignedByte() | (elf.readUnsignedByte() << 8);
            if (machine != 183) throw new java.io.IOException("driver library is not AArch64 (EM_AARCH64)");
        }
        return new Driver(dir.getName(), name, version, library);
    }

    private static File findFile(File dir, String name) {
        File direct = new File(dir, name);
        if (direct.isFile()) return direct;
        File[] children = dir.listFiles();
        if (children != null) for (File child : children) {
            if (child.isDirectory()) {
                File found = findFile(child, name);
                if (found != null) return found;
            }
        }
        return null;
    }

    String driverDirectory(String id) throws Exception {
        File dir = new File(root, id).getCanonicalFile();
        if (!dir.getParentFile().equals(root.getCanonicalFile())) throw new java.io.IOException("invalid driver id");
        return dir.getAbsolutePath() + File.separator;
    }

    Driver get(String id) throws Exception { return readDriver(new File(root, id)); }

    private static String safeId(String value) {
        String id = value.toLowerCase(Locale.ROOT).replaceAll("[^a-z0-9._-]+", "-");
        return id.length() > 72 ? id.substring(0, 72) : id;
    }

    static void deleteTree(File file) {
        if (file == null || !file.exists()) return;
        File[] children = file.listFiles();
        if (children != null) for (File child : children) deleteTree(child);
        file.delete();
    }
}
