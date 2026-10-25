/* pm_node glue tests: fake platform (RAM config with deferred notifications, fake port engine, recorded UDP and
 * panel events), controlled time. */
#include <stdalign.h>
#include <stddef.h>

#include "pm_node.h"
#include "pm_pids.h"
#include "test.h"

void test_node(void);

#define IP(a, b, c, d) ((uint32_t)(((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d)))
#define NODE_IP IP(2, 0, 0, 10)
#define CTL IP(2, 0, 0, 100)
#define MAX_SENT 64
#define MAX_UI 64

typedef struct {
    uint32_t ip;
    uint16_t op;
    size_t len;
    uint8_t data[1300];
} sent_t;

typedef struct {
    pm_node_ui_event_t ev;
    uint8_t data[PM_RDM_MAX_MSG_LEN];
} ui_rec_t;

static struct {
    uint32_t now;
    /* config store */
    pm_settings_t cfg;
    pm_change_mask_t pend[PM_SRC_SYSTEM + 1];
    unsigned modifies, factory_resets;
    /* network */
    pm_artnet_net_info_t net;
    unsigned ip_applies, renews;
    pm_ip_settings_t last_ip;
    sent_t sent[MAX_SENT];
    unsigned nsent;
    /* port engine */
    unsigned configures[PM_NUM_PORTS];
    pm_node_port_cfg_t port_cfg[PM_NUM_PORTS];
    unsigned submits;
    pm_port_idx_t sub_port;
    uint32_t sub_token;
    uint16_t sub_len;
    uint8_t sub_buf[PM_RDM_MAX_MSG_LEN];
    pm_err_t submit_result;
    uint8_t qfree;
    unsigned input_reads;
    uint8_t input[512];
    uint16_t input_len;
    unsigned outputs[PM_NUM_PORTS];
    /* system */
    unsigned resets;
    uint8_t reset_kind;
    ui_rec_t ui[MAX_UI];
    unsigned nui;
    pm_artnet_port_live_t live[PM_NUM_PORTS];
    unsigned live_calls;
} F;

static alignas(max_align_t) uint8_t MEM[64 * 1024];
static pm_node_t *N;

static const pm_uid_t NODE_UID = {{0x7F, 0xF0, 0x80, 0x12, 0x34, 0x56}};
static const pm_uid_t DEV_UID = {{0x7F, 0xF0, 0x00, 0x00, 0x00, 0x42}};
static const pm_uid_t CTL_UID = {{0x7F, 0xF0, 0x7F, 0x00, 0x00, 0x01}};
static const pm_uid_t ALL_UID = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

/* ------------------------------------------------------------------------------------------------ fakes */

static uint32_t f_now(void *ctx) { (void)ctx; return F.now; }
static uint32_t f_random(void *ctx) { (void)ctx; return 0; }
static void f_config_get(void *ctx, pm_settings_t *out) { (void)ctx; *out = F.cfg; }

static void store(const pm_settings_t *next, pm_change_source_t src)
{
    static pm_settings_t t;
    t = *next;
    pm_settings_sanitize(&t);
    F.pend[src] |= pm_settings_diff(&F.cfg, &t);
    F.cfg = t;
}

static pm_err_t f_config_modify(void *ctx, pm_node_modify_fn fn, void *fctx, pm_change_source_t src)
{
    (void)ctx;
    static pm_settings_t t;
    t = F.cfg;
    fn(fctx, &t);
    store(&t, src);
    F.modifies++;
    return PM_OK;
}

static pm_err_t f_factory(void *ctx, pm_change_source_t src)
{
    (void)ctx;
    static pm_settings_t t;
    pm_settings_defaults(&t);
    t.hw_revision_override = F.cfg.hw_revision_override;
    store(&t, src);
    F.factory_resets++;
    return PM_OK;
}

static pm_err_t f_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    (void)ctx; (void)port;
    if (F.nsent >= MAX_SENT || len > sizeof(F.sent[0].data)) return PM_ERR_FULL;
    sent_t *s = &F.sent[F.nsent++];
    s->ip = ip;
    s->len = len;
    memcpy(s->data, data, len);
    s->op = len >= 10 ? pm_get_le16(data + 8) : 0;
    return PM_OK;
}

static void f_net_info(void *ctx, pm_artnet_net_info_t *out) { (void)ctx; *out = F.net; }

static void f_apply_ip(void *ctx, const pm_ip_settings_t *ip)
{
    (void)ctx;
    F.ip_applies++;
    F.last_ip = *ip;
}

static void f_renew(void *ctx) { (void)ctx; F.renews++; }

static pm_err_t f_configure(void *ctx, pm_port_idx_t port, const pm_node_port_cfg_t *cfg)
{
    (void)ctx;
    F.configures[port]++;
    F.port_cfg[port] = *cfg;
    return PM_OK;
}

static void f_output(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    (void)ctx; (void)data; (void)len; (void)defer;
    F.outputs[port]++;
}

static void f_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out)
{
    (void)ctx; (void)port;
    *out = (pm_artnet_port_status_t){.output_active = true, .input_signal = true};
}

static uint16_t f_input_read(void *ctx, pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    (void)ctx; (void)port;
    F.input_reads++;
    uint16_t n = F.input_len < cap ? F.input_len : cap;
    memcpy(out, F.input, n);
    return n;
}

static pm_err_t f_submit(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *wire, uint16_t len)
{
    (void)ctx;
    if (F.submit_result != PM_OK) return F.submit_result;
    F.submits++;
    F.sub_port = port;
    F.sub_token = token;
    F.sub_len = len;
    memcpy(F.sub_buf, wire, len);
    return PM_OK;
}

static uint8_t f_qfree(void *ctx, pm_port_idx_t port) { (void)ctx; (void)port; return F.qfree; }

static void f_reset(void *ctx, uint8_t kind)
{
    (void)ctx;
    F.resets++;
    F.reset_kind = kind;
}

static void f_ui(void *ctx, const pm_node_ui_event_t *ev)
{
    (void)ctx;
    if (F.nui >= MAX_UI) return;
    ui_rec_t *r = &F.ui[F.nui++];
    r->ev = *ev;
    if (ev->data && ev->len <= sizeof(r->data)) memcpy(r->data, ev->data, ev->len);
    r->ev.data = r->data;
}

static void f_live(void *ctx, const pm_artnet_port_live_t *live)
{
    (void)ctx;
    memcpy(F.live, live, sizeof(F.live));
    F.live_calls++;
}

/** Deliver the pending notifications like main.c does (network task, per source). */
static void flush(void)
{
    for (int src = 0; src <= PM_SRC_SYSTEM; src++) {
        pm_change_mask_t m = F.pend[src];
        F.pend[src] = 0;
        if (m) pm_node_settings_changed(N, m, (pm_change_source_t)src);
    }
}

static void setup(void)
{
    memset(&F, 0, sizeof(F));
    F.now = 5000;
    F.qfree = 8;
    pm_settings_defaults(&F.cfg);
    F.net = (pm_artnet_net_info_t){.ip = NODE_IP, .mask = 0xFF000000u, .mac = {2, 0, 0, 0x12, 0x34, 0x56},
                                   .link_up = true};
    const pm_node_identity_t id = {
        .artnet = {.esta_man = PM_ESTA_MANUFACTURER, .oem = PM_ARTNET_OEM_NODE, .vers_info = 0x0001,
                   .uid = NODE_UID, .num_ports = 4},
        .responder = {.uid = NODE_UID, .model_id = PM_MODEL_NODE, .sw_version_id = 0x000100,
                      .sw_version_label = "0.1.0", .model_description = "Pixelmapper Node",
                      .manufacturer_label = "Prototype", .hw_revision = 0},
    };
    const pm_node_ops_t ops = {
        .now_ms = f_now, .random = f_random, .config_get = f_config_get, .config_modify = f_config_modify,
        .config_factory_reset = f_factory, .send_udp = f_send, .net_info = f_net_info, .net_apply_ip = f_apply_ip,
        .net_renew_dhcp = f_renew, .port_configure = f_configure, .port_output = f_output,
        .port_status = f_status, .port_input_read = f_input_read, .port_rdm_submit = f_submit,
        .port_rdm_queue_free = f_qfree, .reset = f_reset, .ui_event = f_ui, .port_live = f_live,
    };
    CHECK(pm_node_size() <= sizeof(MEM));
    N = pm_node_init(MEM, &id, &ops, &F.cfg);
    CHECK(N != NULL);
}

static void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        F.now += 10;
        pm_node_tick(N);
    }
}

static uint8_t PKT[1300];

/** RDM request wire bytes (with start code). */
static int mk_rdm(uint8_t *buf, pm_uid_t dest, uint8_t cc, uint16_t pid, const uint8_t *pd, uint8_t pdl)
{
    const pm_rdm_msg_t m = {.dest = dest, .src = CTL_UID, .tn = 7, .port_or_resp = 1, .cc = cc, .pid = pid,
                            .pdl = pdl, .pd = pd};
    return pm_rdm_encode(&m, buf, PM_RDM_MAX_MSG_LEN);
}

/** ArtRdm from CTL to port index `port` (default Port-Address = port + 1). */
static void art_rdm(pm_port_idx_t port, pm_uid_t dest, uint8_t cc, uint16_t pid, const uint8_t *pd, uint8_t pdl)
{
    uint8_t wire[PM_RDM_MAX_MSG_LEN];
    int n = mk_rdm(wire, dest, cc, pid, pd, pdl);
    CHECK(n > 0);
    const pm_artrdm_t r = {.rdm_ver = 1, .net = 0, .command = 0, .address = (uint8_t)(port + 1u), .rdm = wire + 1,
                           .rdm_len = (size_t)n - 1u};
    int len = pm_artrdm_encode(&r, PKT, sizeof(PKT));
    pm_node_handle_packet(N, CTL, PM_ARTNET_PORT, NODE_IP, PKT, (size_t)len);
}

static void art_address(uint8_t bind, uint8_t command, uint8_t sw_out)
{
    pm_artaddress_t a;
    memset(&a, 0, sizeof(a));
    a.bind_index = bind;
    a.net_switch = 0x7F;
    a.sub_switch = 0x7F;
    memset(a.sw_in, 0x7F, 4);
    memset(a.sw_out, 0x7F, 4);
    a.sw_out[0] = sw_out;
    a.acn_priority = 255;
    a.command = command;
    int len = pm_artaddress_encode(&a, PKT, sizeof(PKT));
    pm_node_handle_packet(N, CTL, PM_ARTNET_PORT, NODE_IP, PKT, (size_t)len);
}

/** Decoded RDM message of the n-th ArtRdm sent to CTL (pd points into F.sent). */
static bool sent_rdm(unsigned nth, pm_rdm_msg_t *m)
{
    for (unsigned i = 0; i < F.nsent; i++) {
        if (F.sent[i].op != PM_OP_RDM || F.sent[i].ip != CTL || nth-- != 0) continue;
        pm_artrdm_t r;
        if (pm_artrdm_decode(F.sent[i].data, F.sent[i].len, &r) != PM_OK) return false;
        return pm_rdm_decode_nosc(r.rdm, r.rdm_len, m) == PM_OK;
    }
    return false;
}

static unsigned count_sent(uint16_t op)
{
    unsigned c = 0;
    for (unsigned i = 0; i < F.nsent; i++) c += F.sent[i].op == op;
    return c;
}

static unsigned count_ui(pm_node_ui_ev_type_t type)
{
    unsigned c = 0;
    for (unsigned i = 0; i < F.nui; i++) c += F.ui[i].ev.type == type;
    return c;
}

static const pm_node_ui_event_t *last_ui(pm_node_ui_ev_type_t type)
{
    for (unsigned i = F.nui; i-- > 0;) {
        if (F.ui[i].ev.type == type) return &F.ui[i].ev;
    }
    return NULL;
}

/** A device answer on the line for the last submitted request. */
static void line_answer(uint8_t cc, uint16_t pid, const uint8_t *pd, uint8_t pdl)
{
    pm_rdm_msg_t req;
    CHECK_EQ(pm_rdm_decode(F.sub_buf, F.sub_len, &req), PM_OK);
    const pm_rdm_msg_t m = {.dest = req.src, .src = req.dest, .tn = req.tn, .port_or_resp = PM_RDM_RESP_ACK,
                            .cc = cc, .pid = pid, .pdl = pdl, .pd = pd};
    uint8_t wire[PM_RDM_MAX_MSG_LEN];
    int n = pm_rdm_encode(&m, wire, sizeof(wire));
    const pm_node_port_event_t ev = {.type = PM_NODE_PEV_RDM_RESPONSE, .port = F.sub_port, .token = F.sub_token,
                                     .len = (uint16_t)n, .data = wire};
    pm_node_port_event(N, &ev);
}

/* ------------------------------------------------------------------------------------------------ tests */

static void test_routing(void)
{
    g_context = "node routing";
    setup();
    pm_rdm_msg_t m;
    /* to the node UID: responder only, never on the line */
    art_rdm(1, NODE_UID, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
    CHECK_EQ(F.submits, 0);
    CHECK(sent_rdm(0, &m));
    CHECK(m.cc == PM_RDM_CC_GET_RESPONSE && m.pid == PM_RDM_PID_DEVICE_INFO && pm_uid_equal(&m.src, &NODE_UID));
    /* to a device: line of the addressed port, token with bit 31 clear */
    F.nsent = 0;
    art_rdm(2, DEV_UID, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
    CHECK_EQ(F.submits, 1);
    CHECK_EQ(F.sub_port, 2);
    CHECK(F.sub_token != 0 && (F.sub_token & PM_NODE_UI_TOKEN_FLAG) == 0);
    CHECK_EQ(F.sub_buf[0], PM_RDM_START_CODE);
    CHECK_EQ(count_sent(PM_OP_RDM), 0);
    CHECK(pm_node_busy(N));
    uint8_t info[19] = {1, 0};
    line_answer(PM_RDM_CC_GET_RESPONSE, PM_RDM_PID_DEVICE_INFO, info, sizeof(info));
    CHECK(sent_rdm(0, &m));
    CHECK(m.pid == PM_RDM_PID_DEVICE_INFO && pm_uid_equal(&m.src, &DEV_UID));
    pm_artrdm_t ar;
    if (F.nsent) {
        CHECK_EQ(pm_artrdm_decode(F.sent[0].data, F.sent[0].len, &ar), PM_OK);
        CHECK_EQ(ar.address, 3);
        CHECK_EQ(ar.fifo_max, PM_RDM_QUEUE_LEN); /* FifoMax from the port queue size */
        CHECK_EQ(ar.fifo_avail, 8);
    }
    advance(PM_NODE_BUSY_MS + 10);
    CHECK(!pm_node_busy(N));
    /* broadcast: both */
    F.nsent = 0;
    uint8_t on = 1;
    art_rdm(0, ALL_UID, PM_RDM_CC_SET, PM_RDM_PID_IDENTIFY_DEVICE, &on, 1);
    CHECK_EQ(F.submits, 2);
    CHECK(pm_rdm_responder_identify(pm_node_responder(N))); /* executed, not answered */
    CHECK_EQ(count_sent(PM_OP_RDM), 0);
    /* broadcast while the line queue is full: still reaches the responder */
    F.submit_result = PM_ERR_FULL;
    on = 0;
    art_rdm(0, ALL_UID, PM_RDM_CC_SET, PM_RDM_PID_IDENTIFY_DEVICE, &on, 1);
    CHECK(!pm_rdm_responder_identify(pm_node_responder(N)));
    /* port without RDM (input): only node requests pass (pm_artnet), nothing on the line */
    F.submit_result = PM_OK;
    F.cfg.port[3].direction = PM_PORT_DIR_INPUT;
    F.pend[PM_SRC_LOCAL_UI] |= PM_CHG_PORT(PM_CHG_P_DIRECTION, 3);
    flush();
    unsigned subs = F.submits;
    art_rdm(3, DEV_UID, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
    CHECK_EQ(F.submits, subs);
    g_context = NULL;
}

static void test_tokens(void)
{
    g_context = "node tokens";
    setup();
    /* many ArtRdm requests: every token has bit 31 clear and is unique */
    uint32_t seen[64];
    for (unsigned i = 0; i < 64; i++) {
        art_rdm(0, DEV_UID, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
        seen[i] = F.sub_token;
        CHECK((F.sub_token & PM_NODE_UI_TOKEN_FLAG) == 0);
        for (unsigned k = 0; k < i; k++) CHECK(seen[k] != seen[i]);
    }
    /* panel request: tagged on the line, answer goes to the panel with the original token */
    const uint32_t ui_token = 0x55490007u; /* PM_UI_RDM_TOKEN_TAG | 7 */
    uint8_t wire[PM_RDM_MAX_MSG_LEN];
    int n = mk_rdm(wire, DEV_UID, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
    F.nsent = 0;
    CHECK_EQ(pm_node_ui_rdm_request(N, 1, ui_token, wire, (uint16_t)n), PM_OK);
    CHECK_EQ(F.sub_token, ui_token | PM_NODE_UI_TOKEN_FLAG);
    CHECK_EQ(F.sub_port, 1);
    uint8_t info[19] = {1, 0};
    line_answer(PM_RDM_CC_GET_RESPONSE, PM_RDM_PID_DEVICE_INFO, info, sizeof(info));
    const pm_node_ui_event_t *ev = last_ui(PM_NODE_UI_RDM_RESPONSE);
    CHECK(ev != NULL);
    if (ev) {
        CHECK_EQ(ev->token, ui_token);
        CHECK_EQ(ev->data[0], PM_RDM_START_CODE);
    }
    CHECK_EQ(count_sent(PM_OP_RDM), 0); /* not to Art-Net */
    /* panel request to the node itself: answered at once */
    F.nui = 0;
    n = mk_rdm(wire, NODE_UID, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
    unsigned subs = F.submits;
    CHECK_EQ(pm_node_ui_rdm_request(N, 0, ui_token + 1, wire, (uint16_t)n), PM_OK);
    CHECK_EQ(F.submits, subs);
    ev = last_ui(PM_NODE_UI_RDM_RESPONSE);
    CHECK(ev != NULL && ev->token == ui_token + 1);
    /* bit 31 in a panel token is rejected */
    CHECK_EQ(pm_node_ui_rdm_request(N, 0, 0x80000001u, wire, (uint16_t)n), PM_ERR_INVALID_ARG);
    /* panel SET to a device -> LOCAL_RDM_SET queued message */
    uint8_t before = pm_rdm_responder_message_count(pm_node_responder(N));
    uint8_t pd[2] = {0, 5};
    n = mk_rdm(wire, DEV_UID, PM_RDM_CC_SET, PM_RDM_PID_DMX_START_ADDRESS, pd, 2);
    CHECK_EQ(pm_node_ui_rdm_request(N, 2, ui_token + 2, wire, (uint16_t)n), PM_OK);
    CHECK(pm_rdm_responder_message_count(pm_node_responder(N)) > before);
    g_context = NULL;
}

static void test_identify(void)
{
    g_context = "node identify";
    setup();
    pm_artnet_node_t *an = pm_node_artnet(N);
    pm_rdm_responder_t *rr = pm_node_responder(N);
    /* ArtAddress Locate -> RDM identify + panel */
    art_address(1, PM_AC_LED_LOCATE, 0x7F);
    CHECK(pm_rdm_responder_identify(rr));
    CHECK(pm_node_identify(N));
    CHECK(last_ui(PM_NODE_UI_LED_MODE) && last_ui(PM_NODE_UI_LED_MODE)->value == PM_AC_LED_LOCATE);
    CHECK(last_ui(PM_NODE_UI_IDENTIFY) && last_ui(PM_NODE_UI_IDENTIFY)->value == 1);
    CHECK_EQ(F.modifies, 0); /* no settings change */
    /* RDM IDENTIFY off -> indicator Normal, panel */
    F.nui = 0;
    uint8_t off = 0;
    art_rdm(0, NODE_UID, PM_RDM_CC_SET, PM_RDM_PID_IDENTIFY_DEVICE, &off, 1);
    CHECK_EQ(pm_artnet_indicator(an), PM_AC_LED_NORMAL);
    CHECK(!pm_node_identify(N));
    CHECK(last_ui(PM_NODE_UI_IDENTIFY) && last_ui(PM_NODE_UI_IDENTIFY)->value == 0);
    CHECK_EQ(count_ui(PM_NODE_UI_LED_MODE), 0); /* no ArtAddress echo */
    /* panel identify -> both, no event back to the panel */
    F.nui = 0;
    pm_node_ui_identify(N, true);
    CHECK_EQ(pm_artnet_indicator(an), PM_AC_LED_LOCATE);
    CHECK(pm_rdm_responder_identify(rr));
    CHECK_EQ(F.nui, 0);
    /* the unsolicited ArtPollReply shows Locate (Status1 bits 7-6 = 01) to a poller with Flags bit 1 */
    pm_node_ui_identify(N, false);
    const pm_artpoll_t poll = {.flags = PM_POLL_FLAG_REPLY_ON_CHANGE};
    int len = pm_artpoll_encode(&poll, PKT, sizeof(PKT));
    pm_node_handle_packet(N, CTL, PM_ARTNET_PORT, IP(2, 255, 255, 255), PKT, (size_t)len);
    advance(1100);
    F.nsent = 0;
    pm_node_ui_identify(N, true);
    advance(20);
    CHECK(count_sent(PM_OP_POLL_REPLY) >= 1);
    for (unsigned i = 0; i < F.nsent; i++) {
        pm_artpollreply_t r;
        if (F.sent[i].op == PM_OP_POLL_REPLY && pm_artpollreply_decode(F.sent[i].data, F.sent[i].len, &r) == PM_OK) {
            CHECK_EQ(r.status1 & 0xC0u, 0x40u);
        }
    }
    /* Mute stays when identify ends */
    art_address(1, PM_AC_LED_MUTE, 0x7F);
    CHECK(!pm_rdm_responder_identify(rr));
    pm_node_ui_identify(N, false);
    CHECK_EQ(pm_artnet_indicator(an), PM_AC_LED_MUTE);
    g_context = NULL;
}

static void test_settings(void)
{
    g_context = "node settings fan-out";
    setup();
    /* panel changes MBS of port 3: only that port is reconfigured */
    F.cfg.port[2].dmx_mbs_us = 20;
    F.pend[PM_SRC_LOCAL_UI] |= PM_CHG_PORT(PM_CHG_P_TIMING, 2);
    flush();
    CHECK_EQ(F.configures[2], 1);
    CHECK_EQ(F.configures[0] + F.configures[1] + F.configures[3], 0);
    CHECK_EQ(F.port_cfg[2].mbs_us, 20);
    CHECK_EQ(F.port_cfg[2].mode, PM_NODE_PORT_OUTPUT);
    CHECK(last_ui(PM_NODE_UI_SETTINGS_CHANGED) && last_ui(PM_NODE_UI_SETTINGS_CHANGED)->value == PM_SRC_LOCAL_UI);
    /* address/merge changes do not touch the port engine */
    F.cfg.port[1].address = 0x22;
    F.pend[PM_SRC_LOCAL_UI] |= PM_CHG_PORT(PM_CHG_P_ADDRESS, 1);
    flush();
    CHECK_EQ(F.configures[1], 0);
    /* RDM interval: all ports */
    F.cfg.rdm_bg_interval_s = 9;
    F.pend[PM_SRC_RDM] |= PM_CHG_RDM_INTERVAL;
    flush();
    for (unsigned p = 0; p < 4; p++) CHECK_EQ(F.port_cfg[p].rdm_bg_interval_ms, 9000);

    /* IP from the panel applies at once, from RDM only with INTERFACE_APPLY_CONFIGURATION */
    F.cfg.ip.dhcp = false;
    F.pend[PM_SRC_LOCAL_UI] |= PM_CHG_IP;
    flush();
    CHECK_EQ(F.ip_applies, 1);
    uint8_t pd[9] = {0, 0, 0, 1, 10, 0, 0, 7, 8};
    art_rdm(0, NODE_UID, PM_RDM_CC_SET, PM_RDM_PID_IPV4_STATIC_ADDRESS, pd, 9);
    CHECK_EQ(F.cfg.ip.static_ip, IP(10, 0, 0, 7));
    flush();
    CHECK_EQ(F.ip_applies, 1);
    uint8_t ifc[4] = {0, 0, 0, 1};
    art_rdm(0, NODE_UID, PM_RDM_CC_SET, PM_RDM_PID_INTERFACE_APPLY_CONFIGURATION, ifc, 4);
    CHECK_EQ(F.ip_applies, 2);
    CHECK_EQ(F.last_ip.static_ip, IP(10, 0, 0, 7));
    CHECK_EQ(F.last_ip.static_mask, 0xFF000000u);

    /* ArtIpProg: applied after its reply (ip_apply), not again by the notification */
    pm_artipprog_t ipp = {.command = PM_IPPROG_ENABLE | PM_IPPROG_PROG_IP, .ip = IP(10, 0, 0, 9)};
    int len = pm_artipprog_encode(&ipp, PKT, sizeof(PKT));
    pm_node_handle_packet(N, CTL, PM_ARTNET_PORT, NODE_IP, PKT, (size_t)len);
    CHECK_EQ(F.ip_applies, 3);
    CHECK_EQ(F.last_ip.static_ip, IP(10, 0, 0, 9));
    flush();
    CHECK_EQ(F.ip_applies, 3);

    /* RDM FACTORY_DEFAULTS: IP back to DHCP at once */
    art_rdm(0, NODE_UID, PM_RDM_CC_SET, PM_RDM_PID_FACTORY_DEFAULTS, NULL, 0);
    CHECK_EQ(F.factory_resets, 1);
    CHECK_EQ(F.ip_applies, 4);
    CHECK(F.last_ip.dhcp);
    flush();
    CHECK_EQ(F.ip_applies, 4);
    g_context = NULL;
}

/* A panel change that is stored but not yet distributed must survive an ArtAddress for another port. */
static void test_no_stale_overwrite(void)
{
    g_context = "node atomic writes";
    setup();
    F.cfg.port[0].address = 0x0010; /* panel wrote it, notification still pending */
    F.cfg.short_name[0] = 'Q';
    art_address(2, PM_AC_NONE, 0x85); /* port 2: SwOut = 5 */
    CHECK_EQ(F.modifies, 1);
    CHECK_EQ(F.cfg.port[1].address, 0x0005);
    CHECK_EQ(F.cfg.port[0].address, 0x0010);
    CHECK_EQ(F.cfg.short_name[0], 'Q');
    /* same for an RDM SET (PORT2_DMX_MBS) */
    F.cfg.display_level = 9;
    uint8_t v[2] = {0, 44};
    art_rdm(0, NODE_UID, PM_RDM_CC_SET, PM_PID_PORT_DMX_MBS(2), v, 2);
    CHECK_EQ(F.cfg.port[1].dmx_mbs_us, 44);
    CHECK_EQ(F.cfg.display_level, 9);
    CHECK_EQ(F.cfg.port[0].address, 0x0010);
    /* apply_masked copies exactly the masked fields */
    static pm_settings_t a, b;
    pm_settings_defaults(&a);
    pm_settings_defaults(&b);
    b.port[3].scene_len = 3;
    b.port[3].scene[2] = 9;
    b.port[3].failsafe = PM_FAILSAFE_SCENE;
    b.port[3].address = 77;
    b.panel_lock = true;
    pm_node_apply_masked(&a, &b, PM_CHG_PORT(PM_CHG_P_OUTPUT, 3));
    CHECK(a.port[3].scene_len == 3 && a.port[3].scene[2] == 9 && a.port[3].failsafe == PM_FAILSAFE_SCENE);
    CHECK(a.port[3].address != 77 && !a.panel_lock);
    pm_node_apply_masked(&a, &b, ~(pm_change_mask_t)0);
    CHECK_EQ(pm_settings_diff(&a, &b), 0);
    g_context = NULL;
}

static void test_port_events(void)
{
    g_context = "node port events";
    setup();
    pm_rdm_responder_t *rr = pm_node_responder(N);
    /* BOOT_CATCH -> responder (queued BOOT_CATCH_STATUS + status message) + panel */
    uint8_t before = pm_rdm_responder_message_count(rr);
    pm_node_port_event_t ev = {.type = PM_NODE_PEV_BOOT_CATCH, .port = 1, .value = PM_BOOT_CATCH_STATE_CAUGHT};
    pm_node_port_event(N, &ev);
    CHECK_EQ(pm_rdm_responder_message_count(rr), before + 2);
    CHECK(last_ui(PM_NODE_UI_BOOT_CATCH) && last_ui(PM_NODE_UI_BOOT_CATCH)->port == 1 &&
          last_ui(PM_NODE_UI_BOOT_CATCH)->value == PM_BOOT_CATCH_STATE_CAUGHT);
    /* TOD changed -> panel (and ArtTodData to subscribers, none here) */
    ev = (pm_node_port_event_t){.type = PM_NODE_PEV_TOD_CHANGED, .port = 2};
    pm_node_port_event(N, &ev);
    CHECK(last_ui(PM_NODE_UI_TOD_CHANGED) && last_ui(PM_NODE_UI_TOD_CHANGED)->port == 2);
    /* error -> panel */
    ev = (pm_node_port_event_t){.type = PM_NODE_PEV_ERROR, .port = 3, .value = 0x101};
    pm_node_port_event(N, &ev);
    CHECK(last_ui(PM_NODE_UI_ERROR) && last_ui(PM_NODE_UI_ERROR)->value == 0x101);
    /* input frame: read from the port engine and looped back to an output on the same universe */
    F.cfg.port[3].direction = PM_PORT_DIR_INPUT;
    F.cfg.port[3].address = F.cfg.port[0].address;
    F.pend[PM_SRC_LOCAL_UI] |= PM_CHG_PORT(PM_CHG_P_DIRECTION | PM_CHG_P_ADDRESS, 3);
    flush();
    CHECK_EQ(F.port_cfg[3].mode, PM_NODE_PORT_INPUT);
    F.input_len = 3;
    F.input[0] = 1;
    unsigned outs = F.outputs[0];
    ev = (pm_node_port_event_t){.type = PM_NODE_PEV_INPUT_FRAME, .port = 3};
    pm_node_port_event(N, &ev);
    CHECK_EQ(F.input_reads, 1);
    CHECK_EQ(F.outputs[0], outs + 1);
    /* RDM response with a token nobody waits for: ignored */
    uint8_t junk[30] = {PM_RDM_START_CODE};
    ev = (pm_node_port_event_t){.type = PM_NODE_PEV_RDM_RESPONSE, .port = 0, .token = 12345, .len = 1, .data = junk};
    pm_node_port_event(N, &ev);
    CHECK_EQ(count_sent(PM_OP_RDM), 0);
    g_context = NULL;
}

static void test_tube_update(void)
{
    g_context = "node tube update";
    setup();
    const uint8_t boot[4] = {'B', 'O', 'O', 'T'};
    art_rdm(1, DEV_UID, PM_RDM_CC_SET, PM_PID_ENTER_BOOTLOADER, boot, 4);
    const pm_node_ui_event_t *ev = last_ui(PM_NODE_UI_TUBE_UPDATE);
    CHECK(ev && ev->port == 1 && ev->value == 0);
    /* BOOT_INFO response: app_end 0x1000 */
    art_rdm(1, DEV_UID, PM_RDM_CC_GET, PM_PID_BOOT_INFO, NULL, 0);
    uint8_t info[PM_PDL_BOOT_INFO_GET_RESPONSE] = {3, 64, 0x10, 0x00};
    line_answer(PM_RDM_CC_GET_RESPONSE, PM_PID_BOOT_INFO, info, sizeof(info));
    uint8_t page[66] = {0x07, 0xC0};
    art_rdm(1, DEV_UID, PM_RDM_CC_SET, PM_PID_BOOT_WRITE_PAGE, page, 66);
    ev = last_ui(PM_NODE_UI_TUBE_UPDATE);
    CHECK(ev && ev->value == 50); /* (0x7C0 + 64) / 0x1000 */
    unsigned n = count_ui(PM_NODE_UI_TUBE_UPDATE);
    art_rdm(1, DEV_UID, PM_RDM_CC_SET, PM_PID_BOOT_WRITE_PAGE, page, 66); /* resend: same percent, no event */
    CHECK_EQ(count_ui(PM_NODE_UI_TUBE_UPDATE), n);
    page[0] = 0x0F;
    page[1] = 0xC0;
    art_rdm(1, DEV_UID, PM_RDM_CC_SET, PM_PID_BOOT_WRITE_PAGE, page, 66);
    CHECK(last_ui(PM_NODE_UI_TUBE_UPDATE)->value == 99);
    art_rdm(1, DEV_UID, PM_RDM_CC_SET, PM_PID_BOOT_FINISH, NULL, 0);
    CHECK(last_ui(PM_NODE_UI_TUBE_UPDATE)->value == -1);
    /* timeout without BOOT_FINISH (default app_end 0x1800 on port 0) */
    page[0] = 0x0B;
    page[1] = 0xC0;
    art_rdm(0, DEV_UID, PM_RDM_CC_SET, PM_PID_BOOT_WRITE_PAGE, page, 66);
    ev = last_ui(PM_NODE_UI_TUBE_UPDATE);
    CHECK(ev && ev->port == 0 && ev->value == 50); /* (0xBC0 + 64) / 0x1800 */
    advance(PM_NODE_TUBE_UPDATE_TIMEOUT_MS - 100);
    CHECK(last_ui(PM_NODE_UI_TUBE_UPDATE)->value == 50);
    advance(200);
    CHECK(last_ui(PM_NODE_UI_TUBE_UPDATE)->value == -1);
    g_context = NULL;
}

static void test_live_and_tick(void)
{
    g_context = "node live snapshot / tick";
    setup();
    uint8_t d[512] = {0};
    for (unsigned i = 0; i < 40; i++) { /* 25 fps for 1.6 s on port 1 (Port-Address 2) */
        const pm_artdmx_t dmx = {.sequence = (uint8_t)(i + 1), .port_address = 2, .length = 512, .data = d};
        int len = pm_artdmx_encode(&dmx, PKT, sizeof(PKT));
        pm_node_handle_packet(N, CTL, PM_ARTNET_PORT, NODE_IP, PKT, (size_t)len);
        advance(40);
    }
    advance(100);
    CHECK(F.live_calls >= 10);
    CHECK(F.live[1].rx_active);
    CHECK(F.live[1].rx_fps >= 24 && F.live[1].rx_fps <= 26);
    CHECK_EQ(F.live[1].merge_sources, 1);
    CHECK(!F.live[1].merging && !F.live[1].data_loss && !F.live[1].synced);
    CHECK(!F.live[0].rx_active);
    advance(PM_ARTNET_DATA_LOSS_MS + 200);
    CHECK(F.live[1].data_loss && !F.live[1].rx_active && F.live[1].rx_fps == 0);
    /* RESET_DEVICE runs from the tick after the response left */
    uint8_t warm = 0x01;
    art_rdm(0, NODE_UID, PM_RDM_CC_SET, PM_RDM_PID_RESET_DEVICE, &warm, 1);
    CHECK_EQ(F.resets, 0);
    advance(PM_RDM_RESP_RESET_DELAY_MS + 20);
    CHECK_EQ(F.resets, 1);
    CHECK_EQ(F.reset_kind, 1);
    /* firmware progress -> panel */
    pm_fw_status_t st = {.state = PM_FW_STATE_RECEIVING, .received = 50, .total = 200};
    pm_node_fw_progress(N, &st);
    CHECK(last_ui(PM_NODE_UI_FW_PROGRESS) && last_ui(PM_NODE_UI_FW_PROGRESS)->value == 25);
    st.state = PM_FW_STATE_FAILED;
    pm_node_fw_progress(N, &st);
    CHECK(last_ui(PM_NODE_UI_FW_PROGRESS)->value == -1);
    unsigned n = F.nui;
    st = (pm_fw_status_t){.state = PM_FW_STATE_IDLE};
    pm_node_fw_progress(N, &st);
    CHECK_EQ(F.nui, n);
    g_context = NULL;
}

static void test_port_cfg(void)
{
    g_context = "node port cfg";
    static pm_settings_t s;
    pm_settings_defaults(&s);
    s.port[0].enabled = false;
    s.port[1].direction = PM_PORT_DIR_INPUT;
    s.port[2].output_style = PM_OUTPUT_DELTA;
    s.port[2].dmx_mbs_us = 64;
    s.rdm_bg_interval_s = 5;
    pm_node_port_cfg_t c;
    pm_node_port_cfg_from(&s, 0, &c);
    CHECK_EQ(c.mode, PM_NODE_PORT_OFF);
    pm_node_port_cfg_from(&s, 1, &c);
    CHECK_EQ(c.mode, PM_NODE_PORT_INPUT);
    pm_node_port_cfg_from(&s, 2, &c);
    CHECK(c.mode == PM_NODE_PORT_OUTPUT && !c.continuous && c.mbs_us == 64 && c.rdm_bg_interval_ms == 5000 &&
          c.break_us == 176 && c.mab_us == 16 && c.rate_hz == 40 && c.rdm_enabled && c.rdm_bg_discovery);
    g_context = NULL;
}

void test_node(void)
{
    test_port_cfg();
    test_routing();
    test_tokens();
    test_identify();
    test_settings();
    test_no_stale_overwrite();
    test_port_events();
    test_tube_update();
    test_live_and_tick();
}
