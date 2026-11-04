/*
 * The three config files: every key, its range and unit, and the comment text
 * written above it. This table is the single source of truth: the files DSXP
 * writes at runtime and the committed defaults in configs/ are both generated
 * from it (a test fails if configs/ drifts from it).
 */
#include "core_internal.h"
#include <stddef.h>

#define OFF(field) offsetof(config_t, field)
#define K(key, type, field, lo, hi, dec, unit, doc) \
    { key, type, OFF(field), lo, hi, dec, unit, NULL, 0, doc }
#define KE(key, type, field, names, doc) \
    { key, type, OFF(field), 0, 0, 0, "", names, (int)(sizeof names / sizeof names[0]), doc }
#define KX(key, type, field, doc) { key, type, OFF(field), 0, 0, 0, "", NULL, 0, doc }
#define SEC(title, doc) { title, CT_SECTION, 0, 0, 0, 0, "", NULL, 0, doc }
#define BTN(slot, name) { "button." name, CT_BUTTON, OFF(s.bind[slot]), 0, 0, 0, "", NULL, 0, NULL }
#define AXK(src, name)  { "axis." name, CT_AXIS, OFF(s.axis[src]), 0, 0, 0, "", NULL, 0, NULL }

static const char *const FXSRC[]   = { "manual", "wind" };
static const char *const RSRC[]    = { "manual", "sim" };
static const char *const LSRC[]    = { "manual", "sim" };
static const char *const RPATH[]   = { "classic", "v2" };
static const char *const TMODE[]   = { "off", "resist", "weapon", "vibrate" };
static const char *const PATTERN[] = { "solid", "breathe", "pulse", "strobe", "rainbow", "alert" };

/* ------------------------------------------------------------- bridge.conf */

static const conf_key_t BRIDGE_KEYS[] = {
    SEC("X-Plane connection", NULL),
    K("xplane.port", CT_INT, bridge.port, 1024, 65535, 0, "",
      "X-Plane's UDP port for incoming data (X-Plane: Settings > Network > UDP ports).\n"
      "Leave it at 49000 unless you changed that port in X-Plane: the two must match, or\n"
      "X-Plane ignores everything DSXP sends and the menu shows \"running, not answering\".\n"
      "Do not use 49010: it belongs to X-Plane's multiplayer networking."),

    SEC("Timing", NULL),
    K("timing.tick_rate", CT_REAL, bridge.tick_hz, 30, 120, 0, "Hz",
      "How many times a second DSXP reads the controller and sends it to X-Plane.\n"
      "Higher: lower control latency (60 Hz adds at most about 17 ms, 120 Hz about 8 ms),\n"
      "for more packets and CPU. Over Bluetooth the controller reports about 62 times a\n"
      "second, so going far above that gains little.\n"
      "Lower: less work, but control feels steppy and laggy, most of all in the flare."),
    K("timing.button_repeat_rate", CT_REAL, bridge.repeat_hz, 1, 60, 0, "Hz",
      "How fast a button set to \"hold\" repeats its command while held (trim, zoom, views).\n"
      "Higher: trim and views move faster while held, but are harder to stop exactly.\n"
      "Lower: slower, more precise steps."),
    K("timing.input_stale", CT_TIME, bridge.input_stale_s, 50, 2000, 0, "ms",
      "If no controller report arrives for this long, DSXP holds the stick axes at center\n"
      "and releases every button instead of repeating the last values.\n"
      "Lower: a Bluetooth dropout, or a controller switched off mid-turn, centers the\n"
      "controls sooner; too low and normal Bluetooth hiccups cause brief twitches to center\n"
      "(reports arrive about every 16 ms). Higher: rides out longer hiccups, but after a\n"
      "real dropout the aircraft keeps the last stick deflection for this long."),
    K("timing.input_lost", CT_TIME, bridge.input_lost_s, 0.5, 10, 1, "s",
      "If no controller report arrives for this long, DSXP disengages and gives X-Plane its\n"
      "controls back. Lower: X-Plane's own joystick or mouse takes over sooner after the\n"
      "controller dies. Higher: short Bluetooth losses do not bounce control back and forth."),
    K("timing.override_reassert", CT_TIME, bridge.override_reassert_s, 0.2, 10, 1, "s",
      "How often DSXP repeats \"DSXP is flying this axis\" to X-Plane while engaged.\n"
      "X-Plane drops these overrides if it was not listening yet, and (observed) when an\n"
      "aircraft is reloaded; until the next repeat the stick does nothing.\n"
      "Lower: control comes back sooner after a reload, for a few more tiny packets that\n"
      "never leave this Mac. Higher: fewer packets, but after a reload the stick can be dead\n"
      "for up to this long. Release messages for an axis handed back use the same spacing."),
    K("timing.override_release_repeats", CT_INT, bridge.override_release_repeats, 1, 10, 0, "",
      "How many extra times DSXP repeats \"release this axis\" after handing an axis back to\n"
      "X-Plane (unmapping it, disengaging), in case X-Plane was busy and dropped the first.\n"
      "Higher: safer when X-Plane is loading or stuttering at that moment. Lower: fewer packets."),
    K("timing.throttle_keepalive", CT_TIME, bridge.throttle_keepalive_s, 0, 60, 1, "s",
      "The throttle has no override, so DSXP sends it only when the stick moves, plus this\n"
      "keepalive. Lower: X-Plane's throttle snaps back to your stick sooner after something\n"
      "else moved it. Higher, or 0 to turn it off: the autothrottle, mouse or keyboard can move\n"
      "the throttle without DSXP putting it back."),
    K("timing.gate_poll", CT_TIME, bridge.gate_poll_s, 0.5, 10, 1, "s",
      "How often DSXP checks whether X-Plane is running and whether the controller is on USB\n"
      "or Bluetooth. Lower: engages sooner after X-Plane starts and steps aside sooner when you\n"
      "plug in the cable; each check scans the process list, a small CPU cost. Higher: slower\n"
      "to notice either."),
    K("timing.save_delay", CT_TIME, bridge.save_delay_s, 100, 5000, 0, "ms",
      "After a change in the mapper, DSXP waits this long for more changes before writing\n"
      "controls.conf or feedback.conf. Lower: saved sooner. Higher: fewer disk writes while\n"
      "you drag a slider."),

    SEC("Telemetry", "Flight data X-Plane sends back: airspeed, stall, g-load, turbulence and so on.\n"
                     "It feeds the wind trigger effects, simulator rumble and the simulator light bar, and its arrival\n"
                     "is how DSXP knows X-Plane is answering."),
    K("telemetry.rate", CT_REAL, bridge.telemetry_hz, 1, 30, 0, "Hz",
      "How many times a second X-Plane sends the flight data.\n"
      "Higher: triggers and rumble react faster (the stall buzz starts sooner).\n"
      "Lower: less traffic, but feedback lags the aircraft."),
    K("telemetry.timeout", CT_TIME, bridge.telemetry_timeout_s, 1, 30, 1, "s",
      "If no flight data arrives for this long, DSXP treats X-Plane as gone: sim rumble and\n"
      "wind triggers stop and the bridge disengages. Lower: rumble stops sooner if X-Plane\n"
      "hangs; too low and X-Plane's short stalls while loading scenery switch feedback off\n"
      "and on. It must cover at least three telemetry periods."),
    K("telemetry.resubscribe", CT_TIME, bridge.telemetry_resub_s, 1, 60, 1, "s",
      "While no flight data arrives, DSXP asks X-Plane for it again this often (X-Plane may\n"
      "drop the request across reloads). Lower: feedback returns sooner after a reload."),

    SEC("Logging", "Logs are in ~/Library/Logs/DSXP (menu: Open Logs Folder)."),
    K("log.retention_days", CT_INT, bridge.log_retention_days, 1, 365, 0, "days",
      "Log files older than this are deleted. Higher keeps more history for troubleshooting."),
    K("log.file_cap", CT_INT, bridge.log_file_cap_mb, 1, 100, 0, "MB",
      "A single day's log file stops growing at this size (later lines that day are dropped).\n"
      "Higher: a long verbose session is logged in full, for more disk space.\n"
      "Lower: less disk space, but the end of a busy day may be missing."),
    K("log.report_window", CT_TIME, bridge.report_window_s, 1, 300, 0, "s",
      "Length of each controller report-rate window in the reports log (also the Hz shown in\n"
      "the menu). Shorter: the rate reacts faster but is noisier."),
    K("log.idle_report_interval", CT_TIME, bridge.idle_report_s, 60, 86400, 0, "s",
      "With nothing happening, a report line is written only this often.\n"
      "Lower: a steady heartbeat in the reports log (shows DSXP was alive). Higher: quieter logs."),
    KX("log.verbose", CT_BOOL, bridge.log_verbose,
      "yes: also log every command sent to X-Plane. Useful when a binding seems to do\n"
      "nothing; the logs grow quickly."),
};

/* ----------------------------------------------------------- controls.conf */

static const conf_key_t CONTROLS_KEYS[] = {
    SEC("Sticks", NULL),
    K("stick.deadzone", CT_PCT, s.tune.deadzone, 0, 45, 1, "%",
      "How far a stick must leave center before DSXP sends any input to X-Plane.\n"
      "DSXP sends your stick input to X-Plane through override datarefs; X-Plane's null zone does not\n"
      "apply to them (observed), so this is the only thing filtering stick drift.\n"
      "Raise it if the aircraft rolls or pitches by itself with your thumbs off the sticks\n"
      "(worn sticks rest a few percent off center). Lower it for finer corrections in the\n"
      "flare or on a localizer; too low and hands-off flight wanders.\n"
      "Steps under 0.8 % do nothing (the stick has 127 steps each way)."),
    K("stick.curve", CT_REAL, s.tune.curve, 0.40, 3.00, 2, "",
      "Response curve, applied after the deadzone. 1.00 is linear.\n"
      "Above 1: softer around center with full deflection unchanged - finer control for small\n"
      "corrections (holding altitude, the flare), but brisk maneuvers need bigger movements.\n"
      "Below 1: more sensitive around center - quick and twitchy, harder to fly smoothly."),

    SEC("Touchpad gestures", NULL),
    K("gesture.sensitivity", CT_PCT, s.tune.gesture, 0, 100, 1, "%",
      "How easily swipes and taps are recognized. It moves the swipe and tap limits below\n"
      "between their \"at_0\" and \"at_100\" values.\n"
      "Higher: short flicks count as swipes - quick, but a thumb resting on the pad can\n"
      "trigger one. Lower: only deliberate strokes count."),
    K("gesture.tap_time", CT_TIME, s.gesture.tap_time_s, 100, 1000, 0, "ms",
      "A touch must lift within this time to count as a tap. Higher: slow taps still count,\n"
      "but a short press-and-hold does too. Lower: only quick taps count."),
    K("gesture.pulse", CT_TIME, s.gesture.pulse_s, 20, 500, 0, "ms",
      "How long a recognized gesture holds its button down. It must last at least 1.5 ticks\n"
      "so the loop sees it. Higher: a gesture bound to a \"hold\" command repeats a few times.\n"
      "Lower: one clean press."),
    K("gesture.swipe_length_at_0", CT_PCT, s.gesture.swipe_length_at0, 5, 80, 1, "%",
      "Shortest swipe, as a share of the pad width, when gesture.sensitivity is 0 %.\n"
      "Higher: at low sensitivity only long, deliberate strokes count. Lower: shorter ones do."),
    K("gesture.swipe_length_at_100", CT_PCT, s.gesture.swipe_length_at100, 2, 50, 1, "%",
      "Shortest swipe when gesture.sensitivity is 100 %. Must stay above tap_wander_at_100.\n"
      "Lower: at high sensitivity a small flick is a swipe.\n"
      "Higher: even at high sensitivity a real stroke is needed."),
    K("gesture.tap_wander_at_0", CT_PCT, s.gesture.tap_wander_at0, 1, 20, 1, "%",
      "Largest finger movement that still counts as a tap, at sensitivity 0 %.\n"
      "Higher: sloppy taps still count. Lower: a tap must land and lift in place."),
    K("gesture.tap_wander_at_100", CT_PCT, s.gesture.tap_wander_at100, 1, 20, 1, "%",
      "Largest finger movement that still counts as a tap, at sensitivity 100 %.\n"
      "Must stay below swipe_length_at_100, or every short swipe reads as a tap."),

    SEC("Axes",
        "Each stick axis and analog trigger moves one X-Plane control: none, roll, pitch, yaw,\n"
        "throttle, rudder_left, rudder_right, brake_left or brake_right. Add \"inverted\" to\n"
        "reverse it. rudder_left and rudder_right use the two triggers as differential rudder\n"
        "pedals. An axis set to none is left entirely to X-Plane."),
    AXK(S_LX, "left_x"), AXK(S_LY, "left_y"), AXK(S_RX, "right_x"), AXK(S_RY, "right_y"),
    AXK(S_L2, "l2"), AXK(S_R2, "r2"),

    SEC("Buttons",
        "Each control sends one X-Plane command. Write the command path as X-Plane lists it\n"
        "(Resources/plugins/Commands.txt in your X-Plane folder, or search in the mapper), then\n"
        "\"hold\" to repeat it while held (trim, zoom, views) or \"once\" for one send per press\n"
        "(gear, flaps). \"none\" leaves the control unassigned. D-pad diagonals and touchpad\n"
        "gestures count as buttons too."),
    BTN(B_DP_UP, "dpad_up"), BTN(B_DP_UR, "dpad_up_right"), BTN(B_DP_R, "dpad_right"),
    BTN(B_DP_DR, "dpad_down_right"), BTN(B_DP_DOWN, "dpad_down"), BTN(B_DP_DL, "dpad_down_left"),
    BTN(B_DP_L, "dpad_left"), BTN(B_DP_UL, "dpad_up_left"),
    BTN(B_SQUARE, "square"), BTN(B_CROSS, "cross"), BTN(B_CIRCLE, "circle"), BTN(B_TRIANGLE, "triangle"),
    BTN(B_L1, "l1"), BTN(B_R1, "r1"), BTN(B_L2_CLICK, "l2_click"), BTN(B_R2_CLICK, "r2_click"),
    BTN(B_CREATE, "create"), BTN(B_OPTIONS, "options"), BTN(B_L3, "l3"), BTN(B_R3, "r3"),
    BTN(B_PS, "ps"), BTN(B_TOUCHPAD_CLICK, "touchpad_click"), BTN(B_MUTE, "mute"),
    BTN(B_TP_TAP, "tap"), BTN(B_TP_TAP2, "two_finger_tap"),
    BTN(B_TP_SWIPE_L, "swipe_left"), BTN(B_TP_SWIPE_R, "swipe_right"),
    BTN(B_TP_SWIPE_U, "swipe_up"), BTN(B_TP_SWIPE_D, "swipe_down"),
    BTN(B_TP_SWIPE_UL, "swipe_up_left"), BTN(B_TP_SWIPE_UR, "swipe_up_right"),
    BTN(B_TP_SWIPE_DL, "swipe_down_left"), BTN(B_TP_SWIPE_DR, "swipe_down_right"),
};

/* ----------------------------------------------------------- feedback.conf */

#define TRIGGER_KEYS(side, idx, name)                                                          \
    KE("trigger." side ".mode", CT_ENUM, s.fx_manual[idx].mode, TMODE,                         \
       "Effect on " name ":\n"                                                                 \
       "off, resist (constant force past a point), weapon (resists across a band, then breaks\n" \
       "through like a gun trigger) or vibrate."),                                             \
    K("trigger." side ".start", CT_PCTB, s.fx_manual[idx].start, 0, 100, 1, "%",               \
      "Where the effect begins, as a share of trigger travel (vibrate: the threshold).\n"      \
      "Lower: you feel it almost at once. Higher: free travel first. The trigger has 10\n"     \
      "zones, so it moves in steps of about 11 %; weapon starts between 22 % and 78 %."),     \
    K("trigger." side ".end", CT_PCTB, s.fx_manual[idx].end, 0, 100, 1, "%",                   \
      "Weapon only: where the resisting band ends and the trigger breaks through.\n"           \
      "Close to start: a short, sharp click. Further away: a long, heavy pull."),              \
    K("trigger." side ".strength", CT_PCTB, s.fx_manual[idx].strength, 0, 100, 1, "%",         \
      "Force for resist and weapon, amplitude for vibrate.\n"                                 \
      "Higher: a heavier pull (or stronger buzz) - more feel, more finger fatigue on long\n"   \
      "flights. Lower: lighter. The trigger has 8 force levels, so it moves in steps of\n"     \
      "about 14 %, and 0 % is the lightest level, not off (use mode = off for that)."),        \
    K("trigger." side ".frequency", CT_U8, s.fx_manual[idx].freq, 1, 255, 0, "Hz",             \
      "Vibrate only: how fast the trigger vibrates.\n"                                         \
      "Lower: a slow, coarse rumble. Higher: a faster, finer buzz.")

static const conf_key_t FEEDBACK_KEYS[] = {
    SEC("Adaptive triggers", NULL),
    KE("trigger.source", CT_ENUM, s.trigger_source, FXSRC,
       "manual: the triggers use the effects set below.\n"
       "wind: X-Plane's airflow sets them (rudder-pedal force, crosswind, buffet); the manual\n"
       "values below are kept and come back when you switch to manual."),
    TRIGGER_KEYS("left", 0, "L2 (left trigger)"),
    TRIGGER_KEYS("right", 1, "R2 (right trigger)"),

    SEC("Wind model", "How trigger.source = wind turns X-Plane's airflow into trigger force."),
    K("wind.full_force_airspeed", CT_REAL, s.wind.full_force_kt, 40, 600, 0, "kt",
      "Indicated airspeed at which the rudder triggers reach full force.\n"
      "Force grows with the square of airspeed, like a real rudder pedal, so at half this\n"
      "speed you feel a quarter of the airspeed force. Set it near your aircraft's cruise\n"
      "speed: about 110 kt for a C172, 160 kt for a Baron, 300 kt for an airliner.\n"
      "Lower: the triggers stiffen early in the takeoff roll and are rock hard in cruise.\n"
      "Higher: light, easy pedals; on a slow aircraft you may never feel much force."),
    K("wind.no_airflow_below", CT_REAL, s.wind.no_airflow_kt, 0, 60, 0, "kt",
      "Below this indicated airspeed the triggers are free (parked, slow taxi). Raise it if\n"
      "the pedals stiffen while taxiing."),
    K("wind.free_play", CT_PCT, s.wind.free_play, 0, 90, 1, "%",
      "Trigger travel before wind resistance starts, like slack in a rudder cable.\n"
      "Higher: more free movement first. Lower: force from the very start."),
    K("wind.base_force", CT_PCT, s.wind.base_force, 0, 100, 1, "%",
      "Resistance at any airflow. Higher: the pedals always feel loaded. Lower: light pedals\n"
      "at low speed."),
    K("wind.airspeed_force", CT_PCT, s.wind.airspeed_force, 0, 100, 1, "%",
      "Extra resistance added at full_force_airspeed. Higher: pedals get much heavier as you\n"
      "speed up. Lower: airspeed changes are felt less."),
    K("wind.crosswind_full", CT_REAL, s.wind.crosswind_full_kt, 2, 60, 1, "kt",
      "Crosswind (the part across the nose) that gives the full left/right bias.\n"
      "Lower: even light crosswinds make one pedal clearly heavier. Higher: only strong ones."),
    K("wind.crosswind_bias", CT_PCT, s.wind.crosswind_bias, 0, 100, 1, "%",
      "How far a full crosswind shifts force between the pedals: the pedal into the wind gets\n"
      "this much heavier and the other this much lighter. 0 % turns the bias off."),
    K("wind.buffet_turbulence", CT_REAL, s.wind.buffet_turbulence, 0, 10, 1, "",
      "Turbulence (X-Plane's 0-10 scale, at the aircraft's altitude) above which the triggers\n"
      "buzz instead of resisting. Lower: buffet in light chop. Higher: only in severe turbulence."),
    K("wind.buffet_start", CT_PCT, s.wind.buffet_start, 0, 100, 1, "%",
      "Trigger travel where the buffet vibration begins.\n"
      "Lower: you feel the buffet with the trigger barely pressed. Higher: only when pressed in."),
    K("wind.buffet_strength_min", CT_PCT, s.wind.buffet_strength_min, 0, 100, 1, "%",
      "Buffet amplitude just above the turbulence threshold.\n"
      "Higher: even light buffet is felt clearly. Lower: light buffet is a faint tremor."),
    K("wind.buffet_strength_max", CT_PCT, s.wind.buffet_strength_max, 0, 100, 1, "%",
      "Buffet amplitude at turbulence 10 (scales between min and max with turbulence).\n"
      "Higher: severe turbulence really shakes the triggers. Lower: a gentler ceiling."),
    K("wind.buffet_frequency_min", CT_REAL, s.wind.buffet_freq_min_hz, 1, 255, 0, "Hz",
      "Buffet frequency just above the threshold: low is a slow, heavy shudder."),
    K("wind.buffet_frequency_max", CT_REAL, s.wind.buffet_freq_max_hz, 1, 255, 0, "Hz",
      "Buffet frequency at turbulence 10: high is a fast, harsh buzz."),

    SEC("Rumble", NULL),
    KE("rumble.source", CT_ENUM, s.rumble_source, RSRC,
       "manual: the motors run only from the mapper's test buttons.\n"
       "sim: the flight model sets them - stall buffet per wing, g-load jolts, and a crash shake."),
    KE("rumble.path", CT_ENUMB, s.rumble_v2, RPATH,
       "classic works on most DualSense firmware. v2 is the newer vibration path; on some\n"
       "firmware it produces no rumble at all. Try v2 only if classic does nothing."),
    K("rumble.stall_warning_floor", CT_PCT, s.rumble.stall_floor, 0, 100, 1, "%",
      "How hard both motors buzz on a straight-ahead stall warning, before either wing\n"
      "actually stalls. Higher: an earlier, stronger warning. Lower: rely on the per-wing\n"
      "buffet, which tells you which wing is dropping."),
    K("rumble.g_jolt_start", CT_REAL, s.rumble.jolt_start_g, 1, 10, 1, "g",
      "Load factor where a jolt begins (hard landing, steep pull-up).\n"
      "Lower: firm landings and brisk turns rumble. Higher: only really hard landings do."),
    K("rumble.g_jolt_full", CT_REAL, s.rumble.jolt_full_g, 1.5, 15, 1, "g",
      "Load factor of a full-strength jolt; between the two the jolt grows in proportion.\n"
      "Must be at least 0.5 g above g_jolt_start. Lower: moderate loads already hit hard.\n"
      "Higher: a gentler build-up, full strength only for real overstress."),
    K("rumble.crash_duration", CT_TIME, s.rumble.crash_s, 0, 30, 1, "s",
      "How long both motors shake at full power after a crash. 0 turns the crash shake off.\n"
      "Higher: an unmistakable crash cue. Lower: a short jolt that does not drag on while\n"
      "X-Plane shows its crash screen."),

    SEC("Light bar", NULL),
    KX("light.on", CT_BOOL, s.light.on,
       "yes: DSXP controls the light bar and player lamps. no: both stay dark."),
    KE("light.source", CT_ENUM, s.light.source, LSRC,
       "manual: the color and pattern below.\n"
       "sim: the bar is an annunciator - green clean, amber caution, red stall, flashing red\n"
       "crash, cyan turbulence."),
    KX("light.color", CT_COLOR, s.light.r, "Light bar color in manual mode."),
    KE("light.pattern", CT_ENUM, s.light.pattern, PATTERN,
       "solid, breathe (slow fade), pulse (quick rise, slow decay), strobe, rainbow, or alert\n"
       "(alternates with amber)."),
    K("light.speed", CT_PCT, s.light.speed, 0, 100, 1, "%",
      "Pattern speed: 0 % is one cycle every 4 seconds, 100 % is 2.5 cycles a second."),
    K("light.brightness", CT_PCT, s.light.bright, 0, 100, 1, "%",
      "Light bar brightness. Lower: easier on the eyes in a dark room, and slightly longer\n"
      "battery life. Higher: readable as an annunciator in a bright room."),
    KX("light.player_lamps", CT_LAMPS, s.light.player,
       "The five small lamps under the touchpad: off, single (center), two, three, four, all,\n"
       "or a pattern such as x...x (x lit, . dark, left to right)."),
    K("light.sim.stall_alarm", CT_PCT, s.light_sim.stall_alarm, 1, 100, 1, "%",
      "Sim mode: stall warning level (X-Plane's stall warning ratio) where the bar pulses red.\n"
      "Lower: red earlier as you approach the stall."),
    K("light.sim.stall_caution", CT_PCT, s.light_sim.stall_caution, 0, 99, 1, "%",
      "Stall warning level where the bar turns amber. Must stay below the alarm level.\n"
      "Lower: amber as soon as the warning starts building. Higher: amber only close to red."),
    K("light.sim.g_caution", CT_REAL, s.light_sim.g_caution, 1, 10, 1, "g",
      "Load factor, positive or negative, that shows amber (level flight is 1 g).\n"
      "Lower: steep turns and firm pull-ups light it. Higher: only aerobatic loads."),
    K("light.sim.turbulence_caution", CT_REAL, s.light_sim.turbulence_caution, 0, 10, 1, "",
      "Turbulence (X-Plane's 0-10 scale) that shows cyan.\n"
      "Lower: cyan in light chop. Higher: only in strong turbulence."),
};

#define N(a) (int)(sizeof a / sizeof a[0])

static const conf_schema_t SCHEMAS[CF_COUNT] = {
    [CF_BRIDGE] = { "bridge.conf", "bridge",
        "How DSXP talks to X-Plane: its network port, how often DSXP does things, and logging.",
        false, BRIDGE_KEYS, N(BRIDGE_KEYS) },
    [CF_CONTROLS] = { "controls.conf", "controls",
        "What the controller does in X-Plane: sticks, touchpad gestures, axes and buttons.",
        true, CONTROLS_KEYS, N(CONTROLS_KEYS) },
    [CF_FEEDBACK] = { "feedback.conf", "feedback",
        "What the controller does back: adaptive triggers, rumble and the light bar, and the\n"
        "flight-model numbers behind their simulator modes.",
        true, FEEDBACK_KEYS, N(FEEDBACK_KEYS) },
};

const conf_schema_t *conf_schema(conf_file_t f) {
    return (f >= 0 && f < CF_COUNT) ? &SCHEMAS[f] : NULL;
}
