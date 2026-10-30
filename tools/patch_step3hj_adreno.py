#!/usr/bin/env python3
"""Apply Adreno-safe patches to lsfg_step3hj.cpp (half-res, fence watchdog, cooldown)."""
from pathlib import Path
import sys

p = Path("android/app/jni/lsfg/lsfg_step3hj.cpp")
if not p.exists():
    print("missing", p, file=sys.stderr)
    sys.exit(1)
src = p.read_text()

old = """static bool ensure_imgs(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h) {
    if (g_imgs_ok && g_gen) return true;
    if (!create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, &g_gen, &g_gen_mem, &g_gen_view))
        return false;
    uint32_t mw = w, mh = h;"""
new = """static bool ensure_imgs(VkDevice dev, VkPhysicalDevice phys, uint32_t w, uint32_t h) {
    if (g_imgs_ok && g_gen) return true;
    uint32_t cap = 1280;
    if (w > cap || h > cap) {
        if (w >= h) { h = (uint32_t)((uint64_t)h * cap / w); w = cap; }
        else        { w = (uint32_t)((uint64_t)w * cap / h); h = cap; }
        if (w < 1) w = 1; if (h < 1) h = 1;
        lsfg_log("I", "step3h-j: intermediate capped to %ux%u (Adreno-safe)", w, h);
    }
    if (!create_img(dev, phys, w, h, VK_FORMAT_R8G8B8A8_UNORM, &g_gen, &g_gen_mem, &g_gen_view))
        return false;
    uint32_t mw = w, mh = h;"""
if old not in src:
    raise SystemExit("ensure_imgs pattern missing")
src = src.replace(old, new, 1)

old = """static void poll_fence(VkDevice dev) {
    if (g_inflight < 0) return;
    if (vkGetFenceStatus(dev, g_fence[g_inflight]) == VK_SUCCESS) {
        g_gen_ready = 1;
        g_inflight = -1;
    }
}"""
new = """static void poll_fence(VkDevice dev) {
    if (g_inflight < 0) return;
    VkResult st = vkGetFenceStatus(dev, g_fence[g_inflight]);
    if (st == VK_SUCCESS) {
        g_gen_ready = 1;
        g_inflight = -1;
        return;
    }
    static unsigned s_busy;
    if (st == VK_NOT_READY) {
        s_busy++;
        if (s_busy > 120) {
            lsfg_log("E", "step3h-j: fence busy >120 frames — GPU hang risk, FG OFF");
            g_fg_live = 0; g_fg_inject = 0; g_inflight = -1; g_gen_ready = 0; s_busy = 0;
        }
    } else {
        lsfg_log("W", "step3h-j: fence status %d — disable FG", (int)st);
        g_fg_live = 0; g_fg_inject = 0; g_inflight = -1; g_gen_ready = 0; s_busy = 0;
    }
}"""
if old not in src:
    raise SystemExit("poll_fence pattern missing")
src = src.replace(old, new, 1)

if "static int g_cooldown;" not in src:
    src = src.replace(
        "static unsigned g_inject_n;",
        "static unsigned g_inject_n;\nstatic int g_cooldown;",
        1)

old = """static bool submit_full(VkDevice dev) {
    if (!g_ready || g_inflight >= 0) return false;"""
new = """static bool submit_full(VkDevice dev) {
    if (!g_ready || g_inflight >= 0) return false;
    if (g_cooldown > 0) { g_cooldown--; return false; }"""
if old not in src:
    raise SystemExit("submit_full guard missing")
src = src.replace(old, new, 1)

old = '''extern "C" void lsfg_step3hj_note_inject(void) {
    g_inject_n++;
    g_gen_ready = 0;
    if (g_inject_n <= 5 || g_inject_n == 60 || (g_inject_n % 300) == 0)
        lsfg_log("I", "step3h-j: INJECT #%u (gen→swapchain GPU)", g_inject_n);
}'''
new = '''extern "C" void lsfg_step3hj_note_inject(void) {
    g_inject_n++;
    g_gen_ready = 0;
    g_cooldown = 3;
    if (g_inject_n <= 5 || g_inject_n == 60 || (g_inject_n % 300) == 0)
        lsfg_log("I", "step3h-j: INJECT #%u (gen→swapchain GPU)", g_inject_n);
}'''
if old not in src:
    raise SystemExit("note_inject missing")
src = src.replace(old, new, 1)

src = src.replace("if (n < 10) return;", "if (n < 30) return;", 1)
src = src.replace(
    "if (g_fg_live && g_throttle < 2) g_throttle = 2;",
    "if (g_fg_live && g_throttle < 4) g_throttle = 4;",
    1)

p.write_text(src)
print("step3hj Adreno-safe OK", p.stat().st_size)
