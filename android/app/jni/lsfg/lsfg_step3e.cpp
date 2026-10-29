#include "lsfg_log.h"
#include "lsfg_host.h"

#include <stdio.h>
#include <string.h>
#include <atomic>

#ifdef __ANDROID__
#include <vulkan/vulkan.h>
#else
#include <vulkan/vulkan_core.h>
#endif

extern "C" void lsfg_set_present_image_hook(void (*fn)(void*, void*, uint32_t, uint32_t, uint32_t));
extern "C" int  lsfg_step3d_frames_ready(void);
extern "C" void *lsfg_step3d_get_image(int idx);
extern "C" unsigned lsfg_step3d_fb_width(void);
extern "C" unsigned lsfg_step3d_fb_height(void);

static std::atomic<unsigned> g_n{0};
static int g_logged_init;
static int g_pool_ok;
static int g_probe_done;
static int g_3f_logged;
static int g_3f_hist_done;
static int g_null_skipped;

static VkDevice         g_dev   = VK_NULL_HANDLE;
static VkQueue          g_queue = VK_NULL_HANDLE;
static uint32_t         g_qfam  = 0;
static VkCommandPool    g_pool  = VK_NULL_HANDLE;
static VkCommandBuffer  g_cmd   = VK_NULL_HANDLE;
static VkFence          g_fence = VK_NULL_HANDLE;

extern "C" void lsfg_step3e_set_queue(void *queue, uint32_t family) {
    g_queue = (VkQueue)queue;
    g_qfam  = family;
}

static bool ensure_pool(VkDevice dev) {
    if (g_pool_ok && g_dev == dev) return true;
    if (!dev || !g_queue) {
        lsfg_log("W", "step3e: no device/queue for cmd pool");
        return false;
    }
    g_dev = dev;

    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = g_qfam;
    if (vkCreateCommandPool(dev, &pci, nullptr, &g_pool) != VK_SUCCESS) {
        lsfg_log("W", "step3e: vkCreateCommandPool failed");
        return false;
    }

    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = g_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev, &ai, &g_cmd) != VK_SUCCESS) {
        lsfg_log("W", "step3e: vkAllocateCommandBuffers failed");
        return false;
    }

    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(dev, &fi, nullptr, &g_fence) != VK_SUCCESS) {
        lsfg_log("W", "step3e: vkCreateFence failed");
        return false;
    }

    g_pool_ok = 1;
    lsfg_log("I", "step3e: command pool OK family=%u pool=%p cmd=%p",
             g_qfam, (void*)g_pool, (void*)g_cmd);
    return true;
}

static void probe_empty_submit(VkDevice dev) {
    if (g_probe_done || !g_pool_ok) return;
    g_probe_done = 1;

    vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &g_fence);
    vkResetCommandBuffer(g_cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_cmd, &bi) != VK_SUCCESS) {
        lsfg_log("W", "step3e: begin cmd failed");
        return;
    }
    if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) {
        lsfg_log("W", "step3e: end cmd failed");
        return;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    VkResult r = vkQueueSubmit(g_queue, 1, &si, g_fence);
    if (r != VK_SUCCESS) {
        lsfg_log("W", "step3e: empty submit FAILED VkResult=%d", (int)r);
        return;
    }
    vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    lsfg_log("I", "step3e: empty queue submit OK \u2014 dispatch path ready");
}

static void step3f_history_probe(VkDevice dev) {
    if (g_3f_hist_done || !g_pool_ok) return;
    if (!lsfg_step3d_frames_ready()) return;

    VkImage hist = (VkImage)lsfg_step3d_get_image(0);
    if (!hist) {
        lsfg_log("W", "step3f: no history image yet");
        return;
    }
    g_3f_hist_done = 1;

    uint32_t w = lsfg_step3d_fb_width();
    uint32_t h = lsfg_step3d_fb_height();

    vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    vkResetFences(dev, 1, &g_fence);
    vkResetCommandBuffer(g_cmd, 0);

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(g_cmd, &bi) != VK_SUCCESS) {
        lsfg_log("W", "step3f: begin failed");
        return;
    }

    VkImageMemoryBarrier bar{};
    bar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = hist;
    bar.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bar.subresourceRange.levelCount = 1;
    bar.subresourceRange.layerCount = 1;
    bar.srcAccessMask = 0;
    bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &bar);

    VkClearColorValue clear{};
    clear.float32[3] = 1.f;
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(g_cmd, hist, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

    bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(g_cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &bar);

    if (vkEndCommandBuffer(g_cmd) != VK_SUCCESS) {
        lsfg_log("W", "step3f: end failed");
        return;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g_cmd;
    VkResult r = vkQueueSubmit(g_queue, 1, &si, g_fence);
    if (r != VK_SUCCESS) {
        lsfg_log("W", "step3f: history clear submit FAILED %d", (int)r);
        return;
    }
    vkWaitForFences(dev, 1, &g_fence, VK_TRUE, UINT64_MAX);
    lsfg_log("I", "step3f: history clear OK %ux%u hist=%p (queue write proven)",
             w, h, (void*)hist);
    lsfg_log("I", "NEXT step 3g: map DXBC slots->Vulkan bindings + optical-flow/generate graph");
    lsfg_log("I", "NOTE: swapchain copy still deferred \u2014 needs DXVK cmd-stream insert");
}

extern "C" void lsfg_step3e_on_image(void *device, void *image,
                                     uint32_t w, uint32_t h, uint32_t vkfmt) {
    if (!lsfg_host_active()) return;

    if (!image) {
        g_null_skipped++;
        return;
    }

    unsigned n = ++g_n;
    VkDevice dev = (VkDevice)device;

    if (!g_logged_init) {
        g_logged_init = 1;
        lsfg_log("I", "=== LSFG step 3e/3f: present-image hook LIVE (real VkImage) ===");
        lsfg_log("I", "step3f: first real image %ux%u fmt=0x%x image=%p device=%p",
                 w, h, vkfmt, image, device);
    }

    /*
     * Do not submit raw Vulkan command buffers from this present hook.
     * DXVK owns and asynchronously submits work to this queue; a direct
     * vkQueueSubmit here is not coordinated with its submission queue.
     * The old step3f probe also cleared history[0], potentially racing
     * with the DXVK history copy and destroying the pixels being tested.
     * Keep this hook observational until diagnostics can be inserted into
     * the DXVK command list with proper resource/layout tracking.
     */
    if (!g_3f_logged) {
        g_3f_logged = 1;
        lsfg_log("I", "=== LSFG step 3f: observational present hook ===");
        lsfg_log("I", "step3f: raw queue submit/history clear disabled; waiting for DXVK cmd-stream probe");
    }

    if (n == 1 || n == 60 || n == 300 || (n % 1800) == 0) {
        lsfg_log("I", "step3f: image #%u %ux%u fmt=0x%x img=%p history_probe=disabled",
                 n, w, h, vkfmt, image);
    }
}

extern "C" void lsfg_step3e_install(void) {
    lsfg_set_present_image_hook(lsfg_step3e_on_image);
    lsfg_log("I", "LSFG: present-image hook installed (step 3e/3f \u2014 real image only)");
}
