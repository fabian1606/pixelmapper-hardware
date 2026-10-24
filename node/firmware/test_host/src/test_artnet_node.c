/* pm_artnet node behaviour tests: fake platform ops, controlled time (protocol/contract.md §3-§6, §9.2). */
#include <stdalign.h>
#include <stddef.h>

#include "pm_artnet.h"
#include "test.h"

void test_artnet_node(void);

/* ------------------------------------------------------------------------------------------------ fake platform */

#define IP(a, b, c, d) ((uint32_t)(((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d)))
#define NODE_IP IP(2, 0, 0, 10)
#define BCAST IP(2, 255, 255, 255)
#define CTL_A IP(2, 0, 0, 100)
#define CTL_B IP(2, 0, 0, 101)
#define CTL_C IP(2, 0, 0, 102)
#define MAX_SENT 160
#define FW_IMAGE_MAX 8192

typedef struct {
    uint32_t ip;
    uint16_t op;
    unsigned seq;
    size_t len;
    uint8_t data[1300];
} sent_t;

static struct {
    uint32_t now;
    uint32_t rnd;
    unsigned seq;
    sent_t sent[MAX_SENT];
    unsigned nsent;
    pm_artnet_net_info_t net;
    pm_settings_t settings;
    bool sync_cb;
    pm_err_t apply_result;
    unsigned applies;
    pm_change_mask_t last_mask;
    pm_change_source_t last_src;
    unsigned ip_applies, ip_apply_seq;
    struct {
        unsigned count;
        uint16_t len;
        bool defer;
        uint8_t data[512];
    } out[PM_NUM_PORTS];
    unsigned commits, clears;
    pm_artnet_port_status_t st[PM_NUM_PORTS];
    pm_uid_t tod[600];
    uint16_t tod_n[PM_NUM_PORTS];
    unsigned tod_ctl_count;
    uint8_t tod_ctl_cmd, tod_ctl_port;
    unsigned rdm_count;
    pm_artnet_rdm_request_t rdm_last;
    uint8_t rdm_buf[300];
    pm_err_t rdm_result;
    bool rdm_answer_sync;
    uint8_t fifo_avail;
    uint8_t indicator;
    /* firmware sink */
    pm_fw_error_t fw_begin_result, fw_finish_result;
    unsigned fw_begins, fw_writes, fw_finishes, fw_aborts, fw_reboots;
    pm_fw_error_t fw_abort_reason;
    pm_image_desc_t fw_desc;
    uint8_t fw_image[FW_IMAGE_MAX];
    uint32_t fw_received;
} F;

static alignas(max_align_t) uint8_t MEM[32 * 1024];
static pm_artnet_node_t *N;

static uint32_t f_now(void *ctx) { (void)ctx; return F.now; }
static uint32_t f_random(void *ctx) { (void)ctx; return F.rnd; }

static pm_err_t f_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    (void)ctx;
    CHECK_EQ(port, PM_ARTNET_PORT);
    CHECK(ip != 0xFFFFFFFFu);
    if (F.nsent >= MAX_SENT || len > sizeof(F.sent[0].data)) return PM_ERR_FULL;
    sent_t *s = &F.sent[F.nsent++];
    s->ip = ip;
    s->len = len;
    s->seq = ++F.seq;
    memcpy(s->data, data, len);
    s->op = len >= 10 ? pm_get_le16(data + 8) : 0;
    return PM_OK;
}

static void f_net_info(void *ctx, pm_artnet_net_info_t *out) { (void)ctx; *out = F.net; }

static pm_err_t f_settings_apply(void *ctx, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src)
{
    (void)ctx;
    if (F.apply_result != PM_OK) return F.apply_result;
    F.settings = *s;
    F.applies++;
    F.last_mask = mask;
    F.last_src = src;
    if (F.sync_cb) pm_artnet_settings_changed(N, &F.settings, mask);
    return PM_OK;
}

static void f_ip_apply(void *ctx) { (void)ctx; F.ip_applies++; F.ip_apply_seq = ++F.seq; }

static void f_port_output(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    (void)ctx;
    F.out[port].count++;
    F.out[port].len = len;
    F.out[port].defer = defer;
    memcpy(F.out[port].data, data, len);
}

static void f_commit(void *ctx) { (void)ctx; F.commits++; }
static void f_port_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out) { (void)ctx; *out = F.st[port]; }
static void f_port_clear(void *ctx, pm_port_idx_t port) { (void)ctx; (void)port; F.clears++; }

static uint16_t f_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)ctx;
    uint16_t n = F.tod_n[port] < cap ? F.tod_n[port] : cap;
    memcpy(out, F.tod, n * sizeof(pm_uid_t));
    return F.tod_n[port];
}

static void f_tod_control(void *ctx, pm_port_idx_t port, uint8_t cmd)
{
    (void)ctx;
    F.tod_ctl_count++;
    F.tod_ctl_cmd = cmd;
    F.tod_ctl_port = port;
}

static pm_err_t f_rdm_request(void *ctx, const pm_artnet_rdm_request_t *req)
{
    (void)ctx;
    F.rdm_count++;
    F.rdm_last = *req;
    memcpy(F.rdm_buf, req->rdm, req->rdm_len);
    F.rdm_last.rdm = F.rdm_buf;
    if (F.rdm_result == PM_OK && F.rdm_answer_sync) pm_artnet_rdm_response(N, req->token, req->rdm, req->rdm_len);
    return F.rdm_result;
}

static uint8_t f_fifo_avail(void *ctx, pm_port_idx_t port) { (void)ctx; (void)port; return F.fifo_avail; }
static void f_indicator(void *ctx, uint8_t cmd) { (void)ctx; F.indicator = cmd; }

static pm_fw_error_t fw_begin(void *ctx, const pm_image_desc_t *d)
{
    (void)ctx;
    F.fw_begins++;
    F.fw_desc = *d;
    F.fw_received = 0;
    return F.fw_begin_result;
}

static pm_fw_error_t fw_write(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    (void)ctx;
    F.fw_writes++;
    CHECK_EQ(off, F.fw_received); /* sequential */
    if (off + len > FW_IMAGE_MAX) return PM_FW_ERROR_TOO_LARGE;
    memcpy(F.fw_image + off, data, len);
    F.fw_received = (uint32_t)(off + len);
    return PM_FW_ERROR_NONE;
}

static pm_fw_error_t fw_finish(void *ctx) { (void)ctx; F.fw_finishes++; return F.fw_finish_result; }
static void fw_abort(void *ctx, pm_fw_error_t r) { (void)ctx; F.fw_aborts++; F.fw_abort_reason = r; }
static void fw_status(void *ctx, pm_fw_status_t *out) { (void)ctx; memset(out, 0, sizeof(*out)); }
static void fw_reboot(void *ctx) { (void)ctx; F.fw_reboots++; }

static const pm_fw_sink_t SINK = {NULL, fw_begin, fw_write, fw_finish, fw_abort, fw_status, fw_reboot};

static const pm_uid_t NODE_UID = {{0x7F, 0xF0, 0x80, 0x12, 0x34, 0x56}};

/** Fresh node with factory settings (optionally modified by `tweak`). */
static void setup_with(void (*tweak)(pm_settings_t *))
{
    memset(&F, 0, sizeof(F));
    F.now = 1000;
    F.net = (pm_artnet_net_info_t){.ip = NODE_IP, .mask = 0xFF000000u, .mac = {2, 0, 0, 0x12, 0x34, 0x56},
                                   .link_up = true};
    for (unsigned p = 0; p < PM_NUM_PORTS; p++) F.st[p] = (pm_artnet_port_status_t){.output_active = true, .input_signal = true};
    F.sync_cb = true;
    F.fifo_avail = 4;
    pm_settings_defaults(&F.settings);
    if (tweak) tweak(&F.settings);
    const pm_artnet_identity_t id = {.esta_man = PM_ESTA_MANUFACTURER, .oem = PM_ARTNET_OEM_NODE, .vers_info = 0x0001,
                                     .uid = NODE_UID, .num_ports = 4};
    const pm_artnet_ops_t ops = {
        .now_ms = f_now, .random = f_random, .send_udp = f_send, .net_info = f_net_info,
        .settings_apply = f_settings_apply, .ip_apply = f_ip_apply, .port_output = f_port_output,
        .port_commit = f_commit, .port_status = f_port_status, .port_clear = f_port_clear, .tod_get = f_tod_get,
        .tod_control = f_tod_control, .rdm_request = f_rdm_request, .rdm_fifo_avail = f_fifo_avail,
        .indicator = f_indicator, .fw_sink = &SINK,
    };
    N = pm_artnet_node_init(MEM, &id, &ops, &F.settings);
    CHECK(N != NULL);
}

static void setup(void) { setup_with(NULL); }

static void clear_sent(void) { F.nsent = 0; }

static void advance(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 10) {
        F.now += 10;
        pm_artnet_tick(N);
    }
}

static void rx_dst(uint32_t src, uint32_t dst, const uint8_t *buf, int len)
{
    CHECK(len > 0);
    if (len > 0) pm_artnet_handle_packet_ex(N, src, PM_ARTNET_PORT, dst, buf, (size_t)len);
}

static void rx(uint32_t src, const uint8_t *buf, int len) { rx_dst(src, NODE_IP, buf, len); }

static unsigned count_sent(uint16_t op, uint32_t ip)
{
    unsigned c = 0;
    for (unsigned i = 0; i < F.nsent; i++) c += F.sent[i].op == op && (ip == 0 || F.sent[i].ip == ip);
    return c;
}

static const sent_t *nth_sent(uint16_t op, uint32_t ip, unsigned nth)
{
    for (unsigned i = 0; i < F.nsent; i++) {
        if (F.sent[i].op == op && (ip == 0 || F.sent[i].ip == ip) && nth-- == 0) return &F.sent[i];
    }
    return NULL;
}

/* packet builders */
static uint8_t PKT[1300];

static int mk_poll(uint8_t flags)
{
    const pm_artpoll_t p = {.flags = flags, .diag_priority = 0x10};
    return pm_artpoll_encode(&p, PKT, sizeof(PKT));
}

static int mk_dmx(pm_port_address_t pa, uint8_t seq, uint8_t physical, const uint8_t *data, uint16_t len)
{
    const pm_artdmx_t d = {.sequence = seq, .physical = physical, .port_address = pa, .length = len, .data = data};
    return pm_artdmx_encode(&d, PKT, sizeof(PKT));
}

static int mk_dmx_fill(pm_port_address_t pa, uint8_t seq, uint8_t physical, uint8_t v0, uint8_t v1)
{
    const uint8_t d[4] = {v0, v1, 0, 0};
    return mk_dmx(pa, seq, physical, d, 4);
}

static int mk_address(uint8_t bind, uint8_t command)
{
    pm_artaddress_t a;
    memset(&a, 0, sizeof(a));
    a.net_switch = a.sub_switch = 0x7F;
    memset(a.sw_in, 0x7F, 4);
    memset(a.sw_out, 0x7F, 4);
    a.acn_priority = 255;
    a.bind_index = bind;
    a.command = command;
    return pm_artaddress_encode(&a, PKT, sizeof(PKT));
}

static int mk_sync(void) { return pm_artsync_encode(PKT, sizeof(PKT)); }

static pm_artpollreply_t reply_of(const sent_t *s)
{
    pm_artpollreply_t r;
    memset(&r, 0, sizeof(r));
    CHECK(s != NULL);
    if (s) CHECK_EQ(pm_artpollreply_decode(s->data, s->len, &r), PM_OK);
    return r;
}

/* ------------------------------------------------------------------------------------------------ discovery */

static void test_poll_reply(void)
{
    g_context = "poll_reply";
    setup();
    CHECK(pm_artnet_node_size() <= 24u * 1024u); /* main.c reserves 24 KiB */
    F.rnd = 500;
    clear_sent();
    rx(CTL_A, PKT, mk_poll(PM_POLL_FLAG_REPLY_ON_CHANGE));
    CHECK_EQ(F.nsent, 0); /* random delay */
    advance(480);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, 0), 0);
    advance(40);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 4);
    CHECK_EQ(F.nsent, 4); /* unicast only */
    for (unsigned p = 0; p < 4; p++) {
        pm_artpollreply_t r = reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, p));
        CHECK_EQ(r.bind_index, p + 1);
        CHECK_EQ(r.num_ports, 1);
        CHECK_EQ(r.port_types[0], 0x80);
        CHECK_EQ(r.port_types[1], 0);
        CHECK_EQ(r.sw_out[0], p + 1);
        CHECK_EQ(r.ip, NODE_IP);
        CHECK_EQ(r.bind_ip, NODE_IP);
        CHECK_EQ(r.status1, 0xE2);
        CHECK_EQ(r.status2, 0xCC);
        CHECK_EQ(r.status3, 0x28);
        CHECK_EQ(r.good_output_a[0], 0);
        CHECK_EQ(r.good_output_b[0], 0x60);
        CHECK_EQ(r.esta_man, PM_ESTA_MANUFACTURER);
        CHECK_EQ(r.oem, PM_ARTNET_OEM_NODE);
        CHECK_EQ(r.refresh_rate, 44);
        CHECK(pm_uid_equal(&r.default_resp_uid, &NODE_UID));
        CHECK(strcmp(r.short_name, "PM Node") == 0);
        CHECK(strcmp(r.long_name, "Pixelmapper Node") == 0);
    }
    pm_artpollreply_t r0 = reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0));
    CHECK(strcmp(r0.node_report, "#0001 [0001] Power On Tests successful") == 0);

    /* targeted mode: Port-Address 2..3 only */
    clear_sent();
    F.rnd = 0;
    const pm_artpoll_t tp = {.flags = PM_POLL_FLAG_TARGETED, .target_bottom = 2, .target_top = 3};
    rx(CTL_B, PKT, pm_artpoll_encode(&tp, PKT, sizeof(PKT)));
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_B), 2);
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_B, 0)).bind_index, 2);
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_B, 1)).bind_index, 3);

    /* own packets are ignored; no link -> no reply */
    clear_sent();
    rx(NODE_IP, PKT, mk_poll(0));
    F.net.link_up = false;
    pm_artnet_net_changed(N);
    rx(CTL_C, PKT, mk_poll(0));
    CHECK_EQ(F.nsent, 0);
    g_context = NULL;
}

static void test_unsolicited(void)
{
    g_context = "unsolicited";
    setup();
    rx(CTL_A, PKT, mk_poll(PM_POLL_FLAG_REPLY_ON_CHANGE));
    rx(CTL_B, PKT, mk_poll(0));
    advance(20);
    clear_sent();
    /* ArtAddress from C renames the node: C gets its page, A (Flags bit 1) all 4 pages, B nothing */
    pm_artaddress_t a;
    memset(&a, 0, sizeof(a));
    a.net_switch = a.sub_switch = 0x7F;
    memset(a.sw_in, 0x7F, 4);
    memset(a.sw_out, 0x7F, 4);
    a.bind_index = 2;
    snprintf(a.short_name, sizeof(a.short_name), "Stage L");
    rx(CTL_C, PKT, pm_artaddress_encode(&a, PKT, sizeof(PKT)));
    CHECK_EQ(F.applies, 1);
    CHECK_EQ(F.last_src, PM_SRC_ARTNET);
    CHECK_EQ(F.last_mask, PM_CHG_NAMES);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_C), 1);
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_C, 0)).bind_index, 2);
    CHECK(strcmp(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_C, 0)).short_name, "Stage L") == 0);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 4);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_B), 0);

    /* asynchronous platform: reply waits for settings_changed */
    F.sync_cb = false;
    clear_sent();
    rx(CTL_C, PKT, mk_address(1, PM_AC_MERGE_LTP0));
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, 0), 0);
    pm_artnet_settings_changed(N, &F.settings, F.last_mask);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_C), 1);
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_C, 0)).good_output_a[0], 0x02); /* LTP */
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 1); /* only page 1 changed */
    /* ... or at most PM_ARTNET_ADDRESS_REPLY_WAIT_MS when the platform never answers */
    clear_sent();
    rx(CTL_C, PKT, mk_address(1, PM_AC_MERGE_HTP0));
    advance(PM_ARTNET_ADDRESS_REPLY_WAIT_MS - 20);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_C), 0);
    advance(30);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_C), 1);

    /* status change (port engine) -> unsolicited reply; controller forgotten after the keep time */
    F.sync_cb = true;
    pm_artnet_settings_changed(N, &F.settings, F.last_mask);
    clear_sent();
    F.st[3].rdm_discovery_running = true; /* volatile bit: no unsolicited reply */
    pm_artnet_port_status_changed(N, 3);
    CHECK_EQ(F.nsent, 0);
    F.net.dhcp_active = true;
    pm_artnet_net_changed(N);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 4);
    advance(PM_ARTNET_CONTROLLER_KEEP_MS);
    clear_sent();
    F.net.dhcp_active = false;
    pm_artnet_net_changed(N);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 0);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ merge */

static void test_merge_unit(void)
{
    g_context = "merge_unit";
    static pm_artnet_merge_t m;
    const uint8_t a[4] = {10, 200, 30, 40}, b[2] = {100, 5};
    pm_artnet_merge_init(&m, PM_MERGE_HTP);
    CHECK_EQ(pm_artnet_merge_feed(&m, 1, 0, a, 4, 0), PM_OK);
    CHECK_EQ(pm_artnet_merge_sources(&m), 1);
    CHECK_EQ(pm_artnet_merge_feed(&m, 2, 0, b, 2, 10), PM_OK);
    CHECK_EQ(pm_artnet_merge_sources(&m), 2);
    CHECK_EQ(m.out_len, 4);
    CHECK(m.out[0] == 100 && m.out[1] == 200 && m.out[2] == 30 && m.out[3] == 40);
    CHECK_EQ(pm_artnet_merge_feed(&m, 3, 0, b, 2, 20), PM_ERR_FULL);
    CHECK_EQ(pm_artnet_merge_feed(&m, 1, 1, b, 2, 20), PM_ERR_FULL); /* same IP, other Physical = other source */
    CHECK_EQ(pm_artnet_merge_feed(NULL, 1, 0, a, 4, 0), PM_ERR_INVALID_ARG);
    m.mode = PM_MERGE_LTP;
    CHECK_EQ(pm_artnet_merge_feed(&m, 2, 0, b, 2, 30), PM_OK);
    CHECK(m.out[0] == 100 && m.out[1] == 5 && m.out[2] == 30); /* latest wins, rest from the other */
    CHECK(!pm_artnet_merge_expire(&m, PM_ARTNET_MERGE_TIMEOUT_MS - 1));
    CHECK(pm_artnet_merge_expire(&m, PM_ARTNET_MERGE_TIMEOUT_MS)); /* source 1 (last 0) expires */
    CHECK_EQ(pm_artnet_merge_sources(&m), 1);
    CHECK(m.out_len == 2 && m.out[0] == 100 && m.out[1] == 5);
    CHECK(pm_artnet_merge_expire(&m, 30 + PM_ARTNET_MERGE_TIMEOUT_MS));
    CHECK_EQ(pm_artnet_merge_sources(&m), 0);
    CHECK(m.out_len == 2 && m.out[0] == 100); /* last result kept */
    /* cancel merge */
    pm_artnet_merge_init(&m, PM_MERGE_HTP);
    pm_artnet_merge_feed(&m, 1, 0, a, 4, 0);
    pm_artnet_merge_feed(&m, 2, 0, b, 2, 0);
    pm_artnet_merge_cancel(&m);
    CHECK_EQ(pm_artnet_merge_feed(&m, 2, 0, b, 2, 10), PM_OK);
    CHECK_EQ(pm_artnet_merge_sources(&m), 1);
    CHECK_EQ(pm_artnet_merge_feed(&m, 1, 0, a, 4, 20), PM_ERR_REJECTED);
    CHECK_EQ(pm_artnet_merge_feed(&m, 2, 3, a, 4, 20), PM_OK); /* same IP is fine */
    pm_artnet_merge_expire(&m, 20 + PM_ARTNET_MERGE_TIMEOUT_MS);
    CHECK_EQ(pm_artnet_merge_feed(&m, 1, 0, a, 4, 20000), PM_OK); /* exclusivity ends with the source */
    g_context = NULL;
}

static void test_dmx_merge(void)
{
    g_context = "dmx_merge";
    setup();
    rx(CTL_C, PKT, mk_poll(PM_POLL_FLAG_REPLY_ON_CHANGE));
    advance(10);
    clear_sent();
    rx(CTL_A, PKT, mk_dmx_fill(1, 1, 0, 10, 200));
    CHECK_EQ(F.out[0].count, 1);
    CHECK(F.out[0].len == 4 && F.out[0].data[0] == 10 && !F.out[0].defer);
    CHECK_EQ(F.out[1].count, 0);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_C), 1); /* GoodOutputA bit 7 appeared */
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_C, 0)).good_output_a[0], 0x80);
    clear_sent();
    rx(CTL_B, PKT, mk_dmx_fill(1, 1, 0, 100, 5));
    CHECK(F.out[0].data[0] == 100 && F.out[0].data[1] == 200); /* HTP */
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_C), 1); /* merge start reported */
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_C, 0)).good_output_a[0], 0x88);
    unsigned before = F.out[0].count;
    rx(CTL_C, PKT, mk_dmx_fill(1, 1, 0, 255, 255)); /* third source dropped */
    CHECK_EQ(F.out[0].count, before);
    CHECK(F.out[0].data[0] == 100);
    /* B stops: after 10 s only A remains, output follows A */
    for (unsigned i = 0; i < 105; i++) {
        F.now += 100;
        rx(CTL_A, PKT, mk_dmx_fill(1, (uint8_t)(2 + i), 0, 10, 200));
        pm_artnet_tick(N);
    }
    CHECK(F.out[0].data[0] == 10 && F.out[0].data[1] == 200);
    /* disabled or input ports ignore ArtDmx */
    F.settings.port[1].enabled = false;
    F.settings.port[2].direction = PM_PORT_DIR_INPUT;
    pm_artnet_settings_changed(N, &F.settings, 0);
    rx(CTL_A, PKT, mk_dmx_fill(2, 0, 0, 1, 1));
    rx(CTL_A, PKT, mk_dmx_fill(3, 0, 0, 1, 1));
    CHECK_EQ(F.out[1].count + F.out[2].count, 0);
    g_context = NULL;
}

static void test_sequence(void)
{
    g_context = "sequence";
    setup();
    rx(CTL_A, PKT, mk_dmx_fill(1, 10, 0, 10, 0));
    rx(CTL_A, PKT, mk_dmx_fill(1, 9, 0, 9, 0)); /* older: dropped */
    CHECK_EQ(F.out[0].data[0], 10);
    rx(CTL_A, PKT, mk_dmx_fill(1, 11, 0, 11, 0));
    CHECK_EQ(F.out[0].data[0], 11);
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 1, 0)); /* 0 = sequencing off */
    CHECK_EQ(F.out[0].data[0], 1);
    rx(CTL_A, PKT, mk_dmx_fill(1, 200, 0, 200, 0));
    rx(CTL_A, PKT, mk_dmx_fill(1, 5, 0, 5, 0)); /* wrap: 200 -> 5 is newer */
    CHECK_EQ(F.out[0].data[0], 5);
    /* a restarted sender is accepted again after SEQ_MAX_DROPS (8) drops */
    for (unsigned i = 0; i < 8; i++) rx(CTL_A, PKT, mk_dmx_fill(1, (uint8_t)(200 + i), 0, 7, 0));
    CHECK_EQ(F.out[0].data[0], 5);
    rx(CTL_A, PKT, mk_dmx_fill(1, 210, 0, 77, 0));
    CHECK_EQ(F.out[0].data[0], 77);
    g_context = NULL;
}

static void test_cancel_merge(void)
{
    g_context = "cancel_merge";
    setup();
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 50, 0));
    rx(CTL_B, PKT, mk_dmx_fill(1, 0, 0, 20, 80));
    rx(CTL_C, PKT, mk_address(1, PM_AC_CANCEL_MERGE));
    rx(CTL_B, PKT, mk_dmx_fill(1, 0, 0, 20, 80));
    CHECK(F.out[0].data[0] == 20 && F.out[0].data[1] == 80);
    unsigned before = F.out[0].count;
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 50, 0));
    CHECK_EQ(F.out[0].count, before);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ failsafe */

static void test_failsafe(void)
{
    g_context = "failsafe";
    setup();
    rx(CTL_A, PKT, mk_poll(PM_POLL_FLAG_REPLY_ON_CHANGE));
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 33, 44));
    rx(CTL_A, PKT, mk_dmx_fill(2, 0, 0, 33, 44));
    rx(CTL_A, PKT, mk_dmx_fill(3, 0, 0, 33, 44));
    rx(CTL_A, PKT, mk_dmx_fill(4, 0, 0, 33, 44));
    /* record a scene on port 4, configure Zero / Full / Scene on ports 2..4, port 1 holds */
    rx(CTL_A, PKT, mk_address(4, PM_AC_FAIL_RECORD));
    CHECK_EQ(F.settings.port[3].scene_len, 4);
    CHECK_EQ(F.settings.port[3].scene[1], 44);
    rx(CTL_A, PKT, mk_address(2, PM_AC_FAIL_ZERO));
    rx(CTL_A, PKT, mk_address(3, PM_AC_FAIL_FULL));
    rx(CTL_A, PKT, mk_address(4, PM_AC_FAIL_SCENE));
    CHECK_EQ(F.settings.port[1].failsafe, PM_FAILSAFE_ZERO);
    CHECK_EQ(F.last_mask, PM_CHG_PORT(PM_CHG_P_OUTPUT, 3));
    unsigned c0 = F.out[0].count;
    advance(PM_ARTNET_DATA_LOSS_MS - 20);
    CHECK_EQ(F.out[1].data[0], 33);
    clear_sent();
    advance(30);
    CHECK_EQ(F.out[0].count, c0); /* hold: nothing */
    CHECK(F.out[1].len == 512 && F.out[1].data[0] == 0 && F.out[1].data[511] == 0);
    CHECK(F.out[2].len == 512 && F.out[2].data[0] == 0xFF && F.out[2].data[511] == 0xFF);
    CHECK(F.out[3].len == 4 && F.out[3].data[0] == 33 && F.out[3].data[1] == 44);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 4); /* GoodOutputA bit 7 cleared on all pages */
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 1)).good_output_a[0], 0);
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 1)).status3, 0x68); /* failsafe zero */
    /* changing the mode while data is lost applies it at once */
    rx(CTL_A, PKT, mk_address(1, PM_AC_FAIL_FULL));
    CHECK(F.out[0].len == 512 && F.out[0].data[0] == 0xFF);
    /* data returns */
    rx(CTL_A, PKT, mk_dmx_fill(2, 0, 0, 7, 8));
    CHECK(F.out[1].len == 4 && F.out[1].data[0] == 7);
    /* failsafe also applies when no data ever came (4 s after start) */
    setup();
    F.settings.port[0].failsafe = PM_FAILSAFE_FULL;
    pm_artnet_settings_changed(N, &F.settings, PM_CHG_PORT(PM_CHG_P_OUTPUT, 0));
    advance(PM_ARTNET_DATA_LOSS_MS + 10);
    CHECK(F.out[0].len == 512 && F.out[0].data[0] == 0xFF);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ sync */

static void test_sync(void)
{
    g_context = "sync";
    setup();
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 1, 0));
    rx_dst(CTL_A, NODE_IP, PKT, mk_sync()); /* unicast: ignored */
    CHECK_EQ(F.commits, 0);
    rx_dst(CTL_B, BCAST, PKT, mk_sync()); /* foreign IP: ignored */
    CHECK_EQ(F.commits, 0);
    rx_dst(CTL_A, BCAST, PKT, mk_sync());
    CHECK_EQ(F.commits, 1);
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 2, 0));
    CHECK(F.out[0].defer);
    rx_dst(CTL_A, IP(2, 0, 255, 254), PKT, mk_sync()); /* not a broadcast of 2.0.0.0/8 */
    CHECK_EQ(F.commits, 1);
    rx_dst(CTL_A, 0xFFFFFFFFu, PKT, mk_sync());
    CHECK_EQ(F.commits, 2);
    /* 4 s without ArtSync -> immediate output again */
    for (unsigned i = 0; i < 40; i++) {
        F.now += 100;
        rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 3, 0));
        pm_artnet_tick(N);
    }
    CHECK_EQ(F.commits, 3); /* pending frames released once */
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 4, 0));
    CHECK(!F.out[0].defer);
    /* merge from two IPs: sync mode ends and ArtSync is ignored */
    rx_dst(CTL_A, BCAST, PKT, mk_sync());
    CHECK_EQ(F.commits, 4);
    rx(CTL_B, PKT, mk_dmx_fill(1, 0, 0, 9, 0));
    CHECK_EQ(F.commits, 5);
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 4, 0));
    CHECK(!F.out[0].defer);
    rx_dst(CTL_A, BCAST, PKT, mk_sync());
    CHECK_EQ(F.commits, 5);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ configuration */

static void test_address(void)
{
    g_context = "address";
    setup();
    pm_artaddress_t a;
    memset(&a, 0, sizeof(a));
    a.bind_index = 2;
    a.net_switch = 0x81;
    a.sub_switch = 0x82;
    memset(a.sw_in, 0x7F, 4);
    a.sw_out[0] = 0x85;
    a.acn_priority = 255;
    clear_sent();
    rx(CTL_A, PKT, pm_artaddress_encode(&a, PKT, sizeof(PKT)));
    CHECK_EQ(F.settings.port[1].address, 0x0125);
    CHECK_EQ(F.last_mask, PM_CHG_PORT(PM_CHG_P_ADDRESS, 1));
    pm_artpollreply_t r = reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0));
    CHECK(r.bind_index == 2 && r.net_switch == 1 && r.sub_switch == 2 && r.sw_out[0] == 5);
    /* 0x00 = back to the default (Port-Address 2), 0x7F = no change */
    a.net_switch = 0x00;
    a.sub_switch = 0x7F;
    a.sw_out[0] = 0x00;
    rx(CTL_A, PKT, pm_artaddress_encode(&a, PKT, sizeof(PKT)));
    CHECK_EQ(F.settings.port[1].address, 0x0022);
    a.sub_switch = 0x00;
    rx(CTL_A, PKT, pm_artaddress_encode(&a, PKT, sizeof(PKT)));
    CHECK_EQ(F.settings.port[1].address, 2);
    /* commands */
    rx(CTL_A, PKT, mk_address(3, PM_AC_STYLE_DELTA0));
    CHECK_EQ(F.settings.port[2].output_style, PM_OUTPUT_DELTA);
    rx(CTL_A, PKT, mk_address(3, PM_AC_RDM_DISABLE0));
    CHECK(!F.settings.port[2].rdm_enabled);
    clear_sent();
    rx(CTL_A, PKT, mk_address(3, PM_AC_NONE));
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0)).good_output_b[0], 0xA0); /* RDM off, delta */
    rx(CTL_A, PKT, mk_address(1, PM_AC_LED_LOCATE));
    CHECK_EQ(F.indicator, PM_AC_LED_LOCATE);
    clear_sent();
    rx(CTL_A, PKT, mk_address(1, PM_AC_LED_MUTE));
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0)).status1, 0xA2);
    rx(CTL_A, PKT, mk_address(1, PM_AC_CLEAR_OP0));
    CHECK_EQ(F.clears, 1);
    unsigned applies = F.applies;
    rx(CTL_A, PKT, mk_address(1, 0x11)); /* deprecated AcMergeLtp1: ignored */
    rx(CTL_A, PKT, mk_address(4, PM_AC_ACN_SEL0));
    CHECK_EQ(F.applies, applies);
    clear_sent();
    rx(CTL_A, PKT, mk_address(5, PM_AC_LED_NORMAL)); /* no such BindIndex */
    CHECK_EQ(F.nsent, 0);
    rx(CTL_A, PKT, mk_address(0, PM_AC_LED_NORMAL)); /* 0 = root */
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 1);
    /* direction */
    rx(CTL_A, PKT, mk_address(4, PM_AC_DIRECTION_RX0));
    CHECK_EQ(F.settings.port[3].direction, PM_PORT_DIR_INPUT);
    clear_sent();
    rx(CTL_A, PKT, mk_address(4, PM_AC_NONE));
    r = reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0));
    CHECK(r.port_types[0] == 0x40 && r.sw_in[0] == 4 && r.sw_out[0] == 0 && r.good_output_b[0] == 0xE0);
    /* ArtInput: disables inputs only */
    pm_artinput_t in = {.bind_index = 4, .num_ports = 1, .input = {1, 0, 0, 0}};
    clear_sent();
    rx(CTL_A, PKT, pm_artinput_encode(&in, PKT, sizeof(PKT)));
    CHECK(!F.settings.port[3].enabled);
    CHECK_EQ(F.last_mask, PM_CHG_PORT(PM_CHG_P_ENABLED, 3));
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0)).good_input[0], 0x08);
    in.bind_index = 1;
    applies = F.applies;
    clear_sent();
    rx(CTL_A, PKT, pm_artinput_encode(&in, PKT, sizeof(PKT)));
    CHECK_EQ(F.applies, applies);
    CHECK(F.settings.port[0].enabled);
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 1);
    g_context = NULL;
}

static void test_ip_prog(void)
{
    g_context = "ip_prog";
    setup();
    pm_artipprog_t p = {.command = PM_IPPROG_ENABLE | PM_IPPROG_PROG_IP | PM_IPPROG_PROG_MASK,
                        .ip = IP(10, 1, 2, 3), .mask = 0xFFFF0000u};
    clear_sent();
    rx(CTL_A, PKT, pm_artipprog_encode(&p, PKT, sizeof(PKT)));
    CHECK_EQ(F.last_mask, PM_CHG_IP);
    CHECK_EQ(F.last_src, PM_SRC_IPPROG);
    CHECK(!F.settings.ip.dhcp && F.settings.ip.static_ip == IP(10, 1, 2, 3) &&
          F.settings.ip.static_mask == 0xFFFF0000u);
    const sent_t *s = nth_sent(PM_OP_IP_PROG_REPLY, CTL_A, 0);
    CHECK(s != NULL);
    pm_artipprogreply_t r;
    if (s) {
        CHECK_EQ(pm_artipprogreply_decode(s->data, s->len, &r), PM_OK);
        CHECK(r.ip == IP(10, 1, 2, 3) && r.mask == 0xFFFF0000u && r.status == 0);
        CHECK_EQ(F.ip_applies, 1);
        CHECK(F.ip_apply_seq > s->seq); /* reply first, then the new address */
    }
    /* enquiry: no change */
    p = (pm_artipprog_t){0};
    clear_sent();
    rx(CTL_A, PKT, pm_artipprog_encode(&p, PKT, sizeof(PKT)));
    CHECK_EQ(F.ip_applies, 1);
    s = nth_sent(PM_OP_IP_PROG_REPLY, CTL_A, 0);
    CHECK(s != NULL);
    if (s) {
        pm_artipprogreply_decode(s->data, s->len, &r);
        CHECK(r.ip == NODE_IP && r.status == 0);
    }
    /* gateway, then DHCP (lower bits ignored), then reset */
    p = (pm_artipprog_t){.command = PM_IPPROG_ENABLE | PM_IPPROG_PROG_GATEWAY, .gateway = IP(10, 1, 0, 1)};
    rx(CTL_A, PKT, pm_artipprog_encode(&p, PKT, sizeof(PKT)));
    CHECK_EQ(F.settings.ip.gateway, IP(10, 1, 0, 1));
    p = (pm_artipprog_t){.command = PM_IPPROG_ENABLE | PM_IPPROG_DHCP | PM_IPPROG_PROG_IP, .ip = IP(1, 1, 1, 1)};
    clear_sent();
    rx(CTL_A, PKT, pm_artipprog_encode(&p, PKT, sizeof(PKT)));
    CHECK(F.settings.ip.dhcp && F.settings.ip.static_ip == IP(10, 1, 2, 3));
    s = nth_sent(PM_OP_IP_PROG_REPLY, CTL_A, 0);
    CHECK(s != NULL);
    if (s) {
        pm_artipprogreply_decode(s->data, s->len, &r);
        CHECK_EQ(r.status, 0x40);
    }
    p = (pm_artipprog_t){.command = PM_IPPROG_ENABLE | PM_IPPROG_RESET};
    rx(CTL_A, PKT, pm_artipprog_encode(&p, PKT, sizeof(PKT)));
    CHECK(F.settings.ip.dhcp && F.settings.ip.static_ip == 0 && F.settings.ip.gateway == 0 &&
          F.settings.ip.static_mask == 0xFF000000u);
    CHECK_EQ(F.ip_applies, 4);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ TOD / RDM */

static void test_tod(void)
{
    g_context = "tod";
    setup();
    for (unsigned i = 0; i < 600; i++) F.tod[i] = pm_uid_make(0x7FF0, i + 1);
    F.tod_n[0] = 450;
    rx(CTL_B, PKT, mk_poll(PM_POLL_FLAG_REPLY_ON_CHANGE)); /* a poller is no TOD subscriber */
    advance(10);
    pm_arttodrequest_t q = {.net = 0, .command = 0, .adcount = 2, .address = {1, 4}};
    clear_sent();
    rx(CTL_A, PKT, pm_arttodrequest_encode(&q, PKT, sizeof(PKT)));
    CHECK_EQ(count_sent(PM_OP_TOD_DATA, CTL_A), 4); /* 3 blocks for port 1 + 1 for port 4 */
    static const uint8_t counts[3] = {200, 200, 50};
    for (unsigned b = 0; b < 3; b++) {
        const sent_t *s = nth_sent(PM_OP_TOD_DATA, CTL_A, b);
        pm_arttoddata_t t;
        CHECK(s != NULL);
        if (!s) continue;
        CHECK_EQ(pm_arttoddata_decode(s->data, s->len, &t), PM_OK);
        CHECK(t.command_response == PM_TOD_FULL && t.uid_total == 450 && t.block_count == b);
        CHECK(t.uid_count == counts[b] && t.bind_index == 1 && t.port == 1 && t.address == 1);
        CHECK_EQ(pm_uid_device(&t.uids[0]), 1 + 200 * b);
    }
    const sent_t *s4 = nth_sent(PM_OP_TOD_DATA, CTL_A, 3);
    pm_arttoddata_t t4;
    CHECK(s4 != NULL);
    if (s4 && pm_arttoddata_decode(s4->data, s4->len, &t4) == PM_OK) {
        CHECK(t4.uid_total == 0 && t4.uid_count == 0 && t4.bind_index == 4 && t4.command_response == PM_TOD_FULL);
    }
    /* TOD change -> only the requester */
    clear_sent();
    F.tod_n[0] = 3;
    pm_artnet_tod_changed(N, 0);
    CHECK_EQ(count_sent(PM_OP_TOD_DATA, CTL_A), 1);
    CHECK_EQ(count_sent(PM_OP_TOD_DATA, 0), 1);
    pm_artnet_tod_changed(N, 1); /* nobody subscribed */
    CHECK_EQ(count_sent(PM_OP_TOD_DATA, 0), 1);
    /* RDM off -> TodNak */
    rx(CTL_A, PKT, mk_address(1, PM_AC_RDM_DISABLE0));
    clear_sent();
    rx(CTL_A, PKT, pm_arttodrequest_encode(&q, PKT, sizeof(PKT)));
    pm_arttoddata_t t;
    const sent_t *s = nth_sent(PM_OP_TOD_DATA, CTL_A, 0);
    CHECK(s != NULL);
    if (s && pm_arttoddata_decode(s->data, s->len, &t) == PM_OK) {
        CHECK(t.command_response == PM_TOD_NAK && t.uid_count == 0);
    }
    /* ArtTodControl */
    rx(CTL_A, PKT, mk_address(1, PM_AC_RDM_ENABLE0));
    pm_arttodcontrol_t c = {.net = 0, .command = PM_ATC_FLUSH, .address = 1};
    clear_sent();
    rx(CTL_C, PKT, pm_arttodcontrol_encode(&c, PKT, sizeof(PKT)));
    CHECK(F.tod_ctl_count == 1 && F.tod_ctl_cmd == PM_ATC_FLUSH && F.tod_ctl_port == 0);
    CHECK_EQ(count_sent(PM_OP_TOD_DATA, CTL_C), 1);
    c.command = PM_ATC_INC_OFF;
    c.address = 3;
    rx(CTL_C, PKT, pm_arttodcontrol_encode(&c, PKT, sizeof(PKT)));
    CHECK(!F.settings.port[2].rdm_bg_discovery);
    CHECK_EQ(F.last_mask, PM_CHG_PORT(PM_CHG_P_RDM, 2));
    CHECK_EQ(F.tod_ctl_cmd, PM_ATC_INC_OFF);
    c.address = 99; /* nothing addressed */
    clear_sent();
    rx(CTL_C, PKT, pm_arttodcontrol_encode(&c, PKT, sizeof(PKT)));
    CHECK_EQ(F.nsent, 0);
    g_context = NULL;
}

static void test_rdm(void)
{
    g_context = "rdm";
    setup();
    static const uint8_t msg[25] = {0x01, 0x18, 0x7f, 0xf0, 0, 0, 0, 1, 0x7f, 0xf0, 0x80, 0x12, 0x34, 0x56, 5, 1,
                                    0, 0, 0, 0x20, 0x00, 0x60, 0x00, 0x05, 0x66};
    pm_artrdm_t r = {.rdm_ver = 1, .net = 0, .command = 0, .address = 2, .rdm = msg, .rdm_len = sizeof(msg)};
    F.rdm_answer_sync = true;
    F.fifo_avail = 3;
    clear_sent();
    rx(CTL_A, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    CHECK(F.rdm_count == 1 && F.rdm_last.port == 1 && F.rdm_last.rdm_len == 25);
    CHECK(memcmp(F.rdm_buf, msg, 25) == 0);
    const sent_t *s = nth_sent(PM_OP_RDM, CTL_A, 0);
    pm_artrdm_t a;
    CHECK(s != NULL);
    if (s && pm_artrdm_decode(s->data, s->len, &a) == PM_OK) {
        CHECK(a.fifo_avail == 3 && a.fifo_max == 4 && a.address == 2 && a.rdm_len == 25);
    }
    CHECK_EQ(F.nsent, 1);
    /* asynchronous answer by token, unknown token, rejected request */
    F.rdm_answer_sync = false;
    clear_sent();
    rx(CTL_B, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    uint32_t tok = F.rdm_last.token;
    pm_artnet_rdm_response(N, tok + 1000, msg, sizeof(msg));
    CHECK_EQ(F.nsent, 0);
    pm_artnet_rdm_response(N, tok, msg, sizeof(msg));
    CHECK_EQ(count_sent(PM_OP_RDM, CTL_B), 1);
    pm_artnet_rdm_response(N, tok, msg, sizeof(msg)); /* only once */
    CHECK_EQ(count_sent(PM_OP_RDM, CTL_B), 1);
    F.rdm_result = PM_ERR_FULL;
    rx(CTL_C, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    pm_artnet_rdm_response(N, F.rdm_last.token, msg, sizeof(msg));
    CHECK_EQ(count_sent(PM_OP_RDM, CTL_C), 0);
    /* unanswered requests are forgotten */
    F.rdm_result = PM_OK;
    rx(CTL_C, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    tok = F.rdm_last.token;
    advance(PM_ARTNET_RDM_TIMEOUT_MS + 10);
    pm_artnet_rdm_response(N, tok, msg, sizeof(msg));
    CHECK_EQ(count_sent(PM_OP_RDM, CTL_C), 0);
    /* unknown Port-Address: not forwarded */
    unsigned cnt = F.rdm_count;
    r.address = 9;
    rx(CTL_A, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    CHECK_EQ(F.rdm_count, cnt);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ input mode */

static void port4_input(pm_settings_t *s) { s->port[3].direction = PM_PORT_DIR_INPUT; }

static void test_input(void)
{
    g_context = "input";
    setup_with(port4_input);
    advance(10);
    CHECK_EQ(count_sent(PM_OP_POLL, BCAST), 1); /* own ArtPoll to the directed broadcast */
    advance(PM_ARTNET_OWN_POLL_MS);
    CHECK_EQ(count_sent(PM_OP_POLL, BCAST), 2);
    /* an output node of Port-Address 4 answers; another one on universe 5 does not subscribe */
    pm_artpollreply_t pr;
    memset(&pr, 0, sizeof(pr));
    pr.ip = IP(2, 0, 0, 50);
    pr.num_ports = 1;
    pr.port_types[0] = 0x80;
    pr.sw_out[0] = 4;
    rx(IP(2, 0, 0, 50), PKT, pm_artpollreply_encode(&pr, PKT, sizeof(PKT)));
    pr.sw_out[0] = 5;
    rx(IP(2, 0, 0, 51), PKT, pm_artpollreply_encode(&pr, PKT, sizeof(PKT)));
    rx(CTL_A, PKT, mk_poll(0));
    advance(10);
    clear_sent();
    const uint8_t frame[3] = {1, 2, 3};
    pm_artnet_input_frame(N, 3, frame, 3);
    CHECK_EQ(count_sent(PM_OP_DMX, IP(2, 0, 0, 50)), 1);
    CHECK_EQ(count_sent(PM_OP_DMX, CTL_A), 1);
    CHECK_EQ(count_sent(PM_OP_DMX, 0), 2);
    const sent_t *s = nth_sent(PM_OP_DMX, CTL_A, 0);
    pm_artdmx_t d;
    CHECK(s != NULL);
    if (s && pm_artdmx_decode(s->data, s->len, &d) == PM_OK) {
        CHECK(d.port_address == 4 && d.length == 4 && d.data[2] == 3 && d.data[3] == 0 && d.sequence == 1);
        CHECK_EQ(d.physical, 3);
    }
    clear_sent();
    pm_artnet_input_frame(N, 3, frame, 3); /* unchanged: no packet */
    CHECK_EQ(count_sent(PM_OP_DMX, 0), 0);
    advance(PM_ARTNET_INPUT_KEEPALIVE_MS + 10); /* keepalive */
    CHECK_EQ(count_sent(PM_OP_DMX, CTL_A), 1);
    /* signal lost: no keepalive */
    F.st[3].input_signal = false;
    clear_sent();
    advance(2000);
    CHECK_EQ(count_sent(PM_OP_DMX, 0), 0);
    F.st[3].input_signal = true;
    /* controller silent > 10 s, subscriber keeps answering own polls */
    for (unsigned i = 0; i < 12; i++) {
        advance(1000);
        pr.sw_out[0] = 4;
        rx(IP(2, 0, 0, 50), PKT, pm_artpollreply_encode(&pr, PKT, sizeof(PKT)));
    }
    clear_sent();
    const uint8_t frame2[3] = {9, 9, 9};
    pm_artnet_input_frame(N, 3, frame2, 3);
    CHECK_EQ(count_sent(PM_OP_DMX, CTL_A), 0);
    CHECK_EQ(count_sent(PM_OP_DMX, IP(2, 0, 0, 50)), 1);
    /* never broadcast */
    CHECK_EQ(count_sent(PM_OP_DMX, BCAST), 0);
    /* ArtInput disables the input; AcDirectionRx clears the subscriber list */
    pm_artinput_t in = {.bind_index = 4, .num_ports = 1, .input = {1, 0, 0, 0}};
    rx(CTL_A, PKT, pm_artinput_encode(&in, PKT, sizeof(PKT)));
    clear_sent();
    pm_artnet_input_frame(N, 3, frame, 3);
    CHECK_EQ(count_sent(PM_OP_DMX, 0), 0);
    in.input[0] = 0;
    rx(CTL_A, PKT, pm_artinput_encode(&in, PKT, sizeof(PKT)));
    rx(CTL_A, PKT, mk_address(4, PM_AC_DIRECTION_RX0));
    clear_sent();
    pm_artnet_input_frame(N, 3, frame2, 3);
    CHECK_EQ(count_sent(PM_OP_DMX, IP(2, 0, 0, 50)), 0);
    /* local loopback into an output port on the same universe */
    F.settings.port[0].address = 4;
    pm_artnet_settings_changed(N, &F.settings, PM_CHG_PORT(PM_CHG_P_ADDRESS, 0));
    pm_artnet_input_frame(N, 3, frame, 3);
    CHECK(F.out[0].len == 3 && F.out[0].data[2] == 3);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ firmware */

#define IMG_LEN 3001u /* odd: one pad byte */
static uint8_t ALF[PM_ALF_HEADER_LEN + IMG_LEN + 1 + 2048];
static uint32_t ALF_WORDS;

static void build_alf(uint16_t oem)
{
    memset(ALF, 0, sizeof(ALF));
    pm_put_be16(ALF + 2, 0x0102);
    memcpy(ALF + 4, "PM Node test", 12);
    pm_put_be16(ALF + 34 + 2 * 5, oem);
    pm_image_desc_t d;
    memset(&d, 0, sizeof(d));
    d.magic = PM_NODE_IMAGE_MAGIC;
    d.format = PM_NODE_IMAGE_FORMAT;
    d.size = 64;
    d.esta_manufacturer = PM_ESTA_MANUFACTURER;
    d.device_model_id = PM_MODEL_NODE;
    d.fw_version_id = 0x010200;
    d.hw_rev_mask = 1;
    d.image_length = IMG_LEN;
    for (unsigned i = 0; i < IMG_LEN; i++) ALF[PM_ALF_HEADER_LEN + i] = (uint8_t)(i * 7u + 3u);
    d.image_crc32 = pm_crc32(0, ALF + PM_ALF_HEADER_LEN, IMG_LEN);
    pm_image_desc_encode(&d, true, ALF + 546);
    uint32_t words = (IMG_LEN + 1) / 2;
    pm_put_be32(ALF + 1056, words);
    ALF_WORDS = 530 + words;
}

static unsigned alf_blocks(void) { return (ALF_WORDS * 2 + 1023) / 1024; }

static uint8_t send_block(uint32_t src, unsigned b, uint8_t type, uint8_t block_id)
{
    const pm_artfirmwaremaster_t m = {.type = type, .block_id = block_id, .firmware_length_words = ALF_WORDS,
                                      .data = ALF + 1024 * b};
    unsigned before = count_sent(PM_OP_FIRMWARE_REPLY, src);
    rx(src, PKT, pm_artfirmwaremaster_encode(&m, PKT, sizeof(PKT)));
    if (count_sent(PM_OP_FIRMWARE_REPLY, src) != before + 1) return 0xEE; /* no / several replies */
    const sent_t *s = nth_sent(PM_OP_FIRMWARE_REPLY, src, before);
    uint8_t t = 0xEE;
    if (s) pm_artfirmwarereply_decode(s->data, s->len, &t);
    return t;
}

static uint8_t type_of(unsigned b, unsigned nb)
{
    return b == 0 ? PM_FIRM_FIRST : (b == nb - 1 ? PM_FIRM_LAST : PM_FIRM_CONT);
}

static void test_firmware(void)
{
    g_context = "firmware";
    setup();
    build_alf(PM_ARTNET_OEM_NODE);
    unsigned nb = alf_blocks();
    CHECK_EQ(nb, 4);
    CHECK_EQ(send_block(CTL_A, 0, PM_FIRM_FIRST, 0), PM_FIRM_BLOCK_GOOD);
    CHECK_EQ(F.fw_begins, 1);
    CHECK_EQ(F.fw_desc.image_length, IMG_LEN);
    CHECK_EQ(F.fw_writes, 0); /* first block = header only */
    CHECK_EQ(send_block(CTL_B, 1, PM_FIRM_CONT, 1), PM_FIRM_FAIL); /* second source */
    CHECK_EQ(send_block(CTL_A, 1, PM_FIRM_CONT, 1), PM_FIRM_BLOCK_GOOD);
    unsigned writes = F.fw_writes;
    CHECK_EQ(send_block(CTL_A, 1, PM_FIRM_CONT, 1), PM_FIRM_BLOCK_GOOD); /* retransmission */
    CHECK_EQ(F.fw_writes, writes);
    for (unsigned b = 2; b < nb; b++) {
        F.now += 1000;
        CHECK_EQ(send_block(CTL_A, b, type_of(b, nb), (uint8_t)b),
                 b == nb - 1 ? PM_FIRM_ALL_GOOD : PM_FIRM_BLOCK_GOOD);
    }
    CHECK_EQ(F.fw_received, IMG_LEN);
    CHECK(memcmp(F.fw_image, ALF + PM_ALF_HEADER_LEN, IMG_LEN) == 0);
    CHECK_EQ(F.fw_finishes, 1);
    CHECK_EQ(F.fw_aborts, 0);
    advance(PM_ARTNET_REBOOT_DELAY_MS - 20);
    CHECK_EQ(F.fw_reboots, 0);
    advance(30);
    CHECK_EQ(F.fw_reboots, 1);

    /* wrong OEM: rejected in the first block, sink untouched */
    setup();
    build_alf(0x1234);
    CHECK_EQ(send_block(CTL_A, 0, PM_FIRM_FIRST, 0), PM_FIRM_FAIL);
    CHECK_EQ(F.fw_begins, 0);
    clear_sent();
    rx(CTL_A, PKT, mk_poll(0));
    CHECK(strncmp(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0)).node_report, "#000e", 5) == 0);
    /* sink rejects the description */
    setup();
    build_alf(PM_ARTNET_OEM_NODE);
    F.fw_begin_result = PM_FW_ERROR_DESC_HW_REV;
    CHECK_EQ(send_block(CTL_A, 0, PM_FIRM_FIRST, 0), PM_FIRM_FAIL);
    CHECK_EQ(F.fw_aborts, 0); /* begin failed: no session to abort */
    CHECK_EQ(send_block(CTL_A, 1, PM_FIRM_CONT, 1), PM_FIRM_FAIL); /* no session */
    /* FirmwareLength inconsistent with image_length */
    setup();
    build_alf(PM_ARTNET_OEM_NODE);
    ALF_WORDS++;
    CHECK_EQ(send_block(CTL_A, 0, PM_FIRM_FIRST, 0), PM_FIRM_FAIL);
    CHECK_EQ(F.fw_begins, 0);
    /* wrong BlockId */
    setup();
    build_alf(PM_ARTNET_OEM_NODE);
    CHECK_EQ(send_block(CTL_A, 0, PM_FIRM_FIRST, 0), PM_FIRM_BLOCK_GOOD);
    CHECK_EQ(send_block(CTL_A, 1, PM_FIRM_CONT, 2), PM_FIRM_FAIL);
    CHECK(F.fw_aborts == 1 && F.fw_abort_reason == PM_FW_ERROR_SEQUENCE);
    /* FirmLast too early */
    CHECK_EQ(send_block(CTL_A, 0, PM_FIRM_FIRST, 0), PM_FIRM_BLOCK_GOOD);
    CHECK_EQ(send_block(CTL_A, 1, PM_FIRM_LAST, 1), PM_FIRM_FAIL);
    CHECK_EQ(F.fw_finishes, 0);
    /* timeout */
    setup();
    build_alf(PM_ARTNET_OEM_NODE);
    CHECK_EQ(send_block(CTL_A, 0, PM_FIRM_FIRST, 0), PM_FIRM_BLOCK_GOOD);
    advance(PM_ARTNET_FIRMWARE_TIMEOUT_MS - 20);
    CHECK_EQ(F.fw_aborts, 0);
    advance(30);
    CHECK(F.fw_aborts == 1 && F.fw_abort_reason == PM_FW_ERROR_TIMEOUT);
    CHECK_EQ(send_block(CTL_B, 0, PM_FIRM_FIRST, 0), PM_FIRM_BLOCK_GOOD); /* free for a new session */
    /* finish fails (CRC / image invalid) */
    setup();
    build_alf(PM_ARTNET_OEM_NODE);
    F.fw_finish_result = PM_FW_ERROR_CRC;
    for (unsigned b = 0; b < nb; b++) {
        uint8_t want = b == nb - 1 ? PM_FIRM_FAIL : PM_FIRM_BLOCK_GOOD;
        CHECK_EQ(send_block(CTL_A, b, type_of(b, nb), (uint8_t)b), want);
    }
    advance(1000);
    CHECK_EQ(F.fw_reboots, 0);
    /* UBEA */
    CHECK_EQ(send_block(CTL_A, 0, 0x03, 0), PM_FIRM_FAIL);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ diagnostics */

static void test_diag(void)
{
    g_context = "diag";
    setup();
    pm_artnet_diag(N, 0x80, "nobody asked");
    CHECK_EQ(F.nsent, 0);
    pm_artpoll_t p = {.flags = PM_POLL_FLAG_DIAG_ENABLE | PM_POLL_FLAG_DIAG_UNICAST, .diag_priority = 0x40};
    rx(CTL_A, PKT, pm_artpoll_encode(&p, PKT, sizeof(PKT)));
    advance(1000);
    clear_sent();
    pm_artnet_diag(N, 0x10, "too low");
    CHECK_EQ(F.nsent, 0);
    pm_artnet_diag(N, 0x80, "hello");
    CHECK_EQ(count_sent(PM_OP_DIAG_DATA, CTL_A), 1);
    p.flags = PM_POLL_FLAG_DIAG_ENABLE;
    p.diag_priority = 0x10;
    rx(CTL_B, PKT, pm_artpoll_encode(&p, PKT, sizeof(PKT)));
    advance(1000);
    clear_sent();
    pm_artnet_diag(N, 0x10, "two requesters");
    CHECK_EQ(count_sent(PM_OP_DIAG_DATA, BCAST), 1);
    CHECK_EQ(F.nsent, 1);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ review fixes */

static void test_edge_cases(void)
{
    g_context = "edge_rdm_routing";
    setup();
    F.settings.port[3].direction = PM_PORT_DIR_INPUT;
    pm_artnet_settings_changed(N, &F.settings, PM_CHG_PORT(PM_CHG_P_DIRECTION, 3));
    uint8_t msg[25] = {0x01, 0x18, 0x7f, 0xf0, 0, 0, 0, 1, 0x7f, 0xf0, 0, 0, 0, 9, 5, 1, 0, 0, 0, 0x20, 0, 0x60, 0};
    pm_artrdm_t r = {.rdm_ver = 1, .address = 4, .rdm = msg, .rdm_len = sizeof(msg)};
    rx(CTL_A, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    CHECK_EQ(F.rdm_count, 0); /* tube UID on an input port: no RDM there */
    memcpy(msg + 2, NODE_UID.b, 6);
    rx(CTL_A, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    CHECK_EQ(F.rdm_count, 1); /* the node responder is reachable through every port */
    rx(CTL_A, PKT, mk_address(1, PM_AC_RDM_DISABLE0));
    memcpy(msg + 2, "\x7f\xf0\x00\x00\x00\x01", 6);
    r.address = 1;
    rx(CTL_A, PKT, pm_artrdm_encode(&r, PKT, sizeof(PKT)));
    CHECK_EQ(F.rdm_count, 1);

    g_context = "edge_ip_prog";
    setup();
    pm_artipprog_t ip = {.command = PM_IPPROG_ENABLE | PM_IPPROG_PROG_IP | PM_IPPROG_PROG_MASK, .ip = 0,
                         .mask = 0xFF00FF00u};
    rx(CTL_A, PKT, pm_artipprog_encode(&ip, PKT, sizeof(PKT)));
    CHECK_EQ(F.applies, 0);
    CHECK_EQ(F.ip_applies, 0);
    F.apply_result = PM_ERR_IO;
    ip = (pm_artipprog_t){.command = PM_IPPROG_ENABLE | PM_IPPROG_PROG_IP, .ip = IP(10, 0, 0, 5)};
    clear_sent();
    rx(CTL_A, PKT, pm_artipprog_encode(&ip, PKT, sizeof(PKT)));
    CHECK_EQ(F.ip_applies, 0);
    const sent_t *s = nth_sent(PM_OP_IP_PROG_REPLY, CTL_A, 0);
    pm_artipprogreply_t ir;
    CHECK(s != NULL);
    if (s && pm_artipprogreply_decode(s->data, s->len, &ir) == PM_OK) CHECK(ir.ip == NODE_IP && ir.status == 0x40);

    g_context = "edge_firmware";
    setup();
    build_alf(PM_ARTNET_OEM_NODE);
    unsigned nb = alf_blocks();
    for (unsigned b = 0; b < nb; b++) send_block(CTL_A, b, type_of(b, nb), (uint8_t)b);
    CHECK_EQ(send_block(CTL_A, nb - 1, PM_FIRM_LAST, (uint8_t)(nb - 1)), PM_FIRM_ALL_GOOD); /* reply was lost */
    CHECK_EQ(F.fw_finishes, 1);
    CHECK_EQ(send_block(CTL_B, 0, PM_FIRM_FIRST, 0), PM_FIRM_FAIL); /* during the reboot delay */
    CHECK_EQ(F.fw_begins, 1);
    advance(PM_ARTNET_REBOOT_DELAY_MS + 10);
    CHECK_EQ(F.fw_reboots, 1);

    g_context = "edge_tod_flush";
    setup();
    F.tod_n[0] = 5;
    pm_arttodcontrol_t c = {.net = 0, .command = PM_ATC_FLUSH, .address = 1};
    clear_sent();
    rx(CTL_A, PKT, pm_arttodcontrol_encode(&c, PKT, sizeof(PKT)));
    pm_arttoddata_t t;
    s = nth_sent(PM_OP_TOD_DATA, CTL_A, 0);
    CHECK(s != NULL);
    if (s && pm_arttoddata_decode(s->data, s->len, &t) == PM_OK) CHECK_EQ(t.command_response, PM_TOD_NAK);
    clear_sent();
    pm_artnet_tod_changed(N, 0); /* the flushing controller gets the new TOD */
    CHECK_EQ(count_sent(PM_OP_TOD_DATA, CTL_A), 1);

    g_context = "edge_await_reply";
    setup();
    rx(CTL_A, PKT, mk_poll(PM_POLL_FLAG_REPLY_ON_CHANGE));
    advance(10);
    F.sync_cb = false;
    pm_artaddress_t a;
    memset(&a, 0, sizeof(a));
    a.net_switch = a.sub_switch = 0x7F;
    memset(a.sw_in, 0x7F, 4);
    a.sw_out[0] = 0x89;
    a.bind_index = 1;
    a.command = PM_AC_LED_LOCATE; /* visible at once, the address only after settings_changed */
    clear_sent();
    rx(CTL_A, PKT, pm_artaddress_encode(&a, PKT, sizeof(PKT)));
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 0);
    pm_artnet_settings_changed(N, &F.settings, F.last_mask);
    CHECK(count_sent(PM_OP_POLL_REPLY, CTL_A) >= 1);
    CHECK_EQ(reply_of(nth_sent(PM_OP_POLL_REPLY, CTL_A, 0)).sw_out[0], 9);

    g_context = "edge_time_wrap";
    setup();
    F.now = 0xFFFFFF00u;
    pm_artnet_tick(N);
    F.rnd = 500;
    clear_sent();
    rx(CTL_A, PKT, mk_poll(0));
    advance(600); /* crosses 2^32 */
    CHECK_EQ(count_sent(PM_OP_POLL_REPLY, CTL_A), 4);
    rx(CTL_A, PKT, mk_dmx_fill(1, 0, 0, 1, 2));
    F.settings.port[0].failsafe = PM_FAILSAFE_FULL;
    pm_artnet_settings_changed(N, &F.settings, PM_CHG_PORT(PM_CHG_P_OUTPUT, 0));
    advance(PM_ARTNET_DATA_LOSS_MS - 20);
    CHECK_EQ(F.out[0].len, 4);
    advance(30);
    CHECK_EQ(F.out[0].len, 512);

    g_context = "edge_no_mask";
    setup_with(port4_input);
    F.net.mask = 0;
    pm_artnet_net_changed(N);
    clear_sent();
    advance(PM_ARTNET_OWN_POLL_MS + 10);
    CHECK_EQ(count_sent(PM_OP_POLL, 0), 0); /* never 255.255.255.255 */
    g_context = NULL;
}

void test_artnet_node(void)
{
    test_edge_cases();
    test_poll_reply();
    test_unsolicited();
    test_merge_unit();
    test_dmx_merge();
    test_sequence();
    test_cancel_merge();
    test_failsafe();
    test_sync();
    test_address();
    test_ip_prog();
    test_tod();
    test_rdm();
    test_input();
    test_firmware();
    test_diag();
}
