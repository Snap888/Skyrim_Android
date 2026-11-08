# Android in-game mouse control

## User-facing behavior

- The in-game overlay menu exposes **Mouse control: ON/OFF**.
- The choice persists in the gamepad_overlay SharedPreferences store (mouse_control_enabled).
- When ON, a finger that begins outside a virtual gamepad hitbox controls Skyrim camera look through relative SDL mouse deltas. A short tap on the trackpad area generates a left click.
- A finger that begins on a visible virtual control is reserved for that control for its entire gesture. The existing XInput buttons, triggers, and analog sticks are unchanged.
- Trackpad and controller fingers are tracked independently, allowing simultaneous cursor movement and button presses.
- Turning the mode OFF clears any active trackpad pointer state immediately.

## Input routing

GamepadOverlayView performs virtual-control hit testing before trackpad routing. It keeps separate pointer maps for controller controls and trackpad fingers. Relative deltas and clicks are forwarded through org.libsdl.app.SDLActivity.onNativeMouse, which reaches SDL's Android Android_OnMouse implementation. Motion uses `relative=true`, causing SDL to deliver `xrel/yrel` deltas consumed by Skyrim's camera controls. Absolute cursor coordinates are not suitable here: Skyrim may enable SDL relative-mouse mode and ignore absolute positioning for camera look.

The touchpad does not draw an opaque surface or add a second overlay view; it uses the existing full-screen gamepad overlay's empty hit regions. This preserves the existing overlay z-order and avoids a second view competing with the buttons.

## Device validation checklist

1. Open the in-game menu and switch Mouse control ON.
2. Drag a finger through empty screen space; verify the game's mouse cursor or pointer moves.
3. Tap empty space; verify a left click is received.
4. Press and hold A/B/X/Y, D-pad, shoulders, triggers and both sticks; verify each still works.
5. Move the cursor with one finger while holding a gamepad control with another.
6. Start a gesture on a button and move outside its hitbox; it must remain a button gesture and must not move the mouse.
7. Switch Mouse control OFF; empty-space touches must no longer move/click the mouse.
8. Repeat in both X11 and Wayland launch modes.

Sensitivity is currently fixed at 1.8x and can be tuned after device testing.


## SDL mouse delivery correction (2026-11-08)

The touchpad now maintains a virtual cursor in the overlay/SDL surface pixel space and sends absolute SDL mouse motion (`ACTION_MOVE`, `relative=false`). This avoids relying on SDL relative-mouse mode being enabled by the game. A tap sends `BUTTON_PRIMARY` down followed by a zero-button-state up at the current virtual cursor position; it no longer clicks at the top-left corner. The implementation preserves pointer ownership, so fingers that start on virtual controller controls remain controller inputs.


## Test build 2026-11-08

Build target: Android ARM64 debug APK. This test revision routes touchpad deltas as relative SDL mouse motion for Skyrim camera look and emits primary-button press/release events. Gamepad hitbox priority and independent multi-touch pointer ownership are retained. Validation required on-device: cursor movement, click/menu interaction, and simultaneous mouse/gamepad input. This is a test build; physical-device validation is pending.
