#include "snapframe_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void snapframe_bootstrap(void) {
    extern char g_game_dir[1024];
    SnapFrameConfig config;
    snapframe_config_defaults(&config);

    char path[1200];
    char logPath[1200];
    snprintf(path, sizeof(path), "%s/port/snapframe.conf", g_game_dir);
    snprintf(logPath, sizeof(logPath), "%s/port/snapframe.log", g_game_dir);
    snapframe_host_set_log_path(logPath);
    FILE *file = fopen(path, "r");
    if (file) {
        char line[512];
        while (fgets(line, sizeof(line), file)) {
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq++ = 0;
            char *key = line;
            while (*key == ' ' || *key == '\t') ++key;
            while (*eq == ' ' || *eq == '\t') ++eq;
            char *end = eq + strlen(eq);
            while (end > eq && (end[-1] == '\n' || end[-1] == '\r'
                              || end[-1] == ' ' || end[-1] == '\t'))
                *--end = 0;

            if (!strcmp(key, "enabled"))
                config.enabled = atoi(eq) != 0;
            else if (!strcmp(key, "multiplier"))
                config.multiplier = atoi(eq);
            else if (!strcmp(key, "quality"))
                config.quality = atoi(eq);
            else if (!strcmp(key, "flow_scale_percent"))
                config.flow_scale_percent = atoi(eq);
            else if (!strcmp(key, "profiling_enabled"))
                config.profiling_enabled = atoi(eq) != 0;
            else if (!strcmp(key, "diagnostic_suite"))
                config.diagnostic_suite = atoi(eq) != 0;
            else if (!strcmp(key, "diagnostic_phase_frames"))
                config.diagnostic_phase_frames = atoi(eq);
        }
        fclose(file);
    }

    snapframe_host_log("bootstrap entered");
    snapframe_host_init(&config);
    char summary[1400];
    snprintf(summary, sizeof(summary), "config=%s log=%s enabled=%d multiplier=%d quality=%d flow_scale=%d profiling=%d",
             path, logPath, config.enabled, config.multiplier, config.quality,
             config.flow_scale_percent, config.profiling_enabled);
    snapframe_host_log(summary);
}
