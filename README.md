# DSXP

Use a PS5 DualSense controller over **Bluetooth** in **X-Plane 12** on macOS, with a
live mapper for every X-Plane command, adaptive triggers that feel the airflow, rumble
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

DSXP reads report `0x31` itself, decodes it, and flies X-Plane through X-Plane's
own UDP interface: the override datarefs for the yoke and rudder, and commands
for the buttons.

## Features

- **Mapper** with a live controller drawing: press a button or click a control to
  select it, then search X-Plane's whole command list (with descriptions) and
  assign. Hold-to-repeat or fire-once per button.
- **Axes**: any stick or trigger to roll, pitch, yaw, throttle, differential rudder
  or toe brakes, with invert, dead zone and response curve.
- **Motion control**: fly by tilting the controller - bank it to roll, raise or
  lower its top edge to pitch, twist it flat to yaw (hold-to-steer: the twist eases
  back by itself and never overshoots on the way back). Each axis is flown by motion
  or the sticks, never both, with its own sensitivity; switching fades over 0.3 s.
  Uses the controller's own factory calibration. The mapper draws the aircraft at
  the attitude DSXP sent to X-Plane. See [Motion control](#motion-control).
- **Touchpad gestures**: tap, two-finger tap and eight swipe directions as buttons.
- **Adaptive triggers**: hand-set Resist / Weapon / Vibrate effects, or *wind mode*,
  where rudder-pedal force follows airspeed squared, crosswind loads one side, and
  turbulence becomes buffet.
- **Rumble**: hand-set, or *sim mode*, where each motor follows its wing's stall,
  hard landings jolt, and a crash shakes both.
- **Light bar**: color and patterns, or *sim mode*, where it shows stall, g-load,
  turbulence and crash.
- **Safe by default**: every override is released when X-Plane closes, the cable
  is plugged in, the controller goes quiet, the Mac sleeps, or DSXP quits. A small
  guard process does the same if DSXP itself ever crashes.
- **Plain-text settings**: three commented config files, applied while DSXP runs
  (see [Settings files](#settings-files)).

## Requirements

- macOS 14 Sonoma or later, Apple Silicon or Intel
- X-Plane 12 (any install folder), with *Settings › Network › Accept incoming
  connections* enabled. DSXP talks to it on `127.0.0.1:49000` (`xplane.port` in
  `bridge.conf`) and takes control only once X-Plane answers its telemetry
  request, so with networking off DSXP stays idle instead of sending into the void.
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
3. Turn on the controller. The menu shows *Controller: Bluetooth, 62 Hz* or so.
4. Start X-Plane. Once a flight is loaded the menu shows *Flight controls: Active*.
5. If the controller is connected but no input arrives, the mapper and the menu
   offer **Allow Input Monitoring…**, which opens *Privacy & Security › Input
   Monitoring*.

If you used the earlier `dualsense-xplane` prototype, its `bindings.conf` is
imported once on first launch.

## Settings files

Everything DSXP can be tuned with lives in three text files in
`~/Library/Application Support/DSXP/` (menu: **Open Config Folder**):

| File | What it holds | Written by |
|---|---|---|
| `bridge.conf` | How DSXP talks to X-Plane: port, loop and repeat rates, timeouts, telemetry rate, logging | you only (DSXP creates it once) |
| `controls.conf` | Stick dead zone and curve, touchpad gestures, the six axes, motion control, the 33 buttons | you and the mapper |
| `feedback.conf` | Trigger effects and the wind model, rumble, light bar and its simulator thresholds | you and the mapper |

- DSXP writes them on first run with every setting at its default, and each
  setting is explained in place: what it does, its range and default, and for
  numbers what raising or lowering it does in flight. The same files, with the
  defaults, are in [`configs/`](configs/) in this repository.
- Edit with any text editor and save: DSXP applies the change within about
  2 seconds, no restart needed (a new X-Plane port or loop rate included).
- A line DSXP cannot use is ignored and keeps the previous value; the mapper and
  the menu list it with its file and line number. Delete a line to use its default;
  delete a file to reset all of it.
- When the mapper saves `controls.conf` or `feedback.conf` it rewrites the file
  from the same template, so the documentation stays, but comments you added
  yourself are lost. If you edited the file meanwhile, both changes are kept.

## Motion control

The first time the mapper opens it asks you to **calibrate**, and motion control can't
be switched on until that's done (your sticks keep working meanwhile). It takes about a
minute and finishes each step by itself:

1. **Lie flat**: face up on a table, hands off, for 3 seconds. DSXP measures each
   gyro axis's resting drift and checks that gravity reads 1 g.
2. **Hold level**: the way you fly, steady, for 2 seconds. That attitude is level from
   now on.
3. **Bank**, 4. **Pitch**, 5. **Twist**: as far as is comfortable each way, then back.
   Each axis's full deflection becomes 90 % of your smaller side, so full control is
   reachable without straining.

The results are saved in `controls.conf` (`motion.calibrated`, `motion.level.*`,
`motion.gyro_bias.*` and the full-deflection angles). Run it again any time with
**Calibrate…** in the Motion control panel; Cancel keeps the previous calibration.
Then switch motion control on in the panel (or `motion.enabled = yes`). **Recenter**
makes the way you hold the controller now level until you switch motion off or restart.

- **Roll**: bank the controller. **Pitch**: raise the top edge (the triggers) to pull,
  lower it to push. **Yaw**: twist the controller about its face, like a steering
  wheel lying flat; the rudder eases back to center over `motion.yaw_return`
  (4 s by default).
- Tick **Motion** for each axis it should fly. An axis on motion ignores every stick
  set to it (yaw also ignores the rudder triggers); the others work as set under Axes.
- **Full deflection at** is the angle that gives full control (40° bank, 30° pitch,
  30° twist by default); lower is more sensitive. A small deadzone (2°, yaw 4°) hides
  hand tremor.
- Bind DSXP's own commands to buttons like any X-Plane command; they come first in
  the command list: `dsxp/motion/recenter`, `dsxp/motion/toggle`, and
  `dsxp/motion/hold` (a clutch: the controls freeze while held, and the attitude you
  let go at becomes level). They work whether or not DSXP is flying, and the player
  lamps flash to confirm.
- Face down or stood on its edge, the controller holds every motion axis neutral.
- DSXP reads the controller's factory calibration (feature report 0x05) when it
  connects and keeps refining the gyro's drift whenever the controller is still.
- What the sensors can read (measured; Sony publishes no figures): turn rates up to
  2000°/s, accelerations up to 4 g (a hard shake reaches that and is clipped), 64
  samples a second over Bluetooth. Bank and pitch come from gravity, so they never
  drift; the twist has no compass to anchor it, which is why yaw is hold-to-steer.
  Details in [`docs/REFERENCES.md`](docs/REFERENCES.md).
  `dsxp-cli --verify` shows bank, pitch, the held twist and what each would send.

## Files

| What | Where |
|---|---|
| Settings | `~/Library/Application Support/DSXP/` `bridge.conf`, `controls.conf`, `feedback.conf` |
| Logs (`log.retention_days`, 14 days by default) | `~/Library/Logs/DSXP/` (*Open Logs Folder* in the menu) |
| Console | subsystem `io.github.aakkss69.DSXP` |

## Uninstall

Quit DSXP, then delete `/Applications/DSXP.app`,
`~/Library/Application Support/DSXP` and `~/Library/Logs/DSXP`. If you enabled
*Launch at Login*, turn it off first. To clear the permission:
`tccutil reset ListenEvent io.github.aakkss69.DSXP`.

## Project layout

| Path | Contents |
|---|---|
| `Sources/Core/` | The bridge, in C, by responsibility: `input/` (report decoding, gestures, motion calibration and fusion), `output/` (controller output, trigger/rumble/light models), `sim/` (X-Plane UDP), `settings/`, `api/` (mapper JSON API), `runtime/` (state machine, threads), `platform/` (macOS services). `include/` is the public API. Pure logic is separated from I/O so it can be tested without hardware. |
| `Sources/App/` | The macOS app, in Swift: `Lifecycle/`, `Bridge/` (C bridge, `dsxp://` scheme), `UI/` (menu-bar item, mapper window), `System/`, `Support/` (Info.plist, entitlements). |
| `Resources/` | What ships inside the app: `web/` (the mapper UI, plain HTML/CSS/JS; `web/plane3d/` draws the motion panel's aircraft) and the asset catalog. |
| `Tests/CoreTests/` | C unit tests, run by `Scripts/test-core.sh` (AddressSanitizer + UBSan) and by `xcodebuild test`. |
| `configs/` | The three settings files with their defaults, generated from `Sources/Core/settings/config_schema.c` (`Scripts/test-core.sh --update-configs`); a test fails if they drift. |
| `docs/` | [`REFERENCES.md`](docs/REFERENCES.md): every outside source the code relies on (X-Plane, DualSense, Apple), with links and the lines that use it, plus known limits and pitfalls. |
| `Tools/` | `dsxp-cli/` (headless runner: `--verify`, `--capture N`, `--write-default-configs DIR`) and `make-icon.swift`. |
| `Scripts/` | `bootstrap.sh`, `build.sh`, `test-core.sh`. |
| `BuildSettings/` | Xcode build settings (`.xcconfig`); your signing team goes in the gitignored `Local.xcconfig`. |
| `project.yml` | XcodeGen spec; `DSXP.xcodeproj` is generated from it. |

## License and trademarks

Apache License 2.0, see [LICENSE](LICENSE). The controller drawing in the mapper
was traced from a stock image whose license is unknown; see
[docs/REFERENCES.md](docs/REFERENCES.md#controller-drawing).

DSXP is not affiliated with or endorsed by Sony Interactive Entertainment or
Laminar Research. "DualSense" and "PlayStation" are trademarks of Sony
Interactive Entertainment Inc. X-Plane is made by Laminar Research.
