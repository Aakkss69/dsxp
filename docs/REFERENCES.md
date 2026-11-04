# References

Every outside source DSXP's code relies on: what it is, where to read it, and
the lines that use it. Then the limits and pitfalls that shaped the code, and
the claims that are observed rather than documented.

Conventions:
- `XP12/` is the X-Plane 12 folder (`~/X-Plane/X-Plane 12` by default). Line
  numbers in X-Plane's own files are from the copy DSXP was checked against:
  `DataRefs.txt` dated 30 Sep 2026 (header `2 1210`), X-Plane 12.4.3-r2 per its
  `Log.txt`, app bundle version `12.4.4-b3-5bfd5a53`. They move between versions;
  search for the name instead if a line does not match.
- Code citations are `path:line` in this repository at the commit that last
  changed this file.
- "Verified" means the page or file was opened and its content matched (web pages
  on 2026-11-05).

## X-Plane

| Reference | What it is | Where to read it | Used at |
|---|---|---|---|
| Exchanging Data with X-Plane | Laminar's spec of the UDP interface: `DREF` (write a dataref), `CMND` (run a command), `RREF` (subscribe to datarefs; frequency 0 cancels), port 49000 | No official online copy found. Local: `XP12/Instructions/Exchanging Data with X-Plane.rtfd/TXT.rtf` — line 36 (X-Plane always receives on 49000), 397–412 `CMND`, 416–475 `RREF` (475: frequency 0 stops it), 479–512 `DREF` (509 bytes), 839 `raknet_port` 49010. Third-party summary: [xppython3 UDP](https://xppython3.readthedocs.io/en/latest/development/udp/index.html), [CMND](https://xppython3.readthedocs.io/en/latest/development/udp/cmnd.html), [RREF](https://xppython3.readthedocs.io/en/latest/development/udp/rref.html) | `Sources/Core/sim/xp_packets.c:2`, `Sources/Core/sim/xp_packets.c:25`, `Sources/Core/sim/telemetry.c:6`, `Sources/Core/internal/core_internal.h:369`, `Sources/Core/platform/guard.c:126` |
| `DataRefs.txt` | Laminar's list of built-in datarefs: path, type, writable (`y`/`n`), units, description | [developer.x-plane.com/datarefs](https://developer.x-plane.com/datarefs/) (loads, but entries are rendered by JavaScript, so not checked online). Local: `XP12/Resources/plugins/DataRefs.txt`; every dataref DSXP uses is in [the table below](#datarefs) | `Sources/Core/sim/xplane.c:9`, `Sources/Core/internal/core_internal.h:198` |
| `Commands.txt` | Laminar's command list: a path, whitespace, a description per line. Most lines use two or more spaces; lines 1672 and 1847 use one, so DSXP ends the path at the first whitespace | Local only: `XP12/Resources/plugins/Commands.txt` (3165 lines; line 1 is `sim/none/none`) | `Sources/Core/sim/commands.c:28`, `Sources/App/System/XPlaneLocator.swift:3`, `Tests/CoreTests/test_wire.c:112` |
| Override datarefs | Without the override flag X-Plane keeps writing its own values over the yoke and brake ratios | `DataRefs.txt` 1996–2002 (yoke ratios: "Use override_joystick or override_joystick_pitch"), 3472–3474 and 3506 (`override_toe_brakes` "Gives plugins control of … left_brake_ratio and right_brake_ratio"); Laminar on how overrides work, with prop pitch as the example: [understanding override_prop_pitch](https://developer.x-plane.com/article/understanding-override_prop_pitch-and-override_prop_mode) | `Sources/Core/sim/xplane.c:124`, `Sources/Core/sim/xplane.c:131` |
| Wing datarefs | `sim/flightmodel2/wing` arrays are indexed by wing: 0 = left wing 1, 1 = right wing 1, 2 and 3 = wing 2, and so on, 10 elements each | [sim/flightmodel2/wing](https://developer.x-plane.com/article/sim-flightmodel2-wing/), [Using the correct wing datarefs](https://developer.x-plane.com/article/using-the-correct-wing-datarefs/). `DataRefs.txt` 5662: `element_is_stalled` is `float[480]`; its siblings at 5658 and 5663–5670 are `float[48][10]` | `Sources/Core/internal/core_internal.h:200`, `Sources/Core/sim/telemetry.c:41` |
| X-Plane 12 weather layers | Weather is published per layer: 13 turbulence values with 13 layer altitudes. The old `sim/weather/turbulence[0..2]` are marked REPLACED | `DataRefs.txt` 3750 (`wind_altitude_msl_m float[13]`, "The center altitude of this layer of wind"), 3755 (`turbulence float[13]`), 3726–3728 (REPLACED) | `Sources/Core/internal/core_internal.h:197`, `Sources/Core/sim/telemetry.c:50` |
| `sim/time/paused` | 1 while the simulator is paused | `DataRefs.txt` 3589 | `Sources/Core/sim/telemetry.c:24`, `Sources/Core/sim/telemetry.c:212` |
| `joy_use_*` axis enum | X-Plane's axis functions (1 pitch, 2 roll, 3 heading, 52 commands, 74/75 left/right rudder) | [Joystick datarefs](https://developer.x-plane.com/article/joystick-datarefs/) (verified: 1, 2, 3 and 52 match; the published table ends at 66). 74 and 75 are not published: recovered from the ordered string table in `XP12/X-Plane.app/Contents/MacOS/X-Plane` | `Sources/Core/settings/bindings.c:20` |
| Control profile (`.prf`) | The author's saved X-Plane joystick profile, from which the default bindings were transcribed. Keys `_joy_AXIS_useN` / `_joy_BUTN_useN`; 25 axes and 160 buttons per device, matching `joystick_axis_assignments int[500]` and `joystick_button_assignments int[3200]` (`DataRefs.txt` 2025–2026) | User file, not online: `XP12/Output/preferences/control profiles/final - Joysticks.prf` (lines 4711–4716 `_joy_AXIS_use0..5` = 2, 1, 3, 52, 74, 75; 5211 `_joy_BUTN_use0 sim/none/none`; 5213 `_joy_BUTN_use2 …landing_gear_down`). Not committed (`.gitignore`) | `Sources/Core/settings/bindings.c:12`, `Sources/Core/internal/core_internal.h:90` |
| `Log.txt` "SKIPPED (ff00/..)" | X-Plane's own log showing it discards the DualSense's vendor-page elements over Bluetooth | Local: `XP12/Log.txt` (line 388 when checked: "SKIPPED (ff00/3b) … because of usage page") | `Sources/Core/internal/core_internal.h:8` |
| RakNet, port 49010 | X-Plane's multiplayer networking; binding that port makes X-Plane fail with "Failed to initialize UDP RakNet connectivity" | Spec `TXT.rtf` line 839 (`raknet_port` defaults to 49010); the error string is in the X-Plane binary | `Sources/Core/runtime/core_thread.c:631`, `Sources/Core/settings/config_schema.c:35` |
| Accept incoming connections | X-Plane 12 setting that must be on for UDP input | Label confirmed in the X-Plane binary only; xppython3's UDP page (above) says X-Plane 12 needs it | `Sources/Core/runtime/core.c:112`, `README.md` (Requirements) |
| Install and app bundle | Default folders, the simulator executable, and its version | `~/X-Plane/X-Plane 12`, `/Applications/X-Plane 12`; `X-Plane.app/Contents/MacOS/X-Plane`; `X-Plane.app/Contents/Info.plist` `CFBundleShortVersionString` | `Sources/App/System/XPlaneLocator.swift:30`, `Sources/Core/platform/gate.c:9`, `Sources/Core/platform/gate.c:14` |
| Joystick configurations (`.joy`) | Per-device joystick definitions | [Creating joystick configuration files](https://developer.x-plane.com/article/creating-joystick-configuration-joy-files/); local `XP12/Resources/joystick configs/` | Not used by DSXP |

### Datarefs

All 24 datarefs in the code exist in `DataRefs.txt`, and every one DSXP writes is
marked writable (`y`). Read-only ones are only subscribed to.

| Dataref | `DataRefs.txt` line | Type | Writable | DSXP | Code |
|---|---|---|---|---|---|
| `sim/joystick/yoke_pitch_ratio` | 1996 | float | y | writes | `Sources/Core/sim/xplane.c:11`, `Sources/Core/platform/guard.c:108` |
| `sim/joystick/yoke_roll_ratio` | 1999 | float | y | writes | `Sources/Core/sim/xplane.c:12`, `Sources/Core/platform/guard.c:109` |
| `sim/joystick/yoke_heading_ratio` | 2002 | float | y | writes | `Sources/Core/sim/xplane.c:13`, `Sources/Core/platform/guard.c:110` |
| `sim/cockpit2/engine/actuators/throttle_ratio_all` | 4276 | float | y | writes | `Sources/Core/sim/xplane.c:14` |
| `sim/cockpit2/controls/left_brake_ratio` | 4139 | float | y | writes | `Sources/Core/sim/xplane.c:15`, `Sources/Core/platform/guard.c:111` |
| `sim/cockpit2/controls/right_brake_ratio` | 4140 | float | y | writes | `Sources/Core/sim/xplane.c:16`, `Sources/Core/platform/guard.c:112` |
| `sim/operation/override/override_joystick_pitch` | 3473 | int | y | writes | `Sources/Core/sim/xplane.c:19`, `Sources/Core/platform/guard.c:113`, `Sources/Core/runtime/core_thread.c:728` |
| `sim/operation/override/override_joystick_roll` | 3474 | int | y | writes | `Sources/Core/sim/xplane.c:20`, `Sources/Core/platform/guard.c:114`, `Sources/Core/runtime/core_thread.c:729` |
| `sim/operation/override/override_joystick_heading` | 3472 | int | y | writes | `Sources/Core/sim/xplane.c:21`, `Sources/Core/platform/guard.c:115`, `Sources/Core/runtime/core_thread.c:730` |
| `sim/operation/override/override_toe_brakes` | 3506 | int | y | writes | `Sources/Core/sim/xplane.c:22`, `Sources/Core/platform/guard.c:116`, `Sources/Core/runtime/core_thread.c:731` |
| `sim/flightmodel/position/indicated_airspeed` | 1749 | float | y | reads | `Sources/Core/sim/telemetry.c:17` |
| `sim/weather/aircraft/wind_now_speed_msc` | 3766 | float | n | reads | `Sources/Core/sim/telemetry.c:18` |
| `sim/weather/aircraft/wind_now_direction_degt` | 3765 | float | n | reads | `Sources/Core/sim/telemetry.c:19` |
| `sim/flightmodel/position/psi` | 1732 | float | y | reads | `Sources/Core/sim/telemetry.c:20` |
| `sim/cockpit2/annunciators/stall_warning_ratio` | 3939 | float | y | reads | `Sources/Core/sim/telemetry.c:21` |
| `sim/flightmodel2/misc/has_crashed` | 5601 | int | n | reads | `Sources/Core/sim/telemetry.c:22` |
| `sim/flightmodel/forces/g_nrml` | 1618 | float | n | reads | `Sources/Core/sim/telemetry.c:23` |
| `sim/time/paused` | 3589 | int | n | reads | `Sources/Core/sim/telemetry.c:24` |
| `sim/flightmodel/position/elevation` | 1729 | double | n | reads | `Sources/Core/sim/telemetry.c:25` |
| `sim/weather/aircraft/turbulence[0..12]` | 3755 | float[13] | n | reads | `Sources/Core/sim/telemetry.c:36` |
| `sim/weather/aircraft/wind_altitude_msl_m[0..12]` | 3750 | float[13] | n | reads | `Sources/Core/sim/telemetry.c:38` |
| `sim/flightmodel2/wing/elements/element_is_stalled[w*10+e]`, wings 0–3, elements 2, 5, 8 | 5662 | float[480] | n | reads | `Sources/Core/sim/telemetry.c:41`, `Sources/Core/sim/telemetry.c:27` |

Default commands (`Sources/Core/settings/bindings.c:30` onward), with their
`Commands.txt` lines: `throttle_down` 75, `throttle_up` 76, `flaps_up` 440,
`flaps_down` 441, `landing_gear_down` 464, `landing_gear_up` 465, `pitch_trim_up`
2837, `pitch_trim_down` 2838, `general/left` 3069, `general/right` 3070,
`zoom_in` 3075, `zoom_out` 3076, `hat_switch_up_left` / `up_right` / `down_left` /
`down_right` 3081–3084, `sim/none/none` 1.

## DualSense and HID

| Reference | What it is | Where to read it | Used at |
|---|---|---|---|
| Linux `hid-playstation.c` | The upstream DualSense driver, the only public description of the Bluetooth reports. Pinned to tag v6.12 because lines move on master | [v6.12 on GitHub](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c) (verified; mirrors: [git.kernel.org](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/hid/hid-playstation.c?h=v6.12), [elixir](https://elixir.bootlin.com/linux/v6.12/source/drivers/hid/hid-playstation.c)). Anchors below | see rows below |
| CRC seed 0xA2 | Bluetooth output reports end in a CRC-32 over 0xA2 + the report | [#L76](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L76), [#L1242-L1245](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L1242-L1245); 0xEDB88320 is the standard reflected CRC-32 polynomial | `Sources/Core/output/output.c:13`, `Sources/Core/output/output.c:154` |
| Input report 0x31, 78 bytes | Bluetooth input report id and size; payload starts at byte 2 | [#L81-L82](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L81-L82), structs [#L198-L229](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L198-L229), Bluetooth parse [#L1336-L1346](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L1336-L1346) | `Sources/Core/internal/core_internal.h:67` |
| Output report layout | Tag 0x10, sequence in the high nibble, `valid_flag0/1/2`, light bar setup, the common block whose `reserved2` holds the two trigger effects | [#L129-L141](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L129-L141), [#L232-L266](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L232-L266) | `Sources/Core/output/output.c:8`, `Sources/Core/output/output.c:25` |
| Touchpad 1920 × 1080 | Touch coordinates and the 4-byte touch point | [#L148-L149](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L148-L149), [#L198-L203](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L198-L203) | `Sources/Core/internal/core_internal.h:82` |
| Player lamps | "Player 1" is the center lamp, `BIT(2)` | [#L1570-L1571](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-playstation.c#L1570-L1571) | `Sources/Core/settings/bindings.c:61` |
| Bluetooth report descriptor | Report 0x31 is one vendor blob: usage page 0xFF00, usage 0x3B, size 8 × count 77, no standard axes or buttons | [nondebug/dualsense, report-descriptor-bluetooth.txt#L49-L58](https://github.com/nondebug/dualsense/blob/main/report-descriptor-bluetooth.txt#L49-L58) | `Sources/Core/internal/core_internal.h:6`, `README.md` (Why it is needed) |
| VID/PID 054C:0CE6 | Sony's vendor id, the DualSense product id | [hid-ids.h#L1194](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-ids.h#L1194) and [#L1202](https://github.com/torvalds/linux/blob/v6.12/drivers/hid/hid-ids.h#L1202); also in Apple's `AppleGameControllerPersonality.kext` Info.plist (1356 / 3302) | `Sources/Core/internal/core_internal.h:42` |
| Adaptive-trigger effects | 0x21 Feedback, 0x25 Weapon, 0x26 Vibration: 10 zones, strength stored as strength − 1 (3 bits), Weapon start zone 2–7 and end after it, up to 8 | Sony publishes nothing. Community source: [Nielk1's notes](https://gist.github.com/Nielk1/6d54cc2c00d2201ccb8c2720ad7538db) (verified: the values and packing match) | `Sources/Core/output/output.c:57`, `Sources/Core/output/output.c:65`, `Sources/Core/output/output.c:95`, `Sources/Core/output/wind.c:17` |
| `AppleGCHIDUserEventDriver` | Apple's private driver that claims the DualSense | No public documentation. Local: `/System/Library/Extensions/AppleGameControllerPersonality.kext/Contents/Info.plist` ("DualSense Controller IOService" → `IOClass AppleGCHIDUserEventDriver`) | `Sources/Core/internal/core_internal.h:13`, `Sources/Core/output/output.c:6` |

### Controller drawing

The controller and trigger drawings in `Resources/web/index.html` were traced
with [potrace](https://potrace.sourceforge.net/) from a stock wireframe image (a
4706 × 3188 JPEG made in Photoshop CS6, embedded description "Sony Play Station 5
dualsense wireless controller wireframe black and white icon"). **Its license is
unknown.** The image itself is not in the repository (`.gitignore:26`);
the traced outlines are (`Resources/web/index.html:74`).
Before distributing DSXP widely, replace the drawing with one you have rights to,
or confirm the image's license. The app icon is original
(`Tools/make-icon.swift:1`).

## Apple and macOS

| Reference | What it is | Where to read it | Used at |
|---|---|---|---|
| IOHIDManager | HID device access | [IOHIDManager.h](https://developer.apple.com/documentation/iokit/iohidmanager_h), [IOHIDManagerOpen](https://developer.apple.com/documentation/iokit/1438369-iohidmanageropen), [IOHIDManagerCreate](https://developer.apple.com/documentation/iokit/1438383-iohidmanagercreate), [IOHIDManagerCopyDevices](https://developer.apple.com/documentation/iokit/1438391-iohidmanagercopydevices), [IOHIDManagerSetDeviceMatching](https://developer.apple.com/documentation/iokit/1438371-iohidmanagersetdevicematching), [kIOHIDTransportKey](https://developer.apple.com/documentation/iokit/kiohidtransportkey) | `Sources/Core/runtime/core_thread.c:286`, `Sources/Core/runtime/core_thread.c:302`, `Sources/Core/platform/guard.c:150` |
| `IOHIDCheckAccess`, `IOHIDRequestAccess` | Input Monitoring check and request | [kIOHIDRequestTypeListenEvent](https://developer.apple.com/documentation/iokit/iohidrequesttype/kiohidrequesttypelistenevent), [IOHIDAccessType](https://developer.apple.com/documentation/iokit/iohidaccesstype). The two functions have no Apple page; the header: [IOHIDLib.h#L192](https://github.com/apple-oss-distributions/IOKitUser/blob/main/hidsystem.subproj/IOHIDLib.h#L192), [#L213](https://github.com/apple-oss-distributions/IOKitUser/blob/main/hidsystem.subproj/IOHIDLib.h#L213) | `Sources/Core/runtime/core_thread.c:339`, `Sources/Core/runtime/core_thread.c:843` |
| Input Monitoring | The privacy permission DSXP needs to read the controller | [Control access to input monitoring on Mac](https://support.apple.com/guide/mac-help/control-access-to-input-monitoring-on-mac-mchl4cedafb6/mac). The `x-apple.systempreferences:…?Privacy_ListenEvent` link that opens it is not documented by Apple | `Sources/App/Lifecycle/AppDelegate.swift:177` |
| `tccutil` | Resets a privacy permission | `man 1 tccutil` (no Apple web page) | `README.md` (Uninstall) |
| SMAppService | Launch at Login, with the "requires approval" state | [SMAppService](https://developer.apple.com/documentation/servicemanagement/smappservice), [mainApp](https://developer.apple.com/documentation/servicemanagement/smappservice/mainapp), [requiresApproval](https://developer.apple.com/documentation/servicemanagement/smappservice/status-swift.enum/requiresapproval), [openSystemSettingsLoginItems()](https://developer.apple.com/documentation/servicemanagement/smappservice/opensystemsettingsloginitems()) | `Sources/App/System/LoginItem.swift:15` |
| WKURLSchemeHandler | Serves the mapper on `dsxp://` with no network port | [WKURLSchemeHandler](https://developer.apple.com/documentation/webkit/wkurlschemehandler) | `Sources/App/Bridge/SchemeHandler.swift:8` |
| ProcessInfo activities | Keep the 60 Hz loop from being throttled; no automatic termination | [beginActivity(options:reason:)](https://developer.apple.com/documentation/foundation/processinfo/beginactivity(options:reason:)), [ActivityOptions](https://developer.apple.com/documentation/foundation/processinfo/activityoptions), [disableAutomaticTermination(_:)](https://developer.apple.com/documentation/foundation/processinfo/disableautomatictermination(_:)) (none of these pages uses the term "App Nap") | `Sources/App/Lifecycle/AppDelegate.swift:136`, `Sources/App/Lifecycle/AppDelegate.swift:22` |
| `proc_listpids`, `proc_pidpath` | Find the running X-Plane | No Apple page; the header: [libproc.h#L92](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/libproc/libproc.h#L92), [#L102](https://github.com/apple-oss-distributions/xnu/blob/main/libsyscall/wrappers/libproc/libproc.h#L102) | `Sources/Core/platform/gate.c:51`, `Sources/Core/platform/gate.c:64` |
| `posix_spawn` | Starts the crash guard | [POSIX posix_spawn](https://pubs.opengroup.org/onlinepubs/9799919799/functions/posix_spawn.html); Apple-only `POSIX_SPAWN_CLOEXEC_DEFAULT`: [spawn.h#L73](https://github.com/apple-oss-distributions/xnu/blob/main/bsd/sys/spawn.h#L73); `man posix_spawnattr_setflags` (ignored signals are inherited unless `POSIX_SPAWN_SETSIGDEF`) | `Sources/Core/platform/guard.c:59`, `Sources/Core/platform/guard.c:73` |
| Hardened Runtime | Signing setting | [Hardened Runtime](https://developer.apple.com/documentation/security/hardened-runtime), [App Sandbox](https://developer.apple.com/documentation/security/app-sandbox) (DSXP is not sandboxed) | `BuildSettings/Base.xcconfig:16`, `Sources/App/Support/DSXP.entitlements:6` |
| Certificates | The free Apple Development certificate used to sign builds | [Certificate types](https://developer.apple.com/help/account/reference/certificate-types/) | `README.md` (Build and install), `Scripts/bootstrap.sh` |
| SF Symbols | `gamecontroller` / `gamecontroller.fill` in the menu bar; symbols may not be used in app icons or logos | [SF Symbols](https://developer.apple.com/sf-symbols/), [HIG: SF Symbols](https://developer.apple.com/design/human-interface-guidelines/sf-symbols), [NSImage(systemSymbolName:)](https://developer.apple.com/documentation/appkit/nsimage/init(systemsymbolname:accessibilitydescription:)) | `Sources/App/UI/StatusMenuController.swift:58` |
| App icon | macOS icon canvas 1024 × 1024 | [HIG: App icons](https://developer.apple.com/design/human-interface-guidelines/app-icons) (1024 confirmed; the 824/100 body and margin are not on the page, see below) | `Tools/make-icon.swift:24` |
| Unified logging | `os_log`, visible in Console | [Logging](https://developer.apple.com/documentation/os/logging) | `Sources/Core/platform/log.c:110` |
| Info.plist keys | `LSUIElement`, `NSBluetoothAlwaysUsageDescription` | [LSUIElement](https://developer.apple.com/documentation/bundleresources/information-property-list/lsuielement), [NSBluetoothAlwaysUsageDescription](https://developer.apple.com/documentation/bundleresources/information-property-list/nsbluetoothalwaysusagedescription) | `Sources/App/Support/Info.plist:29`, `Sources/App/Support/Info.plist:31` |

### Mapper design

The mapper follows Apple's [Human Interface Guidelines](https://developer.apple.com/design/human-interface-guidelines/designing-for-macos):
[color](https://developer.apple.com/design/human-interface-guidelines/color),
[dark mode](https://developer.apple.com/design/human-interface-guidelines/dark-mode),
[typography](https://developer.apple.com/design/human-interface-guidelines/typography),
[layout](https://developer.apple.com/design/human-interface-guidelines/layout),
[writing](https://developer.apple.com/design/human-interface-guidelines/writing) (title-style and sentence-style capitalization),
[buttons](https://developer.apple.com/design/human-interface-guidelines/buttons),
[segmented controls](https://developer.apple.com/design/human-interface-guidelines/segmented-controls),
[pop-up buttons](https://developer.apple.com/design/human-interface-guidelines/pop-up-buttons),
[sliders](https://developer.apple.com/design/human-interface-guidelines/sliders),
[toggles](https://developer.apple.com/design/human-interface-guidelines/toggles),
[boxes](https://developer.apple.com/design/human-interface-guidelines/boxes),
[lists and tables](https://developer.apple.com/design/human-interface-guidelines/lists-and-tables),
[labels](https://developer.apple.com/design/human-interface-guidelines/labels),
[menus](https://developer.apple.com/design/human-interface-guidelines/menus),
[alerts](https://developer.apple.com/design/human-interface-guidelines/alerts),
[offering help](https://developer.apple.com/design/human-interface-guidelines/offering-help),
[windows](https://developer.apple.com/design/human-interface-guidelines/windows).

What WebKit provides, and where DSXP uses it:

| Fact | Source | Used at |
|---|---|---|
| `-apple-system-label`, `-secondary-label`, `-separator`, `-control-background`, `-green` and the other semantic color keywords resolve to the system's NSColors in a WKWebView, and `@supports (color: -apple-system-label)` is a valid test (checked in DSXP: `CSS.supports` returns true and the label color resolves to `rgba(0,0,0,0.847)` in light, `rgba(255,255,255,0.847)` in dark) | [CSSValueKeywords.in](https://raw.githubusercontent.com/WebKit/WebKit/main/Source/WebCore/css/CSSValueKeywords.in), WebKit `Source/WebCore/rendering/mac/RenderThemeMac.mm` | `Resources/web/app.css:40` |
| Without `color-scheme: light dark`, system colors and native controls resolve light even in Dark Mode | WebKit `Source/WebCore/dom/Document.cpp` (`useDarkAppearance`) | `Resources/web/app.css:21`, `Resources/web/index.html:6` |
| There is no keyword for the window background, and `-apple-system-control-accent` is fixed blue (not the user's accent) in a normal WKWebView, so the app injects both | `RenderThemeMac.mm`; [WebKit commit 04c640bb](https://github.com/WebKit/WebKit/commit/04c640bb1dcea7ab1a75bbdc59a891dde2e9c499) | `Sources/App/UI/MapperWindowController.swift:63` |
| `accent-color` left unset (`auto`) lets native sliders and checkboxes follow the user's accent | [MDN accent-color](https://developer.mozilla.org/en-US/docs/Web/CSS/accent-color) | `Resources/web/app.css:125` |
| `-apple-system` / `system-ui` select San Francisco | [WebKit: Using the system font in web content](https://webkit.org/blog/3709/using-the-system-font-in-web-content/) | `Resources/web/app.css:26` |
| `font-variant-numeric: tabular-nums` keeps changing numbers from jittering (the CSS form of `monospacedDigitSystemFont`) | [MDN browser compatibility data](https://github.com/mdn/browser-compat-data) | `Resources/web/app.css:64` |
| The color picker is DSXP's own popover (field, hue strip, hex, presets, Cancel and OK), not `<input type=color>`, which opens the system color panel that has no OK button | — | `Resources/web/app.js:267`, `Resources/web/index.html:38` |
| `<input type=checkbox switch>` is drawn as a native switch (seen in DSXP's own WebKit render on macOS 27.2); a WebKit without it shows a plain checkbox, which works the same | DSXP render check | `Resources/web/index.html:368` |

## Build and legal

| Reference | Where to read it | Used at |
|---|---|---|
| XcodeGen | [github.com/yonaskolb/XcodeGen](https://github.com/yonaskolb/XcodeGen) | `project.yml`, `Scripts/bootstrap.sh`, `README.md` |
| Apache License 2.0 | [apache.org/licenses/LICENSE-2.0](https://www.apache.org/licenses/LICENSE-2.0) | `LICENSE`, `README.md`, `Sources/App/Support/Info.plist` |
| "DualSense" and "PlayStation" trademarks | [PlayStation copyright and trademark notice](https://www.playstation.com/en-us/legal/copyright-and-trademark-notice/) (verified: both listed) | `README.md` (License and trademarks) |
| X-Plane | x-plane.com publishes no trademark statement, so the README only says who makes it | `README.md` |

## Limits and known pitfalls

Each item says where the limit comes from and where DSXP handles it.

### Buffers and sizes

No `strcpy`, `strcat`, `sprintf`, `strncpy` or `gets` anywhere; every copy is
`snprintf`, `memcpy` with a checked length, or bounded `sscanf` widths.

| Buffer | Size | Where | Worst case | Bounded by |
|---|---|---|---|---|
| Command path / description | 256 / 256 | `Sources/Core/internal/core_internal.h:48` | Longest path in `Commands.txt`: 51 characters | `cmd_valid` (< 256), parse caps at 255 |
| Command catalog | 8192 entries | `Sources/Core/internal/core_internal.h:47` | 3165 in X-Plane 12.4 | count checked while parsing |
| `DREF` / `RREF` / `CMND` packets | 509 / 413 / ≤ 505 bytes | `Sources/Core/internal/core_internal.h:369` | Fixed by the spec (5 + 4 + 500, 5 + 4 + 4 + 400, 5 + path + NUL) | path capped at 499 / 399 / 499 characters |
| UDP receive buffer | 2048 bytes | `Sources/Core/runtime/core_thread.c:315` | An `RREF` reply holds at most 183 pairs (1469 bytes); DSXP subscribes 47 ids (≤ 381 bytes) | `recvfrom` length |
| Telemetry pairs per reply | 128 | `Sources/Core/sim/telemetry.c:134` | 47 | parse caps at the array size |
| Bluetooth report | 78 bytes in, 78 out | `Sources/Core/internal/core_internal.h:64` | Fixed by the device | reports shorter than 42 bytes are dropped (`Sources/Core/internal/core_internal.h:87`) |
| Status notes | 64 | `Sources/Core/internal/core_internal.h:263` | About 30 characters | `snprintf` truncates |
| Config file | 256 KB per file, 1023 characters per line, 32 reported problems per file | `Sources/Core/internal/config.h:57`, `Sources/Core/internal/config.h:56`, `Sources/Core/settings/conf.c:340` | About 9 KB each | larger files are refused with a warning; further problems are counted (`dropped`) |
| Config text buffer | 64 KB | `Sources/Core/runtime/config_store.c:54` | About 9 KB | `conf_format` returns 0 if it would not fit |
| Old `bindings.conf` | 1 MB, 1023 characters per line | `Sources/Core/internal/core_internal.h:65` | About 3 KB | `file_read_all` cap; long lines skipped |
| Paths | 1024 (data, log folders), 1100 (files) | `Sources/Core/runtime/core_thread.c:46` | User-chosen | `snprintf` result checked; a path that would be cut is refused (`Sources/Core/runtime/config_store.c:88`) |
| `/api/init` JSON | about 212 KB | — | 3165 commands | grows on demand |
| Core thread stack | 1 MB | `Sources/Core/runtime/core_thread.c:747` | Largest locals about 16 KB; big buffers are `static` or heap | Apple's default for secondary threads is 512 KB ([Threading Programming Guide](https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/Multithreading/CreatingThreads/CreatingThreads.html)) |

### UDP

- `sendto` gives no delivery feedback ("No indication of failure to deliver is
  implicit in a send()", `man 2 sendto`). A busy X-Plane can drop DSXP's packets,
  so overrides are re-asserted every `timing.override_reassert`, a handed-back axis
  gets `timing.override_release_repeats` further releases
  (`Sources/Core/sim/xplane.c:107`), and the throttle has a
  keepalive (`Sources/Core/sim/xplane.c:203`).
- `recvfrom` discards bytes beyond the buffer (`man 2 recvfrom`); the buffer is
  larger than any reply X-Plane can send (above).
- This Mac: `net.inet.udp.recvspace` 786896, `net.inet.udp.maxdgram` 9216,
  `kern.ipc.maxsockbuf` 8388608 (`sysctl`, macOS 27.2). DSXP keeps the default
  receive buffer.
- DSXP binds `127.0.0.1` on a system-chosen port
  (`Sources/Core/runtime/core_thread.c:641`)
  and accepts telemetry only from loopback
  (`Sources/Core/runtime/core_thread.c:320`).

### X-Plane behavior

- **Overrides.** The yoke and brake datarefs only take effect with their override
  flag set (`DataRefs.txt` above). Every exit path releases them: disengage
  (`Sources/Core/sim/xplane.c:83`), quit, and the crash guard
  (`Sources/Core/platform/guard.c:106`), which also zeroes
  the brakes.
- **Engagement needs telemetry.** DSXP takes the controls only while X-Plane
  answers its `RREF` subscription
  (`Sources/Core/runtime/core.c:112`); with networking
  off it stays idle and the menu says "Not answering".
- **Subscriptions.** One `RREF` per array element; frequency 0 cancels. While no
  data arrives DSXP resubscribes every `telemetry.resubscribe`
  (`Sources/Core/sim/telemetry.c:199`),
  and it cancels them on exit, from the guard too.
- **Pause.** Telemetry may keep streaming while the sim is paused, so DSXP reads
  `sim/time/paused` and releases sim rumble and triggers while it is 1
  (`Sources/Core/sim/telemetry.c:212`).
- **Ports.** X-Plane receives on 49000 unless changed in its Network settings
  (`xplane.port`); 49010 belongs to RakNet. A port change in `bridge.conf` releases
  everything on the old port first
  (`Sources/Core/runtime/core_thread.c:468`) and
  tells the guard (`Sources/App/Lifecycle/AppDelegate.swift:93`).
- **Weather and wings in X-Plane 12.** Turbulence is per layer, interpolated at the
  aircraft's elevation (`Sources/Core/sim/telemetry.c:50`).
  Stall is sampled on wings 0–3 so aircraft whose outer main wing is "wing 2" in
  Plane Maker still cue.
- **Commands.** `CMND` has no begin/end over UDP (xppython3 CMND page), so "hold"
  repeats the command at `timing.button_repeat_rate`. Commands that need a held
  state, such as engine starters, cannot be held this way.
- **Version.** X-Plane 11 has the same executable path; DSXP checks the bundle
  version and ignores it (`Sources/Core/platform/gate.c:14`).

### DualSense

- Weapon needs its start zone in 2–7 and an end zone after it (Nielk1); DSXP clamps
  (`Sources/Core/output/output.c:94`).
- Force has 8 levels and position 10 zones
  (`Sources/Core/output/output.c:66`), which the config comments state.
- Adaptive-trigger and light-bar writes are coalesced to one report per frame and
  sent only on change, plus a keepalive (`Sources/Core/runtime/core.c:53`).

### C

- **String to number** ([CERT ERR34-C](https://cmu-sei.github.io/secure-coding-standards/sei-cert-c-coding-standard/rules/error-handling-err/err34-c)):
  `atoi` and `sscanf` cannot report overflow. The API uses `strtol`/`strtod` with
  `errno`, end-pointer and range checks (`Sources/Core/api/api.c:50`).
- **Locale.** `strtod` and `printf` follow the process locale, so `6,0` in German;
  the config files use `strtod_l` / `snprintf_l` with the C locale
  (`Sources/Core/settings/conf.c:29`), tested under `de_DE`
  (`Tests/CoreTests/test_config.c:333`).
- **Struct padding** (CERT EXP42-C): padded structs are compared field by field,
  not with `memcmp` (`Sources/Core/output/output.c:125`).
- **Clocks.** `CLOCK_MONOTONIC_RAW` keeps counting during sleep (`man clock_gettime`;
  `Sources/Core/runtime/core_thread.c:116`). macOS
  `pthread_cond_timedwait` compares against the wall clock, so waits use
  `pthread_cond_timedwait_relative_np` (`Sources/Core/runtime/core_thread.c:783`).
- **Spawning.** Signals ignored in the parent stay ignored in the child unless
  `POSIX_SPAWN_SETSIGDEF` is set; descriptors are close-on-exec at once
  (`Sources/Core/platform/guard.c:59`).
- **Shutdown.** A Bluetooth write can block; the writer thread gets 0.3 s, then
  `dsxp_stop` reports failure and the guard cleans up
  (`Sources/Core/runtime/core_thread.c:427`).

### Swift and WebKit

- `MainActor.assumeIsolated` traps off the main thread
  ([fatbobman](https://fatbobman.com/en/posts/mainactor-assumeisolated/)); DSXP only
  calls it after hopping to the main queue
  (`Sources/App/Lifecycle/AppDelegate.swift:57`).
- A `WKURLSchemeTask` must not be touched after `stop`
  ([Apple forums 712430](https://developer.apple.com/forums/thread/712430)); the
  handler tracks live tasks (`Sources/App/Bridge/SchemeHandler.swift:99`).
- `SMAppService` has a "requires approval" state
  ([theevilbit](https://theevilbit.github.io/posts/smappservice)); the menu shows
  it as a third state (`Sources/App/System/LoginItem.swift:17`).
- The page's WebContent process can be killed; the window reloads it
  (`Sources/App/UI/MapperWindowController.swift:153`).
- `NSApp.activate()` is cooperative on macOS 14 and later and may not bring the
  mapper forward from a menu-bar app
  ([openjfx-dev](https://mail.openjdk.org/pipermail/openjfx-dev/2025-January/051876.html));
  not yet tested on hardware (`Sources/App/UI/MapperWindowController.swift:20`).

### Config files

- Polled every 2 s (fixed, so a bad edit cannot slow its own fix); a file modified
  less than 1 s ago waits one more poll; a file only touched is ignored by hash
  (`Sources/Core/internal/config.h:58`).
- Mapper saves are debounced (`timing.save_delay`) and atomic (temporary file,
  `fsync`, rename).

## Observed, not documented

These are in comments as "observed". What would settle each:

| Claim | Where | How to verify |
|---|---|---|
| Overrides reset when an aircraft is reloaded | `Sources/Core/sim/xplane.c:126` | Reload an aircraft while engaged and watch `override_joystick_*` in a dataref inspector |
| `RREF` subscriptions are dropped across reloads | `Sources/Core/sim/telemetry.c:199` | Count replies before and after a reload with DSXP's resubscribe disabled |
| X-Plane's null zone does not apply to override datarefs | `Sources/Core/input/decode.c:8`, `Sources/Core/settings/config_schema.c:126`, `Resources/web/index.html:263` | Set a large null zone in X-Plane and write small values through the override |
| Telemetry keeps streaming while paused | `Sources/Core/sim/telemetry.c:212` | Pause and count `RREF` replies |
| macOS sets its own light bar color | `Sources/Core/runtime/core.c:49`, `Resources/web/app.js:335` | Stop the keepalive and watch the bar |
| `AppleGCHIDUserEventDriver` latches the controller into report 0x31 | `Sources/Core/internal/core_internal.h:12` | Compare report ids before and after the driver attaches (`ioreg`) |
| The 824 pt body / 100 pt margin of the icon grid | `Tools/make-icon.swift:25` | Apple's macOS icon template |
| "Accept incoming connections" is the setting's label | `README.md` (Requirements) | X-Plane 12 Settings › Network |
| Turbulence layers have the same center altitudes as wind layers | `Sources/Core/sim/telemetry.c:50` | `DataRefs.txt` describes `wind_altitude_msl_m` only for wind |

Considered and not cited: an SDL issue and an X-Plane forum thread (a candidate,
[forums.x-plane.org topic 348775](https://forums.x-plane.org/forums/topic/348775-mac-dualsense-controller-detection-and-calibration/),
returned 403 and was not read), `.joy` files, and `X-Plane Joystick Settings.prf`.
