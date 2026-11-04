# DSXP

Use a PS5 DualSense controller over **Bluetooth** in **X-Plane 12** on macOS, with a
live mapper for every X-Plane command, adaptive triggers driven by the airflow, rumble
for stalls and crashes, and a light bar that works as an annunciator.

DSXP is a small menu-bar app. Over USB-C X-Plane already reads the controller
itself, so DSXP steps aside whenever the cable is plugged in.

## Why it is needed

Over Bluetooth, macOS switches the DualSense into its extended mode, in which it
sends only HID input report `0x31`. The report descriptor declares that report as
a single vendor-defined blob (usage page `0xFF00`), with no standard axes or
buttons. X-Plane deliberately ignores vendor-page elements, so it sees the
controller but never receives a stick movement or a button press. Over USB the
real axes arrive in report `0x01`, which is why the cable works.

DSXP reads report `0x31` itself, decodes it, and drives X-Plane through X-Plane's
own UDP interface: the override datarefs for the yoke and rudder, and commands
for the buttons.

## Features

- **Mapper** with a live controller drawing: press a button or click a control to
  select it, then search X-Plane's whole command list (with descriptions) and
  assign. Hold-to-repeat or fire-once per button.
- **Axes**: any stick or trigger to roll, pitch, yaw, throttle, differential rudder
  or toe brakes, with invert, deadzone and response curve.
- **Touchpad gestures**: tap, two-finger tap and eight swipe directions as buttons.
- **Adaptive triggers**: hand-set Resist / Weapon / Vibrate effects, or *wind mode*,
  where rudder-pedal force follows airspeed squared, crosswind loads one side, and
  turbulence becomes buffet.
- **Rumble**: hand-set, or *sim mode*, where each motor follows its wing's stall,
  hard landings jolt, and a crash shakes both.
- **Light bar**: colour and patterns, or *sim mode*, where it shows stall, g-load,
  turbulence and crash.
- **Safe by default**: every override is released when X-Plane closes, the cable
  is plugged in, the controller goes quiet, the Mac sleeps, or DSXP quits. A small
  guard process does the same if DSXP itself ever crashes.

## Requirements

- macOS 14 Sonoma or later, Apple Silicon or Intel
- X-Plane 12, with *Settings › Network › Accept incoming connections* enabled
  (DSXP talks to it on `127.0.0.1:49000`)
- A DualSense (`054C:0CE6`) paired over Bluetooth
- To build: Xcode, and [XcodeGen](https://github.com/yonaskolb/XcodeGen)
  (`brew install xcodegen`)

## Build and install

```sh
git clone https://github.com/Aakkss69/dsxp.git
cd dsxp
Scripts/bootstrap.sh          # signing settings + generates DSXP.xcodeproj
Scripts/build.sh --install    # Release build into /Applications/DSXP.app
```

Or open `DSXP.xcodeproj` in Xcode and run the `DSXP` scheme.

**Signing.** DSXP is not notarized. Build it yourself with your own (free) Apple
Development certificate: in Xcode open *Settings › Accounts*, add your Apple ID,
then *Manage Certificates › + › Apple Development*, and run
`Scripts/bootstrap.sh` again. With an ad-hoc signature it still runs, but
macOS treats every rebuild as a new app and asks for permissions again.

## First run

1. Open DSXP. Its icon appears in the menu bar and the mapper opens.
2. If the X-Plane folder is not found automatically (it looks in
   `~/X-Plane/X-Plane 12` and `/Applications/X-Plane 12`, and at a running
   X-Plane), click **Choose…** in the mapper and pick your X-Plane 12 folder.
3. Turn on the controller. The menu shows *Controller: Bluetooth · ~60 Hz*.
4. Start X-Plane. Once a flight is loaded the menu shows *Bridge: engaged*.
5. If the controller is connected but no input arrives, the mapper and the menu
   offer **Input access…**, which opens *Privacy & Security › Input Monitoring*.

If you used the earlier `dualsense-xplane` prototype, its `bindings.conf` is
imported once on first launch.

## Files

| What | Where |
|---|---|
| Settings | `~/Library/Application Support/DSXP/bindings.conf` |
| Logs (14 days) | `~/Library/Logs/DSXP/` (*Open Logs Folder* in the menu) |
| Console | subsystem `io.github.aakkss69.DSXP` |

## Uninstall

Quit DSXP, then delete `/Applications/DSXP.app`,
`~/Library/Application Support/DSXP` and `~/Library/Logs/DSXP`. If you enabled
*Launch at Login*, turn it off first. To clear the permission:
`tccutil reset ListenEvent io.github.aakkss69.DSXP`.

## Project layout

| Path | Contents |
|---|---|
| `Core/` | The bridge, in C: HID decoding, X-Plane UDP, trigger/rumble/light output, settings, JSON API. Pure logic is separated from I/O so it can be tested without hardware. |
| `App/` | The macOS app, in Swift: menu-bar item, mapper window, `dsxp://` scheme handler, lifecycle. |
| `App/Resources/web/` | The mapper UI (plain HTML, CSS and JavaScript). |
| `Tests/CoreTests/` | C unit tests, run by `Scripts/test-core.sh` (AddressSanitizer + UBSan) and by `xcodebuild test`. |
| `Tools/dsxp-cli/` | Headless runner: `--verify` shows live input, `--capture N` dumps raw reports. |
| `project.yml` | XcodeGen spec; `DSXP.xcodeproj` is generated from it. |

## Licence and trademarks

Apache License 2.0, see [LICENSE](LICENSE).

DSXP is not affiliated with or endorsed by Sony Interactive Entertainment or
Laminar Research. "DualSense" and "PlayStation" are trademarks of Sony
Interactive Entertainment Inc. "X-Plane" is a trademark of Laminar Research.
