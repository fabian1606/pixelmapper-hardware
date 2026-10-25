/*
 * fuzz_main.c - deterministic random/mutation test of the network-facing parsers (pm_fuzz, built with
 * -fsanitize=address,undefined when the compiler supports it, see CMakeLists.txt option PM_FUZZ_SANITIZE).
 *
 *   1. pm_node_handle_packet: mutated Art-Net packets (every opcode the node handles, valid seeds first),
 *      ArtRdm with RDM to the node UID / broadcast / devices (checksum mostly repaired after the mutation so the
 *      responder's PID handlers are reached), ArtFirmwareMaster sessions -> pm_ota_session (RAM backend, vn_fw.c)
 *   2. pm_rdm_responder_handle directly: every table PID x GET/SET x random PDL, plus mutated frames
 *   3. pm_rdm_ctrl fed with random/mutated line results (DUB, DISC_MUTE, forwarded responses)
 *   4. stand-alone decoders (Art-Net packets, RDM, DUB) on random input
 * Invariants: no sanitizer report, every datagram the node sends is Art-Net, every RDM answer decodes and goes
 * back to the requester with its TN, no answer to broadcasts, TOD stays sorted and bounded.
 *
 * Usage: pm_fuzz [iterations] [seed]  (defaults 100000 / 1)
 */
#include <stdalign.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pm_artnet_packets.h"
#include "pm_node.h"
#include "pm_pids.h"
#include "pm_rdm_ctrl.h"
#include "vn.h"

static unsigned long g_checks, g_failures;

#define CHECK(c)                                                                                     \
    do {                                                                                             \
        g_checks++;                                                                                  \
        if (!(c)) {                                                                                  \
            if (g_failures++ < 20) fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
        }                                                                                            \
    } while (0)

/* ------------------------------------------------------------------------------------------------ PRNG */

static uint64_t s_rng;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 7;
    s_rng ^= s_rng << 17;
    return (uint32_t)(s_rng >> 16);
}

static uint32_t rnd_n(uint32_t n) { return n ? rnd() % n : 0; }
static bool chance(unsigned pct) { return rnd_n(100) < pct; }
static void rnd_fill(uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)rnd();
}

/* ------------------------------------------------------------------------------------------------ fake platform */

#define IP(a, b, c, d) ((uint32_t)(((uint32_t)(a) << 24) | ((b) << 16) | ((c) << 8) | (d)))
#define NODE_IP IP(2, 0, 0, 10)
#define CTL_IP IP(2, 0, 0, 100)

static uint32_t s_now = 1000;
uint32_t vn_now_ms(void) { return s_now; }

static pm_settings_t s_cfg;
static pm_change_mask_t s_pend[PM_SRC_SYSTEM + 1];
static pm_artnet_net_info_t s_net;
static pm_node_t *N;
static alignas(max_align_t) uint8_t MEM[96 * 1024];

static const pm_uid_t NODE_UID = {{0x7F, 0xF0, 0x80, 0x12, 0x34, 0x56}};
static const pm_uid_t DEV_UID = {{0x7F, 0xF0, 0x00, 0x00, 0x00, 0x42}};
static const pm_uid_t CTL_UID = {{0x7F, 0xF0, 0x7F, 0x00, 0x00, 0x01}};

/* last RDM request handed to the node (for the reply checks) */
static struct {
    bool active;
    bool expect_none;    /* broadcast/vendorcast: never answered by the node */
    uint8_t tn;
    pm_uid_t src;
    unsigned answers;
} s_req;

static uint32_t f_now(void *ctx) { (void)ctx; return s_now; }
static uint32_t f_random(void *ctx) { (void)ctx; return rnd(); }
static void f_config_get(void *ctx, pm_settings_t *out) { (void)ctx; *out = s_cfg; }

static void store(const pm_settings_t *next, pm_change_source_t src)
{
    static pm_settings_t t;
    t = *next;
    pm_settings_sanitize(&t);
    s_pend[src] |= pm_settings_diff(&s_cfg, &t);
    s_cfg = t;
}

static pm_err_t f_config_modify(void *ctx, pm_node_modify_fn fn, void *fctx, pm_change_source_t src)
{
    (void)ctx;
    static pm_settings_t t;
    t = s_cfg;
    fn(fctx, &t);
    store(&t, src);
    return PM_OK;
}

static pm_err_t f_factory(void *ctx, pm_change_source_t src)
{
    (void)ctx;
    static pm_settings_t t;
    pm_settings_defaults(&t);
    t.hw_revision_override = s_cfg.hw_revision_override;
    store(&t, src);
    return PM_OK;
}

static void check_rdm_reply(const uint8_t *rdm_nosc, size_t len)
{
    pm_rdm_msg_t m;
    CHECK(pm_rdm_decode_nosc(rdm_nosc, len, &m) == PM_OK);
    if (!s_req.active) return;
    s_req.answers++;
    CHECK(!s_req.expect_none);
    CHECK(m.tn == s_req.tn);
    CHECK(pm_uid_equal(&m.dest, &s_req.src));
    CHECK(pm_uid_equal(&m.src, &NODE_UID));
}

static pm_err_t f_send(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    (void)ctx;
    uint16_t op = 0;
    CHECK(ip != 0);
    CHECK(port == PM_ARTNET_PORT);
    CHECK(len <= 1472u); /* one Ethernet frame (ArtTodData with 200 UIDs = 1228) */
    CHECK(pm_artnet_peek_opcode(data, len, &op) == PM_OK);
    if (op == PM_OP_RDM) {
        pm_artrdm_t r;
        CHECK(pm_artrdm_decode(data, len, &r) == PM_OK);
        if (r.rdm_len > 0) check_rdm_reply(r.rdm, r.rdm_len);
    } else if (op == PM_OP_POLL_REPLY) {
        pm_artpollreply_t r;
        CHECK(pm_artpollreply_decode(data, len, &r) == PM_OK);
        CHECK(r.bind_index >= 1 && r.bind_index <= PM_NUM_PORTS);
    } else if (op == PM_OP_TOD_DATA) {
        pm_arttoddata_t t;
        CHECK(pm_arttoddata_decode(data, len, &t) == PM_OK);
    }
    return PM_OK;
}

static void f_net_info(void *ctx, pm_artnet_net_info_t *out) { (void)ctx; *out = s_net; }
static void f_apply_ip(void *ctx, const pm_ip_settings_t *ip) { (void)ctx; (void)ip; }
static void f_renew(void *ctx) { (void)ctx; }
static pm_err_t f_configure(void *ctx, pm_port_idx_t port, const pm_node_port_cfg_t *cfg)
{
    (void)ctx;
    CHECK(port < PM_NUM_PORTS);
    (void)cfg;
    return PM_OK;
}

static void f_output(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    (void)ctx; (void)defer;
    CHECK(port < PM_NUM_PORTS);
    CHECK(len <= 512);
    volatile uint8_t sink = 0;
    for (uint16_t i = 0; i < len; i++) sink ^= data[i]; /* touch every byte: ASan sees reads past the frame */
    (void)sink;
}

static void f_commit(void *ctx) { (void)ctx; }
static void f_clear(void *ctx, pm_port_idx_t port) { (void)ctx; CHECK(port < PM_NUM_PORTS); }
static void f_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out)
{
    (void)ctx; (void)port;
    *out = (pm_artnet_port_status_t){.output_active = true, .input_signal = true};
}

static uint16_t f_input_read(void *ctx, pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    (void)ctx; (void)port;
    uint16_t n = (uint16_t)(1u + rnd_n(cap));
    rnd_fill(out, n);
    return n;
}

static pm_err_t f_submit(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *wire, uint16_t len)
{
    (void)ctx; (void)token;
    CHECK(port < PM_NUM_PORTS);
    CHECK(len >= PM_RDM_HEADER_LEN + 2u && len <= PM_RDM_MAX_MSG_LEN && wire[0] == PM_RDM_START_CODE);
    return chance(80) ? PM_OK : PM_ERR_FULL;
}

static uint8_t f_qfree(void *ctx, pm_port_idx_t port) { (void)ctx; (void)port; return (uint8_t)rnd_n(9); }
static uint8_t f_qlen(void *ctx, pm_port_idx_t port) { (void)ctx; (void)port; return 8; }

static uint16_t f_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)ctx; (void)port;
    uint16_t total = (uint16_t)rnd_n(600);
    uint16_t n = total < cap ? total : cap;
    for (uint16_t i = 0; i < n; i++) rnd_fill(out[i].b, 6);
    return total;
}

static void f_tod_control(void *ctx, pm_port_idx_t port, uint8_t cmd) { (void)ctx; (void)cmd; CHECK(port < PM_NUM_PORTS); }
static pm_err_t f_catch_start(void *ctx, pm_port_idx_t port, const pm_uid_t *t)
{
    (void)ctx; (void)t;
    CHECK(port < PM_NUM_PORTS);
    return PM_OK;
}
static void f_catch_abort(void *ctx) { (void)ctx; }
static void f_catch_status(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    (void)ctx;
    *port = 0;
    memset(out, 0, sizeof(*out));
}
static void f_reset(void *ctx, uint8_t kind) { (void)ctx; (void)kind; }
static void f_ui(void *ctx, const pm_node_ui_event_t *ev)
{
    (void)ctx;
    if (ev->data && ev->len) {
        volatile uint8_t sink = 0;
        for (uint16_t i = 0; i < ev->len; i++) sink ^= ev->data[i];
        (void)sink;
    }
}
static void f_live(void *ctx, const pm_artnet_port_live_t *live) { (void)ctx; (void)live; }

static void deliver_settings(void)
{
    for (int src = 0; src <= PM_SRC_SYSTEM; src++) {
        pm_change_mask_t m = s_pend[src];
        s_pend[src] = 0;
        if (m) pm_node_settings_changed(N, m, (pm_change_source_t)src);
    }
}

static void node_setup(void)
{
    pm_settings_defaults(&s_cfg);
    for (unsigned p = 0; p < PM_NUM_PORTS; p++) {
        s_cfg.port[p].enabled = true;
        s_cfg.port[p].rdm_enabled = true;
        s_cfg.port[p].address = (pm_port_address_t)p;
    }
    s_cfg.port[3].direction = PM_PORT_DIR_INPUT;
    s_cfg.port[2].rdm_enabled = false;
    s_net = (pm_artnet_net_info_t){.ip = NODE_IP, .mask = 0xFF000000u, .link_up = true,
                                   .mac = {0x02, 0x00, 0x12, 0x34, 0x56, 0x78}};
    vn_fw_init(0);
    static pm_node_ops_t ops = {
        .now_ms = f_now, .random = f_random, .config_get = f_config_get, .config_modify = f_config_modify,
        .config_factory_reset = f_factory, .send_udp = f_send, .net_info = f_net_info, .net_apply_ip = f_apply_ip,
        .net_renew_dhcp = f_renew, .port_configure = f_configure, .port_output = f_output, .port_commit = f_commit,
        .port_clear = f_clear, .port_status = f_status, .port_input_read = f_input_read,
        .port_rdm_submit = f_submit, .port_rdm_queue_free = f_qfree, .port_rdm_queue_len = f_qlen,
        .tod_get = f_tod_get, .tod_control = f_tod_control, .boot_catch_start = f_catch_start,
        .boot_catch_abort = f_catch_abort, .boot_catch_status = f_catch_status, .reset = f_reset,
        .ui_event = f_ui, .port_live = f_live,
    };
    ops.fw_sink = vn_fw_sink();
    const pm_node_identity_t id = {
        .artnet = {.esta_man = PM_ESTA_MANUFACTURER, .oem = PM_ARTNET_OEM_NODE, .vers_info = 0x0100,
                   .uid = NODE_UID, .num_ports = PM_NUM_PORTS},
        .responder = {.uid = NODE_UID, .model_id = PM_MODEL_NODE, .sw_version_id = 0x00010000,
                      .sw_version_label = "1.0.0", .model_description = "Pixelmapper Node",
                      .manufacturer_label = "Prototype", .hw_revision = 0, .boot_sw_version_id = 0x00050500,
                      .boot_sw_version_label = "ESP-IDF v5.5 bootloader"},
    };
    if (pm_node_size() > sizeof(MEM)) {
        fprintf(stderr, "MEM too small (%zu)\n", pm_node_size());
        exit(2);
    }
    N = pm_node_init(MEM, &id, &ops, &s_cfg);
    if (N == NULL) {
        fprintf(stderr, "pm_node_init failed\n");
        exit(2);
    }
}

/* ------------------------------------------------------------------------------------------------ RDM seeds */

static uint16_t s_pids[160];
static unsigned s_npids;

static void add_pid(uint16_t pid)
{
    for (unsigned i = 0; i < s_npids; i++) {
        if (s_pids[i] == pid) return;
    }
    if (s_npids < sizeof(s_pids) / sizeof(s_pids[0])) s_pids[s_npids++] = pid;
}

static const uint16_t STD_PIDS[] = {
    PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_PID_STATUS_MESSAGES, PM_RDM_PID_STATUS_ID_DESCRIPTION,
    PM_RDM_PID_CLEAR_STATUS_ID, PM_RDM_PID_SUPPORTED_PARAMETERS, PM_RDM_PID_PARAMETER_DESCRIPTION,
    PM_RDM_PID_DEVICE_INFO, PM_RDM_PID_DEVICE_LABEL, PM_RDM_PID_IDENTIFY_DEVICE, PM_RDM_PID_RESET_DEVICE,
    PM_RDM_PID_DISC_UNIQUE_BRANCH, PM_RDM_PID_DISC_MUTE, PM_RDM_PID_DISC_UN_MUTE,
};

/** Random RDM request (with start code) into buf; returns its length. */
static size_t make_rdm(uint8_t *buf, size_t cap, pm_uid_t *dest_out)
{
    pm_rdm_msg_t m;
    memset(&m, 0, sizeof(m));
    switch (rnd_n(8)) {
    case 0: memset(m.dest.b, 0xFF, 6); break;                           /* broadcast */
    case 1: m.dest = NODE_UID; memset(m.dest.b + 2, 0xFF, 4); break;   /* our vendorcast */
    case 2: m.dest = DEV_UID; break;
    case 3: rnd_fill(m.dest.b, 6); break;
    default: m.dest = NODE_UID; break;
    }
    m.src = CTL_UID;
    m.tn = (uint8_t)rnd();
    m.port_or_resp = (uint8_t)rnd_n(5);
    m.sub_device = chance(90) ? 0 : (chance(50) ? 0xFFFF : (uint16_t)rnd());
    static const uint8_t CCS[] = {PM_RDM_CC_GET, PM_RDM_CC_SET, PM_RDM_CC_DISCOVERY, 0x31, 0x00};
    m.cc = chance(95) ? CCS[rnd_n(chance(90) ? 2 : 5)] : (uint8_t)rnd();
    m.pid = chance(95) ? s_pids[rnd_n(s_npids)] : (uint16_t)rnd();
    static uint8_t pd[PM_RDM_MAX_PDL];
    unsigned pdl;
    static const uint8_t TYPICAL[] = {0, 0, 0, 1, 1, 2, 2, 4, 5, 7, 8, 9, 64, 228};
    switch (rnd_n(5)) {
    case 4: pdl = TYPICAL[rnd_n(sizeof(TYPICAL))]; break;
    case 0: pdl = 0; break;
    case 1: pdl = rnd_n(12); break;
    case 2: pdl = rnd_n(40); break;
    default: pdl = rnd_n(PM_RDM_MAX_PDL + 1); break;
    }
    rnd_fill(pd, pdl);
    if (pdl >= 4 && chance(60)) pm_put_be32(pd, PM_RDM_RESP_INTERFACE_ID); /* E1.37-2 interface id */
    if (pdl >= 2 && chance(30)) pm_put_be16(pd, s_pids[rnd_n(s_npids)]);   /* PARAMETER_DESCRIPTION */
    if (pdl >= 1 && chance(30)) pd[0] = (uint8_t)rnd_n(5);                  /* status types, booleans */
    m.pdl = (uint8_t)pdl;
    m.pd = pd;
    if (dest_out) *dest_out = m.dest;
    int n = pm_rdm_encode(&m, buf, cap);
    return n > 0 ? (size_t)n : 0;
}

/** Repair message length / PDL / checksum of a (mutated) RDM frame with start code. */
static void fix_rdm(uint8_t *buf, size_t len)
{
    if (len < PM_RDM_HEADER_LEN + 2u) return;
    size_t pdl = len - PM_RDM_HEADER_LEN - 2u;
    if (pdl > PM_RDM_MAX_PDL) pdl = PM_RDM_MAX_PDL;
    buf[0] = PM_RDM_START_CODE;
    buf[1] = PM_RDM_SUB_START_CODE;
    buf[2] = (uint8_t)(PM_RDM_HEADER_LEN + pdl);
    buf[23] = (uint8_t)pdl;
    pm_put_be16(buf + PM_RDM_HEADER_LEN + pdl, pm_rdm_checksum(buf, PM_RDM_HEADER_LEN + pdl));
}

static size_t mutate(uint8_t *buf, size_t len, size_t cap)
{
    unsigned k = rnd_n(4);
    for (unsigned i = 0; i <= k; i++) {
        switch (rnd_n(6)) {
        case 0: if (len) buf[rnd_n((uint32_t)len)] = (uint8_t)rnd(); break;
        case 1: if (len) buf[rnd_n((uint32_t)len)] ^= (uint8_t)(1u << rnd_n(8)); break;
        case 2: len = rnd_n((uint32_t)len + 1); break;                  /* truncate */
        case 3: {                                                       /* extend */
            size_t add = rnd_n(64);
            if (len + add > cap) add = cap - len;
            rnd_fill(buf + len, add);
            len += add;
            break;
        }
        case 4: if (len) buf[rnd_n((uint32_t)len)] = chance(50) ? 0x00 : 0xFF; break;
        default: break;
        }
    }
    return len;
}

/* ------------------------------------------------------------------------------------------------ Art-Net seeds */

static uint8_t s_fw_block[PM_FIRM_BLOCK_BYTES];

static size_t make_artnet(uint8_t *buf, size_t cap)
{
    int n = 0;
    s_req.active = false;
    switch (rnd_n(13)) {
    case 0: {
        pm_artpoll_t p = {.flags = (uint8_t)rnd(), .diag_priority = (uint8_t)rnd(),
                          .target_top = (uint16_t)rnd_n(8), .target_bottom = (uint16_t)rnd_n(4)};
        n = pm_artpoll_encode(&p, buf, cap);
        break;
    }
    case 1: {
        static uint8_t d[512];
        rnd_fill(d, sizeof(d));
        pm_artdmx_t x = {.sequence = (uint8_t)rnd(), .physical = (uint8_t)rnd_n(3),
                         .port_address = (pm_port_address_t)rnd_n(5), .length = (uint16_t)(1u + rnd_n(512)),
                         .data = d};
        n = pm_artdmx_encode(&x, buf, cap);
        break;
    }
    case 2: n = pm_artsync_encode(buf, cap); break;
    case 3: {
        pm_artaddress_t a;
        memset(&a, 0, sizeof(a));
        a.net_switch = (uint8_t)rnd();
        a.sub_switch = (uint8_t)rnd();
        a.bind_index = (uint8_t)rnd_n(6);
        for (unsigned i = 0; i < 4; i++) {
            a.sw_in[i] = (uint8_t)rnd();
            a.sw_out[i] = (uint8_t)rnd();
        }
        a.command = (uint8_t)(chance(70) ? rnd_n(0xF0) : rnd());
        if (chance(30)) snprintf(a.short_name, sizeof(a.short_name), "fz%u", (unsigned)rnd_n(100));
        n = pm_artaddress_encode(&a, buf, cap);
        break;
    }
    case 4: {
        pm_artinput_t in = {.bind_index = (uint8_t)rnd_n(6), .num_ports = 1, .input = {(uint8_t)rnd()}};
        n = pm_artinput_encode(&in, buf, cap);
        break;
    }
    case 5: {
        pm_artipprog_t ip = {.command = (uint8_t)rnd(), .ip = rnd(), .mask = chance(50) ? 0xFFFF0000u : rnd(),
                             .gateway = rnd()};
        n = pm_artipprog_encode(&ip, buf, cap);
        break;
    }
    case 6: {
        pm_arttodrequest_t t;
        memset(&t, 0, sizeof(t));
        t.adcount = (uint8_t)rnd_n(PM_TOD_MAX_ADDRESSES + 1);
        for (unsigned i = 0; i < t.adcount; i++) t.address[i] = (uint8_t)rnd_n(5);
        n = pm_arttodrequest_encode(&t, buf, cap);
        break;
    }
    case 7: {
        pm_arttodcontrol_t t = {.net = 0, .command = (uint8_t)rnd_n(6), .address = (uint8_t)rnd_n(5)};
        n = pm_arttodcontrol_encode(&t, buf, cap);
        break;
    }
    case 8:
    case 9: {
        uint8_t rdm[PM_RDM_MAX_MSG_LEN];
        pm_uid_t dest;
        size_t rl = make_rdm(rdm, sizeof(rdm), &dest);
        pm_artrdm_t r = {.rdm_ver = 1, .net = 0, .command = 0, .address = (uint8_t)rnd_n(5), .rdm = rdm + 1,
                         .rdm_len = rl ? rl - 1u : 0};
        n = pm_artrdm_encode(&r, buf, cap);
        s_req.active = true;
        s_req.expect_none = !pm_uid_equal(&dest, &NODE_UID);
        s_req.tn = rdm[15];
        s_req.src = CTL_UID;
        break;
    }
    case 10: {
        /* .alf block: OEM list with ours, description block copy, random rest */
        rnd_fill(s_fw_block, sizeof(s_fw_block));
        if (chance(70)) pm_put_be16(s_fw_block + 34, PM_ARTNET_OEM_NODE);
        if (chance(70)) {
            pm_image_desc_t d;
            memset(&d, 0, sizeof(d));
            d.magic = PM_NODE_IMAGE_MAGIC;
            d.format = PM_NODE_IMAGE_FORMAT;
            d.size = PM_IMAGE_DESC_SIZE;
            d.esta_manufacturer = PM_ESTA_MANUFACTURER;
            d.device_model_id = PM_MODEL_NODE;
            d.hw_rev_mask = 1;
            d.image_length = 1024u + rnd_n(8192);
            pm_image_desc_encode(&d, true, s_fw_block + 546);
        }
        pm_artfirmwaremaster_t f = {.type = (uint8_t)rnd_n(4), .block_id = (uint8_t)rnd_n(4),
                                    .firmware_length_words = chance(50) ? rnd() : 530u + rnd_n(5000),
                                    .data = s_fw_block};
        n = pm_artfirmwaremaster_encode(&f, buf, cap);
        break;
    }
    case 11: {
        pm_artpollreply_t r;
        memset(&r, 0, sizeof(r));
        r.num_ports = 1;
        r.port_types[0] = 0x80;
        r.sw_out[0] = (uint8_t)rnd_n(5);
        n = pm_artpollreply_encode(&r, buf, cap);
        break;
    }
    default: {
        uint16_t ops[] = {PM_OP_RDM_SUB, PM_OP_DIAG_DATA, PM_OP_TOD_DATA, PM_OP_NZS, PM_OP_FIRMWARE_REPLY,
                          PM_OP_IP_PROG_REPLY, PM_OP_COMMAND};
        size_t len = 10u + rnd_n(600);
        rnd_fill(buf, len);
        memcpy(buf, "Art-Net", 8);
        pm_put_le16(buf + 8, ops[rnd_n(sizeof(ops) / sizeof(ops[0]))]);
        n = (int)len;
        break;
    }
    }
    return n > 0 ? (size_t)n : 0;
}

/* ------------------------------------------------------------------------------------------------ 1: node */

static void fuzz_node(unsigned long iters)
{
    static uint8_t buf[PM_ARTNET_MAX_PACKET + 64];
    for (unsigned long i = 0; i < iters; i++) {
        size_t len = make_artnet(buf, PM_ARTNET_MAX_PACKET);
        bool is_rdm = s_req.active;
        if (chance(35)) {
            len = mutate(buf, len, PM_ARTNET_MAX_PACKET);
            if (is_rdm && len >= 24u + PM_RDM_HEADER_LEN + 1u && chance(70)) {
                /* repair the RDM part (ArtRdm carries no start code: work on a copy with one) */
                uint8_t w[PM_RDM_MAX_MSG_LEN];
                size_t rl = len - 24u + 1u;
                if (rl > sizeof(w)) rl = sizeof(w);
                memcpy(w + 1, buf + 24, rl - 1u);
                fix_rdm(w, rl);
                memcpy(buf + 24, w + 1, rl - 1u);
                len = 24u + rl - 1u;
                s_req.tn = w[15];
                memcpy(s_req.src.b, w + 9, 6);
                pm_uid_t d;
                memcpy(d.b, w + 3, 6);
                s_req.expect_none = !pm_uid_equal(&d, &NODE_UID);
            } else if (is_rdm) {
                s_req.active = false; /* reply content is no longer predictable */
            }
            if (len >= 10 && chance(10)) pm_put_be16(buf + 10, (uint16_t)rnd_n(20)); /* ProtVer */
        }
        uint32_t dst = chance(50) ? 0xFFFFFFFFu : (chance(50) ? NODE_IP : IP(2, 255, 255, 255));
        uint32_t src = chance(90) ? CTL_IP : IP(2, 0, 0, (uint8_t)(101 + rnd_n(20)));
        pm_node_handle_packet(N, src, PM_ARTNET_PORT, dst, buf, len);
        s_req.active = false;
        if (chance(20)) s_now += rnd_n(chance(5) ? 40000 : 50);
        if (chance(20)) {
            pm_node_tick(N);
            vn_fw_poll();
        }
        if (chance(30)) deliver_settings();
        if (chance(1)) {
            pm_image_desc_t d;
            bool have;
            vn_fw_take_reboot(&d, &have);
        }
        if (chance(2)) {
            /* port events with random content */
            static uint8_t data[PM_RDM_MAX_MSG_LEN];
            size_t dl = make_rdm(data, sizeof(data), NULL);
            if (chance(50)) dl = mutate(data, dl, sizeof(data));
            pm_node_port_event_t ev = {.type = (pm_node_port_ev_type_t)rnd_n(6), .port = (pm_port_idx_t)rnd_n(4),
                                       .token = rnd(), .value = (int32_t)rnd_n(6), .len = (uint16_t)dl,
                                       .data = data};
            pm_node_port_event(N, &ev);
        }
        if (chance(2)) {
            uint8_t w[PM_RDM_MAX_MSG_LEN];
            size_t wl = make_rdm(w, sizeof(w), NULL);
            if (chance(30)) {
                wl = mutate(w, wl, sizeof(w));
                if (chance(70)) fix_rdm(w, wl);
            }
            pm_node_ui_rdm_request(N, (pm_port_idx_t)rnd_n(4), rnd() & 0x7FFFFFFFu, w, (uint16_t)wl);
        }
        if (chance(1)) s_now += 0x7FFFFFFFu + rnd_n(3); /* far jumps: u32 wrap-around of all timestamps */
    }
}

/* ------------------------------------------------------------------------------------------------ 2: responder */

static void fuzz_responder(unsigned long iters)
{
    pm_rdm_responder_t *r = pm_node_responder(N);
    uint8_t req[PM_RDM_MAX_MSG_LEN + 16], out[PM_RDM_MAX_MSG_LEN];
    unsigned long acks = 0, nacks = 0;
    for (unsigned long i = 0; i < iters; i++) {
        pm_uid_t dest;
        size_t len = make_rdm(req, sizeof(req), &dest);
        bool fixed = true;
        if (chance(30)) {
            len = mutate(req, len, sizeof(req));
            fixed = chance(70);
            if (fixed) fix_rdm(req, len);
            memcpy(dest.b, req + 3, len >= 9 ? 6 : 0);
        }
        size_t cap = chance(95) ? sizeof(out) : rnd_n(sizeof(out));
        int n = pm_rdm_responder_handle(r, req, len, out, cap);
        if (n > 0) {
            pm_rdm_msg_t m;
            CHECK((size_t)n <= cap);
            CHECK(pm_rdm_decode(out, (size_t)n, &m) == PM_OK);
            CHECK(pm_uid_equal(&dest, &NODE_UID)); /* no answer to broadcast / vendorcast */
            if (fixed && len >= PM_RDM_HEADER_LEN + 2u) CHECK(m.tn == req[15]);
            CHECK(m.cc == PM_RDM_CC_GET_RESPONSE || m.cc == PM_RDM_CC_SET_RESPONSE);
            if (m.port_or_resp == PM_RDM_RESP_NACK_REASON) CHECK(m.pdl == 2);
            if (m.port_or_resp == PM_RDM_RESP_ACK) acks++;
            else nacks++;
        }
        if (chance(10)) pm_rdm_responder_poll(r);
        if (chance(5)) deliver_settings();
        if (chance(5)) s_now += rnd_n(5000);
    }
    printf("pm_fuzz: responder %lu ACK, %lu NACK\n", acks, nacks);
    CHECK(acks > iters / 50); /* the generator reaches the PID handlers */
}

/* ------------------------------------------------------------------------------------------------ 3: controller */

static void cb_resp(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    (void)ctx; (void)port; (void)token;
    pm_rdm_msg_t m;
    CHECK(pm_rdm_decode(data, len, &m) == PM_OK);
}

static void cb_tod(void *ctx, pm_port_idx_t port) { (void)ctx; (void)port; }
static void cb_catch(void *ctx, pm_port_idx_t port, uint8_t st) { (void)ctx; (void)port; (void)st; }

static void fuzz_ctrl(unsigned long iters)
{
    static pm_rdm_ctrl_t c;
    static pm_rdm_txn_t t;
    static pm_rdm_request_t q;
    static pm_uid_t tod[PM_RDM_TOD_MAX];
    const pm_rdm_ctrl_cb_t cb = {.on_response = cb_resp, .on_tod_changed = cb_tod, .on_boot_catch = cb_catch};
    pm_rdm_ctrl_init(&c, 0, &NODE_UID, &cb);
    pm_rdm_ctrl_set_enabled(&c, true, s_now);
    uint8_t rx[PM_RDM_MAX_MSG_LEN + 8];
    for (unsigned long i = 0; i < iters; i++) {
        if (chance(5)) {
            size_t l = make_rdm(q.data, sizeof(q.data), NULL);
            q.len = (uint16_t)l;
            q.token = rnd();
            pm_rdm_ctrl_submit(&c, &q);
        }
        if (chance(1)) pm_rdm_ctrl_flush(&c, s_now);
        if (chance(1)) {
            pm_uid_t u = DEV_UID;
            pm_rdm_ctrl_boot_catch_start(&c, &u, s_now);
        }
        if (chance(1)) pm_rdm_ctrl_set_background(&c, chance(50), 100u + rnd_n(5000));
        s_now += rnd_n(30);
        if (!pm_rdm_ctrl_next(&c, s_now, &t)) continue;
        CHECK(t.len <= PM_RDM_MAX_MSG_LEN);
        pm_rdm_rx_t res = {.status = (pm_rdm_rx_status_t)rnd_n(3), .data = rx};
        size_t rl = 0;
        switch (rnd_n(4)) {
        case 0: { /* DUB answer */
            pm_uid_t u;
            rnd_fill(u.b, 6);
            if (chance(50)) u.b[0] = 0x7F, u.b[1] = 0xF0, u.b[2] = 0, u.b[3] = 0, u.b[4] = 0;
            int n = pm_rdm_dub_encode(&u, rnd_n(8), rx, sizeof(rx));
            rl = n > 0 ? (size_t)n : 0;
            break;
        }
        case 1: { /* response to the request: swap src/dest, keep TN */
            if (t.len >= PM_RDM_HEADER_LEN + 2u) {
                pm_rdm_msg_t m;
                if (pm_rdm_decode(t.data, t.len, &m) == PM_OK) {
                    pm_rdm_msg_t a;
                    pm_rdm_make_response(&m, &m.dest, (uint8_t)rnd_n(4), 0, &a);
                    uint8_t pd[PM_RDM_MAX_PDL];
                    rnd_fill(pd, sizeof(pd));
                    a.pdl = (uint8_t)rnd_n(chance(80) ? 9 : PM_RDM_MAX_PDL + 1);
                    a.pd = pd;
                    int n = pm_rdm_encode(&a, rx, sizeof(rx));
                    rl = n > 0 ? (size_t)n : 0;
                }
            }
            break;
        }
        case 2: rl = rnd_n(sizeof(rx)); rnd_fill(rx, rl); break;
        default: rl = 0; break;
        }
        if (chance(20)) {
            rl = mutate(rx, rl, sizeof(rx));
            if (rl > PM_RDM_MAX_MSG_LEN) rl = PM_RDM_MAX_MSG_LEN;
        }
        res.len = (uint16_t)rl;
        pm_rdm_ctrl_done(&c, s_now, &res);
        uint16_t n = pm_rdm_ctrl_tod(&c, tod, PM_RDM_TOD_MAX);
        CHECK(n <= PM_RDM_TOD_MAX);
        for (uint16_t k = 1; k < n && k < PM_RDM_TOD_MAX; k++) {
            CHECK(pm_uid_to_u64(&tod[k - 1]) < pm_uid_to_u64(&tod[k]));
        }
    }
}

/* ------------------------------------------------------------------------------------------------ 4: decoders */

static void fuzz_decoders(unsigned long iters)
{
    static uint8_t buf[1600];
    for (unsigned long i = 0; i < iters; i++) {
        size_t len = rnd_n(chance(80) ? 300 : sizeof(buf));
        /* heap copy of exactly `len` bytes: ASan catches every read past the end */
        uint8_t *p = malloc(len ? len : 1);
        rnd_fill(buf, len);
        if (len >= 10 && chance(80)) {
            memcpy(buf, "Art-Net", 8);
            static const uint16_t OPS[] = {PM_OP_POLL, PM_OP_POLL_REPLY, PM_OP_DMX, PM_OP_SYNC, PM_OP_ADDRESS,
                                           PM_OP_INPUT, PM_OP_IP_PROG, PM_OP_IP_PROG_REPLY, PM_OP_TOD_REQUEST,
                                           PM_OP_TOD_DATA, PM_OP_TOD_CONTROL, PM_OP_RDM, PM_OP_FIRMWARE_MASTER,
                                           PM_OP_FIRMWARE_REPLY};
            pm_put_le16(buf + 8, OPS[rnd_n(sizeof(OPS) / sizeof(OPS[0]))]);
            if (len >= 12) pm_put_be16(buf + 10, 14);
        }
        memcpy(p, buf, len);
        union {
            pm_artpoll_t poll;
            pm_artpollreply_t reply;
            pm_artdmx_t dmx;
            pm_artaddress_t addr;
            pm_artinput_t in;
            pm_artipprog_t ipp;
            pm_artipprogreply_t ippr;
            pm_arttodrequest_t todr;
            pm_arttoddata_t todd;
            pm_arttodcontrol_t todc;
            pm_artrdm_t rdm;
            pm_artfirmwaremaster_t fm;
        } u;
        uint8_t type;
        if (pm_artdmx_decode(p, len, &u.dmx) == PM_OK) CHECK(18u + u.dmx.length <= len);
        pm_artpoll_decode(p, len, &u.poll);
        pm_artpollreply_decode(p, len, &u.reply);
        pm_artsync_decode(p, len);
        pm_artaddress_decode(p, len, &u.addr);
        pm_artinput_decode(p, len, &u.in);
        pm_artipprog_decode(p, len, &u.ipp);
        pm_artipprogreply_decode(p, len, &u.ippr);
        pm_arttodrequest_decode(p, len, &u.todr);
        if (pm_arttoddata_decode(p, len, &u.todd) == PM_OK) CHECK(28u + 6u * u.todd.uid_count <= len);
        pm_arttodcontrol_decode(p, len, &u.todc);
        if (pm_artrdm_decode(p, len, &u.rdm) == PM_OK) CHECK(24u + u.rdm.rdm_len <= len);
        if (pm_artfirmwaremaster_decode(p, len, &u.fm) == PM_OK) CHECK(len >= 40u + PM_FIRM_BLOCK_BYTES);
        pm_artfirmwarereply_decode(p, len, &type);
        pm_rdm_msg_t m;
        if (pm_rdm_decode(p, len, &m) == PM_OK) CHECK(PM_RDM_HEADER_LEN + m.pdl + 2u <= len);
        pm_rdm_decode_nosc(p, len, &m);
        pm_uid_t uid;
        pm_rdm_dub_decode(p, len, &uid);
        free(p);
    }
}

/* ------------------------------------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    unsigned long iters = argc > 1 ? strtoul(argv[1], NULL, 0) : 100000ul;
    unsigned long seed = argc > 2 ? strtoul(argv[2], NULL, 0) : 1ul;
    s_rng = 0x9E3779B97F4A7C15ull ^ (uint64_t)seed;
    if (s_rng == 0) s_rng = 1;

    node_setup();
    /* PID pool: everything the responder lists + the E1.20 PIDs it handles without listing them */
    {
        uint8_t req[PM_RDM_MAX_MSG_LEN], out[PM_RDM_MAX_MSG_LEN];
        pm_rdm_msg_t m = {.dest = NODE_UID, .src = CTL_UID, .cc = PM_RDM_CC_GET,
                          .pid = PM_RDM_PID_SUPPORTED_PARAMETERS};
        int n = pm_rdm_encode(&m, req, sizeof(req));
        n = pm_rdm_responder_handle(pm_node_responder(N), req, (size_t)n, out, sizeof(out));
        pm_rdm_msg_t a;
        if (n > 0 && pm_rdm_decode(out, (size_t)n, &a) == PM_OK) {
            for (unsigned i = 0; i + 1u < a.pdl; i += 2) add_pid(pm_get_be16(a.pd + i));
        }
        for (size_t i = 0; i < sizeof(STD_PIDS) / sizeof(STD_PIDS[0]); i++) add_pid(STD_PIDS[i]);
        add_pid(PM_PID_BOOT_INFO);
        add_pid(PM_PID_ENTER_BOOTLOADER);
        add_pid(PM_PID_BOOT_WRITE_PAGE);
        add_pid(PM_PID_BOOT_FINISH);
    }
    printf("pm_fuzz: %lu iterations per stage, seed %lu, %u PIDs\n", iters, seed, s_npids);
    fuzz_decoders(iters);
    fuzz_responder(iters);
    fuzz_node(iters);
    fuzz_ctrl(iters);
    printf("pm_fuzz: %lu checks, %lu failures\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
