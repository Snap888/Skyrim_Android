#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum SnapFrameQuality {
    SNAPFRAME_QUALITY_PERFORMANCE = 0,
    SNAPFRAME_QUALITY_BALANCED = 1,
    SNAPFRAME_QUALITY_QUALITY = 2
} SnapFrameQuality;

typedef enum SnapFramePresentMode {
    SNAPFRAME_PRESENT_MODE_AUTO = 0,
    SNAPFRAME_PRESENT_MODE_FIFO = 1,
    SNAPFRAME_PRESENT_MODE_MAILBOX = 2,
    SNAPFRAME_PRESENT_MODE_IMMEDIATE = 3
} SnapFramePresentMode;

typedef enum SnapFrameAIMode {\n    SNAPFRAME_AI_STANDARD = 0,\n    SNAPFRAME_AI_INTELLIGENT = 1,\n    SNAPFRAME_AI_MANUAL = 2\n} SnapFrameAIMode;\n\ntypedef struct SnapFrameConfig {
    int enabled;
    int present_mode;     /* SnapFramePresentMode; applies to the shared DXVK swapchain. */
    int multiplier;       /* Initially only 1 (off) or 2 (one generated frame). */
    int quality;          /* SnapFrameQuality */
    int flow_scale_percent;
    int motion_analysis; /* Enables luma, bidirectional flow and consistency. */
    int profiling_enabled;
    int diagnostic_suite;
    int diagnostic_phase_frames;
} SnapFrameConfig;

typedef struct SnapFrameFrameInfo {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t frame_index;
    int64_t timestamp_ns;
} SnapFrameFrameInfo;

typedef struct SnapFrameStats {
    uint64_t captured_frames;
    uint64_t generated_frames; /* Additional WSI presents queued successfully. */
    uint64_t skipped_frames;
    uint64_t invalid_history_frames;
    float preprocess_gpu_ms;
    float flow_gpu_ms;
    float confidence_gpu_ms;
    float warp_gpu_ms;
    float composite_gpu_ms;
} SnapFrameStats;

#ifdef __cplusplus
}
#endif
