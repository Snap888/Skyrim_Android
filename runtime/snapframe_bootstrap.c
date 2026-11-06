#include "snapframe_host.h"
#include "snap_ai/snap_ai.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void snapframe_bootstrap(void) {
    extern char g_game_dir[1024];
    SnapFrameConfig config;
    snapframe_config_defaults(&config);

    char path[1200];
    char logPath[1200];
    char gpuLogPath[1200];
    char flowLogPath[1200];
    char flowVectorsLogPath[1200];
    char eventLogPath[1200];
    snprintf(path, sizeof(path), "%s/port/snapframe.conf", g_game_dir);
    snprintf(logPath, sizeof(logPath), "%s/port/snapframe.log", g_game_dir);
    snprintf(gpuLogPath, sizeof(gpuLogPath), "%s/port/snapframe_gpu.csv", g_game_dir);
    snprintf(flowLogPath, sizeof(flowLogPath), "%s/port/snapframe_flow.csv", g_game_dir);
    snprintf(flowVectorsLogPath, sizeof(flowVectorsLogPath), "%s/port/snapframe_flow_vectors.csv", g_game_dir);
    snprintf(eventLogPath, sizeof(eventLogPath), "%s/port/snapframe_events.csv", g_game_dir);
    snapframe_host_set_log_path(logPath);
    snapframe_host_set_gpu_log_path(gpuLogPath);
    snapframe_host_set_flow_log_path(flowLogPath);
    snapframe_host_set_flow_vectors_log_path(flowVectorsLogPath);
    snapframe_host_set_event_log_path(eventLogPath);
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
            else if (!strcmp(key, "present_mode"))
                config.present_mode = atoi(eq);
            else if (!strcmp(key, "motion_analysis"))
                config.motion_analysis = atoi(eq) != 0;
            else if (!strcmp(key, "profiling_enabled"))
                config.profiling_enabled = atoi(eq) != 0;
            else if (!strcmp(key, "diagnostic_suite"))
                config.diagnostic_suite = atoi(eq) != 0;
            else if (!strcmp(key, "diagnostic_phase_frames"))
                config.diagnostic_phase_frames = atoi(eq);
            else if (!strcmp(key, "ai_mode"))
                config.ai_mode = atoi(eq);
            else if (!strcmp(key, "ai_gpu_budget_ms"))
                config.ai_gpu_budget_ms = (float)atof(eq);
            else if (!strcmp(key, "ai_pressure_threshold"))
                config.ai_pressure_threshold = (float)atof(eq);
            else if (!strcmp(key, "ai_headroom_threshold"))
                config.ai_headroom_threshold = (float)atof(eq);
            else if (!strcmp(key, "ai_artifact_threshold"))
                config.ai_artifact_threshold = (float)atof(eq);
        }
        fclose(file);
    }

    snapframe_host_log("bootstrap entered");
    snapframe_host_init(&config);
    const uint32_t snapAiApi = snap_ai_get_api_version();
    fprintf(stderr, "SnapAI: library loaded api=%u expected=%u adaptive_control=pending_telemetry_adapter\n",
        snapAiApi, SNAP_AI_API_VERSION);
    char summary[1400];
    snprintf(summary, sizeof(summary), "config=%s log=%s gpu_csv=%s flow_csv=%s events_csv=%s enabled=%d multiplier=%d quality=%d flow_scale=%d present_mode=%d motion_analysis=%d profiling=%d diagnostic_suite=%d phase_frames=%d",
             path, logPath, gpuLogPath, flowLogPath, eventLogPath, config.enabled, config.multiplier,
             config.quality, config.flow_scale_percent, config.present_mode, config.motion_analysis,
             config.profiling_enabled, config.diagnostic_suite, config.diagnostic_phase_frames);
    snapframe_host_log(summary);
}
