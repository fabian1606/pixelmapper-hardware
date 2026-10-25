/* vn_port.c - fake pm_port for the virtual node.
 *
 * Mirrors pm_port.c (apply_cfg, handle_cmd, rdm_allowed, BOOT_CATCH across ports) with the line replaced by the
 * simulated RS-485 bus: per port one pm_rdm_ctrl_t driven by sim_run() (DMX frame + RDM in the gap, >= 30 Hz) up to
 * the real time. Each port has its own bus clock that follows the real clock (it may run up to one frame period
 * ahead). Callbacks of the controller only queue events; vn_port_run() hands them to pm_node afterwards (as the
 * network task drains the pm_port event queue). */
#include <stddef.h>
#include <string.h>

#include "vn.h"

#define EVENT_QUEUE_LEN 512
#define INPUT_PERIOD_MS 25u      /* generated input frames: 40 Hz */
#define INPUT_LOST_MS 1000u      /* signal lost after this long without a frame */
#define BG_INTERVAL_DEFAULT_MS 3000u

typedef struct {
    pm_node_port_ev_type_t type;
    pm_port_idx_t port;
    uint32_t token;
    int32_t value;
    uint16_t len;
    uint8_t data[PM_RDM_MAX_MSG_LEN];
} event_t;

typedef struct {
    pm_port_idx_t idx;
    pm_rdm_ctrl_t ctrl;
    sim_bus_t bus;
    sim_port_stats_t stats;
    pm_node_port_cfg_t cfg;
    bool configured;
    /* output */
    uint8_t out[512];
    uint16_t out_len;
    uint8_t pend[512];
    uint16_t pend_len;
    bool pend_valid;
    /* input */
    int input_mode;
    uint8_t input_value;
    uint8_t in[512];
    uint16_t in_len;
    uint32_t in_counter;
    uint32_t next_in_ms;
    uint32_t last_in_ms;
    bool in_seen;
    bool signal;
} vport_t;

static vport_t s_port[PM_NUM_PORTS];
static sim_model_t s_models[VN_MAX_MODELS];
static unsigned s_nmodels;
static uint16_t s_slots = 512;
static pm_port_idx_t s_catch_port;
static event_t s_events[EVENT_QUEUE_LEN];
static unsigned s_ev_head, s_ev_count;
static uint32_t s_events_dropped;

/* ------------------------------------------------------------------------------------------------ events */

static event_t *ev_push(pm_node_port_ev_type_t type, pm_port_idx_t port)
{
    if (s_ev_count >= EVENT_QUEUE_LEN) {
        s_events_dropped++;
        return NULL;
    }
    event_t *e = &s_events[(s_ev_head + s_ev_count++) % EVENT_QUEUE_LEN];
    memset(e, 0, offsetof(event_t, data));
    e->type = type;
    e->port = port;
    return e;
}

static void cb_response(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    event_t *e = ev_push(PM_NODE_PEV_RDM_RESPONSE, port);
    if (e == NULL) return;
    e->token = token;
    e->len = len > sizeof(e->data) ? (uint16_t)sizeof(e->data) : len;
    memcpy(e->data, data, e->len);
}

static void cb_tod_changed(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    ev_push(PM_NODE_PEV_TOD_CHANGED, port);
}

static void cb_boot_catch(void *ctx, pm_port_idx_t port, uint8_t state)
{
    (void)ctx;
    event_t *e = ev_push(PM_NODE_PEV_BOOT_CATCH, port);
    if (e) e->value = state;
}

/* ------------------------------------------------------------------------------------------------ setup */

static uint32_t bus_ms(const vport_t *p) { return p->bus.now_ms; }

static uint32_t bg_interval(const pm_node_port_cfg_t *c)
{
    return c->rdm_bg_interval_ms ? c->rdm_bg_interval_ms : BG_INTERVAL_DEFAULT_MS;
}

void vn_port_init(const pm_uid_t *node_uid, uint16_t dmx_slots)
{
    memset(s_port, 0, sizeof(s_port));
    s_nmodels = 0;
    s_slots = dmx_slots ? (dmx_slots > 512 ? 512 : dmx_slots) : 512;
    const pm_rdm_ctrl_cb_t cb = {.ctx = NULL, .on_response = cb_response, .on_tod_changed = cb_tod_changed,
                                 .on_boot_catch = cb_boot_catch};
    uint32_t now = vn_now_ms();
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        vport_t *p = &s_port[i];
        p->idx = (pm_port_idx_t)i;
        sim_init(&p->bus, 0x1234u + i);
        p->bus.now_us = (uint64_t)now * 1000u;
        p->bus.now_ms = now;
        pm_rdm_ctrl_init(&p->ctrl, (pm_port_idx_t)i, node_uid, &cb);
        p->out_len = 512;
    }
}

static vport_t *port_of(pm_port_idx_t port) { return port < PM_NUM_PORTS ? &s_port[port] : NULL; }

int vn_port_add(pm_port_idx_t port, char kind, pm_uid_t uid, uint32_t version)
{
    vport_t *p = port_of(port);
    if (p == NULL) return -1;
    int i = sim_add(&p->bus, uid);
    if (i < 0) return -1;
    if (kind == 'p') return i;
    if (s_nmodels >= VN_MAX_MODELS) return i;
    sim_model_t *m = &s_models[s_nmodels++];
    if (kind == 't') {
        static uint8_t img[SIM_TUBE_APP_END];
        if (version != 0) {
            sim_model_build_tube_image(img, 1500, pm_uid_manufacturer(&uid), PM_MODEL_TUBE, version);
        }
        sim_model_tube(&p->bus, i, m, version != 0 ? img : NULL);
    } else {
        sim_model_fixture(&p->bus, i, m, 0x0042, 3);
    }
    return i;
}

void vn_port_add_random(pm_port_idx_t port, int n)
{
    vport_t *p = port_of(port);
    if (p) sim_add_random(&p->bus, n);
}

bool vn_port_remove(pm_port_idx_t port, const pm_uid_t *uid)
{
    vport_t *p = port_of(port);
    if (p == NULL) return false;
    int i = sim_find(&p->bus, uid);
    if (i < 0) return false;
    p->bus.dev[i] = p->bus.dev[p->bus.n - 1];
    p->bus.n--;
    return true;
}

bool vn_port_power_cycle(pm_port_idx_t port, const pm_uid_t *uid, uint32_t off_ms)
{
    vport_t *p = port_of(port);
    if (p == NULL) return false;
    int i = sim_find(&p->bus, uid);
    if (i < 0) return false;
    sim_power_cycle(&p->bus, i, off_ms ? off_ms : 1u);
    return true;
}

sim_dev_t *vn_port_dev(pm_port_idx_t port, const pm_uid_t *uid)
{
    vport_t *p = port_of(port);
    if (p == NULL) return NULL;
    int i = sim_find(&p->bus, uid);
    return i < 0 ? NULL : &p->bus.dev[i];
}

void vn_port_set_input(pm_port_idx_t port, int mode, uint8_t value)
{
    vport_t *p = port_of(port);
    if (p == NULL) return;
    p->input_mode = mode;
    p->input_value = value;
}

uint16_t vn_port_output(pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    vport_t *p = port_of(port);
    if (p == NULL) return 0;
    uint16_t n = p->out_len < cap ? p->out_len : cap;
    memcpy(out, p->out, n);
    return n;
}

uint32_t vn_port_frames(pm_port_idx_t port)
{
    vport_t *p = port_of(port);
    return p ? p->stats.frames : 0;
}

bool vn_port_busy(void)
{
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        if (pm_rdm_ctrl_queue_free(&s_port[i].ctrl) < PM_RDM_QUEUE_LEN) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------------ run */

static void run_input(vport_t *p, uint32_t now)
{
    if (p->cfg.mode != PM_NODE_PORT_INPUT || p->input_mode == 0) {
        if (p->signal && (p->cfg.mode != PM_NODE_PORT_INPUT || now - p->last_in_ms >= INPUT_LOST_MS)) {
            p->signal = false;
            event_t *e = ev_push(PM_NODE_PEV_INPUT_SIGNAL, p->idx);
            if (e) e->value = 0;
        }
        return;
    }
    if ((int32_t)(now - p->next_in_ms) < 0) return;
    p->next_in_ms = now + INPUT_PERIOD_MS;
    uint8_t f[512];
    for (unsigned i = 0; i < 512; i++) {
        f[i] = p->input_mode == 1 ? (uint8_t)(p->in_counter + i) : p->input_value;
    }
    p->in_counter++;
    bool changed = !p->in_seen || p->in_len != 512 || memcmp(f, p->in, 512) != 0;
    memcpy(p->in, f, 512);
    p->in_len = 512;
    p->in_seen = true;
    p->last_in_ms = now;
    if (changed) ev_push(PM_NODE_PEV_INPUT_FRAME, p->idx);
    if (!p->signal) {
        p->signal = true;
        event_t *e = ev_push(PM_NODE_PEV_INPUT_SIGNAL, p->idx);
        if (e) e->value = 1;
    }
}

void vn_port_run(pm_node_t *node)
{
    uint32_t now = vn_now_ms();
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        vport_t *p = &s_port[i];
        if (p->cfg.mode == PM_NODE_PORT_OUTPUT) {
            uint16_t slots = p->out_len < s_slots ? (p->out_len ? p->out_len : 1u) : s_slots;
            sim_run(&p->bus, &p->ctrl, now, slots, NULL, NULL, &p->stats);
        } else if ((int32_t)(now - p->bus.now_ms) > 0) {
            p->bus.now_us = (uint64_t)now * 1000u; /* line idle: the clock just follows */
            p->bus.now_ms = now;
        }
        run_input(p, now);
    }
    while (s_ev_count > 0) {
        event_t *e = &s_events[s_ev_head];
        s_ev_head = (s_ev_head + 1u) % EVENT_QUEUE_LEN;
        s_ev_count--;
        const pm_node_port_event_t ev = {.type = e->type, .port = e->port, .token = e->token, .value = e->value,
                                         .len = e->len, .data = e->data};
        pm_node_port_event(node, &ev);
    }
}

/* ------------------------------------------------------------------------------------------------ ops */

static void apply_cfg(vport_t *p, const pm_node_port_cfg_t *c)
{
    uint32_t ms = bus_ms(p);
    bool mode_changed = !p->configured || c->mode != p->cfg.mode;
    bool rdm_on = c->mode == PM_NODE_PORT_OUTPUT && c->rdm_enabled;
    bool rdm_was = p->configured && p->cfg.mode == PM_NODE_PORT_OUTPUT && p->cfg.rdm_enabled;
    pm_rdm_ctrl_set_timing(&p->ctrl, c->break_us, c->mab_us);
    if (mode_changed || rdm_on != rdm_was) {
        pm_rdm_ctrl_set_enabled(&p->ctrl, false, ms);
        if (rdm_on) pm_rdm_ctrl_set_enabled(&p->ctrl, true, ms);
    }
    pm_rdm_ctrl_set_background(&p->ctrl, c->rdm_bg_discovery, bg_interval(c));
    if (mode_changed) {
        p->in_seen = false;
        p->in_len = 0;
        p->stats.frames = 0;
    }
    p->cfg = *c;
    p->configured = true;
}

void vn_port_reset(void)
{
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        vport_t *p = &s_port[i];
        pm_node_port_cfg_t c = p->cfg;
        p->configured = false;
        p->out_len = 512;
        memset(p->out, 0, sizeof(p->out));
        p->pend_valid = false;
        pm_rdm_ctrl_boot_catch_abort(&p->ctrl);
        apply_cfg(p, &c);
    }
    s_ev_count = 0; /* a reboot loses the queue */
}

pm_err_t vn_port_configure(void *ctx, pm_port_idx_t port, const pm_node_port_cfg_t *cfg)
{
    (void)ctx;
    vport_t *p = port_of(port);
    if (p == NULL || cfg == NULL) return PM_ERR_INVALID_ARG;
    apply_cfg(p, cfg);
    return PM_OK;
}

void vn_port_output_write(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    (void)ctx;
    vport_t *p = port_of(port);
    if (p == NULL) return;
    if (len > 512) len = 512;
    if (defer) {
        memcpy(p->pend, data, len);
        p->pend_len = len;
        p->pend_valid = true;
    } else {
        memcpy(p->out, data, len);
        p->out_len = len;
        p->pend_valid = false;
    }
}

void vn_port_commit(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        vport_t *p = &s_port[i];
        if (!p->pend_valid) continue;
        memcpy(p->out, p->pend, p->pend_len);
        p->out_len = p->pend_len;
        p->pend_valid = false;
    }
}

void vn_port_clear(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    vport_t *p = port_of(port);
    if (p == NULL) return;
    memset(p->out, 0, sizeof(p->out));
    p->pend_valid = false;
}

void vn_port_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
    vport_t *p = port_of(port);
    if (p == NULL) return;
    out->output_active = p->cfg.mode == PM_NODE_PORT_OUTPUT && p->stats.frames > 0;
    out->input_signal = p->signal;
    out->rdm_discovery_running = pm_rdm_ctrl_discovery_running(&p->ctrl);
    out->tod_count = pm_rdm_ctrl_tod(&p->ctrl, NULL, 0);
}

uint16_t vn_port_input_read(void *ctx, pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    (void)ctx;
    vport_t *p = port_of(port);
    if (p == NULL || !p->in_seen) return 0;
    uint16_t n = p->in_len < cap ? p->in_len : cap;
    memcpy(out, p->in, n);
    return n;
}

static bool rdm_allowed(const vport_t *p) { return p->cfg.mode == PM_NODE_PORT_OUTPUT && p->cfg.rdm_enabled; }

pm_err_t vn_port_rdm_submit(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *wire, uint16_t len)
{
    (void)ctx;
    vport_t *p = port_of(port);
    if (p == NULL || wire == NULL || len == 0 || len > PM_RDM_MAX_MSG_LEN) return PM_ERR_INVALID_ARG;
    if (!rdm_allowed(p)) return PM_ERR_STATE;
    static pm_rdm_request_t req;
    req.token = token;
    req.len = len;
    memcpy(req.data, wire, len);
    return pm_rdm_ctrl_submit(&p->ctrl, &req);
}

uint8_t vn_port_rdm_queue_free(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    vport_t *p = port_of(port);
    return p ? pm_rdm_ctrl_queue_free(&p->ctrl) : 0;
}

uint16_t vn_port_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)ctx;
    vport_t *p = port_of(port);
    return p ? pm_rdm_ctrl_tod(&p->ctrl, out, cap) : 0;
}

void vn_port_tod_control(void *ctx, pm_port_idx_t port, uint8_t command)
{
    (void)ctx;
    vport_t *p = port_of(port);
    if (p == NULL) return;
    switch (command) {
    case PM_ATC_FLUSH: pm_rdm_ctrl_flush(&p->ctrl, bus_ms(p)); break;
    case PM_ATC_INC_ON: pm_rdm_ctrl_set_background(&p->ctrl, true, bg_interval(&p->cfg)); break;
    case PM_ATC_INC_OFF: pm_rdm_ctrl_set_background(&p->ctrl, false, bg_interval(&p->cfg)); break;
    case PM_ATC_END: pm_rdm_ctrl_discovery_stop(&p->ctrl, bus_ms(p)); break;
    default: break;
    }
}

pm_err_t vn_port_catch_start(void *ctx, pm_port_idx_t port, const pm_uid_t *target)
{
    (void)ctx;
    vport_t *p = port_of(port);
    if (p == NULL || target == NULL || pm_uid_is_vendorcast(target) || !rdm_allowed(p)) return PM_ERR_STATE;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        if (i != port) pm_rdm_ctrl_boot_catch_abort(&s_port[i].ctrl);
    }
    if (pm_rdm_ctrl_boot_catch_start(&p->ctrl, target, bus_ms(p)) == PM_OK) s_catch_port = port;
    return PM_OK;
}

void vn_port_catch_abort(void *ctx)
{
    (void)ctx;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) pm_rdm_ctrl_boot_catch_abort(&s_port[i].ctrl);
}

void vn_port_catch_status(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    (void)ctx;
    vport_t *p = &s_port[s_catch_port];
    pm_rdm_ctrl_boot_catch_status(&p->ctrl, bus_ms(p), out);
    *port = s_catch_port;
}
