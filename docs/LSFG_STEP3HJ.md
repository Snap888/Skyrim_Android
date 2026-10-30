# LSFG step 3h-j (async FULL FG)

## Best path chosen
- Full 25-pass optical flow chain (not light 2-pass)
- Async QueueSubmit, 1-in-flight, never WaitIdle
- Present inject: blit gen → WSI when fence ready

## Config `port/lsfg.conf`
```
enabled=1
multiplier=2
fg_live=1
fg_inject=1
fg_throttle=30
```

Start with throttle=30. Lower to 2–4 if FPS holds.

## Expected log
```
STEP3: 3h-j async FULL FG installed (live=1 inject=1)
step3h-j: READY live=1 inject=1 throttle=30 stages=25
step3h-j: frame #90 submit=1 ...
step3h-j: INJECT #1 (gen→swapchain)
```

## PresentImage inject (required in d3d9_swapchain.cpp)
After `lsfg_step3g_insert_copy(...)` add:

```cpp
{
  extern void lsfg_step3hj_insert_inject(void*, void*, void*, uint32_t, uint32_t, uint32_t);
  lsfg_step3hj_insert_inject(
      m_context.ptr(),
      m_imageViews.at(imageIndex).ptr(),
      m_device.ptr(),
      info.imageExtent.width,
      info.imageExtent.height,
      static_cast<uint32_t>(info.format.format));
}
```
