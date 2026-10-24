/*
 * ui_core.c - screens, menus, editors and status bar of the front panel (pure C11).
 *
 * Structure:
 *   fields     every editable value has a field id: descriptor (type/range/names), get, format, apply, change mask
 *   lists      every menu page is a builder that emits items (sub-menu, field, action, info, device) each frame;
 *              navigation, inline editing (enum/number) and drawing are generic
 *   editors    Port-Address, IPv4, text (character ring), confirmation dialog - full screens on the stack
 *   overlays   status bar (always), toasts, "changed by Pixelmapper" hint, lock overlay, firmware update
 * Everything is redrawn into the display list each tick; ui_gfx only sends tiles that changed.
 */
#include "ui_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pm_pids.h"
#include "pm_rdm.h"
#include "ui_font.h"
#include "ui_gfx.h"
#include "ui_rdm.h"

/* ------------------------------------------------------------------------------------------------ layout/timing */

#define Y_HDR 16
#define Y_CONTENT 32
#define ROW_H 16
#define ROWS 6
#define X_VAL_END 156
#define Y_HINT 112

#define TOAST_MS 3000u
#define REMOTE_HL_MS 3000u
#define CONFLICT_MS 4000u
#define MENU_TIMEOUT_MS 60000u
#define EDIT_TIMEOUT_MS 120000u
#define HOME_CURSOR_MS 10000u
#define LOCK_OVERLAY_MS 3000u
#define WAKE_MS 30000u
#define ERR_SHOW_MS 10000u
#define RDM_PULSE_MS 150u
#define FLASH_MS 120u
#define TEXT_CLEAR_MS 1000u
#define RESET_HOLD_MS 3000u

#define DIM_LEVEL 13      /* ~5 % */
#define MIN_LEVEL 13

#define STACK_MAX 8
#define LOG_N 16
#define LOG_LEN 21

/* pm_net_addr_mode_t values (pm_net.h; pm_ui does not depend on pm_net) */
#define PM_NET_ADDR_DHCP_UI 1
#define PM_NET_ADDR_FALLBACK_UI 2
#define PM_NET_ADDR_STATIC_UI 3

/* ------------------------------------------------------------------------------------------------ types */

enum {
    F_NONE = 0,
    F_P_ADDR, F_P_MODE, F_P_MERGE, F_P_RDM, F_P_FAILSAFE, F_P_STYLE, F_P_RATE, F_P_BREAK, F_P_MAB, F_P_BGDISC,
    F_RDM_INT, F_DISP_LEVEL, F_DISP_DIM, F_ADDR_FMT,
    F_NET_MODE, F_NET_IP, F_NET_MASK, F_NET_GW,
    F_IDENTIFY, F_SHORT, F_LONG,
    F_D_IDENT, F_D_ADDR, F_D_PERS, F_D_LABEL,
};

enum { FT_ENUM, FT_NUM, FT_LIST, FT_EDITOR };

typedef struct {
    uint8_t type;
    int32_t min, max, step;
    const char *const *names;
    const char *unit;
} fdesc_t;

enum { S_HOME, S_LIST, S_ADDR, S_IP, S_TEXT, S_CONFIRM };

enum {
    L_MAIN, L_PORT, L_PORT_ADV, L_NET, L_NET_INFO, L_DEVS, L_DEV, L_DEV_INFO, L_NODE, L_DISPLAY, L_SYSTEM,
    L_SYS_INFO, L_EVENTS, L_RESET_CHOICE,
};

enum { K_SUB, K_FIELD, K_ACTION, K_INFO, K_DEV };

enum {
    A_NONE, A_NET_APPLY, A_NET_LEAVE, A_RECORD_SCENE, A_RESET_PORT, A_RESTART, A_FACTORY_KEEP, A_FACTORY_ALL,
    A_FACTORY_DO, A_LOCK, A_RESCAN, A_APPLY_FIELD, A_TEXT_DISCARD,
};

typedef struct {
    uint8_t scr, list, port;
    uint16_t cur, top;
    pm_uid_t uid; /* L_DEV / L_DEV_INFO */
} frame_t;

typedef struct {
    uint8_t kind, arg, port;
    uint16_t idx;
    pm_uid_t uid;
    char label[32];
    char value[24];
    uint16_t lcolor, vcolor;
    bool wide; /* label uses the full row */
} item_t;

#define SEL_MAX 2112

static struct {
    int n, start, probe_idx;
    item_t win[ROWS];
    item_t probe, scratch;
    uint8_t sel[SEL_MAX / 8];
    bool edit_found;
} B;

typedef struct {
    bool active;
    uint8_t field, port;
    int32_t value, orig;
} inline_edit_t;

/* Port-Address editor */
enum { AE_QUICK, AE_SELECT, AE_FIELD };
typedef struct {
    uint8_t port, mode, sel; /* sel: 0 Net, 1 Sub, 2 Uni, 3 OK (N:S:U) | 0 number, 1 OK (15-bit) */
    int32_t pa, orig;
} addr_edit_t;

typedef struct {
    uint8_t field, mode, sel; /* sel 0..3 octet, 4 OK */
    uint8_t oct[4];
    bool invalid;
} ip_edit_t;

typedef struct {
    uint8_t field, port; /* F_SHORT, F_LONG, F_D_LABEL */
    char text[64];
    char orig[64];
    uint8_t max, ring;
} text_edit_t;

typedef struct {
    uint8_t action, focus;
    bool danger, hold;
    char title[24];
    char line[4][24];
    uint16_t line_color[4];
    char btn[2][12];
    /* pending field apply */
    uint8_t field, port;
    int32_t value;
    uint8_t pop; /* frames to pop after the action */
} dialog_t;

typedef struct {
    char text[LOG_LEN];
} log_t;

/* ------------------------------------------------------------------------------------------------ state */

static struct {
    ui_platform_t plat;
    pm_ui_model_t model;
    ui_node_info_t info;
    ui_input_t in;
    pm_settings_t s;
    pm_settings_t tmp;
    uint32_t now;

    frame_t stack[STACK_MAX];
    int depth; /* stack[0] = home */

    inline_edit_t ed;
    addr_edit_t ae;
    ip_edit_t ie;
    text_edit_t te;
    dialog_t dlg;

    /* staged network */
    pm_ip_settings_t net;
    bool net_staged;

    /* home */
    int home_cur; /* -1 = hidden; 0 name, 1 ip line, 2..5 ports */
    int home_last;

    /* messages */
    char toast[32];
    uint16_t toast_color;
    uint32_t toast_t;
    char conflict[32];
    uint32_t conflict_t;
    pm_change_mask_t rmask;
    uint32_t rmask_t;
    uint32_t flash_t;
    uint32_t lock_overlay_t;

    /* display */
    uint32_t last_input, wake_t, edit_t;
    bool muted, locate, identify, lock_remote, woke_once;
    uint8_t backlight;
    bool panel_on;
    bool force;

    /* live */
    int fw_pct;
    int8_t tube_pct[PM_NUM_PORTS];
    uint32_t err_t[PM_NUM_PORTS];
    int32_t err_code[PM_NUM_PORTS];
    uint32_t rdm_cnt[PM_NUM_PORTS], rdm_t[PM_NUM_PORTS];
    bool loss_prev[PM_NUM_PORTS];
    bool link_prev;
    bool sys_error;

    log_t log[LOG_N];
    uint8_t log_n, log_head;

    pm_uid_t tod[PM_RDM_TOD_MAX];
} U;

static const char *const N_MODE[] = {"Off", "Output", "Input"};
static const char *const N_MERGE[] = {"HTP", "LTP"};
static const char *const N_ONOFF[] = {"Off", "On"};
static const char *const N_FAILSAFE[] = {"Hold", "Zero", "Full", "Scene"};
static const char *const N_STYLE[] = {"Always", "Delta"};
static const char *const N_FMT[] = {"N:S:U", "15-bit"};
static const char *const N_NETMODE[] = {"DHCP", "Static"};
static const uint16_t DIM_LIST[] = {0, 30, 60, 120, 300, 600, 1800, 3600};
#define DIM_LIST_N (sizeof(DIM_LIST) / sizeof(DIM_LIST[0]))

static const char RING[] = "\x85\x86\x87" "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_./#+():";
#define RING_N ((int)sizeof(RING) - 1)

/* ------------------------------------------------------------------------------------------------ helpers */

/** Bounded string copy (always terminated, silently truncated). */
static void scopy(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n) l = n - 1;
    memcpy(dst, src, l);
    dst[l] = '\0';
}

static bool since(uint32_t t, uint32_t ms) { return t != 0 && U.now - t < ms; }

static void toast(uint16_t color, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.toast, sizeof(U.toast), fmt, ap);
    va_end(ap);
    U.toast_color = color;
    U.toast_t = U.now ? U.now : 1;
}

static void log_add(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(U.log[U.log_head].text, LOG_LEN, fmt, ap);
    va_end(ap);
    U.log_head = (uint8_t)((U.log_head + 1) % LOG_N);
    if (U.log_n < LOG_N) U.log_n++;
}

static void fmt_ip(char *buf, size_t n, uint32_t ip)
{
    snprintf(buf, n, "%u.%u.%u.%u", (unsigned)(ip >> 24) & 0xFF, (unsigned)(ip >> 16) & 0xFF,
             (unsigned)(ip >> 8) & 0xFF, (unsigned)ip & 0xFF);
}

static uint32_t mask_of(int prefix) { return prefix <= 0 ? 0 : 0xFFFFFFFFu << (32 - prefix); }

static int prefix_of(uint32_t mask)
{
    int p = 0;
    while (p < 32 && (mask & (0x80000000u >> p))) p++;
    return p;
}

static void fmt_pa(char *buf, size_t n, uint16_t pa, uint8_t format)
{
    if (format == PM_ADDRESS_FORMAT_PORT_ADDRESS) {
        snprintf(buf, n, "%u", (unsigned)pa);
    } else {
        snprintf(buf, n, "%u:%u:%u", (unsigned)PM_PA_NET(pa), (unsigned)PM_PA_SUB(pa), (unsigned)PM_PA_UNI(pa));
    }
}

static void fmt_uid(char *buf, size_t n, const pm_uid_t *u)
{
    snprintf(buf, n, "%02X%02X:%02X%02X%02X%02X", u->b[0], u->b[1], u->b[2], u->b[3], u->b[4], u->b[5]);
}

static void fmt_secs(char *buf, size_t n, uint32_t s)
{
    if (s == 0) snprintf(buf, n, "Never");
    else if (s < 60 || s % 60) snprintf(buf, n, "%u s", (unsigned)s);
    else snprintf(buf, n, "%u min", (unsigned)(s / 60));
}

static void port_stats(uint8_t p, pm_port_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    if (U.model.port_stats) U.model.port_stats(U.model.ctx, p, st);
}

static void port_flags(uint8_t p, bool *loss, bool *merging, bool *synced)
{
    *loss = *merging = *synced = false;
    if (U.model.port_flags) U.model.port_flags(U.model.ctx, p, loss, merging, synced);
}

static void net_status(uint32_t *ip, uint32_t *mask, uint8_t *mode, bool *link)
{
    *ip = *mask = 0;
    *mode = 0;
    *link = false;
    if (U.model.net_status) U.model.net_status(U.model.ctx, ip, mask, mode, link);
}

static uint16_t tod_get(uint8_t p, pm_uid_t *out, uint16_t cap)
{
    if (!U.model.tod_get) return 0;
    pm_uid_t dummy;
    return U.model.tod_get(U.model.ctx, p, cap ? out : &dummy, cap);
}

static bool port_is_output(uint8_t p) { return U.s.port[p].enabled && U.s.port[p].direction == PM_PORT_DIR_OUTPUT; }
static bool port_is_input(uint8_t p) { return U.s.port[p].enabled && U.s.port[p].direction == PM_PORT_DIR_INPUT; }

/** The port carries data right now (confirmation needed for direction/disable/universe). */
static bool port_has_data(uint8_t p)
{
    if (!U.s.port[p].enabled) return false;
    pm_port_stats_t st;
    bool loss, merging, synced;
    port_stats(p, &st);
    port_flags(p, &loss, &merging, &synced);
    if (port_is_input(p)) return st.input_signal;
    return st.fps > 0 && !loss;
}

static void settings_fresh(pm_settings_t *out)
{
    if (U.plat.settings_get) U.plat.settings_get(U.plat.ctx, out);
    else *out = U.s;
}

static void settings_write(const pm_settings_t *s)
{
    U.s = *s;
    U.conflict_t = 0; /* the local write is the last one now */
    if (U.plat.settings_set) U.plat.settings_set(U.plat.ctx, s);
    U.flash_t = U.now ? U.now : 1;
}

static frame_t *top(void) { return &U.stack[U.depth]; }

static void push_frame(uint8_t scr, uint8_t list, uint8_t port)
{
    if (U.depth + 1 >= STACK_MAX) return;
    U.depth++;
    frame_t *f = top();
    memset(f, 0, sizeof(*f));
    f->scr = scr;
    f->list = list;
    f->port = port;
}

static void pop_frame(void)
{
    if (U.depth > 0) U.depth--;
}

static ui_rdm_dev_t *frame_dev(const frame_t *f) { return ui_rdm_dev(f->port, &f->uid); }

static bool locked(void) { return U.s.panel_lock; }

static void show_lock(void) { U.lock_overlay_t = U.now ? U.now : 1; }

/* ------------------------------------------------------------------------------------------------ fields */

static fdesc_t fdesc(uint8_t f, uint8_t p)
{
    fdesc_t d = {.type = FT_ENUM, .min = 0, .max = 1, .step = 1, .names = N_ONOFF, .unit = ""};
    switch (f) {
    case F_P_MODE: d.max = 2; d.names = N_MODE; break;
    case F_P_MERGE: d.names = N_MERGE; break;
    case F_P_RDM: case F_P_BGDISC: case F_IDENTIFY: case F_D_IDENT: break;
    case F_P_FAILSAFE: d.max = U.s.port[p].scene_len ? 3 : 2; d.names = N_FAILSAFE; break;
    case F_P_STYLE: d.names = N_STYLE; break;
    case F_ADDR_FMT: d.names = N_FMT; break;
    case F_NET_MODE: d.names = N_NETMODE; break;
    case F_P_RATE: d = (fdesc_t){FT_NUM, 1, 44, 1, NULL, " Hz"}; break;
    case F_P_BREAK: d = (fdesc_t){FT_NUM, 176, 352, 4, NULL, UI_G_MICRO "s"}; break;
    case F_P_MAB: d = (fdesc_t){FT_NUM, 12, 88, 4, NULL, UI_G_MICRO "s"}; break;
    case F_RDM_INT: d = (fdesc_t){FT_NUM, 1, 600, 1, NULL, " s"}; break;
    case F_DISP_LEVEL: d = (fdesc_t){FT_NUM, 5, 100, 5, NULL, " %"}; break;
    case F_DISP_DIM: d = (fdesc_t){FT_LIST, 0, DIM_LIST_N - 1, 1, NULL, ""}; break;
    case F_NET_MASK: d = (fdesc_t){FT_NUM, 8, 30, 1, NULL, ""}; break;
    case F_D_ADDR: d = (fdesc_t){FT_NUM, 1, 512, 1, NULL, ""}; break;
    case F_D_PERS: {
        const frame_t *fr = top();
        ui_rdm_dev_t *dev = ui_rdm_find(fr->port, &fr->uid);
        d = (fdesc_t){FT_NUM, 1, dev && dev->pers_count ? dev->pers_count : 1, 1, NULL, ""};
        break;
    }
    default: d.type = FT_EDITOR; break;
    }
    return d;
}

static int32_t fget(uint8_t f, uint8_t p)
{
    const pm_port_settings_t *ps = &U.s.port[p < PM_NUM_PORTS ? p : 0];
    const frame_t *fr = top();
    ui_rdm_dev_t *dev = (f >= F_D_IDENT) ? ui_rdm_find(fr->port, &fr->uid) : NULL;
    switch (f) {
    case F_P_ADDR: return ps->address;
    case F_P_MODE: return !ps->enabled ? 0 : (ps->direction == PM_PORT_DIR_INPUT ? 2 : 1);
    case F_P_MERGE: return ps->merge;
    case F_P_RDM: return ps->rdm_enabled;
    case F_P_FAILSAFE: return ps->failsafe;
    case F_P_STYLE: return ps->output_style;
    case F_P_RATE: return ps->dmx_rate_hz;
    case F_P_BREAK: return ps->dmx_break_us;
    case F_P_MAB: return ps->dmx_mab_us;
    case F_P_BGDISC: return ps->rdm_bg_discovery;
    case F_RDM_INT: return U.s.rdm_bg_interval_s;
    case F_DISP_LEVEL: return ((U.s.display_level * 100 + 127) / 255 + 2) / 5 * 5;
    case F_DISP_DIM: return U.s.display_dim_timeout_s;
    case F_ADDR_FMT: return U.s.address_format;
    case F_NET_MODE: return U.net.dhcp ? 0 : 1;
    case F_NET_IP: return (int32_t)U.net.static_ip;
    case F_NET_MASK: return prefix_of(U.net.static_mask);
    case F_NET_GW: return (int32_t)U.net.gateway;
    case F_IDENTIFY: return U.identify;
    case F_D_IDENT: return dev && (dev->flags & UI_RDM_IDENTIFY);
    case F_D_ADDR: return dev ? dev->start : 0;
    case F_D_PERS: return dev ? dev->pers : 0;
    default: return 0;
    }
}

static pm_change_mask_t fmask(uint8_t f, uint8_t p)
{
    switch (f) {
    case F_P_ADDR: return PM_CHG_PORT(PM_CHG_P_ADDRESS, p);
    case F_P_MODE: return PM_CHG_PORT(PM_CHG_P_ENABLED | PM_CHG_P_DIRECTION, p);
    case F_P_MERGE: return PM_CHG_PORT(PM_CHG_P_MERGE, p);
    case F_P_RDM: case F_P_BGDISC: return PM_CHG_PORT(PM_CHG_P_RDM, p);
    case F_P_FAILSAFE: case F_P_STYLE: return PM_CHG_PORT(PM_CHG_P_OUTPUT, p);
    case F_P_RATE: case F_P_BREAK: case F_P_MAB: return PM_CHG_PORT(PM_CHG_P_TIMING, p);
    case F_RDM_INT: return PM_CHG_RDM_INTERVAL;
    case F_DISP_LEVEL: case F_DISP_DIM: case F_ADDR_FMT: return PM_CHG_DISPLAY;
    case F_NET_MODE: case F_NET_IP: case F_NET_MASK: case F_NET_GW: return PM_CHG_IP;
    case F_SHORT: case F_LONG: return PM_CHG_NAMES;
    default: return 0;
    }
}

static void fformat(uint8_t f, uint8_t p, int32_t v, char *buf, size_t n)
{
    fdesc_t d = fdesc(f, p);
    switch (f) {
    case F_P_ADDR: fmt_pa(buf, n, (uint16_t)v, U.s.address_format); return;
    case F_NET_IP: case F_NET_GW: fmt_ip(buf, n, (uint32_t)v); return;
    case F_NET_MASK: fmt_ip(buf, n, mask_of(v)); return;
    case F_DISP_DIM: fmt_secs(buf, n, (uint32_t)v); return;
    case F_D_ADDR:
        if (v < 1 || v > 512) snprintf(buf, n, "---");
        else snprintf(buf, n, "%03d", (int)v);
        return;
    case F_D_PERS: {
        const ui_rdm_pers_t *ps = ui_rdm_personality((uint8_t)v);
        if (ps && ps->name[0]) snprintf(buf, n, "%d %.6s", (int)v, ps->name);
        else snprintf(buf, n, "%d/%d", (int)v, (int)d.max);
        return;
    }
    case F_SHORT: snprintf(buf, n, "%.8s", U.s.short_name); return;
    case F_LONG: snprintf(buf, n, "%.8s", U.s.long_name); return;
    default: break;
    }
    if (d.type == FT_ENUM && d.names && v >= d.min && v <= d.max) snprintf(buf, n, "%s", d.names[v]);
    else snprintf(buf, n, "%d%s", (int)v, d.unit ? d.unit : "");
}

static void open_dialog(uint8_t action, const char *title, bool danger);

/** Write a field (fresh copy of the settings, only this field changes: last writer wins). */
static void fapply(uint8_t f, uint8_t p, int32_t v)
{
    if (f == F_NET_MODE) {
        U.net.dhcp = v == 0;
        U.net_staged = true;
        if (v && U.net.static_ip == 0) { /* switching to static: start from the address in use */
            uint32_t ip, mask;
            uint8_t mode;
            bool link;
            net_status(&ip, &mask, &mode, &link);
            if (ip) {
                U.net.static_ip = ip;
                U.net.static_mask = mask;
            }
        }
        return;
    }
    if (f == F_NET_IP) { U.net.static_ip = (uint32_t)v; U.net_staged = true; return; }
    if (f == F_NET_MASK) { U.net.static_mask = mask_of(v); U.net_staged = true; return; }
    if (f == F_NET_GW) { U.net.gateway = (uint32_t)v; U.net_staged = true; return; }
    if (f == F_IDENTIFY) {
        U.identify = v != 0;
        if (U.plat.identify) U.plat.identify(U.plat.ctx, U.identify);
        U.flash_t = U.now;
        return;
    }
    if (f >= F_D_IDENT) {
        ui_rdm_dev_t *dev = frame_dev(top());
        if (f == F_D_IDENT) ui_rdm_set_identify(dev, v != 0);
        if (f == F_D_ADDR) { ui_rdm_set_start(dev, (uint16_t)v); dev->start = (uint16_t)v; }
        if (f == F_D_PERS) { ui_rdm_set_personality(dev, (uint8_t)v); dev->pers = (uint8_t)v; }
        U.flash_t = U.now;
        return;
    }
    pm_settings_t *s = &U.tmp;
    settings_fresh(s);
    pm_port_settings_t *ps = &s->port[p < PM_NUM_PORTS ? p : 0];
    switch (f) {
    case F_P_ADDR: ps->address = (pm_port_address_t)v; break;
    case F_P_MODE:
        ps->enabled = v != 0;
        if (v) ps->direction = v == 2 ? PM_PORT_DIR_INPUT : PM_PORT_DIR_OUTPUT;
        break;
    case F_P_MERGE: ps->merge = (pm_merge_mode_t)v; break;
    case F_P_RDM: ps->rdm_enabled = v != 0; break;
    case F_P_FAILSAFE: ps->failsafe = (pm_failsafe_t)v; break;
    case F_P_STYLE: ps->output_style = (pm_output_style_t)v; break;
    case F_P_RATE: ps->dmx_rate_hz = (uint8_t)v; break;
    case F_P_BREAK: ps->dmx_break_us = (uint16_t)v; break;
    case F_P_MAB: ps->dmx_mab_us = (uint16_t)v; break;
    case F_P_BGDISC: ps->rdm_bg_discovery = v != 0; break;
    case F_RDM_INT: s->rdm_bg_interval_s = (uint16_t)v; break;
    case F_DISP_LEVEL: s->display_level = (uint8_t)((v * 255 + 50) / 100); break;
    case F_DISP_DIM: s->display_dim_timeout_s = (uint16_t)v; break;
    case F_ADDR_FMT: s->address_format = (uint8_t)v; break;
    default: return;
    }
    settings_write(s);
    if (f == F_P_MODE || (f == F_P_RDM && v == 0)) ui_rdm_forget(p);
}

/** Apply, asking first where the concept wants a confirmation. */
static void fapply_confirm(uint8_t f, uint8_t p, int32_t v, uint8_t pop)
{
    bool ask = false;
    if ((f == F_P_MODE || f == F_P_ADDR) && port_has_data(p) && v != fget(f, p)) ask = true;
    if (f == F_D_PERS && v != fget(f, p)) ask = true;
    if (!ask) {
        fapply(f, p, v);
        while (pop--) pop_frame();
        return;
    }
    dialog_t *d = &U.dlg;
    if (f == F_D_PERS) {
        const ui_rdm_dev_t *dev = ui_rdm_find(top()->port, &top()->uid);
        open_dialog(A_APPLY_FIELD, "Personality", false);
        const ui_rdm_pers_t *np = ui_rdm_personality((uint8_t)v);
        snprintf(d->line[0], sizeof(d->line[0]), "Footprint changes");
        if (dev && np) {
            snprintf(d->line[1], sizeof(d->line[1]), "%u " UI_G_RARROW " %u channels.", (unsigned)dev->footprint,
                     (unsigned)np->footprint);
        } else {
            snprintf(d->line[1], sizeof(d->line[1]), "with the mode.");
        }
        snprintf(d->line[2], sizeof(d->line[2]), "Check the patch.");
    } else {
        pm_port_stats_t st;
        port_stats(p, &st);
        open_dialog(A_APPLY_FIELD, f == F_P_MODE ? "Port mode" : "Port-Address", false);
        snprintf(d->line[0], sizeof(d->line[0]), "Port %u is %s", p + 1u, port_is_input(p) ? "receiving" : "sending");
        snprintf(d->line[1], sizeof(d->line[1]), "data (%u fps).", (unsigned)st.fps);
        if (f == F_P_MODE) {
            snprintf(d->line[2], sizeof(d->line[2]), "%s", port_is_input(p) ? "Input stops." : "Output stops.");
        } else {
            char a[12];
            fmt_pa(a, sizeof(a), (uint16_t)v, U.s.address_format);
            snprintf(d->line[2], sizeof(d->line[2]), "New address %s", a);
        }
    }
    snprintf(d->btn[1], sizeof(d->btn[1]), "Change");
    d->field = f;
    d->port = p;
    d->value = v;
    d->pop = pop;
}

/* ------------------------------------------------------------------------------------------------ dialog */

static void open_dialog(uint8_t action, const char *title, bool danger)
{
    dialog_t *d = &U.dlg;
    memset(d, 0, sizeof(*d));
    d->action = action;
    d->danger = danger;
    snprintf(d->title, sizeof(d->title), "%s", title);
    snprintf(d->btn[0], sizeof(d->btn[0]), "Cancel");
    snprintf(d->btn[1], sizeof(d->btn[1]), "OK");
    for (int i = 0; i < 4; i++) d->line_color[i] = UI_C_WHITE;
    push_frame(S_CONFIRM, 0, 0);
}

static void go_home(void)
{
    U.depth = 0;
    U.ed.active = false;
    U.home_cur = -1;
}

static void discard_staged(bool quiet)
{
    if (!U.net_staged) return;
    U.net = U.s.ip;
    U.net_staged = false;
    if (!quiet) {
        toast(UI_C_YELLOW, "Network changes discarded");
        log_add("Net changes discarded");
    }
}

static void net_apply(void)
{
    pm_settings_t *s = &U.tmp;
    settings_fresh(s);
    s->ip = U.net;
    settings_write(s);
    U.net_staged = false;
    toast(UI_C_WHITE, "Network applied");
    log_add("Network applied");
}

static void dialog_execute(void)
{
    dialog_t *d = &U.dlg;
    uint8_t action = d->action;
    pop_frame(); /* the dialog */
    switch (action) {
    case A_APPLY_FIELD:
        fapply(d->field, d->port, d->value);
        for (uint8_t i = 0; i < d->pop; i++) pop_frame();
        U.ed.active = false;
        break;
    case A_NET_APPLY: net_apply(); break;
    case A_NET_LEAVE:
        net_apply();
        pop_frame();
        break;
    case A_RECORD_SCENE:
        if (U.plat.record_scene) U.plat.record_scene(U.plat.ctx, d->port);
        toast(UI_C_WHITE, "P%u scene recorded", d->port + 1u);
        break;
    case A_RESET_PORT: {
        pm_settings_t *s = &U.tmp, def;
        settings_fresh(s);
        pm_settings_defaults(&def);
        s->port[d->port] = def.port[d->port];
        settings_write(s);
        ui_rdm_forget(d->port);
        toast(UI_C_WHITE, "Port %u reset", d->port + 1u);
        break;
    }
    case A_RESTART:
        if (U.plat.restart) U.plat.restart(U.plat.ctx);
        toast(UI_C_WHITE, "Restarting" UI_G_ELLIPSIS);
        break;
    case A_FACTORY_DO:
        if (U.plat.factory_reset) U.plat.factory_reset(U.plat.ctx, d->value != 0);
        log_add("Factory reset");
        go_home();
        toast(UI_C_WHITE, "Factory reset done");
        break;
    default: break;
    }
}

static void dialog_cancel(void)
{
    dialog_t *d = &U.dlg;
    uint8_t action = d->action;
    pop_frame();
    if (action == A_NET_LEAVE) {
        discard_staged(false);
        pop_frame();
    } else if (action == A_TEXT_DISCARD) {
        /* Cancel = keep editing: nothing */
    }
}

/* ------------------------------------------------------------------------------------------------ list builders */

static item_t *badd(uint8_t kind)
{
    int i = B.n++;
    if (i < SEL_MAX && kind != K_INFO) B.sel[i / 8] |= (uint8_t)(1u << (i % 8));
    item_t *it;
    if (i >= B.start && i < B.start + ROWS) it = &B.win[i - B.start];
    else if (i == B.probe_idx) it = &B.probe;
    else it = &B.scratch;
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->lcolor = UI_C_WHITE;
    it->vcolor = UI_C_WHITE;
    return it;
}

static void add_sub(const char *label, uint8_t list, uint8_t port)
{
    item_t *it = badd(K_SUB);
    snprintf(it->label, sizeof(it->label), "%s", label);
    snprintf(it->value, sizeof(it->value), UI_G_SUB);
    it->arg = list;
    it->port = port;
}

static void add_field(const char *label, uint8_t f, uint8_t p)
{
    item_t *it = badd(K_FIELD);
    snprintf(it->label, sizeof(it->label), "%s", label);
    it->arg = f;
    it->port = p;
    bool editing = U.ed.active && U.ed.field == f && U.ed.port == p;
    if (editing) B.edit_found = true;
    fdesc_t d = fdesc(f, p);
    if (d.type == FT_EDITOR && f != F_P_ADDR && f != F_NET_IP && f != F_NET_GW) {
        fformat(f, p, 0, it->value, sizeof(it->value));
        if (f == F_D_LABEL) {
            ui_rdm_dev_t *dev = ui_rdm_find(top()->port, &top()->uid);
            snprintf(it->value, sizeof(it->value), "%.8s", dev ? dev->label : "");
        }
        if (!it->value[0]) snprintf(it->value, sizeof(it->value), UI_G_SUB);
    } else {
        fformat(f, p, editing ? U.ed.value : fget(f, p), it->value, sizeof(it->value));
    }
    if (f >= F_D_IDENT) {
        ui_rdm_dev_t *dev = ui_rdm_find(top()->port, &top()->uid);
        static const uint16_t pids[] = {PM_RDM_PID_IDENTIFY_DEVICE, PM_RDM_PID_DMX_START_ADDRESS,
                                        PM_RDM_PID_DMX_PERSONALITY, PM_RDM_PID_DEVICE_LABEL};
        if (dev && !editing && ui_rdm_busy(dev, pids[f - F_D_IDENT])) snprintf(it->value, sizeof(it->value), UI_G_ELLIPSIS);
    }
    pm_change_mask_t m = fmask(f, p);
    if (m && (U.rmask & m) && since(U.rmask_t, REMOTE_HL_MS)) it->vcolor = UI_C_ORANGE;
    if ((f == F_NET_MODE || f == F_NET_IP || f == F_NET_MASK || f == F_NET_GW) && U.net_staged) {
        bool differs = (f == F_NET_MODE && U.net.dhcp != U.s.ip.dhcp) ||
                       (f == F_NET_IP && U.net.static_ip != U.s.ip.static_ip) ||
                       (f == F_NET_MASK && U.net.static_mask != U.s.ip.static_mask) ||
                       (f == F_NET_GW && U.net.gateway != U.s.ip.gateway);
        if (differs) it->vcolor = UI_C_CYAN;
    }
}

static void add_action(const char *label, uint8_t action, uint8_t port, uint16_t color)
{
    item_t *it = badd(K_ACTION);
    snprintf(it->label, sizeof(it->label), "%s", label);
    it->arg = action;
    it->port = port;
    it->lcolor = color;
    it->wide = true;
}

static void add_info(const char *label, const char *value, uint16_t color)
{
    item_t *it = badd(K_INFO);
    snprintf(it->label, sizeof(it->label), "%s", label);
    snprintf(it->value, sizeof(it->value), "%s", value ? value : "");
    it->lcolor = value ? UI_C_GRAY : color;
    it->vcolor = color;
    it->wide = value == NULL;
}

static void port_status_line(uint8_t p, char *buf, size_t n, uint16_t *color)
{
    pm_port_stats_t st;
    bool loss, merging, synced;
    port_stats(p, &st);
    port_flags(p, &loss, &merging, &synced);
    const pm_port_settings_t *ps = &U.s.port[p];
    *color = UI_C_GRAY;
    if (since(U.err_t[p], ERR_SHOW_MS)) {
        snprintf(buf, n, "Port error %ld", (long)U.err_code[p]);
        *color = UI_C_RED;
    } else if (!ps->enabled) {
        snprintf(buf, n, "Port disabled");
    } else if (U.tube_pct[p] >= 0) {
        snprintf(buf, n, UI_G_DOWN " Tube update %d%%", U.tube_pct[p]);
        *color = UI_C_CYAN;
    } else if (loss) {
        snprintf(buf, n, "No data " UI_G_MIDDOT " %s", N_FAILSAFE[ps->failsafe & 3]);
        *color = UI_C_YELLOW;
    } else if (port_is_input(p)) {
        if (st.input_signal) snprintf(buf, n, "%ufps input", (unsigned)st.fps);
        else snprintf(buf, n, "No DMX signal");
    } else {
        char rdm[12] = "";
        if (ps->rdm_enabled) snprintf(rdm, sizeof(rdm), " %uRDM", (unsigned)st.tod_count);
        snprintf(buf, n, "%ufps %s%s%s", (unsigned)st.fps, N_MERGE[ps->merge & 1], merging ? " 2src" : "", rdm);
    }
}

static uint16_t dev_total(uint8_t port)
{
    if (port != 0xFF) return port_is_output(port) && U.s.port[port].rdm_enabled ? tod_get(port, NULL, 0) : 0;
    uint16_t n = 0;
    for (uint8_t p = 0; p < PM_NUM_PORTS; p++) n = (uint16_t)(n + dev_total(p));
    return n;
}

static bool dev_overlaps(const ui_rdm_dev_t *d)
{
    if (!(d->flags & UI_RDM_HAVE_INFO) || d->footprint == 0 || d->start < 1 || d->start > 512) return false;
    /* compare with the other cached devices on the same port (the visible rows are always cached) */
    for (int i = 0; i < UI_RDM_CACHE; i++) {
        const ui_rdm_dev_t *o = ui_rdm_cache_at(i);
        if (!o || o == d || o->port != d->port || !(o->flags & UI_RDM_HAVE_INFO) || !o->footprint) continue;
        if (o->start < 1 || o->start > 512) continue;
        if (d->start < o->start + o->footprint && o->start < d->start + d->footprint) return true;
    }
    return false;
}

static void add_devices(uint8_t port)
{
    uint8_t p0 = port == 0xFF ? 0 : port, p1 = port == 0xFF ? PM_NUM_PORTS - 1 : port;
    for (uint8_t p = p0; p <= p1; p++) {
        uint16_t n = dev_total(p);
        if (!n) continue;
        int base = B.n;
        int vis0 = B.start - base, vis1 = B.start + ROWS - base;
        bool need = (vis1 > 0 && vis0 < n) || (B.probe_idx >= base && B.probe_idx < base + n);
        if (need) tod_get(p, U.tod, PM_RDM_TOD_MAX);
        for (uint16_t i = 0; i < n; i++) {
            int idx = B.n;
            bool shown = (idx >= B.start && idx < B.start + ROWS) || idx == B.probe_idx;
            item_t *it = badd(K_DEV);
            if (!shown) continue;
            it->port = p;
            it->uid = U.tod[i < PM_RDM_TOD_MAX ? i : 0];
            ui_rdm_dev_t *d = ui_rdm_dev(p, &it->uid);
            char addr[8], name[20];
            if ((d->flags & UI_RDM_HAVE_INFO) && d->start >= 1 && d->start <= 512) {
                snprintf(addr, sizeof(addr), "%03u", (unsigned)d->start);
            } else {
                snprintf(addr, sizeof(addr), "---");
            }
            if ((d->flags & UI_RDM_HAVE_LABEL) && d->label[0]) snprintf(name, sizeof(name), "%.15s", d->label);
            else fmt_uid(name, sizeof(name), &d->uid);
            bool ov = dev_overlaps(d);
            if (port == 0xFF) snprintf(it->label, sizeof(it->label), "%u %s%s%.14s", p + 1u, addr, ov ? "!" : " ", name);
            else snprintf(it->label, sizeof(it->label), "%s%s%.16s", addr, ov ? "!" : " ", name);
            if (d->flags & UI_RDM_FAILED) it->lcolor = UI_C_GRAY;
            if (ov) it->lcolor = UI_C_RED;
            if (d->flags & UI_RDM_IDENTIFY) snprintf(it->value, sizeof(it->value), UI_G_IDENT);
            it->wide = true;
        }
    }
}

static void build(const frame_t *f)
{
    char v[24], v2[24];
    uint8_t p = f->port;
    switch (f->list) {
    case L_MAIN:
        add_sub("Network", L_NET, 0);
        snprintf(v, sizeof(v), "Devices (%u)", (unsigned)dev_total(0xFF));
        add_sub(v, L_DEVS, 0xFF);
        add_sub("Node", L_NODE, 0);
        add_sub("Display", L_DISPLAY, 0);
        add_field("Discovery", F_RDM_INT, 0);
        add_sub("System", L_SYSTEM, 0);
        break;
    case L_PORT: {
        uint16_t c;
        port_status_line(p, v, sizeof(v), &c);
        add_info(v, NULL, c);
        add_field("Address", F_P_ADDR, p);
        add_field("Mode", F_P_MODE, p);
        if (port_is_output(p)) {
            if (U.s.port[p].rdm_enabled) {
                snprintf(v, sizeof(v), "Devices (%u)", (unsigned)dev_total(p));
                add_sub(v, L_DEVS, p);
            }
            add_field("Merge", F_P_MERGE, p);
            add_field("RDM", F_P_RDM, p);
            add_field("Failsafe", F_P_FAILSAFE, p);
        }
        add_sub("Advanced", L_PORT_ADV, p);
        break;
    }
    case L_PORT_ADV:
        if (port_is_output(p)) {
            add_field("Output", F_P_STYLE, p);
            add_field("Refresh", F_P_RATE, p);
            add_field("Break", F_P_BREAK, p);
            add_field("MAB", F_P_MAB, p);
            if (U.s.port[p].rdm_enabled) add_field("Bg discov.", F_P_BGDISC, p);
            add_action("Record scene" UI_G_ELLIPSIS, A_RECORD_SCENE, p, UI_C_WHITE);
        }
        add_action("Reset port" UI_G_ELLIPSIS, A_RESET_PORT, p, UI_C_WHITE);
        break;
    case L_NET: {
        uint32_t ip, mask;
        uint8_t mode;
        bool link;
        net_status(&ip, &mask, &mode, &link);
        add_field("Mode", F_NET_MODE, 0);
        if (U.net.dhcp) {
            fmt_ip(v, sizeof(v), ip);
            add_info("IP", ip ? v : "-", UI_C_GRAY);
            fmt_ip(v2, sizeof(v2), mask);
            add_info("Mask", mask ? v2 : "-", UI_C_GRAY);
        } else {
            add_field("IP", F_NET_IP, 0);
            add_field("Mask", F_NET_MASK, 0);
            add_field("Gateway", F_NET_GW, 0);
        }
        if (U.net_staged) add_action("Apply" UI_G_ELLIPSIS, A_NET_APPLY, 0, UI_C_CYAN);
        add_sub("Info", L_NET_INFO, 0);
        break;
    }
    case L_NET_INFO: {
        uint32_t ip, mask;
        uint8_t mode;
        bool link;
        static const char *const modes[] = {"-", "DHCP", "Fallback", "Static"};
        net_status(&ip, &mask, &mode, &link);
        add_info("Link", link ? "Up" : "Down", link ? UI_C_WHITE : UI_C_RED);
        add_info("State", modes[mode & 3], mode == PM_NET_ADDR_FALLBACK_UI ? UI_C_YELLOW : UI_C_WHITE);
        fmt_ip(v, sizeof(v), ip);
        add_info("IP", v, UI_C_WHITE);
        fmt_ip(v, sizeof(v), mask);
        add_info("Mask", v, UI_C_WHITE);
        fmt_ip(v, sizeof(v), U.s.ip.gateway);
        add_info("Gateway", U.s.ip.gateway ? v : "-", UI_C_WHITE);
        add_info("MAC", NULL, UI_C_GRAY);
        snprintf(v, sizeof(v), "%02X:%02X:%02X:%02X:%02X:%02X", U.info.mac[0], U.info.mac[1], U.info.mac[2],
                 U.info.mac[3], U.info.mac[4], U.info.mac[5]);
        add_info("", v, UI_C_WHITE);
        break;
    }
    case L_DEVS:
        add_action(UI_G_REMOTE " Rescan", A_RESCAN, p, UI_C_WHITE);
        add_devices(p);
        break;
    case L_DEV: {
        ui_rdm_dev_t *d = ui_rdm_dev(f->port, &f->uid);
        add_field("Identify", F_D_IDENT, f->port);
        if (d->footprint || d->start != 0xFFFF) add_field("DMX addr", F_D_ADDR, f->port);
        if (d->pers_count > 1) add_field("Mode", F_D_PERS, f->port);
        snprintf(v, sizeof(v), "%u ch", (unsigned)d->footprint);
        add_info("Footprint", (d->flags & UI_RDM_HAVE_INFO) ? v : "?", UI_C_GRAY);
        add_field("Label", F_D_LABEL, f->port);
        add_sub("Info", L_DEV_INFO, f->port);
        break;
    }
    case L_DEV_INFO: {
        ui_rdm_dev_t *d = ui_rdm_dev(f->port, &f->uid);
        fmt_uid(v, sizeof(v), &d->uid);
        add_info("UID", NULL, UI_C_GRAY);
        add_info("", v, UI_C_WHITE);
        snprintf(v, sizeof(v), "%04X", d->model);
        add_info("Model", v, UI_C_WHITE);
        snprintf(v, sizeof(v), "%08lX", (unsigned long)d->sw_version);
        add_info("Software", v, UI_C_WHITE);
        snprintf(v, sizeof(v), "%u of %u", d->pers, d->pers_count);
        add_info("Personality", v, UI_C_WHITE);
        snprintf(v, sizeof(v), "Port %u", f->port + 1u);
        add_info("Line", v, UI_C_WHITE);
        if (d->flags & UI_RDM_FAILED) add_info("No response", NULL, UI_C_RED);
        break;
    }
    case L_NODE:
        add_field("Identify", F_IDENTIFY, 0);
        add_field("Short name", F_SHORT, 0);
        add_field("Long name", F_LONG, 0);
        fmt_uid(v, sizeof(v), &U.info.uid);
        add_info("UID", v, UI_C_WHITE);
        break;
    case L_DISPLAY:
        add_field("Brightness", F_DISP_LEVEL, 0);
        add_field("Dim after", F_DISP_DIM, 0);
        add_field("Addr format", F_ADDR_FMT, 0);
        add_action("Lock panel" UI_G_ELLIPSIS, A_LOCK, 0, UI_C_WHITE);
        break;
    case L_SYSTEM:
        add_sub("Info", L_SYS_INFO, 0);
        add_sub("Events", L_EVENTS, 0);
        add_action("Restart" UI_G_ELLIPSIS, A_RESTART, 0, UI_C_WHITE);
        add_action("Factory reset" UI_G_ELLIPSIS, A_FACTORY_KEEP, 0, UI_C_RED);
        break;
    case L_SYS_INFO: {
        add_info("Firmware", U.info.fw_version, UI_C_WHITE);
        snprintf(v, sizeof(v), "%u", U.info.hw_revision);
        add_info("HW revision", v, UI_C_WHITE);
        add_info("RDM UID", NULL, UI_C_GRAY);
        fmt_uid(v, sizeof(v), &U.info.uid);
        add_info("", v, UI_C_WHITE);
        uint32_t up = U.now / 1000u;
        snprintf(v, sizeof(v), "%lu:%02lu:%02lu", (unsigned long)(up / 3600), (unsigned long)(up / 60 % 60),
                 (unsigned long)(up % 60));
        add_info("Uptime", v, UI_C_WHITE);
        snprintf(v, sizeof(v), "%04X/%04X", PM_ESTA_MANUFACTURER, PM_MODEL_NODE);
        add_info("ESTA/model", v, UI_C_WHITE);
        snprintf(v, sizeof(v), "%04X", PM_ARTNET_OEM_NODE);
        add_info("Art-Net OEM", v, UI_C_WHITE);
        break;
    }
    case L_EVENTS:
        if (!U.log_n) add_info("No events", NULL, UI_C_GRAY);
        for (int i = 0; i < U.log_n; i++) {
            int k = (U.log_head - 1 - i + LOG_N) % LOG_N;
            add_info(U.log[k].text, NULL, UI_C_WHITE);
        }
        break;
    case L_RESET_CHOICE:
        add_action("Keep network", A_FACTORY_KEEP, 0, UI_C_WHITE);
        add_action("Everything", A_FACTORY_ALL, 0, UI_C_RED);
        break;
    default: break;
    }
}

/** Build list `f` with a visible window starting at `start` and probe item `probe`. */
static void build_list(const frame_t *f, int start, int probe)
{
    memset(B.sel, 0, sizeof(B.sel));
    B.n = 0;
    B.start = start;
    B.probe_idx = probe;
    B.edit_found = false;
    build(f);
    if (probe >= start && probe < start + ROWS) B.probe = B.win[probe - start];
}

static bool selectable(int i) { return i >= 0 && i < B.n && i < SEL_MAX && (B.sel[i / 8] & (1u << (i % 8))); }

static bool list_info_only(void)
{
    for (int i = 0; i < B.n && i < SEL_MAX; i++) {
        if (selectable(i)) return false;
    }
    return true;
}

static void list_fix_cursor(frame_t *f)
{
    if (B.n == 0) {
        f->cur = f->top = 0;
        return;
    }
    if (f->cur >= B.n) f->cur = (uint16_t)(B.n - 1);
    if (!list_info_only() && !selectable(f->cur)) {
        int c = f->cur;
        while (c < B.n && !selectable(c)) c++;
        if (c >= B.n) {
            c = f->cur;
            while (c > 0 && !selectable(c)) c--;
        }
        f->cur = (uint16_t)c;
    }
    if (list_info_only()) {
        int maxtop = B.n > ROWS ? B.n - ROWS : 0;
        if (f->top > maxtop) f->top = (uint16_t)maxtop;
        return;
    }
    if (f->cur < f->top) f->top = f->cur;
    if (f->cur >= f->top + ROWS) f->top = (uint16_t)(f->cur - ROWS + 1);
    /* keep a leading info row visible when the cursor is on the first selectable item */
    if (f->top > 0 && f->cur < ROWS && !selectable(0)) {
        bool all_before_info = true;
        for (int i = 0; i < f->cur; i++) {
            if (selectable(i)) all_before_info = false;
        }
        if (all_before_info) f->top = 0;
    }
}

/* ------------------------------------------------------------------------------------------------ titles */

static void frame_title(const frame_t *f, char *buf, size_t n)
{
    switch (f->scr) {
    case S_ADDR: snprintf(buf, n, "Address"); return;
    case S_IP: snprintf(buf, n, "%s", U.ie.field == F_NET_GW ? "Gateway" : "IP"); return;
    case S_TEXT:
        snprintf(buf, n, "%s", U.te.field == F_SHORT ? "Short name" : U.te.field == F_LONG ? "Long name" : "Label");
        return;
    case S_CONFIRM: snprintf(buf, n, "%s", U.dlg.title); return;
    default: break;
    }
    switch (f->list) {
    case L_MAIN: snprintf(buf, n, "Menu"); break;
    case L_PORT:
        snprintf(buf, n, "Port %u " UI_G_MIDDOT " %s", f->port + 1u, N_MODE[fget(F_P_MODE, f->port)]);
        break;
    case L_PORT_ADV: snprintf(buf, n, "Advanced"); break;
    case L_NET: snprintf(buf, n, "Network"); break;
    case L_NET_INFO: case L_SYS_INFO: case L_DEV_INFO: snprintf(buf, n, "Info"); break;
    case L_DEVS:
        if (f->port == 0xFF) snprintf(buf, n, "Devices (%u)", (unsigned)dev_total(0xFF));
        else snprintf(buf, n, "Devices (%u)", (unsigned)dev_total(f->port));
        break;
    case L_DEV: {
        ui_rdm_dev_t *d = ui_rdm_find(f->port, &f->uid);
        if (d && d->label[0]) snprintf(buf, n, "P%u %.16s", f->port + 1u, d->label);
        else {
            char u[16];
            fmt_uid(u, sizeof(u), &f->uid);
            snprintf(buf, n, "P%u %s", f->port + 1u, u);
        }
        break;
    }
    case L_NODE: snprintf(buf, n, "Node"); break;
    case L_DISPLAY: snprintf(buf, n, "Display"); break;
    case L_SYSTEM: snprintf(buf, n, "System"); break;
    case L_EVENTS: snprintf(buf, n, "Events"); break;
    case L_RESET_CHOICE: snprintf(buf, n, "Factory reset"); break;
    default: buf[0] = '\0'; break;
    }
}

/** Path of the current page; leading levels are replaced by an ellipsis when it is longer than 26 characters. */
static void path(char *out, size_t n)
{
    char t[32];
    const size_t max = UI_W / 6;
    for (int first = 1; first <= U.depth; first++) {
        out[0] = '\0';
        if (first > 1) snprintf(out, n, UI_G_ELLIPSIS " " UI_G_SUB " ");
        for (int i = first; i <= U.depth; i++) {
            frame_title(&U.stack[i], t, sizeof(t));
            size_t l = strlen(out);
            snprintf(out + l, n - l, "%s%s", i > first ? " " UI_G_SUB " " : "", t);
        }
        if (strlen(out) <= max) return;
    }
    out[max] = '\0';
}

/* ------------------------------------------------------------------------------------------------ drawing */

typedef enum { PS_OFF, PS_IDLE, PS_OUT, PS_IN, PS_LOSS, PS_ERR } port_state_t;

static port_state_t port_state(uint8_t p, pm_port_stats_t *st, bool *loss, bool *merging)
{
    bool synced;
    port_stats(p, st);
    port_flags(p, loss, merging, &synced);
    const pm_port_settings_t *ps = &U.s.port[p];
    if (!ps->enabled || p >= PM_NUM_PORTS) return PS_OFF;
    if (since(U.err_t[p], ERR_SHOW_MS)) return PS_ERR;
    if (*loss) return PS_LOSS;
    if (ps->direction == PM_PORT_DIR_INPUT) return st->input_signal ? PS_IN : PS_IDLE;
    return st->fps > 0 ? PS_OUT : PS_IDLE;
}

static bool blink(uint32_t period_ms) { return (U.now / (period_ms / 2)) % 2 == 0; }

static const char *dir_glyph(uint8_t p)
{
    if (!U.s.port[p].enabled) return UI_G_DASH;
    return U.s.port[p].direction == PM_PORT_DIR_INPUT ? UI_G_IN : UI_G_OUT;
}

static uint16_t state_color(port_state_t s)
{
    switch (s) {
    case PS_OUT: return UI_C_GREEN;
    case PS_IN: return UI_C_BLUE;
    case PS_LOSS: return UI_C_YELLOW;
    case PS_ERR: return UI_C_RED;
    case PS_IDLE: return UI_C_WHITE;
    default: return UI_C_DIM;
    }
}

static void draw_status_bar(void)
{
    if (U.identify || U.locate) {
        if (blink(500)) {
            ui_fill(0, 0, UI_W, 16, UI_C_WHITE);
            ui_text_c(0, UI_W, 0, UI_FONT_N, 1, UI_C_BLACK, UI_G_IDENT " IDENTIFY " UI_G_IDENT);
            return;
        }
    }
    for (uint8_t p = 0; p < PM_NUM_PORTS; p++) {
        int x = p * 25;
        pm_port_stats_t st;
        bool loss, merging;
        port_state_t s = port_state(p, &st, &loss, &merging);
        char d[2] = {(char)('1' + p), 0};
        uint16_t bg = UI_C_BLACK, fg = UI_C_WHITE, frame = 0;
        bool has_frame = false;
        switch (s) {
        case PS_OUT: bg = UI_C_GREEN; fg = UI_C_BLACK; break;
        case PS_IN: bg = UI_C_BLUE; fg = UI_C_WHITE; break;
        case PS_ERR: bg = UI_C_RED; fg = UI_C_WHITE; break;
        case PS_LOSS:
            if (blink(1000)) { bg = UI_C_YELLOW; fg = UI_C_BLACK; }
            else { fg = UI_C_YELLOW; frame = UI_C_YELLOW; has_frame = true; }
            break;
        case PS_IDLE: frame = UI_C_WHITE; has_frame = true; break;
        default: fg = UI_C_DIM; break;
        }
        ui_fill(x, 0, 24, 16, bg);
        if (has_frame) ui_frame(x, 0, 24, 16, frame);
        ui_text(x + 3, 0, UI_FONT_N, fg, d);
        ui_text(x + 12, 0, UI_FONT_N, fg, s == PS_ERR ? "!" : dir_glyph(p));
        /* activity edge (bottom 3 px) */
        if (st.rdm_requests != U.rdm_cnt[p]) {
            U.rdm_cnt[p] = st.rdm_requests;
            U.rdm_t[p] = U.now ? U.now : 1;
        }
        if (U.tube_pct[p] >= 0) {
            ui_fill(x, 13, 24, 3, UI_C_BLACK);
            ui_fill(x, 13, 24 * U.tube_pct[p] / 100, 3, UI_C_CYAN);
        } else if (since(U.rdm_t[p], RDM_PULSE_MS)) {
            ui_fill(x, 13, 24, 3, UI_C_VIOLET);
        }
    }
    /* system zone: link, IP mode, lock, update/error (x 104..159, 14 px each) */
    uint32_t ip, mask;
    uint8_t mode;
    bool link;
    net_status(&ip, &mask, &mode, &link);
    int x = 104;
    ui_text(x + 3, 0, UI_FONT_N, link ? UI_C_GREEN : UI_C_RED, link ? UI_G_LINK : UI_G_NOLINK);
    x += 14;
    if (link) {
        const char *m = "D";
        uint16_t c = UI_C_WHITE;
        if (mode == PM_NET_ADDR_STATIC_UI) m = "S";
        else if (mode == PM_NET_ADDR_FALLBACK_UI) { m = "F"; c = UI_C_YELLOW; }
        else if (mode == 0) { c = blink(1000) ? UI_C_GRAY : UI_C_BLACK; }
        ui_text(x + 3, 0, UI_FONT_N, c, m);
    }
    x += 14;
    if (U.s.panel_lock) ui_text(x + 3, 0, UI_FONT_N, U.lock_remote ? UI_C_ORANGE : UI_C_WHITE, UI_G_LOCK);
    x += 14;
    bool tube = false;
    for (int p = 0; p < (int)PM_NUM_PORTS; p++) tube |= U.tube_pct[p] >= 0;
    if (U.fw_pct >= 0) {
        ui_text(x + 3, 0, UI_FONT_N, UI_C_CYAN, UI_G_DOWN);
        ui_fill(x, 14, 14 * U.fw_pct / 100, 2, UI_C_CYAN);
    } else if (tube) {
        ui_text(x + 3, 0, UI_FONT_N, UI_C_CYAN, UI_G_DOWN);
    } else if (U.sys_error) {
        ui_text(x + 3, 0, UI_FONT_N, UI_C_RED, "!");
    }
}

static void draw_header_lines(const char *l1, const char *l2, uint16_t c1, uint16_t c2)
{
    if (l2 && l2[0]) {
        ui_text(2, Y_HDR, UI_FONT_S, c1, l1);
        ui_text(2, Y_HDR + 8, UI_FONT_S, c2, l2);
    } else {
        ui_text(2, Y_HDR + 4, UI_FONT_S, c1, l1);
    }
}

/** Header of every screen except home: conflict hint > toast > edit hint > path. */
static void draw_header(const char *edit_hint1, const char *edit_hint2)
{
    uint8_t scr = top()->scr;
    bool full_editor = scr == S_ADDR || scr == S_IP || scr == S_TEXT; /* they show the hint at the bottom */
    if (since(U.conflict_t, CONFLICT_MS) && !full_editor) {
        draw_header_lines(UI_G_REMOTE " changed by Pixelmapper", U.conflict, UI_C_ORANGE, UI_C_ORANGE);
    } else if (since(U.toast_t, TOAST_MS)) {
        draw_header_lines(U.toast, NULL, U.toast_color, 0);
    } else if (edit_hint1) {
        draw_header_lines(edit_hint1, edit_hint2, UI_C_CYAN, UI_C_GRAY);
    } else {
        char p[64];
        path(p, sizeof(p));
        draw_header_lines(p, NULL, UI_C_WHITE, 0);
        bool on_net = top()->scr == S_LIST && top()->list == L_NET;
        if (U.net_staged && on_net) ui_text(UI_W - 8, Y_HDR + 4, UI_FONT_S, UI_C_CYAN, UI_G_DOT);
    }
    ui_fill(0, Y_HDR + 15, UI_W, 1, UI_C_LINE);
}

/** Two small hint lines at the bottom of full-screen editors (conflict hint wins). */
static void draw_hint(const char *l1, const char *l2, uint16_t c)
{
    if (since(U.conflict_t, CONFLICT_MS)) {
        ui_text(2, Y_HINT, UI_FONT_S, UI_C_ORANGE, UI_G_REMOTE " changed by Pixelmapper");
        ui_text(2, Y_HINT + 8, UI_FONT_S, UI_C_ORANGE, U.conflict);
        return;
    }
    if (l1) ui_text(2, Y_HINT, UI_FONT_S, c, l1);
    if (l2) ui_text(2, Y_HINT + 8, UI_FONT_S, UI_C_GRAY, l2);
}

static void draw_list(frame_t *f)
{
    build_list(f, f->top, f->cur);
    list_fix_cursor(f);
    build_list(f, f->top, f->cur); /* again with the fixed window */
    if (U.ed.active && !B.edit_found) {
        U.ed.active = false; /* the field disappeared (remote change) */
        toast(UI_C_ORANGE, UI_G_REMOTE " Page changed by Pixelmapper");
    }
    bool info_only = list_info_only();
    const char *h1 = NULL, *h2 = NULL;
    if (U.ed.active) {
        h1 = "Turn: change  Push: OK";
        h2 = U.ed.field == F_DISP_LEVEL ? "BACK: cancel  (preview)" : "BACK: cancel";
    }
    draw_header(h1, h2);
    for (int r = 0; r < ROWS && f->top + r < B.n; r++) {
        const item_t *it = &B.win[r];
        int y = Y_CONTENT + r * ROW_H;
        bool cur = !info_only && f->top + r == f->cur;
        bool editing = cur && U.ed.active;
        bool flash = cur && since(U.flash_t, FLASH_MS);
        uint16_t lc = it->lcolor, vc = it->vcolor;
        if (cur && !editing) {
            ui_fill(0, y, 157, ROW_H, flash ? UI_C_CYAN : UI_C_WHITE);
            lc = vc = UI_C_BLACK;
        }
        if (it->wide) {
            ui_text(2, y, UI_FONT_N, lc, it->label);
            if (it->value[0]) ui_text_r(X_VAL_END, y, UI_FONT_N, cur ? UI_C_BLACK : UI_C_WHITE, it->value);
            continue;
        }
        int vw = ui_text_width(UI_FONT_N, it->value);
        int maxl = (X_VAL_END - vw - 2 - 2) / 8;
        char l[24];
        snprintf(l, sizeof(l), "%.*s", maxl > 0 ? maxl : 0, it->label);
        ui_text(2, y, UI_FONT_N, lc, l);
        if (editing) {
            ui_fill(X_VAL_END - vw - 2, y, vw + 4, ROW_H, UI_C_CYAN);
            ui_text_r(X_VAL_END, y, UI_FONT_N, UI_C_BLACK, it->value);
        } else {
            ui_text_r(X_VAL_END, y, UI_FONT_N, vc, it->value);
        }
    }
    if (B.n > ROWS) {
        int track = ROWS * ROW_H;
        int th = track * ROWS / B.n;
        if (th < 6) th = 6;
        int ty = (track - th) * f->top / (B.n - ROWS);
        ui_fill(157, Y_CONTENT, 3, track, UI_C_LINE);
        ui_fill(157, Y_CONTENT + ty, 3, th, UI_C_WHITE);
    }
}

static void draw_home(void)
{
    /* short name (8x16 bold) */
    int y = 16;
    bool c0 = U.home_cur == 0;
    if (c0) ui_fill(0, y, UI_W, 16, UI_C_WHITE);
    ui_text(2, y, UI_FONT_N, c0 ? UI_C_BLACK : UI_C_WHITE, U.s.short_name);
    if (c0) ui_text_r(X_VAL_END, y, UI_FONT_N, UI_C_BLACK, UI_G_SUB);
    else if ((U.rmask & PM_CHG_NAMES) && since(U.rmask_t, REMOTE_HL_MS)) {
        ui_text(2, y, UI_FONT_N, UI_C_ORANGE, U.s.short_name);
    }
    /* IP + mode (6x8) or toast */
    y = 33;
    bool c1 = U.home_cur == 1;
    if (c1) ui_fill(0, y - 1, UI_W, 10, UI_C_WHITE);
    if (since(U.toast_t, TOAST_MS) && !c1) {
        ui_text(2, y, UI_FONT_S, U.toast_color, U.toast);
    } else {
        uint32_t ip, mask;
        uint8_t mode;
        bool link;
        net_status(&ip, &mask, &mode, &link);
        char line[40], ips[20];
        uint16_t c = c1 ? UI_C_BLACK : UI_C_WHITE;
        if (!link) {
            snprintf(line, sizeof(line), "No link");
            if (!c1) c = UI_C_RED;
        } else if (mode == 0) {
            snprintf(line, sizeof(line), "DHCP" UI_G_ELLIPSIS);
            if (!c1) c = UI_C_GRAY;
        } else {
            static const char *const names[] = {"", "DHCP", "Fallback", "Static"};
            fmt_ip(ips, sizeof(ips), ip);
            snprintf(line, sizeof(line), "%s /%d  %s", ips, prefix_of(mask), names[mode & 3]);
            if (mode == PM_NET_ADDR_FALLBACK_UI && !c1) c = UI_C_YELLOW;
        }
        ui_text(2, y, UI_FONT_S, c, line);
        if (c1) ui_text_r(X_VAL_END, y, UI_FONT_S, UI_C_BLACK, UI_G_SUB);
    }
    /* 4 port rows, 21 px each */
    for (uint8_t p = 0; p < PM_NUM_PORTS; p++) {
        y = 44 + p * 21;
        bool cur = U.home_cur == 2 + p;
        pm_port_stats_t st;
        bool loss, merging;
        port_state_t s = port_state(p, &st, &loss, &merging);
        if (cur) ui_fill(0, y, UI_W, 21, UI_C_WHITE);
        int ty = y + 3;
        uint16_t base = cur ? UI_C_BLACK : UI_C_WHITE;
        char d[3] = {(char)('1' + p), 0, 0};
        uint16_t bc = state_color(s);
        if (cur) bc = UI_C_BLACK;
        if (s == PS_OFF) {
            ui_text(2, ty, UI_FONT_N, cur ? UI_C_BLACK : UI_C_DIM, d);
            ui_text(2 + 24, ty, UI_FONT_N, cur ? UI_C_BLACK : UI_C_DIM, "off");
            continue;
        }
        if (!cur && (s == PS_OUT || s == PS_IN || s == PS_ERR || (s == PS_LOSS))) {
            /* badge: digit + arrow on the state colour */
            ui_fill(0, y + 2, 20, 17, bc);
            uint16_t tc = (s == PS_IN || s == PS_ERR) ? UI_C_WHITE : UI_C_BLACK;
            ui_text(2, ty, UI_FONT_N, tc, d);
            ui_text(10, ty, UI_FONT_N, tc, s == PS_ERR ? "!" : dir_glyph(p));
        } else {
            ui_text(2, ty, UI_FONT_N, cur ? UI_C_BLACK : bc, d);
            ui_text(10, ty, UI_FONT_N, cur ? UI_C_BLACK : bc, dir_glyph(p));
        }
        char a[12];
        fmt_pa(a, sizeof(a), U.s.port[p].address, U.s.address_format);
        bool hl = (U.rmask & PM_CHG_PORT(PM_CHG_P_ADDRESS, p)) && since(U.rmask_t, REMOTE_HL_MS);
        ui_text(26, ty, UI_FONT_N, hl && !cur ? UI_C_ORANGE : base, a);
        char fps[8];
        bool data = port_is_input(p) ? st.input_signal : (st.fps > 0 && !loss);
        if (data) snprintf(fps, sizeof(fps), "%u", (unsigned)(st.fps > 99 ? 99 : st.fps));
        else snprintf(fps, sizeof(fps), "--");
        ui_text_r(126, ty, UI_FONT_N, base, fps);
        /* tag: ERR > failsafe > tube update > merge > RDM count */
        char tag[8] = "";
        uint16_t tc = base;
        if (s == PS_ERR) { snprintf(tag, sizeof(tag), "ERR"); tc = UI_C_RED; }
        else if (s == PS_LOSS) {
            static const char *const fs[] = {"HLD", "ZER", "FUL", "SCN"};
            snprintf(tag, sizeof(tag), "%s", fs[U.s.port[p].failsafe & 3]);
            tc = UI_C_YELLOW;
        } else if (U.tube_pct[p] >= 0) { snprintf(tag, sizeof(tag), UI_G_DOWN "%d", U.tube_pct[p]); tc = UI_C_CYAN; }
        else if (merging) snprintf(tag, sizeof(tag), "MRG");
        else if (port_is_output(p) && U.s.port[p].rdm_enabled && st.tod_count) {
            snprintf(tag, sizeof(tag), "R%u", (unsigned)(st.tod_count > 99 ? 99 : st.tod_count));
        }
        if (cur) tc = UI_C_BLACK;
        if (tag[0]) ui_text_r(X_VAL_END + 2, ty, UI_FONT_N, tc, tag);
    }
}

static void draw_addr(void)
{
    addr_edit_t *e = &U.ae;
    const char *h1 = NULL, *h2 = NULL;
    bool fmt15 = U.s.address_format == PM_ADDRESS_FORMAT_PORT_ADDRESS;
    draw_header(NULL, NULL);
    unsigned net = PM_PA_NET(e->pa), sub = PM_PA_SUB(e->pa), uni = PM_PA_UNI(e->pa);
    char b[16];
    /* geometry: N:S:U boxes first, 15-bit number below (the other way round in 15-bit format) */
    int y_nsu = fmt15 ? 82 : 48, y_num = fmt15 ? 46 : 74;
    struct { int x, w; unsigned v; const char *lbl; } box[3] = {
        {12, 40, net, "Net"}, {68, 32, sub, "Sub"}, {116, 32, uni, "Uni"}};
    for (int i = 0; i < 3; i++) {
        bool edit = !fmt15 && ((e->mode == AE_QUICK && i == 2) || (e->mode == AE_FIELD && e->sel == i));
        bool selb = !fmt15 && e->mode == AE_SELECT && e->sel == i;
        ui_text_c(box[i].x, box[i].w, y_nsu - 10, UI_FONT_S, 1, UI_C_GRAY, box[i].lbl);
        if (edit) ui_fill(box[i].x, y_nsu, box[i].w, 16, UI_C_CYAN);
        if (selb) ui_frame(box[i].x - 1, y_nsu - 1, box[i].w + 2, 18, UI_C_WHITE);
        snprintf(b, sizeof(b), "%u", box[i].v);
        ui_text_r(box[i].x + box[i].w - 4, y_nsu, UI_FONT_N, edit ? UI_C_BLACK : (fmt15 ? UI_C_GRAY : UI_C_WHITE), b);
    }
    ui_text(56, y_nsu, UI_FONT_N, fmt15 ? UI_C_GRAY : UI_C_WHITE, ":");
    ui_text(104, y_nsu, UI_FONT_N, fmt15 ? UI_C_GRAY : UI_C_WHITE, ":");
    /* 15-bit */
    ui_text(12, y_num, UI_FONT_N, fmt15 ? UI_C_WHITE : UI_C_GRAY, "15-bit");
    bool nedit = fmt15 && (e->mode == AE_QUICK || (e->mode == AE_FIELD && e->sel == 0));
    bool nsel = fmt15 && e->mode == AE_SELECT && e->sel == 0;
    snprintf(b, sizeof(b), "%u", (unsigned)e->pa);
    if (nedit) ui_fill(92, y_num, 56, 16, UI_C_CYAN);
    if (nsel) ui_frame(91, y_num - 1, 58, 18, UI_C_WHITE);
    ui_text_r(144, y_num, UI_FONT_N, nedit ? UI_C_BLACK : (fmt15 ? UI_C_WHITE : UI_C_GRAY), b);
    /* OK button in field selection */
    if (e->mode != AE_QUICK) {
        bool ok = e->sel == (fmt15 ? 1 : 3);
        if (ok) ui_fill(116, 94, 40, 16, UI_C_WHITE);
        else ui_frame(116, 94, 40, 16, UI_C_GRAY);
        ui_text_c(116, 40, 94, UI_FONT_N, 1, ok ? UI_C_BLACK : UI_C_WHITE, "OK");
    }
    if (e->mode == AE_QUICK) { h1 = "Push: OK   BACK: fields"; h2 = fmt15 ? "Turn: change" : "Turn: change (with carry)"; }
    else if (e->mode == AE_SELECT) { h1 = "Turn: field  Push: edit"; h2 = "BACK: cancel"; }
    else { h1 = "Turn: change  Push: done"; h2 = "BACK: undo field"; }
    draw_hint(h1, h2, UI_C_CYAN);
}

static bool ip_valid(const ip_edit_t *e, char *why, size_t n)
{
    uint32_t ip = ((uint32_t)e->oct[0] << 24) | ((uint32_t)e->oct[1] << 16) | ((uint32_t)e->oct[2] << 8) | e->oct[3];
    int pre = prefix_of(U.net.static_mask);
    uint32_t m = mask_of(pre);
    if (e->field == F_NET_GW) {
        if (ip == 0) return true;
        if ((ip & m) != (U.net.static_ip & m)) {
            snprintf(why, n, "Not in the IP's /%d net", pre);
            return false;
        }
    }
    if (ip == 0 || e->oct[0] == 0 || e->oct[0] == 127 || e->oct[0] >= 224 || (ip & ~m) == 0 || (ip & ~m) == ~m) {
        snprintf(why, n, "Invalid for /%d", pre);
        return false;
    }
    return true;
}

static void draw_ip(void)
{
    ip_edit_t *e = &U.ie;
    draw_header(NULL, NULL);
    ui_text(2, Y_CONTENT + 6, UI_FONT_S, UI_C_GRAY, e->field == F_NET_GW ? "Gateway (0.0.0.0 = none)" : "Static IP address");
    int y = 60;
    for (int i = 0; i < 4; i++) {
        int x = 8 + i * 36;
        bool edit = (e->mode == AE_QUICK && i == 3) || (e->mode == AE_FIELD && e->sel == i);
        bool sel = e->mode == AE_SELECT && e->sel == i;
        char b[8];
        snprintf(b, sizeof(b), "%u", e->oct[i]);
        if (edit) ui_fill(x, y, 28, 16, UI_C_CYAN);
        if (sel) ui_frame(x - 1, y - 1, 30, 18, UI_C_WHITE);
        ui_text_r(x + 26, y, UI_FONT_N, edit ? UI_C_BLACK : UI_C_WHITE, b);
        if (i < 3) ui_text(x + 28, y, UI_FONT_N, UI_C_WHITE, ".");
    }
    if (e->mode != AE_QUICK) {
        bool ok = e->sel == 4;
        if (ok) ui_fill(116, 94, 40, 16, UI_C_WHITE);
        else ui_frame(116, 94, 40, 16, UI_C_GRAY);
        ui_text_c(116, 40, 94, UI_FONT_N, 1, ok ? UI_C_BLACK : UI_C_WHITE, "OK");
    }
    uint32_t ip, mask;
    uint8_t mode;
    bool link;
    net_status(&ip, &mask, &mode, &link);
    char b[32], a[20];
    fmt_ip(a, sizeof(a), e->field == F_NET_GW ? U.s.ip.gateway : ip);
    snprintf(b, sizeof(b), "active: %s", a);
    ui_text(2, 84, UI_FONT_S, UI_C_GRAY, b);
    ui_text(2, 98, UI_FONT_S, UI_C_CYAN, UI_G_DOT " staged");
    char why[32];
    if (e->invalid && !ip_valid(e, why, sizeof(why))) {
        draw_hint(why, "Push: OK   BACK: fields", UI_C_YELLOW);
    } else if (e->mode == AE_QUICK) {
        draw_hint("Push: OK   BACK: fields", "Turn: change last octet", UI_C_CYAN);
    } else if (e->mode == AE_SELECT) {
        draw_hint("Turn: field  Push: edit", "BACK: cancel", UI_C_CYAN);
    } else {
        draw_hint("Turn: change  Push: done", "BACK: undo field", UI_C_CYAN);
    }
}

static void draw_text_editor(void)
{
    text_edit_t *e = &U.te;
    draw_header(NULL, NULL);
    /* text with cursor block (scrolls left with an ellipsis) */
    size_t len = strlen(e->text);
    char shown[24];
    if (len > 18) snprintf(shown, sizeof(shown), UI_G_ELLIPSIS "%s", e->text + len - 17);
    else scopy(shown, sizeof(shown), e->text);
    int x = ui_text(2, Y_CONTENT, UI_FONT_N, UI_C_WHITE, shown);
    if (len < e->max) ui_fill(x, Y_CONTENT + 13, 8, 3, UI_C_CYAN);
    char cnt[12];
    snprintf(cnt, sizeof(cnt), "%u/%u", (unsigned)len, (unsigned)e->max);
    ui_text_r(X_VAL_END, Y_CONTENT + 18, UI_FONT_S, UI_C_GRAY, cnt);
    ui_fill(0, Y_CONTENT + 28, UI_W, 1, UI_C_LINE);
    /* character ring, selection in the middle */
    int y = 76;
    for (int k = -3; k <= 3; k++) {
        int i = ((int)e->ring + k + RING_N) % RING_N;
        char c[2] = {RING[i], 0};
        int cx = 80 - 4 + k * 22;
        if (k == 0) {
            ui_fill(cx - 4, y - 2, 16, 20, UI_C_CYAN);
            ui_text(cx, y, UI_FONT_N, UI_C_BLACK, c);
        } else {
            ui_text(cx, y, UI_FONT_N, (k == -1 || k == 1) ? UI_C_WHITE : UI_C_GRAY, c);
        }
    }
    char ch = RING[e->ring];
    const char *name = ch == UI_G_CHECK[0] ? "Done" : ch == UI_G_BKSP[0] ? "Delete" : ch == UI_G_SPACE[0] ? "Space" : "";
    if (name[0]) ui_text_c(0, UI_W, 98, UI_FONT_S, 1, UI_C_WHITE, name);
    uint32_t held = ui_input_push_held(&U.in, U.now);
    if (ch == UI_G_BKSP[0] && held > 200) {
        ui_fill(40, 106, 80 * (held > TEXT_CLEAR_MS ? TEXT_CLEAR_MS : held) / TEXT_CLEAR_MS, 3, UI_C_RED);
    }
    if (ch == UI_G_CHECK[0]) draw_hint("Push: apply", "BACK: cancel", UI_C_CYAN);
    else if (ch == UI_G_BKSP[0]) draw_hint("Push: delete  Hold: all", "BACK: cancel", UI_C_CYAN);
    else draw_hint("Push: add character", "BACK: cancel", UI_C_CYAN);
}

static void draw_dialog(void)
{
    dialog_t *d = &U.dlg;
    char t[32];
    snprintf(t, sizeof(t), UI_G_WARN " %s", d->title);
    ui_text(2, Y_HDR, UI_FONT_N, d->danger ? UI_C_RED : UI_C_YELLOW, t);
    ui_fill(0, Y_HDR + 15, UI_W, 1, UI_C_LINE);
    for (int i = 0; i < 4; i++) {
        if (d->line[i][0]) ui_text(2, Y_CONTENT + 2 + i * 16, UI_FONT_N, d->line_color[i], d->line[i]);
    }
    int y = 98;
    for (int b = 0; b < 2; b++) {
        int x = b == 0 ? 4 : 82, w = 74;
        bool foc = d->focus == b;
        uint16_t fill = foc ? ((b == 1 && d->danger) ? UI_C_RED : UI_C_WHITE) : UI_C_BLACK;
        uint16_t tc = foc ? ((b == 1 && d->danger) ? UI_C_WHITE : UI_C_BLACK) : ((b == 1 && d->danger) ? UI_C_RED : UI_C_WHITE);
        ui_fill(x, y, w, 16, fill);
        ui_frame(x, y, w, 16, foc ? fill : UI_C_GRAY);
        if (d->hold && b == 1 && foc) {
            uint32_t held = ui_input_push_held(&U.in, U.now);
            if (held > RESET_HOLD_MS) held = RESET_HOLD_MS;
            ui_fill(x, y, (int)(w * held / RESET_HOLD_MS), 16, UI_C_WHITE);
            tc = held ? UI_C_BLACK : tc;
        }
        ui_text_c(x, w, y, UI_FONT_N, 1, tc, d->btn[b]);
    }
    if (d->hold) ui_text(2, Y_HINT + 6, UI_FONT_S, UI_C_GRAY, "Hold push 3 s on Reset");
    else ui_text(2, Y_HINT + 6, UI_FONT_S, UI_C_GRAY, "Turn: choose  Push: OK");
}

static void draw_lock_overlay(void)
{
    ui_fill(0, Y_HDR, UI_W, UI_H - Y_HDR, UI_C_BLACK);
    uint32_t c = ui_input_combo_ms(&U.in, U.now);
    bool lk = U.s.panel_lock;
    ui_text_c(0, UI_W, 40, UI_FONT_N, 1, UI_C_WHITE, UI_G_LOCK);
    ui_text_c(0, UI_W, 58, UI_FONT_N, 1, UI_C_WHITE, lk ? "Panel locked" : "Lock panel");
    if (lk && U.lock_remote) ui_text_c(0, UI_W, 76, UI_FONT_S, 1, UI_C_ORANGE, "(by Pixelmapper)");
    if (c) {
        ui_frame(20, 92, 120, 10, UI_C_WHITE);
        ui_fill(22, 94, (int)(116 * c / UI_COMBO_MS), 6, UI_C_CYAN);
        ui_text_c(0, UI_W, 108, UI_FONT_S, 1, UI_C_GRAY, lk ? "Unlocking" UI_G_ELLIPSIS : "Locking" UI_G_ELLIPSIS);
    } else {
        ui_text_c(0, UI_W, 98, UI_FONT_S, 1, UI_C_GRAY, lk ? "Unlock: hold BACK + push" : "Hold BACK + push");
        ui_text_c(0, UI_W, 108, UI_FONT_S, 1, UI_C_GRAY, "for 3 seconds");
    }
}

static void draw_fw(void)
{
    ui_text(2, Y_HDR, UI_FONT_N, UI_C_WHITE, "Firmware update");
    ui_fill(0, Y_HDR + 15, UI_W, 1, UI_C_LINE);
    char b[16];
    snprintf(b, sizeof(b), "%d%%", U.fw_pct);
    ui_text_c(0, UI_W, 44, UI_FONT_N, 2, UI_C_WHITE, b);
    ui_frame(8, 80, 144, 12, UI_C_CYAN);
    ui_fill(10, 82, 140 * U.fw_pct / 100, 8, UI_C_CYAN);
    ui_text_c(0, UI_W, 98, UI_FONT_N, 1, UI_C_YELLOW, "Do not power off");
    ui_text_c(0, UI_W, Y_HINT + 8, UI_FONT_S, 1, UI_C_GRAY, U.fw_pct >= 100 ? "Restarting" UI_G_ELLIPSIS : "Receiving image");
}

static void draw(void)
{
    ui_gfx_begin();
    draw_status_bar();
    frame_t *f = top();
    if (U.fw_pct >= 0) {
        draw_fw();
        return;
    }
    switch (f->scr) {
    case S_HOME: draw_home(); break;
    case S_LIST: draw_list(f); break;
    case S_ADDR: draw_addr(); break;
    case S_IP: draw_ip(); break;
    case S_TEXT: draw_text_editor(); break;
    case S_CONFIRM: draw_dialog(); break;
    default: break;
    }
    if (since(U.lock_overlay_t, LOCK_OVERLAY_MS) || ui_input_combo_ms(&U.in, U.now) > 150) draw_lock_overlay();
}

/* ------------------------------------------------------------------------------------------------ editors: open */

static void open_editor(uint8_t f, uint8_t p)
{
    if (f == F_P_ADDR) {
        memset(&U.ae, 0, sizeof(U.ae));
        U.ae.port = p;
        U.ae.pa = U.ae.orig = U.s.port[p].address;
        U.ae.mode = AE_QUICK;
        push_frame(S_ADDR, 0, p);
    } else if (f == F_NET_IP || f == F_NET_GW) {
        memset(&U.ie, 0, sizeof(U.ie));
        U.ie.field = f;
        uint32_t v = f == F_NET_IP ? U.net.static_ip : U.net.gateway;
        if (f == F_NET_IP && v == 0) {
            uint32_t ip, mask;
            uint8_t mode;
            bool link;
            net_status(&ip, &mask, &mode, &link);
            v = ip;
        }
        for (int i = 0; i < 4; i++) U.ie.oct[i] = (uint8_t)(v >> (24 - 8 * i));
        U.ie.mode = AE_QUICK;
        push_frame(S_IP, 0, 0);
    } else {
        memset(&U.te, 0, sizeof(U.te));
        U.te.field = f;
        U.te.port = p;
        if (f == F_SHORT) { snprintf(U.te.text, sizeof(U.te.text), "%s", U.s.short_name); U.te.max = PM_SHORT_NAME_LEN - 1; }
        else if (f == F_LONG) { snprintf(U.te.text, sizeof(U.te.text), "%s", U.s.long_name); U.te.max = PM_LONG_NAME_LEN - 1; }
        else {
            ui_rdm_dev_t *d = frame_dev(top());
            snprintf(U.te.text, sizeof(U.te.text), "%s", d->label);
            U.te.max = 32;
        }
        memcpy(U.te.orig, U.te.text, sizeof(U.te.orig));
        U.te.ring = 0;
        push_frame(S_TEXT, 0, p);
    }
    U.edit_t = U.now;
}

/* ------------------------------------------------------------------------------------------------ keys */

static int32_t step_value(int32_t v, const fdesc_t *d, const ui_key_t *k)
{
    int32_t range = (d->max - d->min) / (d->step ? d->step : 1);
    int32_t mult = 1;
    if (range > 50) {
        mult = k->accel;
        if (mult > range / 20) mult = range / 20 > 0 ? range / 20 : 1;
    }
    v += (int32_t)k->steps * mult * d->step;
    if (v < d->min) v = d->min;
    if (v > d->max) v = d->max;
    return v;
}

static void inline_begin(uint8_t f, uint8_t p)
{
    fdesc_t d = fdesc(f, p);
    U.ed.active = true;
    U.ed.field = f;
    U.ed.port = p;
    int32_t v = fget(f, p);
    if (d.type == FT_LIST) {
        int best = 0;
        for (int i = 0; i < (int)DIM_LIST_N; i++) {
            if (DIM_LIST[i] <= v) best = i;
        }
        v = DIM_LIST[best];
    }
    if (f == F_D_ADDR && (v < 1 || v > 512)) v = 1;
    if (f == F_D_PERS) {
        ui_rdm_dev_t *dev = frame_dev(top());
        ui_rdm_load_personalities(dev);
    }
    U.ed.value = U.ed.orig = v;
    U.edit_t = U.now;
}

static void inline_cancel(void)
{
    if (!U.ed.active) return;
    U.ed.active = false;
    if (U.ed.field == F_DISP_LEVEL) U.backlight = 0xFF ^ U.backlight; /* force backlight refresh */
}

static void inline_key(const ui_key_t *k)
{
    inline_edit_t *e = &U.ed;
    fdesc_t d = fdesc(e->field, e->port);
    if (k->type == UI_KEY_ROT) {
        if (d.type == FT_LIST) {
            int idx = 0;
            for (int i = 0; i < (int)DIM_LIST_N; i++) {
                if (DIM_LIST[i] == e->value) idx = i;
            }
            idx += k->steps;
            if (idx < 0) idx = 0;
            if (idx >= (int)DIM_LIST_N) idx = DIM_LIST_N - 1;
            e->value = DIM_LIST[idx];
        } else {
            e->value = step_value(e->value, &d, k);
        }
        U.edit_t = U.now;
    } else if (k->type == UI_KEY_PUSH) {
        U.ed.active = false;
        if (e->value != e->orig || e->value != fget(e->field, e->port)) fapply_confirm(e->field, e->port, e->value, 0);
        else U.flash_t = U.now;
    } else if (k->type == UI_KEY_BACK) {
        inline_cancel();
    }
}

static void list_push(frame_t *f)
{
    build_list(f, f->top, f->cur);
    if (!selectable(f->cur)) return;
    item_t it = B.probe;
    switch (it.kind) {
    case K_SUB:
        push_frame(S_LIST, it.arg, it.port);
        if (it.arg == L_DEV || it.arg == L_DEV_INFO) top()->uid = f->uid;
        if (it.arg == L_NET && U.depth >= 1 && !U.net_staged) U.net = U.s.ip;
        break;
    case K_DEV:
        push_frame(S_LIST, L_DEV, it.port);
        top()->uid = it.uid;
        break;
    case K_FIELD: {
        if (locked()) { show_lock(); return; }
        fdesc_t d = fdesc(it.arg, it.port);
        if (d.type == FT_EDITOR) open_editor(it.arg, it.port);
        else inline_begin(it.arg, it.port);
        break;
    }
    case K_ACTION:
        if (locked()) { show_lock(); return; }
        switch (it.arg) {
        case A_NET_APPLY: {
            open_dialog(A_NET_APPLY, "Change network", false);
            dialog_t *d = &U.dlg;
            snprintf(d->line[0], sizeof(d->line[0]), "Changing the IP");
            snprintf(d->line[1], sizeof(d->line[1]), "interrupts Art-Net");
            snprintf(d->line[2], sizeof(d->line[2]), "connections.");
            if (U.net.dhcp) snprintf(d->line[3], sizeof(d->line[3]), "New: DHCP");
            else {
                char a[20];
                fmt_ip(a, sizeof(a), U.net.static_ip);
                snprintf(d->line[3], sizeof(d->line[3]), "%s/%d", a, prefix_of(U.net.static_mask));
            }
            d->line_color[3] = UI_C_CYAN;
            snprintf(d->btn[1], sizeof(d->btn[1]), "Apply");
            break;
        }
        case A_RECORD_SCENE:
            open_dialog(A_RECORD_SCENE, "Record scene", false);
            snprintf(U.dlg.line[0], sizeof(U.dlg.line[0]), "Stores the current");
            snprintf(U.dlg.line[1], sizeof(U.dlg.line[1]), "output of port %u as", it.port + 1u);
            snprintf(U.dlg.line[2], sizeof(U.dlg.line[2]), "its failsafe scene.");
            snprintf(U.dlg.btn[1], sizeof(U.dlg.btn[1]), "Record");
            U.dlg.port = it.port;
            break;
        case A_RESET_PORT:
            open_dialog(A_RESET_PORT, "Reset port", false);
            snprintf(U.dlg.line[0], sizeof(U.dlg.line[0]), "All settings of");
            snprintf(U.dlg.line[1], sizeof(U.dlg.line[1]), "port %u go back to", it.port + 1u);
            snprintf(U.dlg.line[2], sizeof(U.dlg.line[2]), "factory defaults.");
            snprintf(U.dlg.btn[1], sizeof(U.dlg.btn[1]), "Reset");
            U.dlg.port = it.port;
            break;
        case A_RESTART:
            open_dialog(A_RESTART, "Restart", false);
            snprintf(U.dlg.line[0], sizeof(U.dlg.line[0]), "The node restarts.");
            snprintf(U.dlg.line[1], sizeof(U.dlg.line[1]), "Output stops for");
            snprintf(U.dlg.line[2], sizeof(U.dlg.line[2]), "a few seconds.");
            snprintf(U.dlg.btn[1], sizeof(U.dlg.btn[1]), "Restart");
            break;
        case A_FACTORY_KEEP:
        case A_FACTORY_ALL:
            if (f->list == L_SYSTEM) {
                push_frame(S_LIST, L_RESET_CHOICE, 0);
                break;
            }
            open_dialog(A_FACTORY_DO, "Factory reset", true);
            snprintf(U.dlg.line[0], sizeof(U.dlg.line[0]), "All settings");
            snprintf(U.dlg.line[1], sizeof(U.dlg.line[1]), "%s", it.arg == A_FACTORY_ALL ? "incl. network" : "except network");
            snprintf(U.dlg.line[2], sizeof(U.dlg.line[2]), "to factory state.");
            snprintf(U.dlg.btn[1], sizeof(U.dlg.btn[1]), "Reset");
            U.dlg.hold = true;
            U.dlg.value = it.arg == A_FACTORY_KEEP;
            break;
        case A_LOCK: {
            pm_settings_t *s = &U.tmp;
            settings_fresh(s);
            s->panel_lock = true;
            settings_write(s);
            U.lock_remote = false;
            toast(UI_C_WHITE, UI_G_LOCK " Panel locked");
            log_add("Panel locked");
            break;
        }
        case A_RESCAN:
            if (U.plat.tod_flush) {
                for (uint8_t p = 0; p < PM_NUM_PORTS; p++) {
                    if ((it.port == 0xFF || it.port == p) && port_is_output(p)) U.plat.tod_flush(U.plat.ctx, p);
                }
            }
            ui_rdm_forget(it.port);
            toast(UI_C_WHITE, "Discovery started");
            break;
        default: break;
        }
        break;
    default: break;
    }
}

static void list_rot(frame_t *f, int steps)
{
    build_list(f, f->top, f->cur);
    if (list_info_only()) {
        int t = (int)f->top + steps;
        int maxtop = B.n > ROWS ? B.n - ROWS : 0;
        if (t < 0) t = 0;
        if (t > maxtop) t = maxtop;
        f->top = (uint16_t)t;
        return;
    }
    int c = f->cur;
    int dir = steps > 0 ? 1 : -1;
    for (int n = steps > 0 ? steps : -steps; n > 0; n--) {
        int next = c + dir;
        while (next >= 0 && next < B.n && !selectable(next)) next += dir;
        if (next < 0 || next >= B.n) break; /* no wrap */
        c = next;
    }
    f->cur = (uint16_t)c;
    list_fix_cursor(f);
}

static void leave_frame(void)
{
    frame_t *f = top();
    if (f->scr == S_LIST && f->list == L_DEV) {
        ui_rdm_dev_t *d = ui_rdm_find(f->port, &f->uid);
        if (d && (d->flags & UI_RDM_IDENTIFY)) ui_rdm_set_identify(d, false); /* identify ends with the page */
    }
    if (f->scr == S_LIST && f->list == L_NET && U.net_staged) {
        open_dialog(A_NET_LEAVE, "Network changes", false);
        snprintf(U.dlg.line[0], sizeof(U.dlg.line[0]), "Network settings");
        snprintf(U.dlg.line[1], sizeof(U.dlg.line[1]), "are staged but not");
        snprintf(U.dlg.line[2], sizeof(U.dlg.line[2]), "applied yet.");
        snprintf(U.dlg.btn[0], sizeof(U.dlg.btn[0]), "Discard");
        snprintf(U.dlg.btn[1], sizeof(U.dlg.btn[1]), "Apply");
        return;
    }
    pop_frame();
}

static void addr_key(const ui_key_t *k)
{
    addr_edit_t *e = &U.ae;
    bool fmt15 = U.s.address_format == PM_ADDRESS_FORMAT_PORT_ADDRESS;
    int nsel = fmt15 ? 2 : 4;
    U.edit_t = U.now;
    if (k->type == UI_KEY_ROT) {
        if (e->mode == AE_SELECT) {
            int s = e->sel + k->steps;
            e->sel = (uint8_t)(s < 0 ? 0 : s >= nsel ? nsel - 1 : s);
        } else if (e->mode == AE_QUICK || (fmt15 && e->sel == 0)) {
            fdesc_t d = {FT_NUM, 0, PM_PA_MAX, 1, NULL, ""};
            e->pa = step_value(e->pa, &d, k); /* Uni carries into Sub/Net */
        } else {
            int net = PM_PA_NET(e->pa), sub = PM_PA_SUB(e->pa), uni = PM_PA_UNI(e->pa);
            int *v = e->sel == 0 ? &net : e->sel == 1 ? &sub : &uni;
            int max = e->sel == 0 ? 127 : 15;
            fdesc_t d = {FT_NUM, 0, max, 1, NULL, ""};
            *v = step_value(*v, &d, k);
            e->pa = PM_PA_MAKE(net, sub, uni);
        }
    } else if (k->type == UI_KEY_PUSH) {
        if (e->mode == AE_QUICK || (e->mode == AE_SELECT && e->sel == nsel - 1)) {
            if (e->pa == U.s.port[e->port].address) {
                pop_frame();
                return;
            }
            fapply_confirm(F_P_ADDR, e->port, e->pa, 1);
        } else if (e->mode == AE_SELECT) {
            e->mode = AE_FIELD;
            e->orig = e->pa;
        } else {
            e->mode = AE_SELECT;
        }
    } else if (k->type == UI_KEY_BACK) {
        if (e->mode == AE_QUICK) { e->mode = AE_SELECT; e->sel = fmt15 ? 0 : 2; }
        else if (e->mode == AE_FIELD) { e->pa = e->orig; e->mode = AE_SELECT; }
        else pop_frame();
    }
}

static void ip_key(const ui_key_t *k)
{
    ip_edit_t *e = &U.ie;
    U.edit_t = U.now;
    e->invalid = false;
    if (k->type == UI_KEY_ROT) {
        if (e->mode == AE_SELECT) {
            int s = e->sel + k->steps;
            e->sel = (uint8_t)(s < 0 ? 0 : s > 4 ? 4 : s);
        } else {
            int i = e->mode == AE_QUICK ? 3 : e->sel;
            fdesc_t d = {FT_NUM, 0, 255, 1, NULL, ""};
            e->oct[i] = (uint8_t)step_value(e->oct[i], &d, k);
        }
    } else if (k->type == UI_KEY_PUSH) {
        if (e->mode == AE_QUICK || (e->mode == AE_SELECT && e->sel == 4)) {
            char why[32];
            if (!ip_valid(e, why, sizeof(why))) {
                e->invalid = true; /* rejected: the editor stays open */
                return;
            }
            uint32_t v = ((uint32_t)e->oct[0] << 24) | ((uint32_t)e->oct[1] << 16) | ((uint32_t)e->oct[2] << 8) | e->oct[3];
            fapply(e->field, 0, (int32_t)v);
            pop_frame();
        } else if (e->mode == AE_SELECT) {
            e->mode = AE_FIELD;
        } else {
            e->mode = AE_SELECT;
        }
    } else if (k->type == UI_KEY_BACK) {
        if (e->mode == AE_QUICK) { e->mode = AE_SELECT; e->sel = 3; }
        else if (e->mode == AE_FIELD) e->mode = AE_SELECT;
        else pop_frame();
    }
}

static void text_apply(void)
{
    text_edit_t *e = &U.te;
    if (strcmp(e->text, e->orig) != 0) {
        if (e->field == F_D_LABEL) {
            ui_rdm_dev_t *d = frame_dev(&U.stack[U.depth - 1]);
            ui_rdm_set_label(d, e->text);
        } else {
            pm_settings_t *s = &U.tmp;
            settings_fresh(s);
            if (e->field == F_SHORT) scopy(s->short_name, sizeof(s->short_name), e->text);
            else scopy(s->long_name, sizeof(s->long_name), e->text);
            settings_write(s);
        }
    }
    pop_frame();
}

static void text_key(const ui_key_t *k)
{
    text_edit_t *e = &U.te;
    U.edit_t = U.now;
    if (k->type == UI_KEY_ROT) {
        int r = ((int)e->ring + k->steps) % RING_N; /* the ring is the one place that wraps */
        if (r < 0) r += RING_N;
        e->ring = (uint8_t)r;
    } else if (k->type == UI_KEY_PUSH) {
        char ch = RING[e->ring];
        size_t len = strlen(e->text);
        if (ch == UI_G_CHECK[0]) {
            text_apply();
        } else if (ch == UI_G_BKSP[0]) {
            if (len) e->text[len - 1] = '\0';
        } else if (len < e->max) {
            e->text[len] = ch == UI_G_SPACE[0] ? ' ' : ch;
            e->text[len + 1] = '\0';
        }
    } else if (k->type == UI_KEY_BACK) {
        if (strcmp(e->text, e->orig) != 0) {
            open_dialog(A_TEXT_DISCARD, "Discard text?", false);
            snprintf(U.dlg.line[0], sizeof(U.dlg.line[0]), "The changed text");
            snprintf(U.dlg.line[1], sizeof(U.dlg.line[1]), "will be lost.");
            snprintf(U.dlg.btn[0], sizeof(U.dlg.btn[0]), "Keep");
            snprintf(U.dlg.btn[1], sizeof(U.dlg.btn[1]), "Discard");
        } else {
            pop_frame();
        }
    }
}

static void dialog_key(const ui_key_t *k)
{
    dialog_t *d = &U.dlg;
    if (k->type == UI_KEY_ROT) {
        int f = d->focus + (k->steps > 0 ? 1 : -1);
        d->focus = (uint8_t)(f < 0 ? 0 : f > 1 ? 1 : f);
    } else if (k->type == UI_KEY_PUSH) {
        if (d->focus == 0) {
            dialog_cancel();
        } else if (d->action == A_TEXT_DISCARD) {
            pop_frame();
            pop_frame();
        } else if (!d->hold) {
            dialog_execute();
        }
    } else if (k->type == UI_KEY_BACK) {
        if (d->action == A_NET_LEAVE) pop_frame(); /* BACK = stay on the network page */
        else dialog_cancel();
    }
}

static void home_key(const ui_key_t *k)
{
    if (k->type == UI_KEY_ROT) {
        if (U.home_cur < 0) {
            U.home_cur = U.home_last;
            return;
        }
        int c = U.home_cur + k->steps;
        U.home_cur = c < 0 ? 0 : c > 5 ? 5 : c;
        U.home_last = U.home_cur;
    } else if (k->type == UI_KEY_PUSH) {
        int c = U.home_cur;
        U.home_cur = -1;
        if (c <= 0) push_frame(S_LIST, L_MAIN, 0);
        else if (c == 1) {
            push_frame(S_LIST, L_NET, 0);
            if (!U.net_staged) U.net = U.s.ip;
        } else push_frame(S_LIST, L_PORT, (uint8_t)(c - 2));
    } else if (k->type == UI_KEY_BACK) {
        U.home_cur = -1;
    }
}

static bool editing_open(void)
{
    uint8_t s = top()->scr;
    return U.ed.active || s == S_ADDR || s == S_IP || s == S_TEXT;
}

static void handle_key(const ui_key_t *k)
{
    U.last_input = U.now;
    /* the first input on a dark or dimmed display only wakes it up */
    if (U.muted) {
        U.muted = false;
        return;
    }
    if (U.backlight < U.s.display_level && U.backlight <= DIM_LEVEL && U.s.display_dim_timeout_s) {
        if (k->type != UI_KEY_COMBO) {
            U.wake_t = U.now;
            return;
        }
    }
    if (U.fw_pct >= 0) {
        if (k->type == UI_KEY_BACK || k->type == UI_KEY_PUSH) toast(UI_C_WHITE, "Please wait");
        return;
    }
    if (k->type == UI_KEY_COMBO) {
        pm_settings_t *s = &U.tmp;
        settings_fresh(s);
        bool was_remote = s->panel_lock && U.lock_remote;
        s->panel_lock = !s->panel_lock;
        settings_write(s);
        U.lock_remote = false;
        U.lock_overlay_t = 0;
        if (s->panel_lock) {
            inline_cancel();
            toast(UI_C_WHITE, UI_G_LOCK " Panel locked");
            log_add("Panel locked");
        } else {
            toast(UI_C_WHITE, "Panel unlocked");
            log_add(was_remote ? "Remote lock released" : "Panel unlocked");
        }
        return;
    }
    if (k->type == UI_KEY_BACK_LONG) {
        bool open = editing_open();
        if (U.net_staged) discard_staged(false);
        else if (open) toast(UI_C_YELLOW, "Input discarded");
        inline_cancel();
        go_home();
        return;
    }
    if (since(U.lock_overlay_t, LOCK_OVERLAY_MS) && k->type == UI_KEY_BACK) {
        U.lock_overlay_t = 0;
        return;
    }
    frame_t *f = top();
    switch (f->scr) {
    case S_HOME: home_key(k); break;
    case S_LIST:
        if (U.ed.active) {
            inline_key(k);
        } else if (k->type == UI_KEY_ROT) {
            list_rot(f, k->steps);
        } else if (k->type == UI_KEY_PUSH) {
            list_push(f);
        } else if (k->type == UI_KEY_BACK) {
            leave_frame();
        }
        break;
    case S_ADDR: addr_key(k); break;
    case S_IP: ip_key(k); break;
    case S_TEXT: text_key(k); break;
    case S_CONFIRM: dialog_key(k); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ events */

static const char *const PORT_FIELD_NAMES[] = {"on/off", "Mode", "Address", "Merge", "RDM", "Output", "Timing"};

static int popcount64(pm_change_mask_t m)
{
    int n = 0;
    while (m) {
        m &= m - 1;
        n++;
    }
    return n;
}

static void describe(pm_change_mask_t m, const pm_settings_t *n, char *buf, size_t len)
{
    if (popcount64(m) > 1) {
        /* enabled + direction of one port count as one change */
        for (uint8_t p = 0; p < PM_NUM_PORTS; p++) {
            if (m == PM_CHG_PORT(PM_CHG_P_ENABLED | PM_CHG_P_DIRECTION, p)) {
                m = PM_CHG_PORT(PM_CHG_P_DIRECTION, p);
            }
        }
    }
    if (popcount64(m) != 1) {
        snprintf(buf, len, UI_G_REMOTE " %d changes by Pixelmapper", popcount64(m));
        return;
    }
    for (uint8_t p = 0; p < PM_NUM_PORTS; p++) {
        for (int b = 0; b < 7; b++) {
            if (!(m & PM_CHG_PORT(1u << b, p))) continue;
            char v[16] = "";
            const pm_port_settings_t *ps = &n->port[p];
            if (b == 2) fmt_pa(v, sizeof(v), ps->address, n->address_format);
            else if (b == 0 || b == 1) snprintf(v, sizeof(v), "%s", N_MODE[!ps->enabled ? 0 : ps->direction == PM_PORT_DIR_INPUT ? 2 : 1]);
            else if (b == 3) snprintf(v, sizeof(v), "%s", N_MERGE[ps->merge & 1]);
            else if (b == 4) snprintf(v, sizeof(v), "%s", ps->rdm_enabled ? "On" : "Off");
            if (v[0]) snprintf(buf, len, UI_G_REMOTE " P%u %s " UI_G_RARROW " %s", p + 1u, PORT_FIELD_NAMES[b == 0 ? 1 : b], v);
            else snprintf(buf, len, UI_G_REMOTE " P%u %s changed", p + 1u, PORT_FIELD_NAMES[b]);
            return;
        }
    }
    if (m & PM_CHG_NAMES) snprintf(buf, len, UI_G_REMOTE " Name " UI_G_RARROW " %.14s", n->short_name);
    else if (m & PM_CHG_IP) snprintf(buf, len, UI_G_REMOTE " Network changed");
    else if (m & PM_CHG_PANEL_LOCK) snprintf(buf, len, UI_G_REMOTE " Panel %s", n->panel_lock ? "locked" : "unlocked");
    else if (m & PM_CHG_DISPLAY) snprintf(buf, len, UI_G_REMOTE " Display changed");
    else if (m & PM_CHG_RDM_INTERVAL) snprintf(buf, len, UI_G_REMOTE " Discovery " UI_G_RARROW " %u s", n->rdm_bg_interval_s);
    else snprintf(buf, len, UI_G_REMOTE " Settings changed");
}

/** Remote change of the value that is open in an editor: keep the input, show the new value (last writer wins). */
static void check_conflict(pm_change_mask_t m)
{
    char v[24] = "";
    frame_t *f = top();
    if (U.ed.active && (m & fmask(U.ed.field, U.ed.port))) {
        fformat(U.ed.field, U.ed.port, fget(U.ed.field, U.ed.port), v, sizeof(v));
    } else if (f->scr == S_ADDR && (m & fmask(F_P_ADDR, U.ae.port))) {
        fmt_pa(v, sizeof(v), U.s.port[U.ae.port].address, U.s.address_format);
    } else if (f->scr == S_TEXT && U.te.field != F_D_LABEL && (m & PM_CHG_NAMES)) {
        snprintf(v, sizeof(v), "%.17s", U.te.field == F_SHORT ? U.s.short_name : U.s.long_name);
    } else if (U.net_staged && (m & PM_CHG_IP)) {
        if (U.s.ip.dhcp) snprintf(v, sizeof(v), "DHCP");
        else fmt_ip(v, sizeof(v), U.s.ip.static_ip);
    } else {
        return;
    }
    snprintf(U.conflict, sizeof(U.conflict), "now: %s", v);
    U.conflict_t = U.now ? U.now : 1;
}

static void on_settings_changed(int32_t src)
{
    pm_settings_t *n = &U.tmp;
    settings_fresh(n);
    pm_change_mask_t m = pm_settings_diff(&U.s, n);
    if (!m) return;
    bool remote = src != PM_SRC_LOCAL_UI;
    if (m & PM_CHG_PANEL_LOCK) {
        U.lock_remote = n->panel_lock && remote;
        if (n->panel_lock && remote) {
            inline_cancel();
            if (editing_open()) {
                while (U.depth > 0 && editing_open()) pop_frame();
            }
            log_add("Locked by Pixelmapper");
        }
    }
    if (!U.net_staged) U.net = n->ip;
    U.s = *n;
    if (!remote) return;
    U.rmask = m;
    U.rmask_t = U.now ? U.now : 1;
    char d[40];
    describe(m, n, d, sizeof(d));
    toast(UI_C_ORANGE, "%s", d);
    log_add("%.20s", d);
    check_conflict(m);
}

static void wake(void) { U.wake_t = U.now ? U.now : 1; }

void ui_core_event(const pm_ui_event_t *ev)
{
    U.now = U.plat.now_ms ? U.plat.now_ms(U.plat.ctx) : U.now;
    switch (ev->type) {
    case PM_UI_EV_SETTINGS_CHANGED: on_settings_changed(ev->value); break;
    case PM_UI_EV_LED_MODE:
        if (ev->value == 0x03) { /* AcLedMute: display off */
            U.muted = true;
            U.locate = false;
        } else if (ev->value == 0x04) { /* AcLedLocate */
            U.locate = true;
            U.muted = false;
            wake();
            log_add("Locate (Art-Net)");
        } else { /* AcLedNormal */
            U.muted = false;
            U.locate = false;
        }
        break;
    case PM_UI_EV_IDENTIFY:
        if (U.identify != (ev->value != 0)) log_add(ev->value ? "Identify on" : "Identify off");
        U.identify = ev->value != 0;
        if (U.identify) wake();
        break;
    case PM_UI_EV_TOD_CHANGED: break;
    case PM_UI_EV_RDM_RESPONSE: ui_rdm_on_response(ev->token, ev->data, ev->len); break;
    case PM_UI_EV_FW_PROGRESS:
        if (ev->value < 0) {
            if (U.fw_pct >= 0) {
                toast(UI_C_RED, "Firmware update failed");
                log_add("FW update failed");
                U.sys_error = true;
            }
            U.fw_pct = -1;
        } else {
            if (U.fw_pct < 0) {
                log_add("FW update started");
                inline_cancel();
                go_home();
            }
            U.fw_pct = ev->value > 100 ? 100 : (int)ev->value;
            wake();
        }
        break;
    case PM_UI_EV_BOOT_CATCH: {
        static const char *const st[] = {"idle", "running", "caught", "timed out", "aborted"};
        const char *s = ev->value >= 0 && ev->value <= 4 ? st[ev->value] : "?";
        toast(ev->value == PM_BOOT_CATCH_STATE_TIMEOUT ? UI_C_YELLOW : UI_C_WHITE, "P%u boot catch %s", ev->port + 1u, s);
        log_add("P%u catch %s", ev->port + 1u, s);
        break;
    }
    case PM_UI_EV_ERROR:
        if (ev->port < PM_NUM_PORTS) {
            U.err_t[ev->port] = U.now ? U.now : 1;
            U.err_code[ev->port] = ev->value;
            toast(UI_C_RED, "P%u error %ld", ev->port + 1u, (long)ev->value);
            log_add("P%u error %ld", ev->port + 1u, (long)ev->value);
        } else {
            U.sys_error = true;
            toast(UI_C_RED, "System error %ld", (long)ev->value);
            log_add("Error %ld", (long)ev->value);
        }
        wake();
        break;
    case PM_UI_EV_TUBE_UPDATE:
        if (ev->port < PM_NUM_PORTS) {
            if (U.tube_pct[ev->port] < 0 && ev->value >= 0) log_add("P%u tube update", ev->port + 1u);
            U.tube_pct[ev->port] = (int8_t)(ev->value < 0 ? -1 : ev->value > 100 ? 100 : ev->value);
        }
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------------------------------ tick */

static void update_backlight(void)
{
    uint8_t lvl = U.s.display_level < MIN_LEVEL ? MIN_LEVEL : U.s.display_level;
    if (U.ed.active && U.ed.field == F_DISP_LEVEL) lvl = (uint8_t)((U.ed.value * 255 + 50) / 100); /* live preview */
    uint32_t idle = U.now - U.last_input;
    bool dim = U.s.display_dim_timeout_s && idle > (uint32_t)U.s.display_dim_timeout_s * 1000u &&
               !since(U.wake_t, WAKE_MS) && U.fw_pct < 0;
    if (dim && lvl > DIM_LEVEL) lvl = DIM_LEVEL;
    bool on = !U.muted;
    if (!on) lvl = 0;
    if (lvl != U.backlight || on != U.panel_on) {
        if (on && !U.panel_on) U.force = true;
        U.backlight = lvl;
        U.panel_on = on;
        if (U.plat.backlight) U.plat.backlight(U.plat.ctx, lvl, on);
    }
}

static void watch_live(void)
{
    uint32_t ip, mask;
    uint8_t mode;
    bool link;
    net_status(&ip, &mask, &mode, &link);
    if (link != U.link_prev) {
        log_add(link ? "Link up" : "Link down");
        if (!link) wake();
        U.link_prev = link;
    }
    for (uint8_t p = 0; p < PM_NUM_PORTS; p++) {
        bool loss, merging, synced;
        port_flags(p, &loss, &merging, &synced);
        if (loss != U.loss_prev[p]) {
            log_add(loss ? "P%u data lost" : "P%u data back", p + 1u);
            if (loss) wake();
            U.loss_prev[p] = loss;
        }
    }
}

static void timeouts(void)
{
    uint32_t idle = U.now - U.last_input;
    if (U.home_cur >= 0 && top()->scr == S_HOME && idle > HOME_CURSOR_MS) U.home_cur = -1;
    if (U.depth == 0) return;
    if (editing_open() || U.net_staged) {
        if (U.now - U.edit_t > EDIT_TIMEOUT_MS && idle > EDIT_TIMEOUT_MS) {
            inline_cancel();
            discard_staged(true);
            go_home();
            toast(UI_C_YELLOW, "Input discarded");
            log_add("Input discarded");
        }
        return;
    }
    if (idle > MENU_TIMEOUT_MS) go_home();
}

void ui_core_input(int detents, bool push, bool back)
{
    U.now = U.plat.now_ms ? U.plat.now_ms(U.plat.ctx) : U.now;
    ui_input_feed(&U.in, detents, push, back, U.now);
}

uint32_t ui_core_tick(void)
{
    U.now = U.plat.now_ms ? U.plat.now_ms(U.plat.ctx) : U.now;
    ui_key_t k;
    while (ui_input_pop(&U.in, &k)) handle_key(&k);
    /* hold actions: erase all (text editor), factory reset (dialog) */
    uint32_t held = ui_input_push_held(&U.in, U.now);
    frame_t *f = top();
    if (f->scr == S_TEXT && RING[U.te.ring] == UI_G_BKSP[0] && held >= TEXT_CLEAR_MS && !U.in.push_consumed) {
        ui_input_consume_push(&U.in);
        U.te.text[0] = '\0';
    }
    if (f->scr == S_CONFIRM && U.dlg.hold && U.dlg.focus == 1 && held >= RESET_HOLD_MS && !U.in.push_consumed) {
        ui_input_consume_push(&U.in);
        dialog_execute();
    }
    if (ui_input_combo_ms(&U.in, U.now)) U.last_input = U.now;
    ui_rdm_tick(U.now);
    uint16_t pid;
    ui_rdm_res_t r = ui_rdm_take_result(&pid);
    if (r == UI_RDM_RES_TIMEOUT) toast(UI_C_RED, "Device not responding");
    else if (r == UI_RDM_RES_NACK) toast(UI_C_RED, "Device rejected (NACK)");
    watch_live();
    timeouts();
    update_backlight();
    draw();
    bool force = U.force;
    U.force = false;
    if (!U.panel_on) return 0;
    return ui_gfx_render(force);
}

void ui_core_invalidate(void) { U.force = true; }

static int rdm_send(void *ctx, uint8_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    if (!U.model.rdm_request) return -1;
    return U.model.rdm_request(U.model.ctx, port, token, data, len) == 0 ? 0 : -1;
}

void ui_core_set_info(const ui_node_info_t *info)
{
    U.info = *info;
    ui_rdm_set_self(&info->uid);
}

void ui_core_init(const ui_platform_t *plat, const pm_ui_model_t *model, const ui_node_info_t *info)
{
    memset(&U, 0, sizeof(U));
    U.plat = *plat;
    if (model) U.model = *model;
    if (info) U.info = *info;
    ui_input_init(&U.in);
    U.now = U.plat.now_ms ? U.plat.now_ms(U.plat.ctx) : 0;
    settings_fresh(&U.s);
    U.net = U.s.ip;
    U.home_cur = -1;
    U.home_last = 2;
    U.fw_pct = -1;
    for (int p = 0; p < (int)PM_NUM_PORTS; p++) U.tube_pct[p] = -1;
    U.last_input = U.now;
    U.backlight = 0xFF;
    U.panel_on = false;
    U.force = true;
    ui_rdm_init(&U.info.uid, rdm_send, NULL);
}
