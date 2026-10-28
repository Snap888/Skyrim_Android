#include "lsfg_host.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "skyrim-lsfg", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "skyrim-lsfg", __VA_ARGS__)
#else
#define LOGI(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
#define LOGW(...) LOGI(__VA_ARGS__)
#endif

static LsfgConfig g_cfg;
static int g_inited;
static int g_logged_frame;

bool lsfg_load_config(const char *game_dir, LsfgConfig *out) {
    memset(out, 0, sizeof(*out));
    out->multiplier = 2;
    out->flow_scale = 0.70f;
    out->performance = 1;
    if (!game_dir || !*game_dir) return false;
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
    LOGI("config enabled=%d mult=%d flow=%.2f perf=%d dll=%s",
         out->enabled, out->multiplier, out->flow_scale, out->performance, out->dll_path);
    return true;
}

bool lsfg_host_init(const LsfgConfig *cfg) {
    g_inited = 0;
    g_logged_frame = 0;
    if (!cfg) return false;
    g_cfg = *cfg;
    if (!cfg->enabled || cfg->multiplier <= 1) {
        LOGI("LSFG disabled in config");
        return false;
    }
    FILE *dll = cfg->dll_path[0] ? fopen(cfg->dll_path, "rb") : NULL;
    if (!dll) {
        LOGW("LSFG enabled but Lossless.dll not found at '%s' — FG inactive", cfg->dll_path);
        return false;
    }
    fclose(dll);
    LOGW("LSFG: DLL found; compute chain NOT YET IMPLEMENTED "
         "(scaffold only — see docs/LSFG_IMPLEMENTATION.md). "
         "No frames will be interpolated until dxvk present hook + shader extract land.");
    g_inited = 1;
    return true;
}

void lsfg_host_shutdown(void) {
    if (g_inited) LOGI("LSFG shutdown");
    g_inited = 0;
}

bool lsfg_host_active(void) {
    return g_inited != 0;
}

void lsfg_host_on_frame(void) {
    if (!g_inited || g_logged_frame) return;
    g_logged_frame = 1;
    LOGI("LSFG on_frame stub (no interpolation yet)");
}
