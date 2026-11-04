/* X-Plane side: UDP DREF/CMND packets, override handling, per-frame push. */
#include "dsxp.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Writable datarefs, all confirmed against
 * X-Plane 12/Resources/plugins/DataRefs.txt (writable column = "y"). */
#define DR_PITCH   "sim/joystick/yoke_pitch_ratio"
#define DR_ROLL    "sim/joystick/yoke_roll_ratio"
#define DR_HEADING "sim/joystick/yoke_heading_ratio"
#define DR_THROTL  "sim/cockpit2/engine/actuators/throttle_ratio_all"
#define DR_LBRAKE  "sim/cockpit2/controls/left_brake_ratio"
#define DR_RBRAKE  "sim/cockpit2/controls/right_brake_ratio"
#define OV_PITCH   "sim/operation/override/override_joystick_pitch"
#define OV_ROLL    "sim/operation/override/override_joystick_roll"
#define OV_HEADING "sim/operation/override/override_joystick_heading"
#define OV_BRAKES  "sim/operation/override/override_toe_brakes"

static int g_sock = -1;
static struct sockaddr_in g_xp;
static bool   g_prev_btn[B_COUNT];
static double g_last_cmd_time[B_COUNT];

void xp_udp_open(void) {
    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) { perror("socket"); exit(1); }

    /*
     * Deliberately NOT binding a fixed source port.
     *
     * An earlier version bound 49010 to give X-Plane a stable peer identity.
     * That port belongs to X-Plane's own RakNet stack: taking it made the sim
     * fail with "Failed to initialize UDP RakNet connectivity (port 49010).
     * Local networking will be disabled." - i.e. squatting anywhere in its range
     * breaks the very thing we are trying to talk to. An ephemeral port is
     * correct here; X-Plane replies to whatever source it sees.
     */

    /* a bigger receive buffer so a burst of RREF replies is not dropped */
    int rcv = 256 * 1024;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);

    memset(&g_xp, 0, sizeof g_xp);
    g_xp.sin_family = AF_INET;
    g_xp.sin_port   = htons(XP_PORT);
    inet_pton(AF_INET, XP_IP, &g_xp.sin_addr);
}

/* DREF packet: "DREF" + 0x00 + float32 value + 500-byte NUL-padded path = 509 */
void xp_send_dref(const char *path, float value) {
    unsigned char pkt[509];
    memset(pkt, 0, sizeof pkt);
    memcpy(pkt, "DREF", 4);
    memcpy(pkt + 5, &value, 4);
    strncpy((char *)(pkt + 9), path, 499);
    sendto(g_sock, pkt, sizeof pkt, 0, (struct sockaddr *)&g_xp, sizeof g_xp);
}

/* CMND packet: "CMND" + 0x00 + command path */
void xp_send_cmnd(const char *cmd) {
    unsigned char pkt[512];
    size_t n = strlen(cmd);
    if (n > 500) n = 500;
    memset(pkt, 0, sizeof pkt);
    memcpy(pkt, "CMND", 4);
    memcpy(pkt + 5, cmd, n);
    sendto(g_sock, pkt, 5 + n, 0, (struct sockaddr *)&g_xp, sizeof g_xp);
}

int xp_socket(void) { return g_sock; }
const void *xp_addr(void) { return &g_xp; }

bool xp_axis_in_use(int target) {
    for (int i = 0; i < S_COUNT; i++) if (g_axis[i].target == target) return true;
    return false;
}

/* Only assert the overrides an axis actually needs, so anything unmapped stays
 * under X-Plane's own control instead of being frozen at zero. */
void xp_engage(void) {
    if (g_engaged) return;
    if (xp_axis_in_use(AX_PITCH)) xp_send_dref(OV_PITCH, 1.0f);
    if (xp_axis_in_use(AX_ROLL))  xp_send_dref(OV_ROLL,  1.0f);
    if (xp_axis_in_use(AX_YAW) || xp_axis_in_use(AX_RUDD_L) || xp_axis_in_use(AX_RUDD_R))
        xp_send_dref(OV_HEADING, 1.0f);
    if (xp_axis_in_use(AX_BRAKE_L) || xp_axis_in_use(AX_BRAKE_R))
        xp_send_dref(OV_BRAKES, 1.0f);
    g_engaged = true;
    fprintf(stderr, "[dsxp] ENGAGED - streaming to %s:%d\n", XP_IP, XP_PORT);
}

/* Centre the controls before releasing, so nothing is left deflected. */
void xp_disengage(void) {
    if (!g_engaged) return;
    xp_send_dref(DR_PITCH, 0.0f); xp_send_dref(DR_ROLL, 0.0f); xp_send_dref(DR_HEADING, 0.0f);
    xp_send_dref(OV_PITCH, 0.0f); xp_send_dref(OV_ROLL, 0.0f); xp_send_dref(OV_HEADING, 0.0f);
    xp_send_dref(OV_BRAKES, 0.0f);
    g_engaged = false;
    memset(g_prev_btn, 0, sizeof g_prev_btn);
    fprintf(stderr, "[dsxp] DISENGAGED - X-Plane has controls back\n");
}

void xp_push(void) {
    if (!g_have_report) return;
    pthread_mutex_lock(&g_lock);

    double roll = 0, pitch = 0, yaw = 0, thr = -1, lb = -1, rb = -1;
    for (int i = 0; i < S_COUNT; i++) {
        double v = g_state.val[i];       /* -1..1 for sticks, 0..1 for triggers */
        double u = g_state.raw01[i];     /* always 0..1                          */
        if (g_axis[i].invert) { v = -v; u = 1.0 - u; }
        /* a stick used as a unipolar control needs its centred value remapped */
        double uni = (i >= S_L2) ? u : (v + 1.0) / 2.0;
        switch (g_axis[i].target) {
        case AX_ROLL:     roll  += v;   break;
        case AX_PITCH:    pitch += v;   break;
        case AX_YAW:      yaw   += v;   break;
        case AX_THROTTLE: thr = (i >= S_L2) ? u : (1.0 - u); break;
        case AX_RUDD_L:   yaw -= uni;   break;
        case AX_RUDD_R:   yaw += uni;   break;
        case AX_BRAKE_L:  lb   = uni;   break;
        case AX_BRAKE_R:  rb   = uni;   break;
        default: break;
        }
    }
    if (roll  >  1) roll  =  1; if (roll  < -1) roll  = -1;
    if (pitch >  1) pitch =  1; if (pitch < -1) pitch = -1;
    if (yaw   >  1) yaw   =  1; if (yaw   < -1) yaw   = -1;

    if (xp_axis_in_use(AX_ROLL))  xp_send_dref(DR_ROLL,  (float)roll);
    if (xp_axis_in_use(AX_PITCH)) xp_send_dref(DR_PITCH, (float)pitch);
    if (xp_axis_in_use(AX_YAW) || xp_axis_in_use(AX_RUDD_L) || xp_axis_in_use(AX_RUDD_R))
        xp_send_dref(DR_HEADING, (float)yaw);
    if (thr >= 0) xp_send_dref(DR_THROTL, (float)thr);
    if (lb  >= 0) xp_send_dref(DR_LBRAKE, (float)lb);
    if (rb  >= 0) xp_send_dref(DR_RBRAKE, (float)rb);

    double t = now_sec();
    for (int i = 0; i < B_COUNT; i++) {
        if (!g_bind[i].cmd[0]) { g_prev_btn[i] = g_state.btn[i]; continue; }
        bool down = g_state.btn[i], was = g_prev_btn[i];
        if (down && !was) {
            xp_send_cmnd(g_bind[i].cmd);
            g_last_cmd_time[i] = t;
            if (g_verbose) fprintf(stderr, "[dsxp] %s -> %s\n", BTN_NAME[i], g_bind[i].cmd);
        } else if (down && was && g_bind[i].repeat &&
                   t - g_last_cmd_time[i] >= 1.0 / CMD_REPEAT_HZ) {
            xp_send_cmnd(g_bind[i].cmd);
            g_last_cmd_time[i] = t;
        }
        g_prev_btn[i] = down;
    }
    pthread_mutex_unlock(&g_lock);
}
