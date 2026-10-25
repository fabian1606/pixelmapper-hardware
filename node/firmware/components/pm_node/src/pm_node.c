/*
 * pm_node.c - platform-independent glue of the Pixelmapper Node (pure C11), see pm_node.h.
 *
 * Data flow inside the node's task:
 *   UDP            -> pm_node_handle_packet -> pm_artnet -> artnet ops below -> config_modify / port_* / route()
 *   ArtRdm/panel   -> route(): node UID -> responder (answer at once), others -> ops->port_rdm_submit
 *   port events    -> pm_node_port_event -> pm_artnet (response/TOD/input) / responder (BOOT_CATCH) / ui_event
 *   settings       -> pm_node_settings_changed -> port_configure, net_apply_ip, responder, pm_artnet, ui_event
 *   tick           -> pm_artnet_tick, pm_rdm_responder_poll, tube timeouts, port_live snapshot
 * Large buffers live in the node object (no big stack frames, no malloc).
 */
#include "pm_node.h"

#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#include "pm_pids.h"

#define ALIGN_UP(x) (((x) + alignof(max_align_t) - 1u) & ~(size_t)(alignof(max_align_t) - 1u))

#define RC_FIRMWARE_FAIL 0x000Eu

typedef enum {
    FROM_ARTNET,
    FROM_RDM,
    FROM_UI,
} origin_t;

typedef struct {
    bool active;
    int8_t pct;
    uint16_t app_end;
    uint32_t last_ms;
} tube_t;

struct pm_node {
    pm_node_ops_t ops;
    pm_uid_t uid;
    uint8_t num_ports;
    pm_artnet_node_t *artnet;
    pm_rdm_responder_t *resp;

    bool identify;
    uint32_t live_ms;
    bool submitted;
    uint32_t last_submit_ms;
    tube_t tube[PM_NUM_PORTS];

    pm_settings_t scratch;                 /* settings copy (config_get) */
    uint8_t wire[PM_RDM_MAX_MSG_LEN];      /* ArtRdm payload with start code */
    uint8_t resp_buf[PM_RDM_MAX_MSG_LEN];  /* responder answer */
    uint8_t frame[512];                    /* input frame */
};

/* ------------------------------------------------------------------------------------------------ helpers */

static uint32_t now_ms(const pm_node_t *n) { return n->ops.now_ms ? n->ops.now_ms(n->ops.ctx) : 0; }

static void ui(pm_node_t *n, pm_node_ui_ev_type_t type, pm_port_idx_t port, int32_t value)
{
    if (n->ops.ui_event == NULL) return;
    const pm_node_ui_event_t ev = {.type = type, .port = port, .value = value};
    n->ops.ui_event(n->ops.ctx, &ev);
}

static void config_get(pm_node_t *n, pm_settings_t *out)
{
    if (n->ops.config_get) {
        n->ops.config_get(n->ops.ctx, out);
    } else {
        pm_settings_defaults(out);
    }
}

static void apply_current_ip(pm_node_t *n)
{
    config_get(n, &n->scratch);
    if (n->ops.net_apply_ip) n->ops.net_apply_ip(n->ops.ctx, &n->scratch.ip);
}

void pm_node_port_cfg_from(const pm_settings_t *s, pm_port_idx_t i, pm_node_port_cfg_t *out)
{
    const pm_port_settings_t *p = &s->port[i];
    memset(out, 0, sizeof(*out));
    out->mode = !p->enabled ? PM_NODE_PORT_OFF
                            : (p->direction == PM_PORT_DIR_INPUT ? PM_NODE_PORT_INPUT : PM_NODE_PORT_OUTPUT);
    out->rdm_enabled = p->rdm_enabled;
    out->rdm_bg_discovery = p->rdm_bg_discovery;
    out->rdm_bg_interval_ms = (uint32_t)s->rdm_bg_interval_s * 1000u;
    out->break_us = p->dmx_break_us;
    out->mab_us = p->dmx_mab_us;
    out->rate_hz = p->dmx_rate_hz;
    out->continuous = p->output_style == PM_OUTPUT_CONTINUOUS;
    out->mbs_us = p->dmx_mbs_us;
}

void pm_node_apply_masked(pm_settings_t *dst, const pm_settings_t *src, pm_change_mask_t mask)
{
    if (mask & PM_CHG_NAMES) {
        memcpy(dst->short_name, src->short_name, sizeof(dst->short_name));
        memcpy(dst->long_name, src->long_name, sizeof(dst->long_name));
    }
    if (mask & PM_CHG_IP) dst->ip = src->ip;
    if (mask & PM_CHG_RDM_INTERVAL) dst->rdm_bg_interval_s = src->rdm_bg_interval_s;
    if (mask & PM_CHG_DISPLAY) {
        dst->display_level = src->display_level;
        dst->display_dim_timeout_s = src->display_dim_timeout_s;
        dst->address_format = src->address_format;
    }
    if (mask & PM_CHG_PANEL_LOCK) dst->panel_lock = src->panel_lock;
    if (mask & PM_CHG_HW_REV) dst->hw_revision_override = src->hw_revision_override;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        pm_port_settings_t *d = &dst->port[i];
        const pm_port_settings_t *s = &src->port[i];
        if (mask & PM_CHG_PORT(PM_CHG_P_ENABLED, i)) d->enabled = s->enabled;
        if (mask & PM_CHG_PORT(PM_CHG_P_DIRECTION, i)) d->direction = s->direction;
        if (mask & PM_CHG_PORT(PM_CHG_P_ADDRESS, i)) d->address = s->address;
        if (mask & PM_CHG_PORT(PM_CHG_P_MERGE, i)) d->merge = s->merge;
        if (mask & PM_CHG_PORT(PM_CHG_P_RDM, i)) {
            d->rdm_enabled = s->rdm_enabled;
            d->rdm_bg_discovery = s->rdm_bg_discovery;
        }
        if (mask & PM_CHG_PORT(PM_CHG_P_OUTPUT, i)) {
            d->output_style = s->output_style;
            d->failsafe = s->failsafe;
            d->scene_len = s->scene_len;
            memcpy(d->scene, s->scene, sizeof(d->scene));
        }
        if (mask & PM_CHG_PORT(PM_CHG_P_TIMING, i)) {
            d->dmx_break_us = s->dmx_break_us;
            d->dmx_mab_us = s->dmx_mab_us;
            d->dmx_rate_hz = s->dmx_rate_hz;
            d->dmx_mbs_us = s->dmx_mbs_us;
        }
    }
}

typedef struct {
    const pm_settings_t *s;
    pm_change_mask_t mask;
} masked_t;

static void modify_masked(void *fctx, pm_settings_t *s)
{
    const masked_t *m = fctx;
    pm_node_apply_masked(s, m->s, m->mask);
}

/** Write the `mask` fields of `s` (a protocol stack's edited copy) atomically into the store. */
static pm_err_t store_masked(pm_node_t *n, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src)
{
    if (n->ops.config_modify == NULL) return PM_ERR_STATE;
    masked_t m = {.s = s, .mask = mask};
    return n->ops.config_modify(n->ops.ctx, modify_masked, &m, src);
}

/* ------------------------------------------------------------------------------------------------ identify */

/** Identify changed by `from`: mirror it to the other two, never back to the origin (no loops). */
static void set_identify(pm_node_t *n, bool on, origin_t from)
{
    bool changed = n->identify != on;
    n->identify = on;
    if (from != FROM_RDM) pm_rdm_responder_set_identify(n->resp, on);
    if (from != FROM_ARTNET) {
        uint8_t led = pm_artnet_indicator(n->artnet);
        if (on) {
            led = PM_AC_LED_LOCATE;
        } else if (led == PM_AC_LED_LOCATE) {
            led = PM_AC_LED_NORMAL; /* identify off does not end a Mute */
        }
        pm_artnet_set_indicator(n->artnet, led);
    }
    if (from != FROM_UI && changed) ui(n, PM_NODE_UI_IDENTIFY, 0, on ? 1 : 0);
}

bool pm_node_identify(const pm_node_t *n) { return n != NULL && n->identify; }

void pm_node_ui_identify(pm_node_t *n, bool on)
{
    if (n == NULL) return;
    set_identify(n, on, FROM_UI);
}

/* ------------------------------------------------------------------------------------------------ tube updates */

static void tube_emit(pm_node_t *n, pm_port_idx_t port, int8_t pct)
{
    tube_t *t = &n->tube[port];
    if (t->pct == pct && t->active == (pct >= 0)) return;
    t->pct = pct;
    t->active = pct >= 0;
    ui(n, PM_NODE_UI_TUBE_UPDATE, port, pct);
}

/** Forwarded ArtRdm request on `port`: ENTER_BOOTLOADER starts, BOOT_WRITE_PAGE moves, BOOT_FINISH ends. */
static void tube_request(pm_node_t *n, pm_port_idx_t port, const pm_rdm_msg_t *m)
{
    if (port >= PM_NUM_PORTS) return;
    tube_t *t = &n->tube[port];
    if (m->cc == PM_RDM_CC_SET) {
        switch (m->pid) {
        case PM_PID_ENTER_BOOTLOADER:
            t->last_ms = now_ms(n);
            tube_emit(n, port, 0);
            return;
        case PM_PID_BOOT_WRITE_PAGE: {
            if (m->pdl < 2) return;
            uint32_t end = t->app_end ? t->app_end : PM_NODE_TUBE_APP_END_DEFAULT;
            uint32_t done = (uint32_t)pm_get_be16(m->pd) + PM_BOOT_RDM_PAGE_SIZE;
            uint32_t pct = done * 100u / end;
            t->last_ms = now_ms(n);
            tube_emit(n, port, (int8_t)(pct > 99u ? 99u : pct)); /* 100 % = BOOT_FINISH */
            return;
        }
        case PM_PID_BOOT_FINISH:
            if (t->active) tube_emit(n, port, -1);
            return;
        default:
            break;
        }
    }
    if (t->active && (m->pid == PM_PID_BOOT_INFO || m->pid == PM_PID_BOOT_APP_CRC)) t->last_ms = now_ms(n);
}

/** Response from the line: BOOT_INFO carries app_end (u16 at offset 2). */
static void tube_response(pm_node_t *n, pm_port_idx_t port, const uint8_t *wire, uint16_t len)
{
    if (port >= PM_NUM_PORTS) return;
    pm_rdm_msg_t m;
    if (pm_rdm_decode(wire, len, &m) != PM_OK) return;
    if (m.cc != PM_RDM_CC_GET_RESPONSE || m.pid != PM_PID_BOOT_INFO || m.port_or_resp != PM_RDM_RESP_ACK ||
        m.pdl < 4) {
        return;
    }
    uint16_t end = pm_get_be16(m.pd + 2);
    if (end >= PM_BOOT_RDM_PAGE_SIZE) n->tube[port].app_end = end;
}

/* ------------------------------------------------------------------------------------------------ RDM routing */

static void reply(pm_node_t *n, origin_t origin, uint32_t token, const uint8_t *wire, size_t len)
{
    if (len < 2) return;
    if (origin == FROM_ARTNET) {
        pm_artnet_rdm_response(n->artnet, token, wire + 1, len - 1u); /* ArtRdm carries no start code */
        return;
    }
    if (n->ops.ui_event == NULL) return;
    const pm_node_ui_event_t ev = {.type = PM_NODE_UI_RDM_RESPONSE, .token = token & ~PM_NODE_UI_TOKEN_FLAG,
                                   .len = (uint16_t)len, .data = wire};
    n->ops.ui_event(n->ops.ctx, &ev);
}

/**
 * Route one request (wire bytes incl. start code; `token` already tagged for the line). Node UID -> responder only;
 * a broadcast that includes the node -> responder and line; everything else -> line of `port`.
 */
static pm_err_t route(pm_node_t *n, pm_port_idx_t port, uint32_t token, const uint8_t *wire, size_t len,
                      origin_t origin, pm_rdm_msg_t *m)
{
    if (pm_rdm_decode(wire, len, m) != PM_OK) return PM_ERR_BAD_FORMAT;
    bool handled = false;
    if (pm_uid_addresses(&m->dest, &n->uid)) {
        int r = pm_rdm_responder_handle(n->resp, wire, len, n->resp_buf, sizeof(n->resp_buf));
        if (r > 1) reply(n, origin, token, n->resp_buf, (size_t)r);
        if (pm_uid_equal(&m->dest, &n->uid)) return PM_OK; /* only for us: never on the line */
        handled = true;
    }
    if (origin == FROM_ARTNET) tube_request(n, port, m);
    pm_err_t e = n->ops.port_rdm_submit ? n->ops.port_rdm_submit(n->ops.ctx, port, token, wire, (uint16_t)len)
                                        : PM_ERR_STATE;
    if (e == PM_OK) {
        n->submitted = true;
        n->last_submit_ms = now_ms(n);
    }
    if (e != PM_OK && handled) return PM_OK; /* the broadcast reached the responder; the line was full/off */
    return e;
}

pm_err_t pm_node_ui_rdm_request(pm_node_t *n, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    if (n == NULL || data == NULL || len < 2 || len > PM_RDM_MAX_MSG_LEN || port >= n->num_ports ||
        (token & PM_NODE_UI_TOKEN_FLAG)) {
        return PM_ERR_INVALID_ARG;
    }
    pm_rdm_msg_t m;
    pm_err_t e = route(n, port, token | PM_NODE_UI_TOKEN_FLAG, data, len, FROM_UI, &m);
    if (e == PM_OK && m.cc == PM_RDM_CC_SET && !pm_uid_equal(&m.dest, &n->uid)) {
        pm_rdm_responder_local_set(n->resp, port, &m.dest, m.sub_device, m.pid);
    }
    return e;
}

/* ------------------------------------------------------------------------------------------------ pm_artnet ops */

static uint32_t an_now(void *ctx) { return now_ms(ctx); }

static uint32_t an_random(void *ctx)
{
    pm_node_t *n = ctx;
    return n->ops.random ? n->ops.random(n->ops.ctx) : 0;
}

static pm_err_t an_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    pm_node_t *n = ctx;
    return n->ops.send_udp ? n->ops.send_udp(n->ops.ctx, ip, port, data, len) : PM_ERR_IO;
}

static void an_net_info(void *ctx, pm_artnet_net_info_t *out)
{
    pm_node_t *n = ctx;
    memset(out, 0, sizeof(*out));
    if (n->ops.net_info) n->ops.net_info(n->ops.ctx, out);
}

static pm_err_t an_settings_apply(void *ctx, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src)
{
    return store_masked(ctx, s, mask, src);
}

/** ArtIpProg: after the reply. */
static void an_ip_apply(void *ctx) { apply_current_ip(ctx); }

static void an_port_output(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    pm_node_t *n = ctx;
    if (n->ops.port_output) n->ops.port_output(n->ops.ctx, port, data, len, defer);
}

static void an_port_commit(void *ctx)
{
    pm_node_t *n = ctx;
    if (n->ops.port_commit) n->ops.port_commit(n->ops.ctx);
}

static void an_port_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out)
{
    pm_node_t *n = ctx;
    if (n->ops.port_status) n->ops.port_status(n->ops.ctx, port, out);
}

static void an_port_clear(void *ctx, pm_port_idx_t port)
{
    pm_node_t *n = ctx;
    if (n->ops.port_clear) n->ops.port_clear(n->ops.ctx, port);
}

static uint16_t an_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    pm_node_t *n = ctx;
    return n->ops.tod_get ? n->ops.tod_get(n->ops.ctx, port, out, cap) : 0;
}

static void an_tod_control(void *ctx, pm_port_idx_t port, uint8_t command)
{
    pm_node_t *n = ctx;
    if (n->ops.tod_control) n->ops.tod_control(n->ops.ctx, port, command);
}

static pm_err_t an_rdm_request(void *ctx, const pm_artnet_rdm_request_t *req)
{
    pm_node_t *n = ctx;
    if (req->rdm_len + 1u > sizeof(n->wire)) return PM_ERR_INVALID_ARG;
    n->wire[0] = PM_RDM_START_CODE;
    memcpy(n->wire + 1, req->rdm, req->rdm_len);
    pm_rdm_msg_t m;
    return route(n, req->port, req->token & PM_ARTNET_RDM_TOKEN_MASK, n->wire, req->rdm_len + 1u, FROM_ARTNET, &m);
}

static uint8_t an_fifo_avail(void *ctx, pm_port_idx_t port)
{
    pm_node_t *n = ctx;
    return n->ops.port_rdm_queue_free ? n->ops.port_rdm_queue_free(n->ops.ctx, port) : 0;
}

static uint8_t an_fifo_max(void *ctx, pm_port_idx_t port)
{
    pm_node_t *n = ctx;
    return n->ops.port_rdm_queue_len ? n->ops.port_rdm_queue_len(n->ops.ctx, port) : (uint8_t)PM_RDM_QUEUE_LEN;
}

/** ArtAddress LED command. */
static void an_indicator(void *ctx, uint8_t cmd)
{
    pm_node_t *n = ctx;
    ui(n, PM_NODE_UI_LED_MODE, 0, cmd);
    set_identify(n, cmd == PM_AC_LED_LOCATE, FROM_ARTNET);
}

/* ------------------------------------------------------------------------------------------------ responder ops */

static void rr_settings_get(void *ctx, pm_settings_t *out) { config_get(ctx, out); }

static pm_err_t rr_settings_apply(void *ctx, const pm_settings_t *s, pm_change_mask_t mask)
{
    return store_masked(ctx, s, mask, PM_SRC_RDM);
}

static void rr_net_info(void *ctx, pm_rdm_net_info_t *out)
{
    pm_node_t *n = ctx;
    pm_artnet_net_info_t st;
    an_net_info(n, &st);
    memset(out, 0, sizeof(*out));
    out->ip = st.ip;
    out->mask = st.mask;
    out->gateway = st.gateway;
    out->dhcp_active = st.dhcp_active;
    memcpy(out->mac, st.mac, 6);
}

/** E1.37-2 INTERFACE_APPLY_CONFIGURATION: the stored IP settings become active now. */
static void rr_net_apply(void *ctx) { apply_current_ip(ctx); }

static void rr_net_renew(void *ctx)
{
    pm_node_t *n = ctx;
    if (n->ops.net_renew_dhcp) n->ops.net_renew_dhcp(n->ops.ctx);
}

static void rr_identify(void *ctx, bool on) { set_identify(ctx, on, FROM_RDM); }

static void rr_reset(void *ctx, uint8_t kind)
{
    pm_node_t *n = ctx;
    if (n->ops.reset) n->ops.reset(n->ops.ctx, kind);
}

/** FACTORY_DEFAULTS: a factory reset is not an E1.37-2 staged change, the default IP settings apply at once. */
static void rr_factory(void *ctx)
{
    pm_node_t *n = ctx;
    if (n->ops.config_factory_reset == NULL) return;
    config_get(n, &n->scratch);
    pm_ip_settings_t before = n->scratch.ip;
    n->ops.config_factory_reset(n->ops.ctx, PM_SRC_RDM);
    config_get(n, &n->scratch);
    const pm_ip_settings_t *ip = &n->scratch.ip;
    if (ip->dhcp != before.dhcp || ip->static_ip != before.static_ip || ip->static_mask != before.static_mask ||
        ip->gateway != before.gateway) {
        if (n->ops.net_apply_ip) n->ops.net_apply_ip(n->ops.ctx, ip);
    }
}

static pm_err_t rr_catch_start(void *ctx, pm_port_idx_t port, const pm_uid_t *target)
{
    pm_node_t *n = ctx;
    return n->ops.boot_catch_start ? n->ops.boot_catch_start(n->ops.ctx, port, target) : PM_ERR_STATE;
}

static void rr_catch_abort(void *ctx)
{
    pm_node_t *n = ctx;
    if (n->ops.boot_catch_abort) n->ops.boot_catch_abort(n->ops.ctx);
}

static void rr_catch_status(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    pm_node_t *n = ctx;
    if (n->ops.boot_catch_status) {
        n->ops.boot_catch_status(n->ops.ctx, port, out);
    } else {
        *port = 0;
        memset(out, 0, sizeof(*out));
    }
}

/* ------------------------------------------------------------------------------------------------ public API */

size_t pm_node_size(void)
{
    return ALIGN_UP(sizeof(struct pm_node)) + ALIGN_UP(pm_artnet_node_size()) + pm_rdm_responder_size();
}

pm_node_t *pm_node_init(void *mem, const pm_node_identity_t *id, const pm_node_ops_t *ops,
                        const pm_settings_t *settings)
{
    if (mem == NULL || id == NULL || ops == NULL || settings == NULL || id->artnet.num_ports > PM_NUM_PORTS ||
        !pm_uid_equal(&id->artnet.uid, &id->responder.uid)) {
        return NULL;
    }
    pm_node_t *n = mem;
    memset(n, 0, sizeof(*n));
    n->ops = *ops;
    n->uid = id->artnet.uid;
    n->num_ports = id->artnet.num_ports;
    uint8_t *base = mem;
    void *an_mem = base + ALIGN_UP(sizeof(struct pm_node));
    void *rr_mem = base + ALIGN_UP(sizeof(struct pm_node)) + ALIGN_UP(pm_artnet_node_size());

    const pm_rdm_responder_ops_t rr_ops = {
        .ctx = n, .now_ms = an_now, .settings_get = rr_settings_get, .settings_apply = rr_settings_apply,
        .net_info = rr_net_info, .net_apply = rr_net_apply, .net_renew_dhcp = rr_net_renew,
        .identify = rr_identify, .reset = rr_reset, .factory_defaults = rr_factory,
        .boot_catch_start = rr_catch_start, .boot_catch_abort = rr_catch_abort,
        .boot_catch_status = rr_catch_status, .fw_sink = ops->fw_sink,
    };
    n->resp = pm_rdm_responder_init(rr_mem, &id->responder, &rr_ops);

    const pm_artnet_ops_t an_ops = {
        .ctx = n, .now_ms = an_now, .random = an_random, .send_udp = an_send, .net_info = an_net_info,
        .settings_apply = an_settings_apply, .ip_apply = an_ip_apply, .port_output = an_port_output,
        .port_commit = an_port_commit, .port_status = an_port_status, .port_clear = an_port_clear,
        .tod_get = an_tod_get, .tod_control = an_tod_control, .rdm_request = an_rdm_request,
        .rdm_fifo_avail = an_fifo_avail, .rdm_fifo_max = an_fifo_max, .indicator = an_indicator,
        .fw_sink = ops->fw_sink,
    };
    n->artnet = pm_artnet_node_init(an_mem, &id->artnet, &an_ops, settings);
    if (n->artnet == NULL || n->resp == NULL) return NULL;
    n->live_ms = now_ms(n);
    return n;
}

pm_artnet_node_t *pm_node_artnet(pm_node_t *n) { return n ? n->artnet : NULL; }

pm_rdm_responder_t *pm_node_responder(pm_node_t *n) { return n ? n->resp : NULL; }

void pm_node_handle_packet(pm_node_t *n, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, const uint8_t *data,
                           size_t len)
{
    if (n == NULL) return;
    pm_artnet_handle_packet_ex(n->artnet, src_ip, src_port, dst_ip, data, len);
}

void pm_node_tick(pm_node_t *n)
{
    if (n == NULL) return;
    pm_artnet_tick(n->artnet);
    pm_rdm_responder_poll(n->resp);
    uint32_t now = now_ms(n);
    for (pm_port_idx_t p = 0; p < PM_NUM_PORTS; p++) {
        tube_t *t = &n->tube[p];
        if (t->active && (uint32_t)(now - t->last_ms) >= PM_NODE_TUBE_UPDATE_TIMEOUT_MS) tube_emit(n, p, -1);
    }
    if ((uint32_t)(now - n->live_ms) >= PM_NODE_LIVE_PERIOD_MS) {
        n->live_ms = now;
        if (n->ops.port_live) {
            pm_artnet_port_live_t live[PM_NUM_PORTS];
            for (pm_port_idx_t p = 0; p < PM_NUM_PORTS; p++) pm_artnet_port_live(n->artnet, p, &live[p]);
            n->ops.port_live(n->ops.ctx, live);
        }
    }
}

void pm_node_port_event(pm_node_t *n, const pm_node_port_event_t *ev)
{
    if (n == NULL || ev == NULL || ev->port >= PM_NUM_PORTS) return;
    switch (ev->type) {
    case PM_NODE_PEV_RDM_RESPONSE:
        if (ev->data == NULL || ev->len < 2) break;
        if (ev->token & PM_NODE_UI_TOKEN_FLAG) {
            reply(n, FROM_UI, ev->token, ev->data, ev->len);
        } else {
            tube_response(n, ev->port, ev->data, ev->len);
            pm_artnet_rdm_response(n->artnet, ev->token, ev->data + 1, ev->len - 1u);
        }
        break;
    case PM_NODE_PEV_TOD_CHANGED:
        pm_artnet_tod_changed(n->artnet, ev->port);
        ui(n, PM_NODE_UI_TOD_CHANGED, ev->port, 0);
        break;
    case PM_NODE_PEV_INPUT_FRAME: {
        uint16_t len = n->ops.port_input_read ? n->ops.port_input_read(n->ops.ctx, ev->port, n->frame,
                                                                       sizeof(n->frame))
                                              : 0;
        if (len > 0) pm_artnet_input_frame(n->artnet, ev->port, n->frame, len);
        break;
    }
    case PM_NODE_PEV_INPUT_SIGNAL:
        pm_artnet_port_status_changed(n->artnet, ev->port);
        break;
    case PM_NODE_PEV_BOOT_CATCH:
        pm_rdm_responder_boot_catch_event(n->resp, ev->port, (uint8_t)ev->value);
        ui(n, PM_NODE_UI_BOOT_CATCH, ev->port, ev->value);
        break;
    case PM_NODE_PEV_ERROR:
        ui(n, PM_NODE_UI_ERROR, ev->port, ev->value);
        break;
    }
}

void pm_node_settings_changed(pm_node_t *n, pm_change_mask_t mask, pm_change_source_t src)
{
    if (n == NULL || mask == 0) return;
    config_get(n, &n->scratch); /* last writer wins: the current state is what counts */
    for (pm_port_idx_t p = 0; p < n->num_ports; p++) {
        if ((mask & PM_NODE_PORT_CFG_BITS(p)) && n->ops.port_configure) {
            pm_node_port_cfg_t c;
            pm_node_port_cfg_from(&n->scratch, p, &c);
            n->ops.port_configure(n->ops.ctx, p, &c);
        }
    }
    /* ArtIpProg applies after its reply (ip_apply), E1.37-2 with INTERFACE_APPLY_CONFIGURATION (net_apply) */
    if ((mask & PM_CHG_IP) && src != PM_SRC_IPPROG && src != PM_SRC_RDM && n->ops.net_apply_ip) {
        n->ops.net_apply_ip(n->ops.ctx, &n->scratch.ip);
    }
    pm_rdm_responder_settings_changed(n->resp, mask, src);
    pm_artnet_settings_changed(n->artnet, &n->scratch, mask);
    ui(n, PM_NODE_UI_SETTINGS_CHANGED, 0, (int32_t)src);
}

void pm_node_net_changed(pm_node_t *n)
{
    if (n == NULL) return;
    pm_artnet_net_changed(n->artnet);
}

void pm_node_fw_progress(pm_node_t *n, const pm_fw_status_t *st)
{
    if (n == NULL || st == NULL) return;
    int32_t pct;
    if (st->state == PM_FW_STATE_FAILED) {
        pm_artnet_set_node_report(n->artnet, RC_FIRMWARE_FAIL, "Firmware update failed");
        pct = -1;
    } else if (st->total > 0 && st->state != PM_FW_STATE_IDLE) {
        pct = st->state == PM_FW_STATE_READY ? 100 : (int32_t)((uint64_t)st->received * 100u / st->total);
    } else {
        return; /* idle */
    }
    ui(n, PM_NODE_UI_FW_PROGRESS, 0, pct);
}

bool pm_node_busy(const pm_node_t *n)
{
    return n != NULL && n->submitted && (uint32_t)(now_ms(n) - n->last_submit_ms) < PM_NODE_BUSY_MS;
}
