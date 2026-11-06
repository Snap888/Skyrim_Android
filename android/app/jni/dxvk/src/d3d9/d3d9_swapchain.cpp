#include "d3d9_swapchain.h"
#include "d3d9_surface.h"
#include "d3d9_monitor.h"

#include "d3d9_hud.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>

/* SnapFrame owns source capture and midpoint generation in this DXVK path. */
using SnapFrameCopyHook = void (*)(void*, void*, void*, uint32_t, uint32_t, uint32_t);
using SnapFramePresentModeHook = int (*)(int*);
using SnapFrameSubmissionPressureHook = void (*)(float, float);
static std::atomic<SnapFramePresentModeHook> g_snapFramePresentModeHook{nullptr};
static std::atomic<SnapFrameSubmissionPressureHook> g_snapFrameSubmissionPressureHook{nullptr};
extern "C" void snapframe_dxvk_set_submission_pressure_hook(
    SnapFrameSubmissionPressureHook hook) {
  g_snapFrameSubmissionPressureHook.store(hook, std::memory_order_release);
}
extern "C" void snapframe_dxvk_set_present_mode_hook(SnapFramePresentModeHook hook) {
  g_snapFramePresentModeHook.store(hook, std::memory_order_release);
}
static std::atomic<SnapFrameCopyHook> g_snapFrameCopyHook{nullptr};
using SnapFrameLogHook = void (*)(const char*);
static std::atomic<SnapFrameLogHook> g_snapFrameLogHook{nullptr};
static void snapframeEmitLog(const char* message) {
  SnapFrameLogHook hook = g_snapFrameLogHook.load(std::memory_order_acquire);
  if (hook) hook(message);
  else fprintf(stderr, "%s\n", message);
}
static std::atomic<uint64_t> g_snapFramePresentEntryCount{0};
static std::atomic<uint64_t> g_snapFramePresentPathCount{0};
static std::atomic<uint64_t> g_snapFrameCallbackDispatchCount{0};
static std::atomic<uint32_t> g_snapFramePresentSpikeLogCount{0};
extern "C" void snapframe_dxvk_set_copy_hook(SnapFrameCopyHook hook) {
  g_snapFrameCopyHook.store(hook, std::memory_order_release);
}
extern "C" void snapframe_dxvk_set_log_hook(SnapFrameLogHook hook) {
  g_snapFrameLogHook.store(hook, std::memory_order_release);
}

using SnapFrameCanGenerateHook = bool (*)(void);
using SnapFrameGenerateHook = void* (*)(void*, void*, uint32_t, uint32_t, uint32_t);
using SnapFramePresentResultHook = void (*)(int, int);
static std::atomic<SnapFrameCanGenerateHook> g_snapFrameCanGenerateHook{nullptr};
static std::atomic<SnapFrameGenerateHook> g_snapFrameGenerateHook{nullptr};
static std::atomic<SnapFramePresentResultHook> g_snapFramePresentResultHook{nullptr};

/* Optional diagnostic bypass for a SnapFrame-off control run. */
static bool snapFramePerfBypassEnabled() {
  const char* value = std::getenv("DXVK_PERF_DISABLE_SNAPFRAME");
  return value && std::string(value) == "1";
}

extern "C" void snapframe_dxvk_set_generation_hooks(
    SnapFrameCanGenerateHook canGenerateHook,
    SnapFrameGenerateHook generateHook,
    SnapFramePresentResultHook presentResultHook) {
  g_snapFrameCanGenerateHook.store(canGenerateHook, std::memory_order_release);
  g_snapFrameGenerateHook.store(generateHook, std::memory_order_release);
  g_snapFramePresentResultHook.store(presentResultHook, std::memory_order_release);
}

namespace dxvk {

  namespace {
    struct DxvkPerfSample {
      std::mutex mutex;
      std::ofstream stream;
      uint64_t frames = 0;
      double totalMs = 0.0;
      double minMs = 1.0e30;
      double maxMs = 0.0;
      uint64_t requestedTotal = 0;
      uint64_t generatedTotal = 0;
      uint64_t snapFrameActiveFrames = 0;
      double sourceAcquireMs = 0.0;
      double generatedAcquireMs = 0.0;
      double submitEnqueueMs = 0.0;
      double generateRecordMs = 0.0;
      double sourceHandoffWaitMs = 0.0;
      double generatedHandoffWaitMs = 0.0;
      double sourceSubmissionWaitMs = 0.0;
      double generatedSubmissionWaitMs = 0.0;
      double frameLatencyWaitMs = 0.0;
      std::string run;
      bool initialized = false;

      void init(bool snapFrameBypassed) {
        if (initialized)
          return;
        initialized = true;

        const char* path = std::getenv("DXVK_PERF_LOG");
        std::string filePath = path ? path : "";
        if (filePath.empty()) {
          const char* dir = std::getenv("DXVK_LOG_PATH");
          if (dir && *dir && std::string(dir) != "none")
            filePath = std::string(dir) + "/dxvk_perf.log";
        }

        const char* runName = std::getenv("DXVK_PERF_RUN");
        run = runName && *runName ? runName : "unspecified";
        if (!filePath.empty()) {
          stream.open(filePath.c_str(), std::ios::out | std::ios::app);
          if (stream) {
            stream << "=== DXVK_PERF_START run=" << run
                   << " snapframe_bypassed=" << (snapFrameBypassed ? 1 : 0) << " ===\n";
            stream << "columns: frame,avg_cpu_present_ms,min_cpu_present_ms,max_cpu_present_ms,"
                      "avg_generated_requested,avg_generated_queued,width,height,vsync,"
                      "snapframe_active_frames,avg_source_acquire_ms,avg_generated_acquire_ms,"
                      "avg_submit_enqueue_ms,avg_generate_record_ms,avg_source_handoff_wait_ms,"
                      "avg_generated_handoff_wait_ms,avg_source_submission_wait_ms,"
                      "avg_generated_submission_wait_ms,avg_frame_latency_wait_ms\n";
            stream.flush();
          }
        }
      }

      void record(double ms, uint32_t requested, uint32_t generated,
                  uint32_t width, uint32_t height, bool vsync,
                  bool snapFrameActive, bool snapFrameBypassed,
                  double sourceAcquire, double generatedAcquire,
                  double submitEnqueue, double generateRecord,
                  double sourceHandoffWait, double generatedHandoffWait,
                  double sourceSubmissionWait, double generatedSubmissionWait,
                  double frameLatencyWait) {
        std::lock_guard<std::mutex> lock(mutex);
        init(snapFrameBypassed);
        if (!stream)
          return;

        ++frames;
        totalMs += ms;
        minMs = std::min(minMs, ms);
        maxMs = std::max(maxMs, ms);
        requestedTotal += requested;
        generatedTotal += generated;
        snapFrameActiveFrames += snapFrameActive ? 1 : 0;
        sourceAcquireMs += sourceAcquire;
        generatedAcquireMs += generatedAcquire;
        submitEnqueueMs += submitEnqueue;
        generateRecordMs += generateRecord;
        sourceHandoffWaitMs += sourceHandoffWait;
        generatedHandoffWaitMs += generatedHandoffWait;
        sourceSubmissionWaitMs += sourceSubmissionWait;
        generatedSubmissionWaitMs += generatedSubmissionWait;
        frameLatencyWaitMs += frameLatencyWait;
        if ((frames % 120) == 0) {
          stream << frames << ','
                 << (totalMs / 120.0) << ','
                 << minMs << ',' << maxMs << ','
                 << (double(requestedTotal) / 120.0) << ','
                 << (double(generatedTotal) / 120.0) << ','
                 << width << ',' << height << ','
                 << (vsync ? 1 : 0) << ','
                 << snapFrameActiveFrames << ','
                 << (sourceAcquireMs / 120.0) << ','
                 << (generatedAcquireMs / 120.0) << ','
                 << (submitEnqueueMs / 120.0) << ','
                 << (generateRecordMs / 120.0) << ','
                 << (sourceHandoffWaitMs / 120.0) << ','
                 << (generatedHandoffWaitMs / 120.0) << ','
                 << (sourceSubmissionWaitMs / 120.0) << ','
                 << (generatedSubmissionWaitMs / 120.0) << ','
                 << (frameLatencyWaitMs / 120.0) << '\n';
          stream.flush();
          totalMs = 0.0;
          minMs = 1.0e30;
          maxMs = 0.0;
          requestedTotal = 0;
          generatedTotal = 0;
          snapFrameActiveFrames = 0;
          sourceAcquireMs = generatedAcquireMs = submitEnqueueMs = 0.0;
          generateRecordMs = sourceHandoffWaitMs = 0.0;
          generatedHandoffWaitMs = sourceSubmissionWaitMs = 0.0;
          generatedSubmissionWaitMs = frameLatencyWaitMs = 0.0;
        }
      }
    };

    DxvkPerfSample& dxvkPerfSample() {
      static DxvkPerfSample sample;
      return sample;
    }
  }

#ifndef DXVK_NATIVE
  struct D3D9WindowData {
    bool unicode;
    bool filter;
    WNDPROC proc;
    D3D9SwapChainEx* swapchain;
  };


  static dxvk::recursive_mutex g_windowProcMapMutex;
  static std::unordered_map<HWND, D3D9WindowData> g_windowProcMap;


  template <typename T, typename J, typename ... Args>
  auto CallCharsetFunction(T unicode, J ascii, bool isUnicode, Args... args) {
    return isUnicode
      ? unicode(args...)
      : ascii  (args...);
  }


  class D3D9WindowMessageFilter {

  public:

    D3D9WindowMessageFilter(HWND window, bool filter = true)
      : m_window(window) {
      std::lock_guard lock(g_windowProcMapMutex);
      auto it = g_windowProcMap.find(m_window);
      m_filter = std::exchange(it->second.filter, filter);
    }

    ~D3D9WindowMessageFilter() {
      std::lock_guard lock(g_windowProcMapMutex);
      auto it = g_windowProcMap.find(m_window);
      it->second.filter = m_filter;
    }

    D3D9WindowMessageFilter             (const D3D9WindowMessageFilter&) = delete;
    D3D9WindowMessageFilter& operator = (const D3D9WindowMessageFilter&) = delete;

  private:

    HWND m_window;
    bool m_filter;

  };


  LRESULT CALLBACK D3D9WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);


  void ResetWindowProc(HWND window) {
    std::lock_guard lock(g_windowProcMapMutex);

    auto it = g_windowProcMap.find(window);
    if (it == g_windowProcMap.end())
      return;

    auto proc = reinterpret_cast<WNDPROC>(
      CallCharsetFunction(
      GetWindowLongPtrW, GetWindowLongPtrA, it->second.unicode,
        window, GWLP_WNDPROC));


    if (proc == D3D9WindowProc)
      CallCharsetFunction(
        SetWindowLongPtrW, SetWindowLongPtrA, it->second.unicode,
          window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(it->second.proc));

    g_windowProcMap.erase(window);
  }


  void HookWindowProc(HWND window, D3D9SwapChainEx* swapchain) {
    std::lock_guard lock(g_windowProcMapMutex);

    ResetWindowProc(window);

    D3D9WindowData windowData;
    windowData.unicode = IsWindowUnicode(window);
    windowData.filter  = false;
    windowData.proc = reinterpret_cast<WNDPROC>(
      CallCharsetFunction(
      SetWindowLongPtrW, SetWindowLongPtrA, windowData.unicode,
        window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(D3D9WindowProc)));
    windowData.swapchain = swapchain;

    g_windowProcMap[window] = std::move(windowData);
  }


  LRESULT CALLBACK D3D9WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCALCSIZE && wparam == TRUE)
      return 0;

    D3D9WindowData windowData = {};

    {
      std::lock_guard lock(g_windowProcMapMutex);

      auto it = g_windowProcMap.find(window);
      if (it != g_windowProcMap.end())
        windowData = it->second;
    }

    bool unicode = windowData.proc
      ? windowData.unicode
      : IsWindowUnicode(window);

    if (!windowData.proc || windowData.filter)
      return CallCharsetFunction(
        DefWindowProcW, DefWindowProcA, unicode,
          window, message, wparam, lparam);

    if (message == WM_DESTROY)
      ResetWindowProc(window);
    else if (message == WM_ACTIVATEAPP) {
      D3DDEVICE_CREATION_PARAMETERS create_parms;
      windowData.swapchain->GetDevice()->GetCreationParameters(&create_parms);

      if (!(create_parms.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES)) {
        if (wparam) {
          // Heroes of Might and Magic V needs this to resume drawing after a focus loss
          D3DPRESENT_PARAMETERS params;
          RECT rect;

          GetMonitorRect(GetDefaultMonitor(), &rect);
          windowData.swapchain->GetPresentParameters(&params);
          SetWindowPos(window, nullptr, rect.left, rect.top, params.BackBufferWidth, params.BackBufferHeight,
                       SWP_NOACTIVATE | SWP_NOZORDER);
        }
        else {
          if (IsWindowVisible(window))
            ShowWindow(window, SW_MINIMIZE);
        }
      }
    }

    return CallCharsetFunction(
      CallWindowProcW, CallWindowProcA, unicode,
        windowData.proc, window, message, wparam, lparam);
  }
#endif


  static uint16_t MapGammaControlPoint(float x) {
    if (x < 0.0f) x = 0.0f;
    if (x > 1.0f) x = 1.0f;
    return uint16_t(65535.0f * x);
  }


  struct D3D9PresentInfo {
    float scale[2];
    float offset[2];
  };


  static inline void ConvertDisplayMode(const D3DDISPLAYMODEEX& mode, wsi::WsiMode* wsiMode) {
    wsiMode->width        = mode.Width;
    wsiMode->height       = mode.Height;
    wsiMode->refreshRate  = wsi::WsiRational{ mode.RefreshRate, 1 };
    wsiMode->bitsPerPixel = GetMonitorFormatBpp(EnumerateFormat(mode.Format));
    wsiMode->interlaced   = false;
  }


  static inline void ConvertDisplayMode(const wsi::WsiMode& devMode, D3DDISPLAYMODEEX* pMode) {
      pMode->Size             = sizeof(D3DDISPLAYMODEEX);
      pMode->Width            = devMode.width;
      pMode->Height           = devMode.height;
      pMode->RefreshRate      = devMode.refreshRate.numerator / devMode.refreshRate.denominator;
      pMode->Format           = D3DFMT_X8R8G8B8;
      pMode->ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
  }


  D3D9SwapChainEx::D3D9SwapChainEx(
          D3D9DeviceEx*          pDevice,
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode)
    : D3D9SwapChainExBase(pDevice)
    , m_device           (pDevice->GetDXVKDevice())
    , m_context          (m_device->createContext())
    , m_frameLatencyCap  (pDevice->GetOptions()->maxFrameLatency)
    , m_frameLatencySignal(new sync::Fence(m_frameId))
    , m_dialog           (pDevice->GetOptions()->enableDialogMode) {
    this->NormalizePresentParameters(pPresentParams);
    m_presentParams = *pPresentParams;
    m_window = m_presentParams.hDeviceWindow;

    UpdatePresentRegion(nullptr, nullptr);
    if (!pDevice->GetOptions()->deferSurfaceCreation)
      CreatePresenter();

    CreateBackBuffers(m_presentParams.BackBufferCount);
    CreateBlitter();
    CreateHud();

    InitRamp();

    // Apply initial window mode and fullscreen state
    if (!m_presentParams.Windowed && FAILED(EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
      throw DxvkError("D3D9: Failed to set initial fullscreen state");
  }


  D3D9SwapChainEx::~D3D9SwapChainEx() {
    DestroyBackBuffers();
#ifndef DXVK_NATIVE
    ResetWindowProc(m_window);
#endif

    wsi::restoreDisplayMode(m_monitor);
    NotifyDisplayRefreshRate(0.0);

    WaitForPresentSubmissions();
    m_device->waitForIdle();
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::QueryInterface(REFIID riid, void** ppvObject) {
    if (ppvObject == nullptr)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IDirect3DSwapChain9)
     || (GetParent()->IsExtended() && riid == __uuidof(IDirect3DSwapChain9Ex))) {
      *ppvObject = ref(this);
      return S_OK;
    }

    Logger::warn("D3D9SwapChainEx::QueryInterface: Unknown interface query");
    Logger::warn(str::format(riid));
    return E_NOINTERFACE;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::Present(
    const RECT*    pSourceRect,
    const RECT*    pDestRect,
          HWND     hDestWindowOverride,
    const RGNDATA* pDirtyRegion,
          DWORD    dwFlags) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    uint32_t presentInterval = m_presentParams.PresentationInterval;

    // This is not true directly in d3d9 to to timing differences that don't matter for us.
    // For our purposes...
    // D3DPRESENT_INTERVAL_DEFAULT (0) == D3DPRESENT_INTERVAL_ONE (1) which means VSYNC.
    presentInterval = std::max(presentInterval, 1u);

    if (presentInterval == D3DPRESENT_INTERVAL_IMMEDIATE || (dwFlags & D3DPRESENT_FORCEIMMEDIATE))
      presentInterval = 0;

    auto options = m_parent->GetOptions();

    if (options->presentInterval >= 0)
      presentInterval = options->presentInterval;

    bool vsync  = presentInterval != 0;

    HWND window = m_presentParams.hDeviceWindow;
    if (hDestWindowOverride != nullptr)
      window    = hDestWindowOverride;

    bool recreate = false;
    recreate   |= m_presenter == nullptr;
    recreate   |= window != m_window;    
    recreate   |= m_dialog != m_lastDialog;

    m_window    = window;

    m_dirty    |= vsync != m_vsync;
    m_dirty    |= UpdatePresentRegion(pSourceRect, pDestRect);
    m_dirty    |= recreate;
    m_dirty    |= m_presenter != nullptr &&
                 !m_presenter->hasSwapChain();

    m_vsync     = vsync;

    m_lastDialog = m_dialog;

    try {
      if (recreate)
        CreatePresenter();

      if (std::exchange(m_dirty, false))
        RecreateSwapChain(vsync);

      // We aren't going to device loss simply because
      // 99% of D3D9 games don't handle this properly and
      // just end up crashing (like with alt-tab loss)
      if (!m_presenter->hasSwapChain())
        return D3D_OK;

      PresentImage(presentInterval);
      return D3D_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return D3DERR_DEVICEREMOVED;
    }
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetFrontBufferData(IDirect3DSurface9* pDestSurface) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    // This function can do absolutely everything!
    // Copies the front buffer between formats with an implicit resolve.
    // Oh, and the dest is systemmem...
    // This is a slow function anyway, it waits for the copy to finish.
    // so there's no reason to not just make and throwaway temp images.

    // If extent of dst > src, then we blit to a subrect of the size
    // of src onto a temp image of dst's extents,
    // then copy buffer back to dst (given dst is subresource)

    D3D9Surface* dst = static_cast<D3D9Surface*>(pDestSurface);

    if (unlikely(dst == nullptr))
      return D3DERR_INVALIDCALL;

    D3D9CommonTexture* dstTexInfo = dst->GetCommonTexture();
    D3D9CommonTexture* srcTexInfo = m_backBuffers.back()->GetCommonTexture();

    if (unlikely(dstTexInfo->Desc()->Pool != D3DPOOL_SYSTEMMEM))
      return D3DERR_INVALIDCALL;

    Rc<DxvkBuffer> dstBuffer = dstTexInfo->GetBuffer(dst->GetSubresource());
    Rc<DxvkImage>  srcImage  = srcTexInfo->GetImage();

    if (srcImage->info().sampleCount != VK_SAMPLE_COUNT_1_BIT) {
      DxvkImageCreateInfo resolveInfo;
      resolveInfo.type          = VK_IMAGE_TYPE_2D;
      resolveInfo.format        = srcImage->info().format;
      resolveInfo.flags         = 0;
      resolveInfo.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
      resolveInfo.extent        = srcImage->info().extent;
      resolveInfo.numLayers     = 1;
      resolveInfo.mipLevels     = 1;
      resolveInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                                | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      resolveInfo.stages        = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                | VK_PIPELINE_STAGE_TRANSFER_BIT;
      resolveInfo.access        = VK_ACCESS_SHADER_READ_BIT
                                | VK_ACCESS_TRANSFER_WRITE_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      resolveInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
      resolveInfo.layout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      
      Rc<DxvkImage> resolvedSrc = m_device->createImage(
        resolveInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      m_parent->EmitCs([
        cDstImage = resolvedSrc,
        cSrcImage = srcImage
      ] (DxvkContext* ctx) {
        VkImageSubresourceLayers resolveSubresource;
        resolveSubresource.aspectMask      = VK_IMAGE_ASPECT_COLOR_BIT;
        resolveSubresource.mipLevel        = 0;
        resolveSubresource.baseArrayLayer  = 0;
        resolveSubresource.layerCount      = 1;

        VkImageResolve resolveRegion;
        resolveRegion.srcSubresource = resolveSubresource;
        resolveRegion.srcOffset      = VkOffset3D { 0, 0, 0 };
        resolveRegion.dstSubresource = resolveSubresource;
        resolveRegion.dstOffset      = VkOffset3D { 0, 0, 0 };
        resolveRegion.extent         = cSrcImage->info().extent;

        ctx->resolveImage(
          cDstImage, cSrcImage,
          resolveRegion, VK_FORMAT_UNDEFINED);
      });

      srcImage = std::move(resolvedSrc);
    }

    D3D9Format srcFormat = srcTexInfo->Desc()->Format;
    D3D9Format dstFormat = dstTexInfo->Desc()->Format;

    bool similar = AreFormatsSimilar(srcFormat, dstFormat);

    if (!similar || srcImage->info().extent != dstTexInfo->GetExtent()) {
      DxvkImageCreateInfo blitCreateInfo;
      blitCreateInfo.type          = VK_IMAGE_TYPE_2D;
      blitCreateInfo.format        = dstTexInfo->GetFormatMapping().FormatColor;
      blitCreateInfo.flags         = 0;
      blitCreateInfo.sampleCount   = VK_SAMPLE_COUNT_1_BIT;
      blitCreateInfo.extent        = dstTexInfo->GetExtent();
      blitCreateInfo.numLayers     = 1;
      blitCreateInfo.mipLevels     = 1;
      blitCreateInfo.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                                   | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
                                   | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
      blitCreateInfo.stages        = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                   | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                   | VK_PIPELINE_STAGE_TRANSFER_BIT;
      blitCreateInfo.access        = VK_ACCESS_SHADER_READ_BIT
                                   | VK_ACCESS_TRANSFER_WRITE_BIT
                                   | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                   | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      blitCreateInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
      blitCreateInfo.layout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      
      Rc<DxvkImage> blittedSrc = m_device->createImage(
        blitCreateInfo, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

      const DxvkFormatInfo* dstFormatInfo = imageFormatInfo(blittedSrc->info().format);
      const DxvkFormatInfo* srcFormatInfo = imageFormatInfo(srcImage->info().format);

      const VkImageSubresource dstSubresource = dstTexInfo->GetSubresourceFromIndex(dstFormatInfo->aspectMask, 0);
      const VkImageSubresource srcSubresource = srcTexInfo->GetSubresourceFromIndex(srcFormatInfo->aspectMask, 0);

      VkImageSubresourceLayers dstSubresourceLayers = {
        dstSubresource.aspectMask,
        dstSubresource.mipLevel,
        dstSubresource.arrayLayer, 1 };

      VkImageSubresourceLayers srcSubresourceLayers = {
        srcSubresource.aspectMask,
        srcSubresource.mipLevel,
        srcSubresource.arrayLayer, 1 };

      VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);

      // Blit to a subrect of the src extents
      VkImageBlit blitInfo;
      blitInfo.dstSubresource = dstSubresourceLayers;
      blitInfo.srcSubresource = srcSubresourceLayers;
      blitInfo.dstOffsets[0] = VkOffset3D{ 0, 0, 0 };
      blitInfo.dstOffsets[1] = VkOffset3D{ int32_t(srcExtent.width),  int32_t(srcExtent.height),  1 };
      blitInfo.srcOffsets[0] = VkOffset3D{ 0, 0, 0 };
      blitInfo.srcOffsets[1] = VkOffset3D{ int32_t(srcExtent.width),  int32_t(srcExtent.height),  1 };

      m_parent->EmitCs([
        cDstImage = blittedSrc,
        cDstMap   = dstTexInfo->GetMapping().Swizzle,
        cSrcImage = srcImage,
        cSrcMap   = srcTexInfo->GetMapping().Swizzle,
        cBlitInfo = blitInfo
      ] (DxvkContext* ctx) {
        ctx->blitImage(
          cDstImage, cDstMap,
          cSrcImage, cSrcMap,
          cBlitInfo, VK_FILTER_NEAREST);
      });

      srcImage = std::move(blittedSrc);
    }

    const DxvkFormatInfo* srcFormatInfo = imageFormatInfo(srcImage->info().format);
    const VkImageSubresource srcSubresource = srcTexInfo->GetSubresourceFromIndex(srcFormatInfo->aspectMask, 0);
    VkImageSubresourceLayers srcSubresourceLayers = {
      srcSubresource.aspectMask,
      srcSubresource.mipLevel,
      srcSubresource.arrayLayer, 1 };
    VkExtent3D srcExtent = srcImage->mipLevelExtent(srcSubresource.mipLevel);

    m_parent->EmitCs([
      cBuffer       = dstBuffer,
      cImage        = srcImage,
      cSubresources = srcSubresourceLayers,
      cLevelExtent  = srcExtent
    ] (DxvkContext* ctx) {
      ctx->copyImageToBuffer(cBuffer, 0, 4, 0,
        cImage, cSubresources, VkOffset3D { 0, 0, 0 },
        cLevelExtent);
    });
    
    dstTexInfo->SetWrittenByGPU(dst->GetSubresource(), true);

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetBackBuffer(
          UINT                iBackBuffer,
          D3DBACKBUFFER_TYPE  Type,
          IDirect3DSurface9** ppBackBuffer) {
    // Could be doing a device reset...
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (unlikely(ppBackBuffer == nullptr))
      return D3DERR_INVALIDCALL;

    if (unlikely(iBackBuffer >= m_presentParams.BackBufferCount)) {
      Logger::err(str::format("D3D9: GetBackBuffer: Invalid back buffer index: ", iBackBuffer));
      return D3DERR_INVALIDCALL;
    }

    *ppBackBuffer = ref(m_backBuffers[iBackBuffer].ptr());
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetRasterStatus(D3DRASTER_STATUS* pRasterStatus) {
    // We could use D3DKMTGetScanLine but Wine doesn't implement that.
    // So... we lie here and make some stuff up
    // enough that it makes games work.

    // Assume there's 20 lines in a vBlank.
    constexpr uint32_t vBlankLineCount = 20;

    if (pRasterStatus == nullptr)
      return D3DERR_INVALIDCALL;

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    if (FAILED(this->GetDisplayModeEx(&mode, nullptr)))
      return D3DERR_INVALIDCALL;

    uint32_t scanLineCount = mode.Height + vBlankLineCount;

    auto nowUs = std::chrono::time_point_cast<std::chrono::microseconds>(
      dxvk::high_resolution_clock::now())
      .time_since_epoch();

    auto frametimeUs = std::chrono::microseconds(1000000u / mode.RefreshRate);
    auto scanLineUs  = frametimeUs / scanLineCount;

    pRasterStatus->ScanLine = (nowUs % frametimeUs) / scanLineUs;
    pRasterStatus->InVBlank = pRasterStatus->ScanLine >= mode.Height;

    if (pRasterStatus->InVBlank)
      pRasterStatus->ScanLine = 0;

    return D3D_OK;
  }

  
  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayMode(D3DDISPLAYMODE* pMode) {
    if (pMode == nullptr)
      return D3DERR_INVALIDCALL;

    *pMode = D3DDISPLAYMODE();

    D3DDISPLAYMODEEX mode;
    mode.Size = sizeof(mode);
    HRESULT hr = this->GetDisplayModeEx(&mode, nullptr);

    if (FAILED(hr))
      return hr;

    pMode->Width       = mode.Width;
    pMode->Height      = mode.Height;
    pMode->Format      = mode.Format;
    pMode->RefreshRate = mode.RefreshRate;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentParameters(D3DPRESENT_PARAMETERS* pPresentationParameters) {
    if (pPresentationParameters == nullptr)
      return D3DERR_INVALIDCALL;

    *pPresentationParameters = m_presentParams;

    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetLastPresentCount(UINT* pLastPresentCount) {
    Logger::warn("D3D9SwapChainEx::GetLastPresentCount: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetPresentStats(D3DPRESENTSTATS* pPresentationStatistics) {
    Logger::warn("D3D9SwapChainEx::GetPresentStats: Stub");
    return D3D_OK;
  }


  HRESULT STDMETHODCALLTYPE D3D9SwapChainEx::GetDisplayModeEx(D3DDISPLAYMODEEX* pMode, D3DDISPLAYROTATION* pRotation) {
    if (pMode == nullptr && pRotation == nullptr)
      return D3DERR_INVALIDCALL;

    if (pRotation != nullptr)
      *pRotation = D3DDISPLAYROTATION_IDENTITY;

    if (pMode != nullptr) {
      wsi::WsiMode devMode = { };

      if (!wsi::getCurrentDisplayMode(GetDefaultMonitor(), &devMode)) {
        Logger::err("D3D9SwapChainEx::GetDisplayModeEx: Failed to enum display settings");
        return D3DERR_INVALIDCALL;
      }

      ConvertDisplayMode(devMode, pMode);
    }

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::Reset(
          D3DPRESENT_PARAMETERS* pPresentParams,
          D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    this->SynchronizePresent();
    this->NormalizePresentParameters(pPresentParams);

    m_dirty    |= m_presentParams.BackBufferFormat   != pPresentParams->BackBufferFormat
               || m_presentParams.BackBufferCount    != pPresentParams->BackBufferCount;

    bool changeFullscreen = m_presentParams.Windowed != pPresentParams->Windowed;

    if (pPresentParams->Windowed) {
      if (changeFullscreen)
        this->LeaveFullscreenMode();

      // Port: the platform layer owns the (fullscreen) window; a smaller back
      // buffer is scaled up to it on present (resolution scale), not resized.
    }
    else {
      if (changeFullscreen) {
        if (FAILED(this->EnterFullscreenMode(pPresentParams, pFullscreenDisplayMode)))
          return D3DERR_INVALIDCALL;
      }

#ifdef _WIN32
      D3D9WindowMessageFilter filter(m_window);
#endif

      if (!changeFullscreen) {
        if (FAILED(ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode, false)))
          return D3DERR_INVALIDCALL;
      }
    }

    m_presentParams = *pPresentParams;

    if (changeFullscreen)
      SetGammaRamp(0, &m_ramp);

    CreateBackBuffers(m_presentParams.BackBufferCount);

    return D3D_OK;
  }


  HRESULT D3D9SwapChainEx::WaitForVBlank() {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::warn("D3D9SwapChainEx::WaitForVBlank: Stub");

    return D3D_OK;
  }

  static bool validateGammaRamp(const WORD (&ramp)[256]) {
    if (ramp[0] >= ramp[std::size(ramp) - 1]) {
      Logger::err("validateGammaRamp: ramp inverted or flat");
      return false;
    }

    for (size_t i = 1; i < std::size(ramp); i++) {
      if (ramp[i] < ramp[i - 1]) {
        Logger::err("validateGammaRamp: ramp not monotonically increasing");
        return false;
      }
      if (ramp[i] - ramp[i - 1] >= UINT16_MAX / 2) {
        Logger::err("validateGammaRamp: huuuge jump");
        return false;
      }
    }

    return true;
  }


  void    D3D9SwapChainEx::SetGammaRamp(
            DWORD         Flags,
      const D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (unlikely(pRamp == nullptr))
      return;

    if (unlikely(!validateGammaRamp(pRamp->red)
              && !validateGammaRamp(pRamp->blue)
              && !validateGammaRamp(pRamp->green)))
      return;

    m_ramp = *pRamp;

    bool isIdentity = true;

    std::array<DxvkGammaCp, NumControlPoints> cp;
      
    for (uint32_t i = 0; i < NumControlPoints; i++) {
      uint16_t identity = MapGammaControlPoint(float(i) / float(NumControlPoints - 1));

      cp[i].r = pRamp->red[i];
      cp[i].g = pRamp->green[i];
      cp[i].b = pRamp->blue[i];
      cp[i].a = 0;

      isIdentity &= cp[i].r == identity
                 && cp[i].g == identity
                 && cp[i].b == identity;
    }

    if (!isIdentity && !m_presentParams.Windowed)
      m_blitter->setGammaRamp(NumControlPoints, cp.data());
    else
      m_blitter->setGammaRamp(0, nullptr);
  }


  void    D3D9SwapChainEx::GetGammaRamp(D3DGAMMARAMP* pRamp) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    if (likely(pRamp != nullptr))
      *pRamp = m_ramp;
  }


  void    D3D9SwapChainEx::Invalidate(HWND hWindow) {
    if (hWindow == nullptr)
      hWindow = m_parent->GetWindow();

    if (m_presentParams.hDeviceWindow == hWindow) {
      m_presenter = nullptr;

      WaitForPresentSubmissions();
      m_device->waitForIdle();
    }
  }


  HRESULT D3D9SwapChainEx::SetDialogBoxMode(bool bEnableDialogs) {
    D3D9DeviceLock lock = m_parent->LockDevice();

    // https://docs.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-setdialogboxmode
    // The MSDN documentation says this will error out under many weird conditions.
    // However it doesn't appear to error at all in any of my tests of these
    // cases described in the documentation.

    m_dialog = bEnableDialogs;

    return D3D_OK;
  }


  D3D9Surface* D3D9SwapChainEx::GetBackBuffer(UINT iBackBuffer) {
    if (iBackBuffer >= m_presentParams.BackBufferCount)
      return nullptr;

    return m_backBuffers[iBackBuffer].ptr();
  }


  void D3D9SwapChainEx::NormalizePresentParameters(D3DPRESENT_PARAMETERS* pPresentParams) {
    if (pPresentParams->hDeviceWindow == nullptr)
      pPresentParams->hDeviceWindow    = m_parent->GetWindow();

    pPresentParams->BackBufferCount    = std::max(pPresentParams->BackBufferCount, 1u);

    const int32_t forcedMSAA = m_parent->GetOptions()->forceSwapchainMSAA;
    if (forcedMSAA != -1) {
      pPresentParams->MultiSampleType    = D3DMULTISAMPLE_TYPE(forcedMSAA);
      pPresentParams->MultiSampleQuality = 0;
    }

    if (pPresentParams->Windowed) {
      GetWindowClientSize(pPresentParams->hDeviceWindow,
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }
    else {
      GetMonitorClientSize(GetDefaultMonitor(),
        pPresentParams->BackBufferWidth  ? nullptr : &pPresentParams->BackBufferWidth,
        pPresentParams->BackBufferHeight ? nullptr : &pPresentParams->BackBufferHeight);
    }

    if (pPresentParams->BackBufferFormat == D3DFMT_UNKNOWN)
      pPresentParams->BackBufferFormat = D3DFMT_X8R8G8B8;

    if (env::getEnvVar("DXVK_FORCE_WINDOWED") == "1")
      pPresentParams->Windowed         = TRUE;
  }


  void D3D9SwapChainEx::PresentImage(UINT SyncInterval) {
    const uint64_t presentEntryCount =
      g_snapFramePresentEntryCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (presentEntryCount == 1)
      snapframeEmitLog("SnapFrame: entered D3D9SwapChainEx::PresentImage");
    const auto perfStart = std::chrono::steady_clock::now();
    const bool perfBypassSnapFrame = snapFramePerfBypassEnabled();

    /* Apply UI-requested WSI changes only on the serialized DXVK present
     * thread. RecreateSwapChain drains outstanding source/generated presents
     * and waits for device idle before replacing the shared swapchain. */
    int requestedPresentMode = m_snapFramePresentMode;
    SnapFramePresentModeHook presentModeHook =
      g_snapFramePresentModeHook.load(std::memory_order_acquire);
    if (presentModeHook && presentModeHook(&requestedPresentMode)
        && requestedPresentMode >= 0 && requestedPresentMode <= 3
        && requestedPresentMode != m_snapFramePresentMode) {
      Logger::info(str::format("SnapFrame: applying runtime present mode request=",
        requestedPresentMode, " at present boundary"));
      RecreateSwapChain(m_vsync);
    }
    uint32_t perfGenerated = 0;
    uint32_t perfRequested = 0;
    double perfCommandFlushMs = 0.0;
    double perfSourceAcquireMs = 0.0;
    double perfGeneratedAcquireMs = 0.0;
    double perfSubmitEnqueueMs = 0.0;
    double perfGenerateRecordMs = 0.0;
    double perfSourceHandoffWaitMs = 0.0;
    double perfGeneratedHandoffWaitMs = 0.0;
    double perfSourceSubmissionWaitMs = 0.0;
    double perfGeneratedSubmissionWaitMs = 0.0;
    double perfFrameLatencyWaitMs = 0.0;
    {
      const auto flushStart = std::chrono::steady_clock::now();
      m_parent->Flush();
      perfCommandFlushMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - flushStart).count();
    }

    // Retrieve the image and image view to present
    auto swapImage = m_backBuffers[0]->GetCommonTexture()->GetImage();
    auto swapImageView = m_backBuffers[0]->GetImageView(false);

    // Bump our frame id.
    ++m_frameId;

    for (uint32_t i = 0; i < SyncInterval || i < 1; i++) {
      {
        const double sourceWaitBefore = perfSourceSubmissionWaitMs;
        const double generatedWaitBefore = perfGeneratedSubmissionWaitMs;
        const auto waitStart = std::chrono::steady_clock::now();
        SynchronizePresent(&perfSourceSubmissionWaitMs,
          &perfGeneratedSubmissionWaitMs);
        perfSourceHandoffWaitMs += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - waitStart).count();
        const double sourceWait = perfSourceSubmissionWaitMs - sourceWaitBefore;
        const double generatedWait = perfGeneratedSubmissionWaitMs - generatedWaitBefore;
        /* Both source and generated submission completion can stall the game
         * thread. Feed the larger per-handoff wait to AI; previously source
         * submission stalls were invisible to the WSI throttle. */
        if (const SnapFrameSubmissionPressureHook pressureHook =
              g_snapFrameSubmissionPressureHook.load(std::memory_order_acquire))
          pressureHook(static_cast<float>(sourceWait), static_cast<float>(generatedWait));
      }

      // Presentation semaphores and WSI swap chain image
      vk::PresenterInfo info = m_presenter->info();
      vk::PresenterSync sync;

      uint32_t imageIndex = 0;

      auto sourceAcquireStart = std::chrono::steady_clock::now();
      VkResult status = VK_NOT_READY;
      bool sourceAcquired = false;
      for (uint32_t attempt = 0; attempt < 3; attempt++) {
        status = m_presenter->acquireNextImage(sync, imageIndex);
        if (status == VK_SUCCESS || status == VK_SUBOPTIMAL_KHR) {
          sourceAcquired = true;
          break;
        }

        Logger::warn(str::format("DXVK: source WSI acquire failed (",
          status, "), recreating swap chain; attempt ", attempt + 1, "/3"));
        RecreateSwapChain(m_vsync);
        info = m_presenter->info();
      }
      perfSourceAcquireMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - sourceAcquireStart).count();
      if (!sourceAcquired) {
        Logger::err(str::format("DXVK: source WSI acquire failed after retries: ", status));
        return;
      }

      m_context->beginRecording(
        m_device->createCommandList());

      VkRect2D srcRect = {
        {  int32_t(m_srcRect.left),                    int32_t(m_srcRect.top)                    },
        { uint32_t(m_srcRect.right - m_srcRect.left), uint32_t(m_srcRect.bottom - m_srcRect.top) } };

      VkRect2D dstRect = {
        {  int32_t(m_dstRect.left),                    int32_t(m_dstRect.top)                    },
        { uint32_t(m_dstRect.right - m_dstRect.left), uint32_t(m_dstRect.bottom - m_dstRect.top) } };

      m_blitter->presentImage(m_context.ptr(),
        m_imageViews.at(imageIndex), dstRect,
        swapImageView, srcRect);

      /* Capture the completed game image before HUD composition. The copy is
       * recorded into this same command list and therefore needs no CPU readback. */
      if (i == 0) {
        const SnapFrameCopyHook copyHook =
          g_snapFrameCopyHook.load(std::memory_order_acquire);
        const uint64_t presentPathCount =
          g_snapFramePresentPathCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (presentPathCount == 1) {
          char snapframeMessage[256];
          snprintf(snapframeMessage, sizeof(snapframeMessage),
            "SnapFrame: DXVK present path reached extent=%ux%u format=0x%x usage=0x%x copy_hook=%d",
            info.imageExtent.width, info.imageExtent.height,
            static_cast<unsigned>(info.format.format),
            static_cast<unsigned>(info.imageUsage),
            copyHook != nullptr);
          snapframeEmitLog(snapframeMessage);
        }
        if (copyHook && !perfBypassSnapFrame) {
          const uint64_t dispatchCount =
            g_snapFrameCallbackDispatchCount.fetch_add(1, std::memory_order_relaxed) + 1;
          if (dispatchCount == 1)
            snapframeEmitLog("SnapFrame: dispatching first DXVK copy callback");
          copyHook(
            m_context.ptr(),
            m_imageViews.at(imageIndex).ptr(),
            m_device.ptr(),
            info.imageExtent.width,
            info.imageExtent.height,
            static_cast<uint32_t>(info.format.format));
        }
      }

      if (m_hud != nullptr)
        m_hud->render(m_context, info.format, info.imageExtent);

      if (i + 1 >= SyncInterval)
        m_context->signal(m_frameLatencySignal, m_frameId);

      {
        const auto submitStart = std::chrono::steady_clock::now();
        SubmitPresent(sync, i, m_sourcePresentStatus, true);
        perfSubmitEnqueueMs += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - submitStart).count();
      }
    }

    /* SnapFrame is the only frame-generation backend. */
    const SnapFrameCanGenerateHook snapFrameCanGenerateHook =
      g_snapFrameCanGenerateHook.load(std::memory_order_acquire);
    const SnapFrameGenerateHook snapFrameGenerateHook =
      g_snapFrameGenerateHook.load(std::memory_order_acquire);
    const bool snapFrameCanGenerate = !perfBypassSnapFrame
      && snapFrameCanGenerateHook && snapFrameGenerateHook
      && snapFrameCanGenerateHook();
    const uint32_t generationCount = snapFrameCanGenerate ? 1u : 0u;
    const bool snapFrameGeneration = snapFrameCanGenerate;
    perfRequested = generationCount;
    if (generationCount) {
      /*
       * Keep this handoff until Presenter has explicit per-acquire tickets:
       * the submission thread must consume the source acquire token and advance
       * its frame slot before the generated acquire can read the next token.
       * This waits for the queued vkQueuePresentKHR + non-blocking pre-acquire,
       * not for GPU retirement. The WSI pre-acquire itself uses timeout=0 so it
       * cannot hold the submission thread waiting for scan-out availability.
       */
      {
        const double sourceWaitBefore = perfSourceSubmissionWaitMs;
        const double generatedWaitBefore = perfGeneratedSubmissionWaitMs;
        const auto waitStart = std::chrono::steady_clock::now();
        SynchronizePresent(&perfSourceSubmissionWaitMs,
          &perfGeneratedSubmissionWaitMs);
        perfGeneratedHandoffWaitMs += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - waitStart).count();
        const double sourceWait = perfSourceSubmissionWaitMs - sourceWaitBefore;
        const double generatedWait = perfGeneratedSubmissionWaitMs - generatedWaitBefore;
        if (const SnapFrameSubmissionPressureHook pressureHook =
              g_snapFrameSubmissionPressureHook.load(std::memory_order_acquire))
          pressureHook(static_cast<float>(sourceWait), static_cast<float>(generatedWait));
      }

      for (uint32_t generation = 0; generation < generationCount; generation++) {
        /* Queue order preserves source -> generated presentation. Independent
         * status slots remove the CPU wait between the two submissions. */
        vk::PresenterInfo generatedInfo = m_presenter->info();
        vk::PresenterSync generatedSync;
        uint32_t generatedImageIndex = 0;
        const auto generatedAcquireStart = std::chrono::steady_clock::now();
        VkResult generatedStatus = VK_NOT_READY;
        bool generatedAcquired = false;
        for (uint32_t attempt = 0; attempt < 2; attempt++) {
          // Generated frames are opportunistic: never stall the game thread
          // waiting for a WSI image that is not immediately available.
          generatedStatus = m_presenter->acquireNextImage(
            generatedSync, generatedImageIndex, 0);
          if (generatedStatus == VK_SUCCESS || generatedStatus == VK_SUBOPTIMAL_KHR) {
            generatedAcquired = true;
            break;
          }

          if (generatedStatus == VK_NOT_READY || generatedStatus == VK_TIMEOUT)
            break;

          if (generatedStatus != VK_ERROR_OUT_OF_DATE_KHR
           && generatedStatus != VK_ERROR_SURFACE_LOST_KHR)
            break;

          Logger::warn(str::format("DXVK: generated WSI acquire requires swap-chain recreation (",
            generatedStatus, "), attempt ", attempt + 1, "/2"));
          RecreateSwapChain(m_vsync);
          generatedInfo = m_presenter->info();
        }
        perfGeneratedAcquireMs += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - generatedAcquireStart).count();
        if (!generatedAcquired) {
          if (generatedStatus != VK_NOT_READY && generatedStatus != VK_TIMEOUT)
            Logger::warn(str::format("SnapFrame: generated WSI acquire failed: ",
              generatedStatus, "; skipping generated frame"));
          const SnapFramePresentResultHook resultHook =
            g_snapFramePresentResultHook.load(std::memory_order_acquire);
          if (resultHook) resultHook(0, static_cast<int>(generatedStatus));
          break;
        }

        const auto generateRecordStart = std::chrono::steady_clock::now();
        m_context->beginRecording(m_device->createCommandList());

        VkRect2D fallbackSrcRect = {
          { int32_t(m_srcRect.left), int32_t(m_srcRect.top) },
          { uint32_t(m_srcRect.right - m_srcRect.left),
            uint32_t(m_srcRect.bottom - m_srcRect.top) } };
        VkRect2D generatedDstRect = {
          { 0, 0 },
          { generatedInfo.imageExtent.width, generatedInfo.imageExtent.height } };

        void* generatedOpaque = snapFrameGenerateHook(
          m_context.ptr(), m_device.ptr(),
          generatedInfo.imageExtent.width, generatedInfo.imageExtent.height,
          static_cast<uint32_t>(generatedInfo.format.format));
        auto* generatedView = static_cast<Rc<DxvkImageView>*>(generatedOpaque);

        const bool generationFailed = generatedView == nullptr || *generatedView == nullptr;
        if (!generationFailed) {
          const VkExtent3D generatedExtent = (*generatedView)->mipLevelExtent(0);
          VkRect2D generatedSrcRect = {
            { 0, 0 },
            { generatedExtent.width, generatedExtent.height } };
          if (generatedExtent.width != generatedInfo.imageExtent.width
           || generatedExtent.height != generatedInfo.imageExtent.height) {
            Logger::warn(str::format("SnapFrame: generated image extent ",
              generatedExtent.width, "x", generatedExtent.height,
              " differs from swapchain ", generatedInfo.imageExtent.width,
              "x", generatedInfo.imageExtent.height, "; scaling to full output"));
          }
          m_blitter->presentImage(m_context.ptr(), m_imageViews.at(generatedImageIndex),
            generatedDstRect, *generatedView, generatedSrcRect);
        } else {
          // Keep the acquired WSI image valid if a generation pass cannot be recorded.
          m_blitter->presentImage(m_context.ptr(), m_imageViews.at(generatedImageIndex),
            generatedDstRect, swapImageView, fallbackSrcRect);
        }
        delete generatedView;

        if (m_hud != nullptr)
          m_hud->render(m_context, generatedInfo.format, generatedInfo.imageExtent);

        perfGenerateRecordMs += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - generateRecordStart).count();
        {
          const auto submitStart = std::chrono::steady_clock::now();
          SubmitPresent(generatedSync, generation + 1, m_generatedPresentStatus, false);
          perfSubmitEnqueueMs += std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - submitStart).count();
        }
        if (snapFrameGeneration) {
          if (generationFailed) {
            const SnapFramePresentResultHook resultHook =
              g_snapFramePresentResultHook.load(std::memory_order_acquire);
            if (resultHook) resultHook(0, static_cast<int>(VK_ERROR_UNKNOWN));
          } else {
            // SubmitPresent is asynchronous. Resolve acceptance only after
            // SynchronizePresent observes the actual VkPresent result.
            m_snapFramePresentPending = true;
          }
        }
        if (!generationFailed)
          ++perfGenerated;
        if (generationFailed)
          break;
      }
    }

    {
      const auto latencyStart = std::chrono::steady_clock::now();
      SyncFrameLatency();
      perfFrameLatencyWaitMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - latencyStart).count();
    }

    // Rotate swap chain buffers so that the back
    // buffer at index 0 becomes the front buffer.
    for (uint32_t i = 1; i < m_backBuffers.size(); i++)
      m_backBuffers[i]->Swap(m_backBuffers[i - 1].ptr());

    m_parent->m_flags.set(D3D9DeviceFlag::DirtyFramebuffer);

    const auto perfEnd = std::chrono::steady_clock::now();
    const double perfMs = std::chrono::duration<double, std::milli>(
      perfEnd - perfStart).count();
    const auto perfInfo = m_presenter->info();
    dxvkPerfSample().record(perfMs, perfRequested, perfGenerated,
      perfInfo.imageExtent.width, perfInfo.imageExtent.height, m_vsync,
      snapFrameCanGenerate, perfBypassSnapFrame,
      perfSourceAcquireMs, perfGeneratedAcquireMs,
      perfSubmitEnqueueMs, perfGenerateRecordMs,
      perfSourceHandoffWaitMs, perfGeneratedHandoffWaitMs,
      perfSourceSubmissionWaitMs, perfGeneratedSubmissionWaitMs,
      perfFrameLatencyWaitMs);

    /* Keep rare long PresentImage stalls diagnosable without per-frame log
     * traffic. These timings are disjoint wall-clock sections; the residual
     * includes command recording and any uninstrumented CPU-side work. */
    if (perfMs >= 35.0
        && g_snapFramePresentSpikeLogCount.fetch_add(
             1, std::memory_order_relaxed) < 64u) {
      const double trackedMs = perfCommandFlushMs
        + perfSourceAcquireMs + perfSubmitEnqueueMs
        + perfGenerateRecordMs + perfSourceHandoffWaitMs
        + perfGeneratedHandoffWaitMs + perfGeneratedAcquireMs
        + perfFrameLatencyWaitMs;
      char spikeMessage[768];
      snprintf(spikeMessage, sizeof(spikeMessage),
        "SnapFrame: DXVK present spike frame=%llu total=%.3fms flush=%.3fms source_acquire=%.3fms source_handoff=%.3fms source_submission_wait=%.3fms generated_handoff=%.3fms generated_submission_wait=%.3fms generated_acquire=%.3fms generate_record=%.3fms submit_enqueue=%.3fms frame_latency=%.3fms untracked=%.3fms fg_requested=%u fg_queued=%u",
        static_cast<unsigned long long>(m_frameId), perfMs,
        perfCommandFlushMs, perfSourceAcquireMs, perfSourceHandoffWaitMs,
        perfSourceSubmissionWaitMs, perfGeneratedHandoffWaitMs,
        perfGeneratedSubmissionWaitMs, perfGeneratedAcquireMs,
        perfGenerateRecordMs, perfSubmitEnqueueMs, perfFrameLatencyWaitMs,
        std::max(0.0, perfMs - trackedMs), perfRequested, perfGenerated);
      snapframeEmitLog(spikeMessage);
    }

  }

  void D3D9SwapChainEx::SubmitPresent(const vk::PresenterSync& Sync, uint32_t FrameId,
                                      DxvkSubmitStatus& Status, bool ApplyFrameRateLimit) {
    // Present from CS thread so that we don't
    // have to synchronize with it first.
    Status.result = VK_NOT_READY;
    if (&Status == &m_sourcePresentStatus)
      m_sourcePresentPending = true;
    else if (&Status == &m_generatedPresentStatus)
      m_generatedPresentPending = true;

    m_parent->EmitCs([this,
      cFrameId     = FrameId,
      cSync        = Sync,
      cHud         = m_hud,
      cCommandList = m_context->endRecording(),
      cStatus      = &Status,
      cApplyFrameRateLimit = ApplyFrameRateLimit
    ] (DxvkContext* ctx) {
      m_device->submitCommandList(cCommandList,
        cSync.acquire, cSync.present);

      if (cHud != nullptr && !cFrameId)
        cHud->update();

      // Only source presents participate in the source FPS limiter.
      m_device->presentImage(m_presenter, cStatus, cApplyFrameRateLimit);
    });

    m_parent->FlushCsChunk();
  }


  void D3D9SwapChainEx::WaitForPresentSubmissions(
      double* sourceWaitMs, double* generatedWaitMs) {
    if (m_sourcePresentPending) {
      const auto waitStart = std::chrono::steady_clock::now();
      m_device->waitForSubmission(&m_sourcePresentStatus);
      if (sourceWaitMs)
        *sourceWaitMs += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - waitStart).count();
      m_sourcePresentPending = false;
    }
    if (m_generatedPresentPending) {
      const auto waitStart = std::chrono::steady_clock::now();
      m_device->waitForSubmission(&m_generatedPresentStatus);
      if (generatedWaitMs)
        *generatedWaitMs += std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - waitStart).count();
      m_generatedPresentPending = false;
    }
  }


  void D3D9SwapChainEx::SynchronizePresent(
      double* sourceWaitMs, double* generatedWaitMs) {
    const bool sourceWasPending = m_sourcePresentPending;
    WaitForPresentSubmissions(sourceWaitMs, generatedWaitMs);

    const VkResult sourceStatus = m_sourcePresentStatus.result.load();
    const VkResult generatedStatus = m_generatedPresentStatus.result.load();

    if (sourceWasPending
        && (sourceStatus == VK_SUCCESS || sourceStatus == VK_SUBOPTIMAL_KHR))
      hud::addSnapFrameGameFrame();

    if (m_snapFramePresentPending) {
      const bool accepted = generatedStatus == VK_SUCCESS
                         || generatedStatus == VK_SUBOPTIMAL_KHR;
      const SnapFramePresentResultHook resultHook =
        g_snapFramePresentResultHook.load(std::memory_order_acquire);
      if (resultHook)
        resultHook(accepted ? 1 : 0, static_cast<int>(generatedStatus));
      if (accepted)
        hud::addSnapFrameGeneratedFrame();
      m_snapFramePresentPending = false;
    }

    const bool sourceFailed = sourceStatus != VK_SUCCESS
                           && sourceStatus != VK_SUBOPTIMAL_KHR;
    const bool generatedFailed = generatedStatus != VK_SUCCESS
                              && generatedStatus != VK_SUBOPTIMAL_KHR;
    if (sourceFailed || generatedFailed)
      RecreateSwapChain(m_vsync);
  }


  void D3D9SwapChainEx::RecreateSwapChain(BOOL Vsync) {
    // Ensure that we can safely destroy the swap chain
    WaitForPresentSubmissions();
    m_device->waitForIdle();

    m_sourcePresentStatus.result = VK_SUCCESS;
    m_generatedPresentStatus.result = VK_SUCCESS;

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = GetPresentExtent();
    presenterDesc.imageCount      = PickImageCount(m_presentParams.BackBufferCount + 1);
    presenterDesc.numFormats      = PickFormats(EnumerateFormat(m_presentParams.BackBufferFormat), presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(Vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    if (m_presenter->recreateSwapChain(presenterDesc) != VK_SUCCESS)
      throw DxvkError("D3D9SwapChainEx: Failed to recreate swap chain");
    
    CreateRenderTargetViews();
    int requestedMode = 0;
    SnapFramePresentModeHook presentModeHook =
      g_snapFramePresentModeHook.load(std::memory_order_acquire);
    if (presentModeHook)
      presentModeHook(&requestedMode);
    m_snapFramePresentMode = requestedMode;
  }


  void D3D9SwapChainEx::CreatePresenter() {
    // Ensure that we can safely destroy the swap chain
    WaitForPresentSubmissions();
    m_device->waitForIdle();

    m_sourcePresentStatus.result = VK_SUCCESS;
    m_generatedPresentStatus.result = VK_SUCCESS;

    DxvkDeviceQueue graphicsQueue = m_device->queues().graphics;

    vk::PresenterDevice presenterDevice;
    presenterDevice.queueFamily   = graphicsQueue.queueFamily;
    presenterDevice.queue         = graphicsQueue.queueHandle;
    presenterDevice.adapter       = m_device->adapter()->handle();

    vk::PresenterDesc presenterDesc;
    presenterDesc.imageExtent     = GetPresentExtent();
    presenterDesc.imageCount      = PickImageCount(m_presentParams.BackBufferCount + 1);
    presenterDesc.numFormats      = PickFormats(EnumerateFormat(m_presentParams.BackBufferFormat), presenterDesc.formats);
    presenterDesc.numPresentModes = PickPresentModes(m_vsync, presenterDesc.presentModes);
    presenterDesc.fullScreenExclusive = PickFullscreenMode();

    m_presenter = new vk::Presenter(m_window,
      m_device->adapter()->vki(),
      m_device->vkd(),
      presenterDevice,
      presenterDesc);

    m_presenter->setFrameRateLimit(m_parent->GetOptions()->maxFrameRate);
    m_presenter->setFrameRateLimiterRefreshRate(m_displayRefreshRate);

    CreateRenderTargetViews();
    int requestedMode = 0;
    SnapFramePresentModeHook presentModeHook =
      g_snapFramePresentModeHook.load(std::memory_order_acquire);
    if (presentModeHook)
      presentModeHook(&requestedMode);
    m_snapFramePresentMode = requestedMode;
  }


  void D3D9SwapChainEx::CreateRenderTargetViews() {
    vk::PresenterInfo info = m_presenter->info();
    const char* actualMode =
      info.presentMode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO" :
      info.presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" :
      info.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" :
      info.presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR ? "FIFO_RELAXED" : "OTHER";
    Logger::info(str::format("SnapFrame: actual present mode=", actualMode,
      " image_count=", info.imageCount, " extent=",
      info.imageExtent.width, "x", info.imageExtent.height));

    m_imageViews.clear();
    m_imageViews.resize(info.imageCount);

    DxvkImageCreateInfo imageInfo;
    imageInfo.type        = VK_IMAGE_TYPE_2D;
    imageInfo.format      = info.format.format;
    imageInfo.flags       = 0;
    imageInfo.sampleCount = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.extent      = { info.imageExtent.width, info.imageExtent.height, 1 };
    imageInfo.numLayers   = 1;
    imageInfo.mipLevels   = 1;
    imageInfo.usage       = info.imageUsage;
    imageInfo.stages      = 0;
    imageInfo.access      = 0;
    imageInfo.tiling      = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.layout      = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    imageInfo.shared      = VK_TRUE;

    DxvkImageViewCreateInfo viewInfo;
    viewInfo.type         = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format       = info.format.format;
    viewInfo.usage        = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    viewInfo.aspect       = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.minLevel     = 0;
    viewInfo.numLevels    = 1;
    viewInfo.minLayer     = 0;
    viewInfo.numLayers    = 1;

    for (uint32_t i = 0; i < info.imageCount; i++) {
      VkImage imageHandle = m_presenter->getImage(i).image;
      
      Rc<DxvkImage> image = new DxvkImage(
        m_device->vkd(), imageInfo, imageHandle);

      m_imageViews[i] = new DxvkImageView(
        m_device->vkd(), image, viewInfo);
    }
  }


  void D3D9SwapChainEx::DestroyBackBuffers() {
    for (auto& backBuffer : m_backBuffers)
      backBuffer->ClearContainer();

    m_backBuffers.clear();
  }


  void D3D9SwapChainEx::CreateBackBuffers(uint32_t NumBackBuffers) {
    // Explicitly destroy current swap image before
    // creating a new one to free up resources
    DestroyBackBuffers();

    int NumFrontBuffer = m_parent->GetOptions()->noExplicitFrontBuffer ? 0 : 1;
    m_backBuffers.resize(NumBackBuffers + NumFrontBuffer);

    // Create new back buffer
    D3D9_COMMON_TEXTURE_DESC desc;
    desc.Width              = std::max(m_presentParams.BackBufferWidth,  1u);
    desc.Height             = std::max(m_presentParams.BackBufferHeight, 1u);
    desc.Depth              = 1;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = EnumerateFormat(m_presentParams.BackBufferFormat);
    desc.MultiSample        = m_presentParams.MultiSampleType;
    desc.MultisampleQuality = m_presentParams.MultiSampleQuality;
    desc.Pool               = D3DPOOL_DEFAULT;
    desc.Usage              = D3DUSAGE_RENDERTARGET;
    desc.Discard            = FALSE;
    desc.IsBackBuffer       = TRUE;
    desc.IsAttachmentOnly   = FALSE;

    for (uint32_t i = 0; i < m_backBuffers.size(); i++)
      m_backBuffers[i] = new D3D9Surface(m_parent, &desc, this);

    auto swapImage = m_backBuffers[0]->GetCommonTexture()->GetImage();

    // Initialize the image so that we can use it. Clearing
    // to black prevents garbled output for the first frame.
    VkImageSubresourceRange subresources;
    subresources.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    subresources.baseMipLevel   = 0;
    subresources.levelCount     = 1;
    subresources.baseArrayLayer = 0;
    subresources.layerCount     = 1;

    VkClearColorValue clearColor;
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 0.0f;

    m_context->beginRecording(
      m_device->createCommandList());
    
    for (uint32_t i = 0; i < m_backBuffers.size(); i++) {
      m_context->clearColorImage(
        m_backBuffers[i]->GetCommonTexture()->GetImage(),
        clearColor, subresources);
    }

    m_device->submitCommandList(
      m_context->endRecording(),
      VK_NULL_HANDLE,
      VK_NULL_HANDLE);
  }


  void D3D9SwapChainEx::CreateBlitter() {
    m_blitter = new DxvkSwapchainBlitter(m_device);
  }


  void D3D9SwapChainEx::CreateHud() {
    m_hud = hud::Hud::createHud(m_device);

    if (m_hud != nullptr) {
      m_hud->addItem<hud::HudClientApiItem>("api", 1, GetApiName());
      m_hud->addItem<hud::HudSamplerCount>("samplers", -1, m_parent);
    }
  }


  void D3D9SwapChainEx::InitRamp() {
    for (uint32_t i = 0; i < NumControlPoints; i++) {
      DWORD identity = DWORD(MapGammaControlPoint(float(i) / float(NumControlPoints - 1)));

      m_ramp.red[i]   = identity;
      m_ramp.green[i] = identity;
      m_ramp.blue[i]  = identity;
    }
  }


  void D3D9SwapChainEx::SyncFrameLatency() {
    // Wait for the sync event so that we respect the maximum frame latency
    m_frameLatencySignal->wait(m_frameId - GetActualFrameLatency());
  }


  uint32_t D3D9SwapChainEx::GetActualFrameLatency() {
    uint32_t maxFrameLatency = m_parent->GetFrameLatency();

    if (m_frameLatencyCap)
      maxFrameLatency = std::min(maxFrameLatency, m_frameLatencyCap);

    maxFrameLatency = std::min(maxFrameLatency, m_presentParams.BackBufferCount + 1);
    return maxFrameLatency;
  }


  uint32_t D3D9SwapChainEx::PickFormats(
          D3D9Format                Format,
          VkSurfaceFormatKHR*       pDstFormats) {
    uint32_t n = 0;

    switch (Format) {
      default:
        Logger::warn(str::format("D3D9SwapChainEx: Unexpected format: ", Format));
        
      case D3D9Format::A8R8G8B8:
      case D3D9Format::X8R8G8B8:
      case D3D9Format::A8B8G8R8:
      case D3D9Format::X8B8G8R8: {
        pDstFormats[n++] = { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;

      case D3D9Format::A2R10G10B10:
      case D3D9Format::A2B10G10R10: {
        pDstFormats[n++] = { VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A2R10G10B10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      } break;

      case D3D9Format::X1R5G5B5:
      case D3D9Format::A1R5G5B5: {
        pDstFormats[n++] = { VK_FORMAT_B5G5R5A1_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_A1R5G5B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      }

      case D3D9Format::R5G6B5: {
        pDstFormats[n++] = { VK_FORMAT_B5G6R5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
        pDstFormats[n++] = { VK_FORMAT_R5G6B5_UNORM_PACK16, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
      }
    }

    return n;
  }


  uint32_t D3D9SwapChainEx::PickPresentModes(
          BOOL                      Vsync,
          VkPresentModeKHR*         pDstModes) {
    int requestedMode = 0;
    SnapFramePresentModeHook presentModeHook = g_snapFramePresentModeHook.load(std::memory_order_acquire);
    if (presentModeHook)
      presentModeHook(&requestedMode);

    uint32_t n = 0;
    switch (requestedMode) {
      case 1: // FIFO
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
        break;
      case 2: // MAILBOX; FIFO is the guaranteed fallback.
        pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
        break;
      case 3: // IMMEDIATE; FIFO is the guaranteed fallback.
        pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
        pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
        break;
      default: // Auto preserves the game's existing VSync/tear-free policy.
        if (Vsync) {
          if (m_parent->GetOptions()->tearFree == Tristate::False)
            pDstModes[n++] = VK_PRESENT_MODE_FIFO_RELAXED_KHR;
          pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
        } else {
          if (m_parent->GetOptions()->tearFree != Tristate::True)
            pDstModes[n++] = VK_PRESENT_MODE_IMMEDIATE_KHR;
          pDstModes[n++] = VK_PRESENT_MODE_MAILBOX_KHR;
          pDstModes[n++] = VK_PRESENT_MODE_FIFO_KHR;
        }
        break;
    }

    Logger::info(str::format("SnapFrame: requested present mode=",
      requestedMode == 1 ? "FIFO" :
      requestedMode == 2 ? "MAILBOX" :
      requestedMode == 3 ? "IMMEDIATE" : "AUTO",
      " (fallback=FIFO where applicable)"));
    return n;
  }

  uint32_t D3D9SwapChainEx::PickImageCount(
          UINT                      Preferred) {
    int32_t option = m_parent->GetOptions()->numBackBuffers;
    const uint32_t requested = option > 0
      ? uint32_t(option) : uint32_t(Preferred);

    /* SnapFrame can submit a source image and one generated image in the
     * same presentation cycle. FIFO/IMMEDIATE commonly expose only two
     * images when the game's preferred count is BackBufferCount + 1; one
     * image may still be scanned out while the source is queued, leaving
     * no image for the generated present. Keep a third WSI image available
     * across modes. Presenter clamps this to the surface's supported range. */
    return std::max(requested, 3u);
  }


  void D3D9SwapChainEx::NotifyDisplayRefreshRate(
          double                  RefreshRate) {
    m_displayRefreshRate = RefreshRate;

    if (m_presenter != nullptr)
      m_presenter->setFrameRateLimiterRefreshRate(RefreshRate);
  }


  HRESULT D3D9SwapChainEx::EnterFullscreenMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode) {
    if (FAILED(ChangeDisplayMode(pPresentParams, pFullscreenDisplayMode, true))) {
      Logger::err("D3D9: EnterFullscreenMode: Failed to change display mode");
      return D3DERR_NOTAVAILABLE;
    }

#ifndef DXVK_NATIVE
    // Testing shows we shouldn't hook WM_NCCALCSIZE but we shouldn't change
    // windows style either.
    //
    // Some games restore window styles after we have changed it, so hooking is
    // also required. Doing it will allow us to create fullscreen windows
    // regardless of their style and it also appears to work on Windows.
    HookWindowProc(m_window, this);

    D3D9WindowMessageFilter filter(m_window);
#endif
    
    m_monitor = GetDefaultMonitor();

    if (!wsi::enterFullscreenMode(m_monitor, m_window, &m_windowState, true)) {
        Logger::err("D3D9: EnterFullscreenMode: Failed to enter fullscreen mode");
        return D3DERR_NOTAVAILABLE;
    }

    return D3D_OK;
  }
  
  
  HRESULT D3D9SwapChainEx::LeaveFullscreenMode() {    
    if (!wsi::restoreDisplayMode(m_monitor))
      Logger::warn("D3D9: LeaveFullscreenMode: Failed to restore display mode");
    NotifyDisplayRefreshRate(0.0);
    
    m_monitor = nullptr;

#ifndef DXVK_NATIVE
    ResetWindowProc(m_window);
#endif
    
    if (!wsi::isWindow(m_window))
      return D3D_OK;

    if (!wsi::leaveFullscreenMode(m_window, &m_windowState)) {
      Logger::err("D3D9: LeaveFullscreenMode: Failed to exit fullscreen mode");
      return D3DERR_NOTAVAILABLE;
    }
    
    return D3D_OK;
  }
  
  
  HRESULT D3D9SwapChainEx::ChangeDisplayMode(
          D3DPRESENT_PARAMETERS* pPresentParams,
    const D3DDISPLAYMODEEX*      pFullscreenDisplayMode,
          bool                   EnteringFullscreen) {
    D3DDISPLAYMODEEX mode;

    if (pFullscreenDisplayMode) {
      mode = *pFullscreenDisplayMode;
    } else {
      mode.Width            = pPresentParams->BackBufferWidth;
      mode.Height           = pPresentParams->BackBufferHeight;
      mode.Format           = pPresentParams->BackBufferFormat;
      mode.RefreshRate      = pPresentParams->FullScreen_RefreshRateInHz;
      mode.ScanLineOrdering = D3DSCANLINEORDERING_PROGRESSIVE;
      mode.Size             = sizeof(D3DDISPLAYMODEEX);
    }

    wsi::WsiMode wsiMode = { };
    ConvertDisplayMode(mode, &wsiMode);

    if (!wsi::setWindowMode(GetDefaultMonitor(), m_window, &wsiMode, EnteringFullscreen))
      return D3DERR_NOTAVAILABLE;

    return D3D_OK;
  }


  bool    D3D9SwapChainEx::UpdatePresentRegion(const RECT* pSourceRect, const RECT* pDestRect) {
    if (pSourceRect == nullptr) {
      m_srcRect.top    = 0;
      m_srcRect.left   = 0;
      m_srcRect.right  = m_presentParams.BackBufferWidth;
      m_srcRect.bottom = m_presentParams.BackBufferHeight;
    }
    else
      m_srcRect = *pSourceRect;

    RECT dstRect;
    if (pDestRect == nullptr) {
      // TODO: Should we hook WM_SIZE message for this?
      UINT width, height;
      GetWindowClientSize(m_window, &width, &height);

      dstRect.top    = 0;
      dstRect.left   = 0;
      dstRect.right  = LONG(width);
      dstRect.bottom = LONG(height);
    }
    else
      dstRect = *pDestRect;

    bool recreate = 
       m_dstRect.left   != dstRect.left
    || m_dstRect.top    != dstRect.top
    || m_dstRect.right  != dstRect.right
    || m_dstRect.bottom != dstRect.bottom;

    m_dstRect = dstRect;

    return recreate;
  }

  VkExtent2D D3D9SwapChainEx::GetPresentExtent() {
    return VkExtent2D {
      std::max<uint32_t>(m_dstRect.right  - m_dstRect.left, 1u),
      std::max<uint32_t>(m_dstRect.bottom - m_dstRect.top,  1u) };
  }


  VkFullScreenExclusiveEXT D3D9SwapChainEx::PickFullscreenMode() {
    return m_dialog
      ? VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT
      : VK_FULL_SCREEN_EXCLUSIVE_DEFAULT_EXT;
  }


  std::string D3D9SwapChainEx::GetApiName() {
    return this->GetParent()->IsExtended() ? "D3D9Ex" : "D3D9";
  }

}
