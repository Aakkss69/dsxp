/*
 * The core state machine. Pure: it never makes a system call itself. The I/O
 * layer (core_thread.c) feeds it input reports, device events, UDP packets, the
 * gate and UI commands, and calls core_tick() at 60 Hz; everything going out
 * leaves through c->sinks. That is what lets the tests drive it with a fake
 * clock and capture every packet.
 */
#include "core_internal.h"
#include <math.h>            /* round */
#include <stdio.h>
#include <stdlib.h>          /* abs */
#include <string.h>

static void apply_saved_calibration(core_t *c);   /* below, with the calibration helpers */

void core_init(core_t *c, const settings_t *s, const core_sinks_t *sinks) {
    memset(c, 0, sizeof *c);
    c->s = *s;
    c->bridge = BRIDGE_DEFAULTS;
    if (sinks) c->sinks = *sinks;
    snprintf(c->rumble_note, sizeof c->rumble_note, "%s", c->s.rumble_source == RUMBLE_SRC_SIM ? "Waiting for X-Plane" : "Manual");
    snprintf(c->light_note, sizeof c->light_note, "Manual");
    snprintf(c->wx.trigger_note, sizeof c->wx.trigger_note, "%s",
             c->s.trigger_source == TRIGGER_SRC_WIND ? "Waiting for X-Plane" : "Manual");
    if (c->s.trigger_source == TRIGGER_SRC_MANUAL) {
        c->fx_out[0] = trig_effect(&c->s.fx_manual[0]);
        c->fx_out[1] = trig_effect(&c->s.fx_manual[1]);
    }
    c->out_dirty = true;
    c->force_gate = true;
    motion_init(&c->motion);                     /* level = flat, face up, until calibrated */
    motion_cal_nominal(&c->motion_cal);          /* usable before report 0x05 is read      */
    c->motion_cal_src = MOTION_CAL_NONE;         /* not read yet                           */
    apply_saved_calibration(c);                  /* the calibrated level and gyro drift    */
    /* Start from the saved state, so starting with motion already on is not taken as
     * "just switched on" (which would recenter before any data arrived). */
    c->motion_was_enabled = c->s.motion.enabled;
    for (int i = 0; i < MOTION_AXES; i++) c->motion_was_owned[i] = motion_owns(&c->s.motion, i);
}

void out_mark(core_t *c) { c->out_dirty = true; }

/* True while the calibration routine runs. Meanwhile the sticks fly every axis, motion's
 * included (xplane.c xp_push), and DSXP's own vibration is silenced (out_send), because
 * the routine measures the gyro at rest. */
bool core_calibrating(const core_t *c) {
    return c->calib.step != CALIB_IDLE && c->calib.step != CALIB_DONE;
}

/* True while DSXP makes the controller vibrate: rumble motors, or a trigger in Vibrate.
 * Never during the calibration, which silences both. */
static bool vibrating(const core_t *c) {
    return !core_calibrating(c) &&
           (c->rumble.left || c->rumble.right ||
            c->fx_out[0].mode == TFX_VIBRATE || c->fx_out[1].mode == TFX_VIBRATE);
}

/* Put the calibrated level and gyro drift from the settings into the fusion, and
 * remember what was applied so motion_edges() notices when they change. Before the first
 * calibration the level is "lying flat" and the drift starts from the controller's
 * factory zero (core_set_motion_cal). */
static void apply_saved_calibration(core_t *c) {
    const motion_cfg_t *m = &c->s.motion;
    if (m->calibrated) {
        motion_set_level(&c->motion, m->level_bank_deg, m->level_pitch_deg);
        memcpy(c->motion.bias, m->gyro_bias_dps, sizeof c->motion.bias);
        c->motion.bias_learned = true;           /* measured, so no need to wait for it    */
    }
    c->applied_calibrated = m->calibrated;
    c->applied_level[0] = m->level_bank_deg;
    c->applied_level[1] = m->level_pitch_deg;
    memcpy(c->applied_bias, m->gyro_bias_dps, sizeof c->applied_bias);
}

static void out_send(core_t *c) {
    if (!c->sinks.out_report) return;
    out_frame_t f;
    memset(&f, 0, sizeof f);
    bool hush = core_calibrating(c);             /* the calibration measures the gyro at rest */
    f.rumble_l = hush ? 0 : c->rumble.left;
    f.rumble_r = hush ? 0 : c->rumble.right;
    f.rumble_v2 = c->s.rumble_v2;
    f.light_on = c->s.light.on;
    /* a DSXP command just acted: all five lamps for a moment, even with the light off
     * (the only confirmation the user gets while looking at X-Plane) */
    f.player = c->lamp_blinking ? 0x1F : c->s.light.player;
    f.player_force = c->lamp_blinking;
    f.r = c->eff_r; f.g = c->eff_g; f.b = c->eff_b;
    for (int i = 0; i < 2; i++)                  /* resistance stays; only Vibrate is held off */
        f.fx[i] = hush && c->fx_out[i].mode == TFX_VIBRATE ? (trigger_fx_t){ TFX_OFF, 0, 0, 0 }
                                                          : c->fx_out[i];
    unsigned char rep[OUT_REPORT_LEN];
    out_build_report(&f, c->out_seq++, rep);
    c->sinks.out_report(c->sinks.ctx, rep);
}

/*
 * Writes are coalesced to at most one per frame. macOS has been observed to set
 * its own light bar color (not documented by Apple), so ours is held with a slow
 * keepalive even when nothing changed.
 */
void out_tick(core_t *c, double now) {
    if (!c->have_output_device) return;
    if (c->out_dirty && now - c->last_send >= 1.0 / c->bridge.tick_hz) {
        c->out_dirty = false;
        c->last_send = c->last_keep = now;
        out_send(c);
        return;
    }
    if (now - c->last_keep >= 0.5) {
        c->last_keep = c->last_send = now;
        out_send(c);
    }
}

/* Leave the controller quiet: no resistance, no buzzing, bar and lamps off.
 * The user's saved light settings are not touched. */
void out_quiet(core_t *c) {
    c->fx_out[0] = c->fx_out[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
    c->rumble.left = c->rumble.right = 0;
    c->eff_r = c->eff_g = c->eff_b = 0;
    if (!c->sinks.out_report || !c->have_output_device) return;
    out_frame_t f;
    memset(&f, 0, sizeof f);
    f.rumble_v2 = c->s.rumble_v2;
    unsigned char rep[OUT_REPORT_LEN];
    out_build_report(&f, c->out_seq++, rep);
    c->sinks.out_report(c->sinks.ctx, rep);
}

/* Feed the motion sensors of the report just decoded into the fusion. Runs for every
 * report, motion control on or off, so the mapper shows live angles and switching on
 * starts from a settled filter. */
static void motion_feed(core_t *c, double now) {
    motion_sample_t smp;
    motion_cal_apply(&c->motion_cal, c->state.gyro_raw, c->state.acc_raw,
                     c->state.sensor_ts, &smp);             /* counts -> deg/s and g      */
    /* DSXP's own vibration shakes the gyro: pause bias learning while it runs (the
     * calibration's FLAT and LEVEL steps wait out the same pause) */
    motion_update(&c->motion, &smp, &c->s.motion, vibrating(c));
    if (!core_calibrating(c)) return;
    /* a reading at the end of the 16-bit range is clipped (a hard shake reaches the
     * accelerometer's 4 g; measured raw 32765) */
    bool saturated = false;
    for (int i = 0; i < 3; i++)
        if (abs(c->state.gyro_raw[i]) >= MOTION_RAW_SATURATED || abs(c->state.acc_raw[i]) >= MOTION_RAW_SATURATED)
            saturated = true;
    calib_update(&c->calib, &c->motion, &smp, saturated);
    if (c->calib.step == CALIB_DONE) core_calib_finish(c, now);   /* the last step just ended */
}

void core_on_report(core_t *c, const unsigned char *buf, long len, double now) {
    if (ds_decode_report_cfg(&c->state, &c->gest, &c->s.tune, &c->s.gesture, buf, len, now)) {
        c->have_report = true;
        c->last_report_at = now;
        c->reports_window++;
        motion_feed(c, now);
    }
}

void core_on_device(core_t *c, bool bluetooth, bool added) {
    c->force_gate = true;
    if (!bluetooth) return;
    c->have_output_device = added;
    if (added) c->out_dirty = true;          /* push our light state straight away */
    else {
        c->have_report = false;
        /* the filter and any held yaw belong to the stream that just ended; the captured
         * level, the learned bias and the calibration stay (same hands, same controller) */
        motion_reset(&c->motion);
        /* so does a touch in progress: kept, the first report after a reconnect would end
         * it against the old position and fire a swipe nobody made */
        memset(&c->gest, 0, sizeof c->gest);
        for (int i = 0; i < 2; i++) c->state.touch[i].active = false;
    }
}

/* The runtime read feature report 0x05 for the controller now connected (core thread).
 * Every result is usable: on failure `cal` holds the nominal scales. */
void core_set_motion_cal(core_t *c, const motion_cal_t *cal, mcal_result_t r) {
    c->motion_cal = *cal;
    c->motion_cal_src = (r == MCAL_OK && cal->factory_axes == 6) ? MOTION_CAL_FACTORY
                                                                  : MOTION_CAL_NOMINAL;
    dsxp_log("factory calibration (report 0x%02X): %s (%d of 6 axes from the controller)",
             MOTION_CAL_REPORT_ID, motion_cal_result_name(r), r == MCAL_OK ? cal->factory_axes : 0);
    /* Not calibrated yet: start the gyro drift at the factory's resting offset (close to
     * the measured drift on the controller tested) rather than at zero. */
    if (!c->s.motion.calibrated && !c->motion.bias_learned)
        memcpy(c->motion.bias, cal->gyro_zero_dps, sizeof c->motion.bias);
}

/* Light all five player lamps for LAMP_BLINK_SEC: the user is looking at X-Plane, not
 * at the Mac, when a bound DSXP command acts. */
static void lamp_blink(core_t *c, double now) {
    c->lamp_blink_until = now + LAMP_BLINK_SEC;
}

/* Start a crossfade on every axis motion flies, from what X-Plane was last sent. */
static void blend_owned(core_t *c, double now) {
    for (int i = 0; i < MOTION_AXES; i++)
        if (motion_owns(&c->s.motion, i)) motion_blend_start(&c->mblend[i], c->sent[i], now);
}

/* The calibration reached its last step: its results become the settings (saved to
 * controls.conf), the fusion already uses them, and motion may now be switched on. Each
 * value is rounded the way controls.conf writes it, so reading the file back later gives
 * exactly what is in use and is not taken for an edit (motion_edges). */
void core_calib_finish(core_t *c, double now) {
    motion_cfg_t *m = &c->s.motion;
    calib_t *k = &c->calib;
    for (int i = 0; i < 3; i++)                                /* 3 decimals in the file     */
        m->gyro_bias_dps[i] = round(k->bias[i] * 1000) / 1000;
    m->level_bank_deg = round(k->level_bank * 10) / 10;       /* 1 decimal in the file      */
    m->level_pitch_deg = round(k->level_pitch * 10) / 10;
    for (int i = 0; i < MOTION_AXES; i++) {
        m->full_deg[i] = k->full[i];                           /* whole degrees already      */
        motion_full_fix(m, i);                                 /* past its deadzone, as every
                                                                  writer of full_deg         */
        k->full[i] = m->full_deg[i];                           /* the sheet shows what saved */
    }
    m->calibrated = true;
    apply_saved_calibration(c);                                /* level exactly as saved     */
    blend_owned(c, now);                                       /* motion flies again: fade in */
    core_mark_dirty(c, now, CFB_CONTROLS);
    lamp_blink(c, now);
    out_mark(c);                                               /* vibration may resume       */
    if (k->warning[0]) dsxp_log("motion calibration: %s", k->warning);
    dsxp_log("motion calibration done: drift %+.3f %+.3f %+.3f deg/s, gravity %.3f g, level bank %.1f pitch %.1f, "
             "full roll %.0f pitch %.0f yaw %.0f deg", k->bias[0], k->bias[1], k->bias[2], k->acc_g,
             m->level_bank_deg, m->level_pitch_deg, m->full_deg[MOTION_ROLL], m->full_deg[MOTION_PITCH],
             m->full_deg[MOTION_YAW]);
}

/* Start the calibration (again), or close it. Closing is refused while motion control
 * has never been calibrated: the mapper requires it before first use. */
static void calibrate_cmd(core_t *c, int action, double now) {
    if (action == CALIB_ACTION_START) {
        calib_start(&c->calib);
        blend_owned(c, now);                                   /* motion's axes fade to the sticks */
        c->motion_hold = false;
        out_mark(c);                                           /* silence DSXP's vibration now */
        dsxp_log("motion calibration started");
    } else if (action == CALIB_ACTION_CLOSE && (c->s.motion.calibrated || c->calib.step == CALIB_DONE)) {
        bool was_running = core_calibrating(c);
        calib_cancel(&c->calib);
        if (was_running) {                                     /* abandoned: back to the saved */
            apply_saved_calibration(c);
            blend_owned(c, now);
            out_mark(c);                                       /* vibration may resume        */
            dsxp_log("motion calibration cancelled; the previous calibration stays");
        }
    }
}

/* Recenter: the attitude held now becomes level, faded in so the controls do not jump.
 * Refused, with nothing changed, when the controller cannot be read as a level (motion.c
 * motion_recenter: face down, on its edge, or tilted past MOTION_LEVEL_MAX_DEG). */
bool core_motion_recenter(core_t *c, double now) {
    if (!motion_recenter(&c->motion)) {
        dsxp_log("motion: not recentered - hold the controller the way you fly level");
        return false;
    }
    blend_owned(c, now);
    lamp_blink(c, now);
    dsxp_log("motion: recentered");
    return true;
}

bool core_input_live(const core_t *c, double now) {
    return c->have_report && now - c->last_report_at <= c->bridge.input_lost_s;
}

bool core_gate_due(const core_t *c, double now, double last_gate) {
    return c->force_gate || now - last_gate >= c->bridge.gate_poll_s;
}

/*
 * The activation gate. All must hold before DSXP takes X-Plane's flight controls:
 *   1. a DualSense on Bluetooth, and NOT also on USB - over USB X-Plane's own
 *      HID handling works, so the bridge must stay out of its way
 *   2. its reports are actually arriving
 *   3. X-Plane 12 is running and answering: its RREF telemetry arrives, which
 *      also proves "Accept incoming connections" is on. Without that X-Plane
 *      ignores every packet, so engaging would only claim control falsely.
 */
void core_set_gate(core_t *c, bool bt, bool usb, bool xp, const char *xp_exe, double now) {
    c->force_gate = false;
    c->bt_present = bt;
    c->usb_present = usb;
    c->xp_running = xp;
    snprintf(c->xp_exe, sizeof c->xp_exe, "%s", xp_exe ? xp_exe : "");
    bool input = core_input_live(c, now);
    bool tele = c->wx.live;
    bool ok = bt && !usb && xp && input && tele && !c->suspended && !c->dry_run;
    if (ok && !c->engaged) {
        dsxp_log("gate: bluetooth=%d usb=%d xplane=%d input=%d telemetry=%d -> ACTIVE", bt, usb, xp, input, tele);
        xp_engage(c, now);
    } else if (!ok && c->engaged) {
        dsxp_log("gate: bluetooth=%d usb=%d xplane=%d input=%d telemetry=%d -> INACTIVE", bt, usb, xp, input, tele);
        xp_disengage(c, now);
    }
}

/* The mapper changed something that lives in `files` (CFB_*). It is saved once
 * bridge.save_delay has passed without further changes. */
void core_mark_dirty(core_t *c, double now, unsigned files) {
    c->cfg_dirty_mask |= files;
    c->cfg_dirty_at = now;
}

unsigned core_save_due(core_t *c, double now) {
    if (!c->cfg_dirty_mask || now - c->cfg_dirty_at < c->bridge.save_delay_s) return 0;
    unsigned m = c->cfg_dirty_mask;
    c->cfg_dirty_mask = 0;
    return m;
}

void core_set_trigger_source(core_t *c, int mode, double now) {
    (void)now;
    int want = mode == TRIGGER_SRC_WIND ? TRIGGER_SRC_WIND : TRIGGER_SRC_MANUAL;
    if (want == c->s.trigger_source) return;
    c->s.trigger_source = want;
    if (want == TRIGGER_SRC_MANUAL) {
        /* hand-set effects come straight back: wind never touched them */
        if (!c->suspended) { c->fx_out[0] = trig_effect(&c->s.fx_manual[0]); c->fx_out[1] = trig_effect(&c->s.fx_manual[1]); }
        snprintf(c->wx.trigger_note, sizeof c->wx.trigger_note, "Manual");
    } else {
        c->fx_out[0] = c->fx_out[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        snprintf(c->wx.trigger_note, sizeof c->wx.trigger_note, "Waiting for X-Plane");
    }
    out_mark(c);
    dsxp_log("trigger source -> %s", want == TRIGGER_SRC_WIND ? "wind" : "manual");
}

void core_set_rumble_source(core_t *c, int mode, double now) {
    (void)now;
    int want = mode == RUMBLE_SRC_SIM ? RUMBLE_SRC_SIM : RUMBLE_SRC_MANUAL;
    if (want == c->s.rumble_source) return;
    c->s.rumble_source = want;
    /* never leave the motors spinning when handing control over */
    c->rumble.left = c->rumble.right = 0;
    snprintf(c->rumble_note, sizeof c->rumble_note, "%s", want == RUMBLE_SRC_SIM ? "Waiting for X-Plane" : "Manual");
    out_mark(c);
    dsxp_log("rumble source -> %s", want == RUMBLE_SRC_SIM ? "sim" : "manual");
}

void core_get_config(const core_t *c, config_t *out) {
    out->bridge = c->bridge;
    out->s = c->s;
}

/* Apply what the config files say (an edit outside the mapper, or a merge),
 * through the same mode setters the mapper uses. Nothing is marked for saving. */
void core_apply_config(core_t *c, const config_t *cfg, unsigned files, double now) {
    static config_t cur;
    core_get_config(c, &cur);
    int old_fx = cur.s.trigger_source, old_rm = cur.s.rumble_source;
    for (int f = 0; f < CF_COUNT; f++)
        if (files & (1u << f)) config_copy_file(&cur, cfg, (conf_file_t)f);
    int new_fx = cur.s.trigger_source, new_rm = cur.s.rumble_source;
    cur.s.trigger_source = old_fx;
    cur.s.rumble_source = old_rm;
    c->s = cur.s;
    c->bridge = cur.bridge;
    if (new_fx != old_fx) core_set_trigger_source(c, new_fx, now);
    if (new_rm != old_rm) core_set_rumble_source(c, new_rm, now);
    if (files & CFB_FEEDBACK) {
        if (c->s.trigger_source == TRIGGER_SRC_MANUAL && !c->suspended) {
            c->fx_out[0] = trig_effect(&c->s.fx_manual[0]);
            c->fx_out[1] = trig_effect(&c->s.fx_manual[1]);
        }
        out_mark(c);
    }
}

static void apply_light(light_t *L, const light_t *v, unsigned m) {
    if (m & LF_R) L->r = v->r;
    if (m & LF_G) L->g = v->g;
    if (m & LF_B) L->b = v->b;
    if (m & LF_ON) L->on = v->on;
    if (m & LF_PATTERN) L->pattern = (v->pattern < 0 || v->pattern >= LP_COUNT) ? LP_SOLID : v->pattern;
    if (m & LF_SPEED) L->speed = clampd(v->speed, 0, 1, L->speed);
    if (m & LF_PLAYER) L->player = v->player & 0x1F;
    if (m & LF_BRIGHT) L->bright = clampd(v->bright, 0, 1, L->bright);
    if (m & LF_SOURCE) L->source = v->source == LIGHT_SRC_SIM ? LIGHT_SRC_SIM : LIGHT_SRC_MANUAL;
}

/* CMD_MOTION: copy the fields the mapper sent, each full-deflection angle held inside its
 * documented range and past its deadzone (settings.c motion_full_range / motion_full_fix,
 * the rules config_validate applies too). Saved only when something changed. Logged when
 * the switch, an axis's source or an invert changed: the slider sends many full-deflection
 * values per second while dragged, and those are saved quietly, as the other tuning. */
static void apply_motion(core_t *c, const motion_cmd_t *v, double now) {
    motion_cfg_t *m = &c->s.motion;
    const motion_cfg_t was = *m;                                  /* to tell what changed */
    if (v->has_enabled) {
        m->enabled = v->enabled && m->calibrated;     /* not before the calibration has run */
        if (v->enabled && !m->calibrated) dsxp_log("motion: not switched on - calibrate first");
    }
    for (int i = 0; i < MOTION_AXES; i++) {
        unsigned bit = 1u << i;                                   /* this axis' bit       */
        if (v->use_mask & bit) m->use[i] = v->use[i];
        if (v->inv_mask & bit) m->invert[i] = v->inv[i];
        if (v->full_mask & bit) {                                 /* the documented range */
            double lo, hi;
            motion_full_range(i, &lo, &hi);
            m->full_deg[i] = clampd(round(v->full[i]), lo, hi, m->full_deg[i]);
        }
        motion_full_fix(m, i);                                    /* past its deadzone    */
    }
    bool switched = m->enabled != was.enabled, tuned = false;    /* field by field: the  */
    for (int i = 0; i < MOTION_AXES; i++) {                       /* struct has padding   */
        if (m->use[i] != was.use[i] || m->invert[i] != was.invert[i]) switched = true;
        if (m->full_deg[i] != was.full_deg[i]) tuned = true;
    }
    if (!switched && !tuned) return;                              /* nothing to save      */
    core_mark_dirty(c, now, CFB_CONTROLS);
    if (switched)
        dsxp_log("motion: %s; roll %s%s, pitch %s%s, yaw %s%s", m->enabled ? "on" : "off",
                 m->use[MOTION_ROLL] ? "motion" : "sticks", m->invert[MOTION_ROLL] ? " (inverted)" : "",
                 m->use[MOTION_PITCH] ? "motion" : "sticks", m->invert[MOTION_PITCH] ? " (inverted)" : "",
                 m->use[MOTION_YAW] ? "motion" : "sticks", m->invert[MOTION_YAW] ? " (inverted)" : "");
}

/* System sleep: hand X-Plane its controls back and quiet the controller. */
void core_set_suspended(core_t *c, bool suspended, double now) {
    if (suspended && !c->suspended) {
        c->suspended = true;
        xp_disengage(c, now);
        c->rumble.left = c->rumble.right = 0;
        c->fx_out[0] = c->fx_out[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
        out_mark(c);
        dsxp_log("suspended (system sleep)");
    } else if (!suspended && c->suspended) {
        c->suspended = false;
        c->force_gate = true;
        if (c->s.trigger_source == TRIGGER_SRC_MANUAL) {
            c->fx_out[0] = trig_effect(&c->s.fx_manual[0]);
            c->fx_out[1] = trig_effect(&c->s.fx_manual[1]);
        }
        out_mark(c);
        dsxp_log("resumed");
    }
}

void core_apply_cmd(core_t *c, const cmd_t *cmd, double now) {
    settings_t *s = &c->s;
    switch (cmd->kind) {
    case CMD_BIND: {
        int i = cmd->key;
        if (i < 0 || i >= B_COUNT) return;
        if (cmd->u.bind.cmd[0] && !cmd_valid(cmd->u.bind.cmd)) return;
        snprintf(s->bind[i].cmd, CMD_PATH_MAX, "%s", cmd->u.bind.cmd);
        s->bind[i].repeat = cmd->u.bind.repeat;
        core_mark_dirty(c, now, CFB_CONTROLS);
        dsxp_log("bind %s -> %s%s", BTN_NAME[i], s->bind[i].cmd[0] ? s->bind[i].cmd : "(cleared)",
                 s->bind[i].repeat ? " [hold]" : "");
        break;
    }
    case CMD_AXIS: {
        int i = cmd->key, t = cmd->u.axis.target;
        if (i < 0 || i >= S_COUNT || t < 0 || t >= AX_TARGET_COUNT) return;
        s->axis[i] = cmd->u.axis;
        core_mark_dirty(c, now, CFB_CONTROLS);
        /* the override planner in xp_push picks the change up on the next tick */
        dsxp_log("axis %s -> %s%s", SRC_LABEL[i], AX_LABEL[t], s->axis[i].invert ? " (inverted)" : "");
        break;
    }
    case CMD_TUNE:
        if (cmd->u.tune.has_dz) s->tune.deadzone = clampd(cmd->u.tune.dz, 0, 0.45, s->tune.deadzone);
        if (cmd->u.tune.has_cv) s->tune.curve    = clampd(cmd->u.tune.cv, 0.4, 3.0, s->tune.curve);
        if (cmd->u.tune.has_gs) s->tune.gesture  = clampd(cmd->u.tune.gs, 0, 1, s->tune.gesture);
        core_mark_dirty(c, now, CFB_CONTROLS);
        break;
    case CMD_LIGHT:
        apply_light(&s->light, &cmd->u.light.v, cmd->u.light.mask);
        core_mark_dirty(c, now, CFB_FEEDBACK);
        out_mark(c);
        break;
    case CMD_TRIGGER: {
        int i = cmd->key;
        if (i < 0 || i > 1 || s->trigger_source == TRIGGER_SRC_WIND) return;   /* wind mode owns them */
        trig_manual_t *f = &s->fx_manual[i];
        unsigned m = cmd->u.trig.mask;
        if (m & TF_MODE) {
            int md = cmd->u.trig.v.mode;
            f->mode = (md < TFX_OFF || md > TFX_VIBRATE) ? TFX_OFF : md;
        }
        if (m & TF_START)    f->start    = cmd->u.trig.v.start;
        if (m & TF_END)      f->end      = cmd->u.trig.v.end;
        if (m & TF_STRENGTH) f->strength = cmd->u.trig.v.strength;
        if (m & TF_FREQ)     f->freq     = cmd->u.trig.v.freq ? cmd->u.trig.v.freq : 1;
        if (!c->suspended) c->fx_out[i] = trig_effect(f);
        core_mark_dirty(c, now, CFB_FEEDBACK);
        out_mark(c);
        break;
    }
    case CMD_TRIGGER_SOURCE: {
        core_set_trigger_source(c, cmd->u.source, now);
        core_mark_dirty(c, now, CFB_FEEDBACK);
        out_mark(c);
        break;
    }
    case CMD_RUMBLE:
        if (s->rumble_source == RUMBLE_SRC_SIM || c->suspended) return;        /* sim mode owns the motors */
        if (cmd->u.rumble.has_l) c->rumble.left  = cmd->u.rumble.l;
        if (cmd->u.rumble.has_r) c->rumble.right = cmd->u.rumble.r;
        snprintf(c->rumble_note, sizeof c->rumble_note, "Manual: left %d%%, right %d%%",
                 c->rumble.left * 100 / 255, c->rumble.right * 100 / 255);
        out_mark(c);
        break;
    case CMD_RUMBLE_SOURCE: {
        core_set_rumble_source(c, cmd->u.source, now);
        core_mark_dirty(c, now, CFB_FEEDBACK);
        out_mark(c);
        break;
    }
    case CMD_RUMBLEPATH:
        s->rumble_v2 = cmd->u.flag;
        core_mark_dirty(c, now, CFB_FEEDBACK);
        out_mark(c);
        dsxp_log("rumble path -> %s", s->rumble_v2 ? "v2" : "classic");
        break;
    case CMD_MOTION:
        apply_motion(c, &cmd->u.motion, now);
        break;
    case CMD_CALIBRATE:
        calibrate_cmd(c, cmd->key, now);
        break;
    case CMD_MOTION_RECENTER:            /* not a setting: nothing is saved */
        if (core_calibrating(c)) {       /* the routine measures from its own level */
            dsxp_log("motion: not recentered - the calibration is running");
            break;
        }
        core_motion_recenter(c, now);
        break;
    default:
        break;
    }
}

/* Rolling 10-second report window. A window is logged when anything happened
 * or the link/gate state changed, otherwise once an hour - not every 10 s
 * forever with nothing connected. */
static void reports_tick(core_t *c, double now) {
    if (c->window_start == 0) { c->window_start = now; return; }
    double elapsed = now - c->window_start;
    if (elapsed < c->bridge.report_window_s) return;

    long n = c->reports_window;
    c->reports_window = 0;
    c->reports_total += n;
    c->report_hz = n / elapsed;
    c->window_start = now;

    char sig[64];
    snprintf(sig, sizeof sig, "%s bt=%d usb=%d xp=%d %s", core_input_live(c, now) ? "up" : "down",
             c->bt_present, c->usb_present, c->xp_running, c->engaged ? "engaged" : "idle");
    if (n > 0 || strcmp(sig, c->last_log_sig) != 0 || now - c->last_report_log >= c->bridge.idle_report_s) {
        char line[256];
        snprintf(line, sizeof line, "reports=%-5ld rate=%6.1f Hz  total=%-8ld link=%s",
                 n, c->report_hz, c->reports_total, sig);
        log_report_line(line);
        snprintf(c->last_log_sig, sizeof c->last_log_sig, "%s", sig);
        c->last_report_log = now;
    }
}

/*
 * Buttons bound to DSXP's own commands (DSXP_CMD_*). They have their own previous-button
 * state, separate from xp_push's, so they also work while DSXP is standing by; xp_push
 * never sends them to X-Plane. Recenter and toggle act on the press; hold acts for as
 * long as any button bound to it is down.
 */
static void dsxp_buttons(core_t *c, double now) {
    /* Stale input (the same rule as xp_push, a Bluetooth gap): every button keeps its
     * last known state and nothing acts, so a press held through the gap is not taken for
     * a new one (a toggle would flip twice) and a held clutch is not let go. Only once the
     * input is lost (core_input_live) is a held clutch let go, without recentering (the
     * attitude is unknown then), so motion is not frozen for good. */
    if (!xp_input_fresh(c, now)) {
        if (c->motion_hold && !core_input_live(c, now)) {
            c->motion_hold = false;
            blend_owned(c, now);                        /* fade from the frozen values   */
        }
        return;
    }
    bool hold = false;                                  /* any hold button down now      */
    for (int i = 0; i < B_COUNT; i++) {
        bool down = c->state.btn[i];
        bool was = c->dsxp_prev_btn[i];
        c->dsxp_prev_btn[i] = down;                     /* remember for the next tick    */
        const char *cmd = c->s.bind[i].cmd;
        if (!cmd_is_dsxp(cmd)) continue;                /* an X-Plane command: not ours  */
        if (!strcmp(cmd, DSXP_CMD_HOLD)) { hold = hold || down; continue; }
        if (!down || was) continue;                     /* only the press itself acts    */
        if (core_calibrating(c)) continue;              /* the calibration owns the motion */
        if (!strcmp(cmd, DSXP_CMD_RECENTER)) {
            core_motion_recenter(c, now);
        } else if (!strcmp(cmd, DSXP_CMD_TOGGLE)) {
            if (!c->s.motion.calibrated) { dsxp_log("motion: not switched on - calibrate first"); continue; }
            c->s.motion.enabled = !c->s.motion.enabled; /* motion_edges() fades it in/out */
            core_mark_dirty(c, now, CFB_CONTROLS);      /* saved like a change in the mapper */
            dsxp_log("motion: %s (button)", c->s.motion.enabled ? "on" : "off");
        }
    }
    /* The clutch: while held, the controls stay where they were; on release the
     * attitude held then becomes level, faded in from the frozen values. */
    if (core_calibrating(c)) hold = false;              /* no clutch during the calibration */
    if (hold && !c->motion_hold) {
        c->motion_hold = true;
        memcpy(c->motion_frozen, c->motion.out, sizeof c->motion_frozen);
    } else if (!hold && c->motion_hold) {
        c->motion_hold = false;
        if (!core_motion_recenter(c, now))              /* refused: the old level stays, */
            blend_owned(c, now);                        /*   faded in from the frozen    */
    }
}

/* Three values equal (values, not bytes: -0.0 equals 0.0, which memcmp would miss). */
static bool same3(const double a[3], const double b[3]) {
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
}

/*
 * React to motion settings changing, however they changed (mapper, bound button, or an
 * edit of controls.conf, which replaces the settings wholesale - hence edges against
 * last tick's copy rather than hooks in each setter):
 *   the saved calibration changed (level, drift, or calibrated) -> applied to the fusion,
 *     faded in from what X-Plane was sent (a new level moves every angle at once)
 *   switched on   -> the calibrated level again (dropping any Recenter), lamps blink
 *   switched off  -> the lamps blink
 *   an axis changes hands (motion <-> sticks) -> 0.3 s crossfade from the value sent
 */
static void motion_edges(core_t *c, double now) {
    const motion_cfg_t *m = &c->s.motion;
    if (!core_calibrating(c) &&
        (m->calibrated != c->applied_calibrated || m->level_bank_deg != c->applied_level[0] ||
         m->level_pitch_deg != c->applied_level[1] || !same3(m->gyro_bias_dps, c->applied_bias))) {
        apply_saved_calibration(c);
        blend_owned(c, now);
    }
    if (m->enabled != c->motion_was_enabled) {
        if (m->enabled && m->calibrated && !core_calibrating(c))   /* the calibrated level; */
            /* during a calibration the routine's own level stays in use */
            motion_set_level(&c->motion, m->level_bank_deg, m->level_pitch_deg);
        lamp_blink(c, now);
        c->motion_was_enabled = m->enabled;
    }
    for (int i = 0; i < MOTION_AXES; i++) {
        bool own = motion_owns(m, i);
        if (own != c->motion_was_owned[i]) motion_blend_start(&c->mblend[i], c->sent[i], now);
        c->motion_was_owned[i] = own;
    }
}

void core_tick(core_t *c, double now) {
    reports_tick(c, now);
    telemetry_tick(c, now);
    dsxp_buttons(c, now);
    motion_edges(c, now);
    bool blink = now < c->lamp_blink_until;             /* the confirmation flash        */
    if (blink != c->lamp_blinking) { c->lamp_blinking = blink; out_mark(c); }

    /* manual mode: what goes out is exactly what the user set */
    trigger_fx_t want0 = trig_effect(&c->s.fx_manual[0]), want1 = trig_effect(&c->s.fx_manual[1]);
    if (!c->suspended && c->s.trigger_source == TRIGGER_SRC_MANUAL &&
        (!fx_equal(&c->fx_out[0], &want0) || !fx_equal(&c->fx_out[1], &want1))) {
        c->fx_out[0] = want0;
        c->fx_out[1] = want1;
        out_mark(c);
    }
    if (light_tick(c, now)) out_mark(c);

    /* input or telemetry gone quiet: re-run the gate now, not in 2 s */
    if (c->engaged && (!core_input_live(c, now) || !c->wx.live)) c->force_gate = true;

    xp_push(c, now);
    xp_release_tick(c, now);
    out_tick(c, now);

}

void core_snapshot(const core_t *c, double now, dsxp_snap_t *o) {
    memset(o, 0, sizeof *o);
    o->connected = c->have_report;
    o->reports = c->reports_window;
    o->total = c->reports_total + c->reports_window;
    o->report_hz = c->report_hz;
    o->tune = c->s.tune;
    o->light = c->s.light;
    memcpy(o->light_note, c->light_note, sizeof o->light_note);
    o->fx[0] = c->fx_out[0];
    o->fx[1] = c->fx_out[1];
    o->trigger_manual[0] = c->s.fx_manual[0];
    o->trigger_manual[1] = c->s.fx_manual[1];
    o->trigger_source = c->s.trigger_source;
    o->wx = c->wx;
    o->rumble_source = c->s.rumble_source;
    memcpy(o->rumble_note, c->rumble_note, sizeof o->rumble_note);
    o->rumble = c->rumble;
    o->rumble_v2 = c->s.rumble_v2;
    o->bt = c->bt_present;
    o->usb = c->usb_present;
    o->xp = c->xp_running;
    o->engaged = c->engaged;
    o->suspended = c->suspended;
    o->imported = c->imported;
    memcpy(o->axes, c->state.val, sizeof o->axes);
    memcpy(o->raw, c->state.raw01, sizeof o->raw);
    memcpy(o->btn, c->state.btn, sizeof o->btn);
    memcpy(o->bind, c->s.bind, sizeof o->bind);
    memcpy(o->axis, c->s.axis, sizeof o->axis);
    memcpy(o->touch, c->state.touch, sizeof o->touch);
    memcpy(o->xp_exe, c->xp_exe, sizeof o->xp_exe);
    o->motion = c->s.motion;
    o->motion_angle[MOTION_ROLL]  = c->motion.bank_deg;
    o->motion_angle[MOTION_PITCH] = c->motion.pitch_deg;
    o->motion_angle[MOTION_YAW]   = c->motion.yaw.deg;
    memcpy(o->motion_out, c->motion_hold ? c->motion_frozen : c->motion.out, sizeof o->motion_out);
    o->motion_cal_src = c->motion_cal_src;
    o->motion_seeded = c->motion.seeded && core_input_live(c, now);   /* data is flowing  */
    o->motion_out_of_range = c->motion.out_of_range;
    o->motion_bias_learned = c->motion.bias_learned;
    o->motion_hold = c->motion_hold;
    o->calib = c->calib;
    memcpy(o->sent, c->sent, sizeof o->sent);
    o->sent_valid = c->sent_valid;
}
