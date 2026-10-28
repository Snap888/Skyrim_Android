#include "lsfg_log.h"
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif

static FILE *g_dl;
static FILE *g_port;
static int g_opened;

static void try_open(FILE **fp, const char *path, const char *mode) {
    if (*fp) return;
    *fp = fopen(path, mode);
}

void lsfg_log_open(const char *game_dir) {
    if (g_opened) return;
    g_opened = 1;

    try_open(&g_dl, "/storage/emulated/0/Download/log", "a");
    if (!g_dl)
        try_open(&g_dl, "/sdcard/Download/log", "a");

    if (game_dir && game_dir[0]) {
        char path[900];
        snprintf(path, sizeof path, "%s/port", game_dir);
        mkdir(path, 0777);
        snprintf(path, sizeof path, "%s/port/lsfg.log", game_dir);
        try_open(&g_port, path, "a");
    }

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    if (g_dl)
        fprintf(g_dl, "\n===== skyrim-lsfg session %s =====\n", ts);
    if (g_port)
        fprintf(g_port, "\n===== skyrim-lsfg session %s =====\n", ts);
    if (g_dl) fflush(g_dl);
    if (g_port) fflush(g_port);

#ifdef __ANDROID__
    __android_log_print(ANDROID_LOG_INFO, "skyrim-lsfg",
                        "file log: Download/log %s, port/lsfg.log %s",
                        g_dl ? "OK" : "FAIL", g_port ? "OK" : "FAIL");
#endif
}

void lsfg_log_close(void) {
    if (g_dl) { fclose(g_dl); g_dl = NULL; }
    if (g_port) { fclose(g_port); g_port = NULL; }
    g_opened = 0;
}

void lsfg_log(const char *level, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);

#ifdef __ANDROID__
    int prio = ANDROID_LOG_INFO;
    if (level && level[0] == 'W') prio = ANDROID_LOG_WARN;
    if (level && level[0] == 'E') prio = ANDROID_LOG_ERROR;
    __android_log_print(prio, "skyrim-lsfg", "%s", buf);
#else
    fprintf(stderr, "[lsfg] %s\n", buf);
#endif

    if (!g_opened) lsfg_log_open(NULL);

    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    char ts[32];
    strftime(ts, sizeof ts, "%H:%M:%S", &tm);

    if (g_dl) {
        fprintf(g_dl, "%s [%s] %s\n", ts, level ? level : "I", buf);
        fflush(g_dl);
    }
    if (g_port) {
        fprintf(g_port, "%s [%s] %s\n", ts, level ? level : "I", buf);
        fflush(g_port);
    }
}
