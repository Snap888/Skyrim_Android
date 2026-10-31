#include "dxvk_options.h"

namespace dxvk {

  DxvkOptions::DxvkOptions(const Config& config) {
    enableStateCache      = config.getOption<bool>    ("dxvk.enableStateCache",       true);
    enableOpenVR          = config.getOption<bool>    ("dxvk.enableOpenVR",           true);
    enableOpenXR          = config.getOption<bool>    ("dxvk.enableOpenXR",           true);
    numCompilerThreads    = config.getOption<int32_t> ("dxvk.numCompilerThreads",     0);
    useRawSsbo            = config.getOption<Tristate>("dxvk.useRawSsbo",             Tristate::Auto);
    halveNvidiaHVVHeap    = config.getOption<Tristate>("dxvk.halveNvidiaHVVHeap",     Tristate::Auto);
    // Show the LSFG FPS HUD by default on this Android fork. Explicit dxvk.hud
    // configuration and DXVK_HUD environment overrides remain supported.
    hud                   = config.getOption<std::string>("dxvk.hud", "fps");
  }

}
