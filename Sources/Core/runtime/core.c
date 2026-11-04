/*
 * The core state machine. Pure: it never makes a system call itself. The I/O
 * layer (core_thread.c) feeds it input reports, device events, UDP packets, the
 * gate and UI commands, and calls core_tick() at 60 Hz; everything going out
 * leaves through c->sinks. That is what lets the tests drive it with a fake
 * clock and capture every packet.
 */
#include "core_internal.h"
#include <stdio.h>
#include <string.h>

void core_init(core_t *c, const settings_t *s, const core_sinks_t *sinks) {
    memset(c, 0, sizeof *c);
    c->s = *s;
    if (sinks) c->sinks = *sinks;
    snprintf(c->rnote, sizeof c->rnote, "%s", c->s.rmode == RM_SIM ? "waiting for X-Plane" : "manual");
    snprintf(c->lnote, sizeof c->lnote, "manual");
    snprintf(c->wx.note, sizeof c->wx.note, "%s",
             c->s.fxmode == FXM_WIND ? "waiting for X-Plane" : "manual");
    if (c->s.fxmode == FXM_MANUAL) {
        c->fx_out[0] = c->s.fx_manual[0];
        c->fx_out[1] = c->s.fx_manual[1];
    }
    c->out_dirty = true;
    c->force_gate = true;
}

void out_mark(core_t *c) { c->out_dirty = true; }

static void out_send(core_t *c) {
    if (!c->sinks.out_report) return;
    out_frame_t f;
    memset(&f, 0, sizeof f);
    f.rumble_l = c->rumble.left;
    f.rumble_r = c->rumble.right;
    f.rumble_v2 = c->s.rumble_v2;
    f.light_on = c->s.light.on;
    f.player = c->s.light.player;
    f.r = c->eff_r; f.g = c->eff_g; f.b = c->eff_b;
    f.fx[0] = c->fx_out[0];
    f.fx[1] = c->fx_out[1];
    unsigned char rep[OUT_REPORT_LEN];
    out_build_report(&f, c->out_seq++, rep);
    c->sinks.out_report(c->sinks.ctx, rep);
}

/*
 * Writes are coalesced to at most one per frame. macOS re-asserts its own light
 * bar colour, so ours is held with a slow keepalive even when nothing changed.
 */
void out_tick(core_t *c, double now) {
    if (!c->have_output_device) return;
    if (c->out_dirty && now - c->last_send >= 1.0 / TICK_HZ) {
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

void core_on_report(core_t *c, const unsigned char *buf, long len, double now) {
    if (ds_decode_report(&c->state, &c->gest, &c->s.tune, buf, len, now)) {
        c->have_report = true;
        c->last_report_at = now;
        c->reports_window++;
    }
}

void core_on_device(core_t *c, bool bluetooth, bool added) {
    c->force_gate = true;
    if (!bluetooth) return;
    c->have_output_device = added;
    if (added) c->out_dirty = true;          /* push our light state straight away */
    else c->have_report = false;
}

bool core_input_live(const core_t *c, double now) {
    return c->have_report && now - c->last_report_at <= INPUT_LOST_SEC;
}

bool core_gate_due(const core_t *c, double now, double last_gate) {
    return c->force_gate || now - last_gate >= GATE_POLL_SEC;
}

/*
 * The activation gate. All must hold before anything drives X-Plane:
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

void core_mark_dirty(core_t *c, double now) {
    c->cfg_dirty = true;
    c->cfg_dirty_at = now;
}

void core_flush_save(core_t *c) {
    if (!c->cfg_dirty) return;
    c->cfg_dirty = false;
    if (!c->sinks.save) return;
    char buf[16384];
    size_t n = bindings_format(&c->s, buf, sizeof buf);
    c->sinks.save(c->sinks.ctx, buf, n);
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
    if (m & LF_MODE) L->mode = v->mode == LM_SIM ? LM_SIM : LM_MANUAL;
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
        if (c->s.fxmode == FXM_MANUAL) { c->fx_out[0] = c->s.fx_manual[0]; c->fx_out[1] = c->s.fx_manual[1]; }
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
        core_mark_dirty(c, now);
        dsxp_log("bind %s -> %s%s", BTN_NAME[i], s->bind[i].cmd[0] ? s->bind[i].cmd : "(cleared)",
                 s->bind[i].repeat ? " [hold]" : "");
        break;
    }
    case CMD_AXIS: {
        int i = cmd->key, t = cmd->u.axis.target;
        if (i < 0 || i >= S_COUNT || t < 0 || t >= AX_TARGET_COUNT) return;
        s->axis[i] = cmd->u.axis;
        core_mark_dirty(c, now);
        /* the override planner in xp_push picks the change up on the next tick */
        dsxp_log("axis %s -> %s%s", SRC_LABEL[i], AX_LABEL[t], s->axis[i].invert ? " (inverted)" : "");
        break;
    }
    case CMD_TUNE:
        if (cmd->u.tune.has_dz) s->tune.deadzone = clampd(cmd->u.tune.dz, 0, 0.45, s->tune.deadzone);
        if (cmd->u.tune.has_cv) s->tune.curve    = clampd(cmd->u.tune.cv, 0.4, 3.0, s->tune.curve);
        if (cmd->u.tune.has_gs) s->tune.gesture  = clampd(cmd->u.tune.gs, 0, 1, s->tune.gesture);
        core_mark_dirty(c, now);
        break;
    case CMD_LIGHT:
        apply_light(&s->light, &cmd->u.light.v, cmd->u.light.mask);
        core_mark_dirty(c, now);
        out_mark(c);
        break;
    case CMD_TRIGGER: {
        int i = cmd->key;
        if (i < 0 || i > 1 || s->fxmode == FXM_WIND) return;   /* wind mode owns them */
        trigger_fx_t *f = &s->fx_manual[i];
        unsigned m = cmd->u.trig.mask;
        if (m & TF_MODE) {
            int md = cmd->u.trig.v.mode;
            f->mode = (md < TFX_OFF || md > TFX_VIBRATE) ? TFX_OFF : md;
        }
        if (m & TF_START)    f->start    = cmd->u.trig.v.start;
        if (m & TF_END)      f->end      = cmd->u.trig.v.end;
        if (m & TF_STRENGTH) f->strength = cmd->u.trig.v.strength;
        if (!c->suspended) c->fx_out[i] = *f;
        core_mark_dirty(c, now);
        out_mark(c);
        break;
    }
    case CMD_FXMODE: {
        int want = cmd->u.mode == FXM_WIND ? FXM_WIND : FXM_MANUAL;
        if (want != s->fxmode) {
            s->fxmode = want;
            if (want == FXM_MANUAL) {
                /* hand-set effects come straight back: wind never touched them */
                if (!c->suspended) { c->fx_out[0] = s->fx_manual[0]; c->fx_out[1] = s->fx_manual[1]; }
                snprintf(c->wx.note, sizeof c->wx.note, "manual");
            } else {
                c->fx_out[0] = c->fx_out[1] = (trigger_fx_t){ TFX_OFF, 0, 0, 0 };
                snprintf(c->wx.note, sizeof c->wx.note, "waiting for X-Plane");
            }
            dsxp_log("trigger source -> %s", want == FXM_WIND ? "wind" : "manual");
        }
        core_mark_dirty(c, now);
        out_mark(c);
        break;
    }
    case CMD_RUMBLE:
        if (s->rmode == RM_SIM || c->suspended) return;        /* sim mode owns the motors */
        if (cmd->u.rumble.has_l) c->rumble.left  = cmd->u.rumble.l;
        if (cmd->u.rumble.has_r) c->rumble.right = cmd->u.rumble.r;
        snprintf(c->rnote, sizeof c->rnote, "manual %d / %d", c->rumble.left, c->rumble.right);
        out_mark(c);
        break;
    case CMD_RUMBLEMODE: {
        int want = cmd->u.mode == RM_SIM ? RM_SIM : RM_MANUAL;
        if (want != s->rmode) {
            s->rmode = want;
            /* never leave the motors spinning when handing control over */
            c->rumble.left = c->rumble.right = 0;
            snprintf(c->rnote, sizeof c->rnote, "%s", want == RM_SIM ? "waiting for X-Plane" : "manual");
            dsxp_log("rumble source -> %s", want == RM_SIM ? "sim" : "manual");
        }
        core_mark_dirty(c, now);
        out_mark(c);
        break;
    }
    case CMD_RUMBLEPATH:
        s->rumble_v2 = cmd->u.flag;
        core_mark_dirty(c, now);
        out_mark(c);
        dsxp_log("rumble path -> %s", s->rumble_v2 ? "v2" : "classic");
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
    if (elapsed < REPORT_WINDOW_SEC) return;

    long n = c->reports_window;
    c->reports_window = 0;
    c->reports_total += n;
    c->report_hz = n / elapsed;
    c->window_start = now;

    char sig[64];
    snprintf(sig, sizeof sig, "%s bt=%d usb=%d xp=%d %s", core_input_live(c, now) ? "up" : "down",
             c->bt_present, c->usb_present, c->xp_running, c->engaged ? "engaged" : "idle");
    if (n > 0 || strcmp(sig, c->last_log_sig) != 0 || now - c->last_report_log >= REPORT_IDLE_LOG_SEC) {
        char line[256];
        snprintf(line, sizeof line, "reports=%-5ld rate=%6.1f Hz  total=%-8ld link=%s",
                 n, c->report_hz, c->reports_total, sig);
        log_report_line(line);
        snprintf(c->last_log_sig, sizeof c->last_log_sig, "%s", sig);
        c->last_report_log = now;
    }
}

void core_tick(core_t *c, double now) {
    reports_tick(c, now);
    telemetry_tick(c, now);

    /* manual mode: what goes out is exactly what the user set */
    if (!c->suspended && c->s.fxmode == FXM_MANUAL &&
        (!fx_equal(&c->fx_out[0], &c->s.fx_manual[0]) || !fx_equal(&c->fx_out[1], &c->s.fx_manual[1]))) {
        c->fx_out[0] = c->s.fx_manual[0];
        c->fx_out[1] = c->s.fx_manual[1];
        out_mark(c);
    }
    if (light_tick(c, now)) out_mark(c);

    /* input or telemetry gone quiet: re-run the gate now, not in 2 s */
    if (c->engaged && (!core_input_live(c, now) || !c->wx.live)) c->force_gate = true;

    xp_push(c, now);
    xp_release_tick(c, now);
    out_tick(c, now);

    if (c->cfg_dirty && now - c->cfg_dirty_at >= SAVE_DEBOUNCE_SEC) core_flush_save(c);
}

void core_snapshot(const core_t *c, dsxp_snap_t *o) {
    memset(o, 0, sizeof *o);
    o->connected = c->have_report;
    o->reports = c->reports_window;
    o->total = c->reports_total + c->reports_window;
    o->report_hz = c->report_hz;
    o->tune = c->s.tune;
    o->light = c->s.light;
    memcpy(o->lnote, c->lnote, sizeof o->lnote);
    o->fx[0] = c->fx_out[0];
    o->fx[1] = c->fx_out[1];
    o->fxmode = c->s.fxmode;
    o->wx = c->wx;
    o->rmode = c->s.rmode;
    memcpy(o->rnote, c->rnote, sizeof o->rnote);
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
}
