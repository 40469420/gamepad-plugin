# Gamepad plugin for OBS

OBS source that draws live controller data while a game is using the pad.

It polls **XInput from the OBS process**, so Xbox-compatible controllers keep reporting sticks, buttons, and triggers even when a game has focus. Gyro is read separately through Windows Raw Input (shared, not exclusive).

## Install

Download the Windows installer from the [latest release](https://github.com/40469420/gamepad-plugin/releases/latest).

1. Fully quit OBS, including the tray icon.
2. Run `gamepad-plugin-<version>-windows-x64-Installer.exe`.
3. Reopen OBS. In a scene: **Add Source → Gamepad**.

The installer copies the plugin to `C:\ProgramData\obs-studio\plugins\gamepad-plugin`, which is the directory OBS loads on Windows. The release also includes a zip of the same files.

## Add it in OBS

From a local build, run `.\install-local.ps1` instead of the installer. It copies into `C:\ProgramData\obs-studio\plugins`.

1. Fully quit OBS, including the tray icon, and reopen it.
2. In a scene: **Add Source → Gamepad**.

`Tools > Plugins` is OBS's plugin store, not a list of loaded sources. This plugin will not appear there.

The overlay uses a fixed Xbox layout (LT/RT and bumpers on top, left stick + d-pad on the left, View/Xbox/Menu in the center, ABXY on the right, right stick lower right). Turning an item off in properties hides it but does not move anything else.

The source shows:

- Left / right stick circles with a line from center to the stick position
- Raw `SHORT` values (`-32768…32767`) and normalized `-1…1`
- Button lamps, d-pad, analog triggers `0…255`
- XInput packet number and button mask
- Gyro circle (deg/s) plus raw IMU integers when the pad has motion sensors
- **Test rumble** in the source properties (left/right motor %)

## Gyro

Xbox pads have no IMU. Gyro works when Windows still delivers HID motion reports, typically:

- DualSense / DualSense Edge
- DualShock 4
- GameSir HID reports (`0xA1 0xC8`)
- Switch Pro full reports (`0x30`), if the device is already sending them

If the game opens the HID device exclusively, sticks/buttons still work through XInput; gyro may show **unavailable**.

## Rumble

Rumble uses `XInputSetState`. If the game is also writing rumble every frame, the test pulse can get overwritten. That is a Windows last-writer-wins limit, not a bug in the overlay.

## Build (Windows)

Needs Visual Studio 2022 (or Build Tools) with the C++ workload, CMake 3.28+, and Git.

From this folder:

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
.\install-local.ps1
```

Then restart OBS. `install-local.ps1` copies files to:

`C:\ProgramData\obs-studio\plugins\gamepad-plugin\`

## Controls

Source properties:

| Setting | What it does |
|---|---|
| Controller | XInput slot 1–4 |
| Width / Height | Overlay resolution (default 1280×720) |
| Show stick circles / buttons / numbers / gyro | Toggle HUD pieces |
| Stick deadzone overlay | Draws an inner circle; raw values are never altered |
| Left/right motor % | Strength for the rumble test |
| Test rumble | Pulses the motors |
