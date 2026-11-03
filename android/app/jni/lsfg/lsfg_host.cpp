#include "lsfg_host.h"
#include "lsfg_extract.h"
#include "lsfg_translate.h"
#include "lsfg_present.h"
#include "lsfg_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <atomic>

#define LOGI(...) lsfg_log("I", __VA_ARGS__)
#define LOGW(...) lsfg_log("W", __VA_ARGS__)
#define LOGE(...) lsfg_log("E", __VA_ARGS__)

extern "C" void lsfg_register_vk_context(void*, void*, void*, unsigned);
extern "C" void lsfg_step3g_set_game_fps_limit_hook(int (*hook)(void));
extern "C" void lsfg_step3g_request_temporal_reset(void);
extern "C" void lsfg_set_vk_context_hook(void (*)(void*, void*, void*, unsigned));
extern "C" void lsfg_step3e_install(void);
extern "C" int lsfg_gpu_profiler_begin_session(int);
extern "C" void lsfg_step3e_set_queue(void *queue, unsigned family);
extern "C" void lsfg_step3g_set_copy_hook(void (*hook)(void*, void*, void*, uint32_t, uint32_t, uint32_t));
extern "C" void lsfg_step3g_copy_impl(void*, void*, void*, uint32_t, uint32_t, uint32_t);
extern "C" bool lsfg_step3g_can_generate(void);
extern "C" void* lsfg_step3g_generate_impl_opaque(
    void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
extern "C" void lsfg_step3g_set_generation_hooks(
    unsigned (*)(void), bool (*)(void),
    void* (*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t));

static LsfgConfig g_cfg;
static std::atomic<int> g_inited{0};
static std::atomic<int> g_runtime_enabled{0};
static std::atomic<int> g_multiplier{2};
static std::atomic<int> g_flow_percent{70};
static std::atomic<int> g_performance{1};
static std::atomic<int> g_game_fps_limit{0};
static char g_config_path[768];
static int g_logged_frame;
static thread_local int g_frame_multiplier = 0;
static thread_local float g_frame_flow_scale = -1.0f;
static int g_extract_count;

bool lsfg_load_config(const char *game_dir, LsfgConfig *out) {
    memset(out, 0, sizeof(*out));
    out->multiplier = 2;
    out->flow_scale = 0.70f;
    out->performance = 1;
    out->game_fps_limit = 0;
    if (!game_dir || !*game_dir) return false;

    lsfg_log_open(game_dir);

    char path[768];
    snprintf(path, sizeof path, "%s/port/lsfg.conf", game_dir);
    snprintf(g_config_path, sizeof g_config_path, "%s", path);
    FILE *f = fopen(path, "r");
    if (!f) {
        LOGI("no %s — LSFG off", path);
        return false;
    }
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line;
        char *val = eq + 1;
        while (*key == ' ' || *key == '\t') key++;
        while (*val == ' ' || *val == '\t') val++;
        for (char *p = val; *p; p++) if (*p == '\n' || *p == '\r') *p = 0;
        if (!strcmp(key, "enabled")) out->enabled = atoi(val);
        else if (!strcmp(key, "multiplier")) out->multiplier = atoi(val);
        else if (!strcmp(key, "flow_scale")) out->flow_scale = (float)atof(val);
        else if (!strcmp(key, "performance")) out->performance = atoi(val);
        else if (!strcmp(key, "game_fps_limit")) out->game_fps_limit = atoi(val);
        else if (!strcmp(key, "dll_path")) snprintf(out->dll_path, sizeof out->dll_path, "%s", val);
        else if (!strcmp(key, "shader_cache")) snprintf(out->shader_cache, sizeof out->shader_cache, "%s", val);
        else if (!strcmp(key, "state_cache")) snprintf(out->state_cache, sizeof out->state_cache, "%s", val);
    }
    fclose(f);
    if (out->multiplier < 1) out->multiplier = 1;
    if (out->multiplier > 4) out->multiplier = 4;
    if (out->flow_scale < 0.1f) out->flow_scale = 0.1f;
    if (out->flow_scale > 1.0f) out->flow_scale = 1.0f;
    if (out->performance < 0 || out->performance > 2) out->performance = 1;
    if (out->game_fps_limit != 15 && out->game_fps_limit != 20
     && out->game_fps_limit != 25 && out->game_fps_limit != 30
     && out->game_fps_limit != 60) out->game_fps_limit = 0;
    if (!out->shader_cache[0] && game_dir[0])
        snprintf(out->shader_cache, sizeof out->shader_cache, "%s/port/lsfg_spirv", game_dir);
    LOGI("config enabled=%d mult=%d flow=%.2f perf=%d",
         out->enabled, out->multiplier, out->flow_scale, out->performance);
    LOGI("dll=%s", out->dll_path);
    LOGI("cache=%s", out->shader_cache);
    LOGI("DXVK state cache=%s", out->state_cache);
    if (out->state_cache[0])
        setenv("DXVK_STATE_CACHE_PATH", out->state_cache, 1);
    return true;
}

bool lsfg_host_init(const LsfgConfig *cfg) {
    g_inited = 0;
    g_runtime_enabled.store(0, std::memory_order_release);
    g_logged_frame = 0;
    g_extract_count = 0;
    if (!cfg) return false;
    g_cfg = *cfg;
    g_multiplier.store(cfg->multiplier, std::memory_order_relaxed);
    g_flow_percent.store((int)(cfg->flow_scale * 100.0f + 0.5f), std::memory_order_relaxed);
    g_performance.store(cfg->performance, std::memory_order_relaxed);
    g_game_fps_limit.store(cfg->game_fps_limit, std::memory_order_relaxed);

    LOGI("=== LSFG host init ===");
    if (!lsfg_gpu_profiler_begin_session(cfg->enabled && cfg->multiplier > 1))
        LOGW("LSFG GPU profiler CSV session could not be initialized");
    LOGI("STATUS: step1 extract + step2 SPIR-V + step3 present hook");

    if (!cfg->enabled) {
        LOGI("LSFG disabled in config (enabled=%d mult=%d)", cfg->enabled, cfg->multiplier);
        return false;
    }
    FILE *dll = cfg->dll_path[0] ? fopen(cfg->dll_path, "rb") : NULL;
    if (!dll) {
        LOGW("Lossless.dll not found at '%s'", cfg->dll_path);
        return false;
    }
    fclose(dll);
    LOGI("Lossless.dll found");

    if (!cfg->shader_cache[0]) {
        LOGW("shader_cache path empty — skip extract");
        return false;
    }

    int n = lsfg_extract_dll(cfg->dll_path, cfg->shader_cache);
    if (n < 0) {
        LOGW("extract failed (see messages above)");
        return false;
    }
    if (n == 0) {
        LOGW("DLL has no RT_RCDATA resources");
        return false;
    }
    g_extract_count = n;
    LOGI("extract OK: %d RCDATA blob(s) in %s", n, cfg->shader_cache);

    int tn = lsfg_translate_cache(cfg->shader_cache);
    if (tn < 0) {
        LOGW("SPIR-V translate hard-failed");
        return false;
    }
    if (tn == 0) {
        LOGW("SPIR-V translate produced 0 modules — FG cannot run");
        return false;
    }
    LOGI("translate OK: %d SPIR-V module(s)", tn);
    lsfg_host_set_spirv_count(tn);
    lsfg_host_set_cache_dir(cfg->shader_cache);

    lsfg_set_vk_context_hook(lsfg_register_vk_context);
    lsfg_step3g_set_game_fps_limit_hook(lsfg_host_game_fps_limit);
    LOGI("LSFG: Vk context hook installed (device+phys+queue on DxvkDevice create)");
    lsfg_step3e_install();
    lsfg_step3g_set_copy_hook(lsfg_step3g_copy_impl);
    lsfg_step3g_set_generation_hooks(
        lsfg_host_generation_count,
        lsfg_step3g_can_generate,
        lsfg_step3g_generate_impl_opaque);
    LOGI("LSFG: DXVK frame-copy and generated-frame callbacks registered");

    LOGI("STEP3: present hook + step3e image hook installed");
    g_runtime_enabled.store(cfg->multiplier > 1 ? 1 : 0, std::memory_order_release);
    g_inited = 1;
    return true;
}

void lsfg_host_shutdown(void) {
    if (g_inited.load(std::memory_order_acquire)) LOGI("LSFG shutdown");
    g_runtime_enabled.store(0, std::memory_order_release);
    g_inited.store(0, std::memory_order_release);
    lsfg_log_close();
}

void lsfg_host_begin_frame_settings(void) {
    g_frame_multiplier = g_multiplier.load(std::memory_order_acquire);
    const float quality = (float)g_flow_percent.load(std::memory_order_acquire) / 100.0f;
    const int selected = g_performance.load(std::memory_order_acquire);
    static const float modeScale[] = { 0.75f, 1.0f, 0.5f };
    const unsigned mode = selected >= 0 && selected <= 2 ? (unsigned)selected : 1u;
    g_frame_flow_scale = quality * modeScale[mode];
}

unsigned lsfg_host_generation_count(void) {
    if (!g_inited.load(std::memory_order_acquire)) return 0;
    const int multiplier = g_frame_multiplier > 0
        ? g_frame_multiplier : g_multiplier.load(std::memory_order_relaxed);
    if (multiplier <= 1) return 0;
    unsigned count = (unsigned)(multiplier - 1);
    return count > 3 ? 3 : count;
}

float lsfg_host_flow_scale(void) {
    if (g_frame_flow_scale > 0.0f) return g_frame_flow_scale;
    const float quality = (float)g_flow_percent.load(std::memory_order_relaxed) / 100.0f;
    static const float modeScale[] = { 0.75f, 1.0f, 0.5f };
    const int selected = g_performance.load(std::memory_order_relaxed);
    const unsigned mode = selected >= 0 && selected <= 2 ? (unsigned)selected : 1u;
    return quality * modeScale[mode];
}

int lsfg_host_get_settings(int *multiplier, int *flow_percent, int *performance, int *game_fps_limit) {
    if (multiplier) *multiplier = g_multiplier.load(std::memory_order_relaxed);
    if (flow_percent) *flow_percent = g_flow_percent.load(std::memory_order_relaxed);
    if (performance) *performance = g_performance.load(std::memory_order_relaxed);
    if (game_fps_limit) *game_fps_limit = g_game_fps_limit.load(std::memory_order_relaxed);
    return g_inited.load(std::memory_order_acquire) ? 1 : 0;
}

bool lsfg_host_update_settings(int multiplier, int flow_percent, int performance, int game_fps_limit) {
    if (!g_inited.load(std::memory_order_acquire)) return false;
    if (multiplier < 1 || multiplier > 4 || flow_percent < 40 || flow_percent > 100
        || performance < 0 || performance > 2
        || (game_fps_limit != 0 && game_fps_limit != 15 && game_fps_limit != 20
            && game_fps_limit != 25 && game_fps_limit != 30 && game_fps_limit != 60)) return false;

    // The UI thread updates atomics only. Vulkan resources are rebuilt lazily
    // by the DXVK render thread after it observes a changed effective scale.
    const int wasActive = g_runtime_enabled.load(std::memory_order_acquire);
    const int willBeActive = multiplier > 1 ? 1 : 0;
    if (wasActive != willBeActive)
        lsfg_step3g_request_temporal_reset();
    g_runtime_enabled.store(willBeActive, std::memory_order_release);
    g_multiplier.store(multiplier, std::memory_order_release);
    g_flow_percent.store(flow_percent, std::memory_order_release);
    g_performance.store(performance, std::memory_order_release);
    g_game_fps_limit.store(game_fps_limit, std::memory_order_release);
    g_cfg.multiplier = multiplier;
    g_cfg.flow_scale = (float)flow_percent / 100.0f;
    g_cfg.performance = performance;
    g_cfg.game_fps_limit = game_fps_limit;

    if (g_config_path[0]) {
        char temporary[800];
        snprintf(temporary, sizeof temporary, "%s.tmp", g_config_path);
        FILE *file = fopen(temporary, "w");
        if (file) {
            fprintf(file, "enabled=%d\nmultiplier=%d\nflow_scale=%.2f\nperformance=%d\ngame_fps_limit=%d\n",
                    g_cfg.enabled, multiplier, (double)g_cfg.flow_scale, performance, game_fps_limit);
            if (g_cfg.dll_path[0]) fprintf(file, "dll_path=%s\n", g_cfg.dll_path);
            if (g_cfg.shader_cache[0]) fprintf(file, "shader_cache=%s\n", g_cfg.shader_cache);
            if (g_cfg.state_cache[0]) fprintf(file, "state_cache=%s\n", g_cfg.state_cache);
            const int ok = fclose(file) == 0 && rename(temporary, g_config_path) == 0;
            if (!ok) {
                remove(temporary);
                LOGW("could not persist live settings to %s", g_config_path);
            }
        } else {
            LOGW("could not open live settings file %s", temporary);
        }
    }
    LOGI("live settings applied multiplier=%dx flow=%.2f performance=%d game_fps_limit=%d",
         multiplier, (double)g_cfg.flow_scale, performance, game_fps_limit);
    return true;
}

bool lsfg_host_active(void) {
    return g_inited.load(std::memory_order_acquire) != 0
        && g_runtime_enabled.load(std::memory_order_acquire) != 0;
}

int lsfg_host_extract_count(void) {
    return g_extract_count;
}

void lsfg_host_on_frame(void) {
    if (!g_inited) return;
    if (g_logged_frame) return;
    g_logged_frame = 1;
    LOGI("on_frame: extract_count=%d spirv=%d (present hook is authoritative)",
         g_extract_count, lsfg_host_spirv_count());
}

extern "C" int lsfg_host_game_fps_limit(void) { return g_game_fps_limit.load(std::memory_order_acquire); }
