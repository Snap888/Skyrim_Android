# Skyrim Android — Camera Look Tuning Test Build

## Purpose

This test build improves touchpad-style camera control. It keeps the existing virtual Xbox RIGHT STICK input route and does not synthesize SDL mouse events.

## Changes

- Camera-look tuning controls are available when **Mouse control** is enabled.
- Three persistent sliders: overall sensitivity, global speed, and vertical sensitivity.
- Each slider covers **0.1x–6.0x** in **0.01x** steps.
- Optional vertical-axis inversion.
- Smoothing of virtual right-stick input at a fixed 16 ms cadence, with eased return to neutral.
- Wider input normalization window to retain finer control at low settings.
- Physical and on-screen right-stick control remains independent and takes priority while held.

## How to use

1. Install the APK and start Skyrim.
2. Open the in-game overlay menu.
3. Enable **Mouse control**. The camera-look settings dialog opens automatically.
4. Adjust Sensitivity, Speed, and Vertical sensitivity. Start near 1.0x and increase gradually.
5. Enable **Invert vertical look** only if the Y direction feels reversed.
6. Tap **Apply**. Settings are saved across app restarts.
7. Reopen **Mouse control settings** from the overlay menu whenever you want to retune.

Use **Reset** to restore 1.6x sensitivity, 1.0x speed, 1.0x vertical sensitivity, and normal Y direction.

## Test checklist

- Verify camera movement in both X and Y directions.
- Check fine aiming with low values (0.5x–1.0x).
- Check faster turns with higher values (3.0x–6.0x).
- Check that the camera stops smoothly after lifting the finger.
- Check multi-touch movement while other overlay controls are used.
- Confirm the physical/on-screen right stick still works and takes priority while held.
- Restart the app and confirm slider values persist.
- Report device model, Android version, chosen values, and whether movement feels delayed, jittery, or too fast.

## Validation status

This build is compiled and packaged by GitHub Actions. Camera feel, latency, and smoothness must still be validated on a physical Android device; CI cannot verify in-game camera behavior.
