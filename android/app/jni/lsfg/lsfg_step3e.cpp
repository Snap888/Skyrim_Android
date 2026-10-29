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

static std::atomic<unsigned> g_n{0};
static int g_logged_init;
static int g_pool_ok;
static int g_probe_done;

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
    lsfg_log("I", "step3e: empty queue submit OK — dispatch path ready");
    lsfg_log("I", "NEXT step 3f: record copy swapchain\xe2\x86\x92history + FG dispatch with real descriptor sets");
}

extern "C" void lsfg_step3e_on_image(void *device, void *image,
                                     uint32_t w, uint32_t h, uint32_t vkfmt) {
    if (!lsfg_host_active()) return;
    unsigned n = ++g_n;
    VkDevice dev = (VkDevice)device;

    if (!g_logged_init) {
        g_logged_init = 1;
        lsfg_log("I", "=== LSFG step 3e: present-image hook LIVE ===");
        lsfg_log("I", "step3e: first image %ux%u fmt=0x%x image=%p device=%p",
                 w, h, vkfmt, image, device);
    }

    if (!ensure_pool(dev)) return;
    probe_empty_submit(dev);

    if (n == 1 || n == 60 || n == 300 || (n % 1800) == 0) {
        lsfg_log("I", "step3e: image #%u %ux%u fmt=0x%x img=%p (FG dispatch not yet)",
                 n, w, h, vkfmt, image);
    }
}

extern "C" void lsfg_step3e_install(void) {
    lsfg_set_present_image_hook(lsfg_step3e_on_image);
    lsfg_log("I", "LSFG: present-image hook installed (step 3e)");
}
