#include "lsfg_host.h"
#include "lsfg_extract.h"
#include "lsfg_translate.h"
#include "lsfg_present.h"
#include "lsfg_log.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define LOGI(...) lsfg_log("I", __VA_ARGS__)
#define LOGW(...) lsfg_log("W", __VA_ARGS__)
#define LOGE(...) lsfg_log("E", __VA_ARGS__)

static LsfgConfig g_cfg;
static int g_inited;
static int g_logged_frame;
static int g_extract_count;

bool lsfg_load_config(const char *game_dir, LsfgConfig *out) {
    memset(out, 0, sizeof(*out));
    out->multiplier = 2;
    out->flow_scale = 0.70f;
    out->performance = 1;
    if (!game_dir || !*game_dir) return false;

    lsfg_log_open(game_dir);

    char path[768];
    snprintf(path, sizeof path, "%s/port/lsfg.conf", game_dir);
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
        else if (!strcmp(key, "dll_path")) snprintf(out->dll_path, sizeof out->dll_path, "%s", val);
        else if (!strcmp(key, "shader_cache")) snprintf(out->shader_cache, sizeof out->shader_cache, "%s", val);
    }
    fclose(f);
    if (out->multiplier < 1) out->multiplier = 1;
    if (out->multiplier > 4) out->multiplier = 4;
    if (!out->shader_cache[0] && game_dir[0])
        snprintf(out->shader_cache, sizeof out->shader_cache, "%s/port/lsfg_spirv", game_dir);
    LOGI("config enabled=%d mult=%d flow=%.2f perf=%d",
         out->enabled, out->multiplier, out->flow_scale, out->performance);
    LOGI("dll=%s", out->dll_path);
    LOGI("cache=%s", out->shader_cache);
    return true;
}

bool lsfg_host_init(const LsfgConfig *cfg) {
    g_inited = 0;
    g_logged_frame = 0;
    g_extract_count = 0;
    if (!cfg) return false;
    g_cfg = *cfg;

    LOGI("=== LSFG host init ===");
    LOGI("STATUS: step1 extract + step2 SPIR-V + step3 present hook");

    if (!cfg->enabled || cfg->multiplier <= 1) {
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
    LOGI("STEP3: present hook will log on first Present(); FG compute still TODO");
    g_inited = 1;
    return true;
}

void lsfg_host_shutdown(void) {
    if (g_inited) LOGI("LSFG shutdown");
    g_inited = 0;
    lsfg_log_close();
}

bool lsfg_host_active(void) {
    return g_inited != 0;
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
