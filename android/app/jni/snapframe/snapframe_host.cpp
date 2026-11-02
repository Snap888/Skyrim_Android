#include "snapframe_host.h"

#include <atomic>
#include <string.h>

namespace {
std::atomic<int> g_initialized{0};
std::atomic<int> g_enabled{0};
SnapFrameConfig g_config{};
SnapFrameStats g_stats{};
}

extern "C" void snapframe_config_defaults(SnapFrameConfig *config) {
    if (!config) return;
    memset(config, 0, sizeof(*config));
    config->enabled = 0;
    config->multiplier = 2;
    config->quality = SNAPFRAME_QUALITY_BALANCED;
    config->flow_scale_percent = 50;
    config->profiling_enabled = 1;
}

extern "C" int snapframe_host_init(const SnapFrameConfig *config) {
    if (!config) return 0;

    SnapFrameConfig validated = *config;
    if (validated.multiplier < 1 || validated.multiplier > 2)
        validated.multiplier = 2;
    if (validated.quality < SNAPFRAME_QUALITY_PERFORMANCE
        || validated.quality > SNAPFRAME_QUALITY_QUALITY)
        validated.quality = SNAPFRAME_QUALITY_BALANCED;
    if (validated.flow_scale_percent < 25)
        validated.flow_scale_percent = 25;
    if (validated.flow_scale_percent > 100)
        validated.flow_scale_percent = 100;

    g_config = validated;
    memset(&g_stats, 0, sizeof(g_stats));
    g_enabled.store(validated.enabled ? 1 : 0, std::memory_order_release);
    g_initialized.store(1, std::memory_order_release);
    return 1;
}

extern "C" void snapframe_host_shutdown(void) {
    g_enabled.store(0, std::memory_order_release);
    g_initialized.store(0, std::memory_order_release);
    memset(&g_stats, 0, sizeof(g_stats));
}

extern "C" int snapframe_host_active(void) {
    return g_initialized.load(std::memory_order_acquire)
        && g_enabled.load(std::memory_order_acquire);
}

extern "C" int snapframe_host_set_enabled(int enabled) {
    if (!g_initialized.load(std::memory_order_acquire)) return 0;
    g_enabled.store(enabled ? 1 : 0, std::memory_order_release);
    return 1;
}

extern "C" void snapframe_host_get_stats(SnapFrameStats *stats) {
    if (!stats) return;
    *stats = g_stats;
}
