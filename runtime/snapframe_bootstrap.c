#include "snapframe_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void snapframe_bootstrap(void) {
    extern char g_game_dir[1024];
    SnapFrameConfig config;
    snapframe_config_defaults(&config);

    char path[1200];
    snprintf(path, sizeof(path), "%s/port/snapframe.conf", g_game_dir);
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
        }
        fclose(file);
    }

    snapframe_host_init(&config);
    fprintf(stderr, "SnapFrame: config=%s enabled=%d (prototype captures frames only)\n",
            path, config.enabled);
}
