/*
 * vn_main.c - virtual Pixelmapper Node: the real pm_node (pm_artnet + RDM responder + pm_rdm_ctrl) with fake platform
 * parts on real UDP sockets, for end-to-end tests of pixelmapper-net (crates/pixelmapper-net/tests/e2e_virtual_node.rs,
 * tools/e2e.sh).
 *
 *   virtual_node [--bind 127.0.0.1] [--port 6454] [--bcast-port P+1] [--ctl-port 0] [--ip <node ip>]
 *                [--mac 02:00:00:12:34:56] [--tubes 1:3,2:1] [--fixtures 3:2] [--random 4:210]
 *                [--dmx-slots 512] [--fw-version 0x000100] [--seed N] [--stdin-watch] [--log]
 *
 * Devices: --tubes P:N adds N tubes (app 0.5.0) with UID 7FF0:0P00000k (k = 1..N) to port P, --fixtures P:N adds
 * generic fixtures 1234:0P00100k, --random P:N plain devices with random UIDs. --dmx-slots only sets the DMX frame
 * length the simulated line assumes before the first ArtDmx (shorter frames leave more time for RDM).
 *
 * Sockets (port 0 = ephemeral):
 *   --port        Art-Net "unicast" socket: everything the node sends leaves from here.
 *   --bcast-port  emulated broadcast: a datagram received here is handed to the node with the destination address
 *                 = its directed broadcast (on loopback there is no broadcast; controllers use this port as their
 *                 ArtPoll/ArtSync target). A datagram on --port has destination = node IP (unicast), so the ArtSync
 *                 broadcast rule (contract §4) is testable.
 *   --ctl-port    text control channel (one command per datagram, answer to the sender), see control().
 * On start one line "READY bind=<ip> port=<p> bcast=<b> ctl=<c> uid=<node uid>" is printed on stdout. The process
 * ends on SIGTERM/SIGINT and, with --stdin-watch, when stdin reaches EOF (parent process gone).
 *
 * Addresses: pm_artnet always answers to <ip>:6454 and ignores datagrams from its own IP. On one host every controller
 * shares the node's IP and uses its own UDP port, so every peer endpoint (ip, port) gets a virtual IPv4 address
 * (127.77.0.x) towards the node; sends to it are mapped back to the endpoint. Sends to a broadcast address go to every
 * peer seen in the last 60 s. A peer that already has port 6454 and another IP than the node keeps its real IP.
 *
 * Time: real time (CLOCK_MONOTONIC). The simulated RS-485 lines keep their own bus clock that follows real time.
 * Failsafe, merge timeout, ArtSync timeout etc. therefore take their real 4 s / 10 s.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdalign.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "pm_pids.h"
#include "vn.h"

#define MAX_PEERS 32
#define PEER_BCAST_KEEP_MS 60000u
#define PEER_VIP_BASE 0x7F4D0001u /* 127.77.0.1 */
#define REBOOT_DELAY_MS 500u
#define REBOOT_SILENCE_MS 300u
#define TOKEN_PANEL 0x55490000u

/* ------------------------------------------------------------------------------------------------ state */

typedef struct {
    bool used;
    struct sockaddr_in addr;
    uint32_t vip;
    uint32_t last_ms;
} peer_t;

static struct {
    int uni, bcast, ctl;
    uint32_t bind_ip;
    uint16_t port, bcast_port, ctl_port;
    uint32_t dhcp_ip;          /* address the "DHCP server" hands out (= --ip) */
    pm_artnet_net_info_t net;
    bool net_changed;
    peer_t peers[MAX_PEERS];
    unsigned next_vip;
    bool log;
    uint32_t rng;
    /* node */
    void *mem;
    pm_node_t *node;
    pm_uid_t uid;
    uint32_t fw_version;
    char fw_label[33];
    bool reboot_pending;
    uint32_t reboot_at;
    uint32_t down_until;       /* node "off" (rebooting): no packets, no tick */
    unsigned boots;
    /* panel */
    uint8_t panel_rsp[PM_RDM_MAX_MSG_LEN];
    uint16_t panel_rsp_len;
    unsigned panel_rsp_count;
    unsigned ui_events[16];
    int last_fw_progress;
} S;

static volatile sig_atomic_t s_stop;

uint32_t vn_now_ms(void)
{
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (t0.tv_sec == 0 && t0.tv_nsec == 0) t0 = t;
    int64_t ms = (int64_t)(t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000;
    return (uint32_t)(ms + 1000);
}

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    if (!S.log) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[vn %6u] ", (unsigned)vn_now_ms());
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static const char *ip_str(uint32_t ip, char *buf)
{
    snprintf(buf, 16, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16) & 0xFFu, (unsigned)(ip >> 8) & 0xFFu,
             (unsigned)ip & 0xFFu);
    return buf;
}

/* ------------------------------------------------------------------------------------------------ peers */

static peer_t *peer_by_addr(const struct sockaddr_in *a, bool create)
{
    peer_t *free_slot = NULL, *oldest = NULL;
    for (unsigned i = 0; i < MAX_PEERS; i++) {
        peer_t *p = &S.peers[i];
        if (!p->used) {
            if (!free_slot) free_slot = p;
            continue;
        }
        if (p->addr.sin_addr.s_addr == a->sin_addr.s_addr && p->addr.sin_port == a->sin_port) return p;
        if (!oldest || (int32_t)(p->last_ms - oldest->last_ms) < 0) oldest = p;
    }
    if (!create) return NULL;
    peer_t *p = free_slot ? free_slot : oldest;
    memset(p, 0, sizeof(*p));
    p->used = true;
    p->addr = *a;
    uint32_t real = ntohl(a->sin_addr.s_addr);
    if (ntohs(a->sin_port) == PM_ARTNET_PORT && real != S.net.ip && real != S.bind_ip) {
        p->vip = real;
    } else {
        p->vip = PEER_VIP_BASE + S.next_vip++;
    }
    char b1[16], b2[16];
    logf_("peer %s:%u -> %s", ip_str(real, b1), (unsigned)ntohs(a->sin_port), ip_str(p->vip, b2));
    return p;
}

static peer_t *peer_by_vip(uint32_t vip)
{
    for (unsigned i = 0; i < MAX_PEERS; i++) {
        if (S.peers[i].used && S.peers[i].vip == vip) return &S.peers[i];
    }
    return NULL;
}

static bool is_bcast(uint32_t ip)
{
    if (ip == 0xFFFFFFFFu) return true;
    if (S.net.mask != 0 && S.net.mask != 0xFFFFFFFFu && (ip & ~S.net.mask) == ~S.net.mask) return true;
    return (ip & 0x00FFFFFFu) == 0x00FFFFFFu;
}

/* ------------------------------------------------------------------------------------------------ network ops */

static uint16_t opcode(const uint8_t *d, size_t len) { return len >= 10 ? pm_get_le16(d + 8) : 0; }

static pm_err_t op_send_udp(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    (void)ctx;
    char b[16];
    if (is_bcast(ip)) {
        uint32_t now = vn_now_ms();
        for (unsigned i = 0; i < MAX_PEERS; i++) {
            peer_t *p = &S.peers[i];
            if (!p->used || now - p->last_ms > PEER_BCAST_KEEP_MS) continue;
            sendto(S.uni, data, len, 0, (const struct sockaddr *)&p->addr, sizeof(p->addr));
        }
        logf_("tx bcast %s op 0x%04x len %zu", ip_str(ip, b), opcode(data, len), len);
        return PM_OK;
    }
    peer_t *p = peer_by_vip(ip);
    struct sockaddr_in a;
    if (p) {
        a = p->addr;
    } else {
        if ((ip & 0xFFFF0000u) == (PEER_VIP_BASE & 0xFFFF0000u)) return PM_ERR_IO; /* stale virtual peer */
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(ip);
        a.sin_port = htons(port);
    }
    logf_("tx %s op 0x%04x len %zu", ip_str(ip, b), opcode(data, len), len);
    return sendto(S.uni, data, len, 0, (const struct sockaddr *)&a, sizeof(a)) < 0 ? PM_ERR_IO : PM_OK;
}

static void op_net_info(void *ctx, pm_artnet_net_info_t *out)
{
    (void)ctx;
    *out = S.net;
}

/** Art-Net fallback address 2.B.C.D (contract §2). */
static uint32_t fallback_ip(void)
{
    uint8_t b = (uint8_t)(S.net.mac[3] + (PM_ARTNET_OEM_NODE >> 8) + (PM_ARTNET_OEM_NODE & 0xFFu));
    return (2u << 24) | ((uint32_t)b << 16) | ((uint32_t)S.net.mac[4] << 8) | S.net.mac[5];
}

static void op_net_apply_ip(void *ctx, const pm_ip_settings_t *ip)
{
    (void)ctx;
    pm_artnet_net_info_t n = S.net;
    if (ip->dhcp) {
        n.ip = S.dhcp_ip;
        n.mask = 0xFF000000u;
        n.gateway = 0;
        n.dhcp_active = true;
    } else {
        n.ip = ip->static_ip ? ip->static_ip : fallback_ip();
        n.mask = ip->static_mask ? ip->static_mask : 0xFF000000u;
        n.gateway = ip->gateway;
        n.dhcp_active = false;
    }
    if (memcmp(&n, &S.net, sizeof(n)) != 0) {
        char b[16];
        logf_("net: ip %s dhcp %d", ip_str(n.ip, b), n.dhcp_active);
        S.net = n;
        S.net_changed = true;
    }
}

static void op_net_renew(void *ctx) { (void)ctx; }

/* ------------------------------------------------------------------------------------------------ other ops */

static uint32_t op_now(void *ctx)
{
    (void)ctx;
    return vn_now_ms();
}

static uint32_t op_random(void *ctx)
{
    (void)ctx;
    uint32_t x = S.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    S.rng = x;
    return x;
}

static void op_config_get(void *ctx, pm_settings_t *out)
{
    (void)ctx;
    vn_config_get(out);
}

static pm_err_t op_config_modify(void *ctx, pm_node_modify_fn fn, void *fctx, pm_change_source_t src)
{
    (void)ctx;
    return vn_config_modify(fn, fctx, src);
}

static pm_err_t op_factory(void *ctx, pm_change_source_t src)
{
    (void)ctx;
    return vn_config_factory_reset(src);
}

static uint8_t op_queue_len(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    (void)port;
    return (uint8_t)PM_RDM_QUEUE_LEN;
}

static void schedule_reboot(void)
{
    S.reboot_pending = true;
    S.reboot_at = vn_now_ms() + REBOOT_DELAY_MS;
}

static void op_reset(void *ctx, uint8_t kind)
{
    (void)ctx;
    logf_("RESET_DEVICE 0x%02x", kind);
    schedule_reboot();
}

static void op_ui_event(void *ctx, const pm_node_ui_event_t *ev)
{
    (void)ctx;
    if ((unsigned)ev->type < 16) S.ui_events[ev->type]++;
    if (ev->type == PM_NODE_UI_RDM_RESPONSE && ev->len <= sizeof(S.panel_rsp)) {
        memcpy(S.panel_rsp, ev->data, ev->len);
        S.panel_rsp_len = ev->len;
        S.panel_rsp_count++;
    }
    if (ev->type == PM_NODE_UI_FW_PROGRESS) S.last_fw_progress = (int)ev->value;
}

static void on_fw(const pm_fw_status_t *st)
{
    if (S.node) pm_node_fw_progress(S.node, st);
}

/* ------------------------------------------------------------------------------------------------ node lifecycle */

static void node_start(void)
{
    static pm_node_ops_t ops;
    ops = (pm_node_ops_t){
        .now_ms = op_now, .random = op_random,
        .config_get = op_config_get, .config_modify = op_config_modify, .config_factory_reset = op_factory,
        .send_udp = op_send_udp, .net_info = op_net_info, .net_apply_ip = op_net_apply_ip,
        .net_renew_dhcp = op_net_renew,
        .port_configure = vn_port_configure, .port_output = vn_port_output_write, .port_commit = vn_port_commit,
        .port_clear = vn_port_clear, .port_status = vn_port_status, .port_input_read = vn_port_input_read,
        .port_rdm_submit = vn_port_rdm_submit, .port_rdm_queue_free = vn_port_rdm_queue_free,
        .port_rdm_queue_len = op_queue_len, .tod_get = vn_port_tod_get, .tod_control = vn_port_tod_control,
        .boot_catch_start = vn_port_catch_start, .boot_catch_abort = vn_port_catch_abort,
        .boot_catch_status = vn_port_catch_status,
        .reset = op_reset, .ui_event = op_ui_event, .port_live = NULL, .fw_sink = vn_fw_sink(),
    };
    const pm_node_identity_t id = {
        .artnet = {.esta_man = PM_ESTA_MANUFACTURER, .oem = PM_ARTNET_OEM_NODE,
                   .vers_info = PM_FW_VERSINFO(S.fw_version), .uid = S.uid, .num_ports = PM_NUM_PORTS},
        .responder = {.uid = S.uid, .model_id = PM_MODEL_NODE, .sw_version_id = S.fw_version,
                      .sw_version_label = S.fw_label, .model_description = "Pixelmapper Node",
                      .manufacturer_label = "Prototype", .hw_revision = 0},
    };
    static pm_settings_t settings;
    vn_config_get(&settings);
    op_net_apply_ip(NULL, &settings.ip);
    S.net_changed = false;
    S.node = pm_node_init(S.mem, &id, &ops, &settings);
    if (S.node == NULL) {
        fprintf(stderr, "pm_node_init failed\n");
        exit(1);
    }
    for (pm_port_idx_t i = 0; i < PM_NUM_PORTS; i++) {
        pm_node_port_cfg_t c;
        pm_node_port_cfg_from(&settings, i, &c);
        vn_port_configure(NULL, i, &c);
    }
    S.boots++;
    logf_("node up: fw 0x%06x '%s' (boot %u)", (unsigned)S.fw_version, S.fw_label, S.boots);
}

static void node_reboot(void)
{
    S.node = NULL;
    vn_port_reset();
    S.down_until = vn_now_ms() + REBOOT_SILENCE_MS;
    node_start();
}

/* ------------------------------------------------------------------------------------------------ control channel */

static int parse_port(const char *s)
{
    int p = s ? atoi(s) : 0;
    return (p >= 1 && p <= (int)PM_NUM_PORTS) ? p - 1 : -1;
}

static bool parse_uid(const char *s, pm_uid_t *u) { return s && pm_uid_parse(s, u) == PM_OK; }

static size_t hex(char *out, size_t cap, const uint8_t *d, size_t n)
{
    size_t k = 0;
    for (size_t i = 0; i < n && k + 3 < cap; i++) k += (size_t)snprintf(out + k, cap - k, "%02x", d[i]);
    return k;
}

static int unhex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = strlen(s);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

typedef struct {
    int what;
    const char *text;
    int value;
} panel_t;

static void panel_modify(void *fctx, pm_settings_t *s)
{
    const panel_t *p = fctx;
    switch (p->what) {
    case 0: snprintf(s->short_name, sizeof(s->short_name), "%s", p->text); break;
    case 1: s->panel_lock = p->value != 0; break;
    case 2: s->display_dim_timeout_s = (uint16_t)p->value; break;
    case 3: s->port[0].failsafe = (pm_failsafe_t)p->value; break;
    default: break;
    }
}

static const char *mode_name(sim_mode_t m)
{
    return m == SIM_APP ? "app" : (m == SIM_BOOT_WINDOW ? "window" : "boot");
}

/** One command; answer in `out` ("ok ..." or "err ..."). */
static void control(char *cmd, char *out, size_t cap)
{
    char *argv[8] = {0};
    int argc = 0;
    for (char *t = strtok(cmd, " \t\r\n"); t && argc < 8; t = strtok(NULL, " \t\r\n")) argv[argc++] = t;
    if (argc == 0) {
        snprintf(out, cap, "err empty");
        return;
    }
    const char *c = argv[0];
    int port = argc > 1 ? parse_port(argv[1]) : -1;
    pm_uid_t uid;
    if (strcmp(c, "ping") == 0) {
        snprintf(out, cap, "ok boots=%u", S.boots);
    } else if (strcmp(c, "dmx") == 0 && port >= 0) {
        uint8_t d[512];
        uint16_t n = vn_port_output((pm_port_idx_t)port, d, sizeof(d));
        int k = snprintf(out, cap, "ok %u ", (unsigned)n);
        hex(out + k, cap - (size_t)k, d, n);
    } else if (strcmp(c, "frames") == 0 && port >= 0) {
        snprintf(out, cap, "ok %u", (unsigned)vn_port_frames((pm_port_idx_t)port));
    } else if (strcmp(c, "add") == 0 && port >= 0 && argc >= 4 && parse_uid(argv[3], &uid)) {
        char kind = argv[2][0];
        uint32_t v = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 0) : (kind == 't' ? 0x500u : 0);
        snprintf(out, cap, vn_port_add((pm_port_idx_t)port, kind, uid, v) >= 0 ? "ok" : "err add");
    } else if (strcmp(c, "remove") == 0 && port >= 0 && argc >= 3 && parse_uid(argv[2], &uid)) {
        snprintf(out, cap, vn_port_remove((pm_port_idx_t)port, &uid) ? "ok" : "err unknown uid");
    } else if (strcmp(c, "power") == 0 && port >= 0 && argc >= 3 && parse_uid(argv[2], &uid)) {
        uint32_t off = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 500u;
        snprintf(out, cap, vn_port_power_cycle((pm_port_idx_t)port, &uid, off) ? "ok" : "err unknown uid");
    } else if (strcmp(c, "random") == 0 && port >= 0 && argc >= 3) {
        vn_port_add_random((pm_port_idx_t)port, atoi(argv[2]));
        snprintf(out, cap, "ok");
    } else if (strcmp(c, "tube") == 0 && port >= 0 && argc >= 3 && parse_uid(argv[2], &uid)) {
        sim_dev_t *d = vn_port_dev((pm_port_idx_t)port, &uid);
        if (d == NULL || d->model == NULL) {
            snprintf(out, cap, "err no model device");
            return;
        }
        const sim_model_t *m = d->model;
        snprintf(out, cap, "ok mode=%s present=%d version=0x%06x valid=%d pages=%u finishes=%u enters=%u "
                 "boot_info=%u start=%u identify=%d label=%.*s",
                 mode_name(d->mode), d->present, (unsigned)m->running_version, m->cached_valid,
                 (unsigned)m->pages_written, (unsigned)m->finishes, (unsigned)m->enters,
                 (unsigned)d->boot_info_gets, (unsigned)m->start_address, m->identify, m->label_len, m->label);
    } else if (strcmp(c, "busy") == 0 && port >= 0 && argc >= 4 && parse_uid(argv[2], &uid)) {
        sim_dev_t *d = vn_port_dev((pm_port_idx_t)port, &uid);
        if (d == NULL || d->model == NULL) {
            snprintf(out, cap, "err no model device");
            return;
        }
        d->model->busy_requests = (uint8_t)atoi(argv[3]); /* NACK BUFFER_FULL after each page write */
        snprintf(out, cap, "ok");
    } else if (strcmp(c, "tod") == 0 && port >= 0) {
        static pm_uid_t tod[PM_RDM_TOD_MAX];
        uint16_t n = vn_port_tod_get(NULL, (pm_port_idx_t)port, tod, PM_RDM_TOD_MAX);
        int k = snprintf(out, cap, "ok %u", (unsigned)n);
        for (uint16_t i = 0; i < n && (size_t)k + 16 < cap; i++) {
            char b[PM_UID_STR_LEN];
            k += snprintf(out + k, cap - (size_t)k, " %s", pm_uid_format(&tod[i], b));
        }
    } else if (strcmp(c, "input") == 0 && port >= 0 && argc >= 3) {
        int mode = strcmp(argv[2], "ramp") == 0 ? 1 : (strcmp(argv[2], "static") == 0 ? 2 : 0);
        vn_port_set_input((pm_port_idx_t)port, mode, argc >= 4 ? (uint8_t)atoi(argv[3]) : 0);
        snprintf(out, cap, "ok");
    } else if (strcmp(c, "panel") == 0 && argc >= 3 && S.node) {
        panel_t p = {.what = -1, .text = argv[2], .value = atoi(argv[2])};
        if (strcmp(argv[1], "name") == 0) {
            p.what = 0;
        } else if (strcmp(argv[1], "lock") == 0) {
            p.what = 1;
        } else if (strcmp(argv[1], "dim") == 0) {
            p.what = 2;
        } else if (strcmp(argv[1], "identify") == 0) {
            pm_node_ui_identify(S.node, atoi(argv[2]) != 0);
            snprintf(out, cap, "ok");
            return;
        } else if (strcmp(argv[1], "rdm") == 0 && argc >= 4) {
            int pp = parse_port(argv[2]);
            uint8_t w[PM_RDM_MAX_MSG_LEN];
            int n = unhex(argv[3], w, sizeof(w));
            if (pp < 0 || n < 2) {
                snprintf(out, cap, "err args");
                return;
            }
            pm_err_t e = pm_node_ui_rdm_request(S.node, (pm_port_idx_t)pp, TOKEN_PANEL | (S.panel_rsp_count + 1u),
                                                w, (uint16_t)n);
            snprintf(out, cap, e == PM_OK ? "ok" : "err %s", pm_err_name(e));
            return;
        }
        if (p.what < 0) {
            snprintf(out, cap, "err unknown panel command");
            return;
        }
        vn_config_modify(panel_modify, &p, PM_SRC_LOCAL_UI);
        snprintf(out, cap, "ok");
    } else if (strcmp(c, "panelrsp") == 0) {
        int k = snprintf(out, cap, "ok %u ", S.panel_rsp_count);
        hex(out + k, cap - (size_t)k, S.panel_rsp, S.panel_rsp_len);
    } else if (strcmp(c, "fwfail") == 0) {
        vn_fw_fail_next_begin(true);
        snprintf(out, cap, "ok");
    } else if (strcmp(c, "identify") == 0) {
        snprintf(out, cap, "ok %d", S.node ? pm_node_identify(S.node) : 0);
    } else if (strcmp(c, "net") == 0) {
        char a[16], b[16], g[16];
        snprintf(out, cap, "ok ip=%s mask=%s gw=%s dhcp=%d", ip_str(S.net.ip, a), ip_str(S.net.mask, b),
                 ip_str(S.net.gateway, g), S.net.dhcp_active);
    } else if (strcmp(c, "settings") == 0) {
        pm_settings_t s;
        vn_config_get(&s);
        int k = snprintf(out, cap, "ok short=%s panel_lock=%d dim=%u", s.short_name, s.panel_lock,
                         (unsigned)s.display_dim_timeout_s);
        for (unsigned i = 0; i < PM_NUM_PORTS && (size_t)k < cap; i++) {
            const pm_port_settings_t *q = &s.port[i];
            k += snprintf(out + k, cap - (size_t)k, " p%u=en%d,dir%d,pa%u,merge%d,rdm%d,bg%d,fs%d", i + 1,
                          q->enabled, q->direction, (unsigned)q->address, q->merge, q->rdm_enabled,
                          q->rdm_bg_discovery, q->failsafe);
        }
    } else {
        snprintf(out, cap, "err unknown command");
    }
}

/* ------------------------------------------------------------------------------------------------ sockets */

static int open_udp(uint32_t ip, uint16_t port, uint16_t *bound)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int big = 1 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    a.sin_port = htons(port);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        close(fd);
        return -1;
    }
    socklen_t al = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &al);
    *bound = ntohs(a.sin_port);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return fd;
}

static void drain(int fd, bool broadcast)
{
    static uint8_t buf[2048];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return;
        if (S.node == NULL || (int32_t)(vn_now_ms() - S.down_until) < 0) continue; /* rebooting */
        peer_t *p = peer_by_addr(&from, true);
        p->last_ms = vn_now_ms();
        uint32_t dst = broadcast ? (S.net.ip | ~S.net.mask) : S.net.ip;
        char b[16];
        logf_("rx %s%s op 0x%04x len %zd", broadcast ? "bcast " : "", ip_str(p->vip, b), opcode(buf, (size_t)n), n);
        pm_node_handle_packet(S.node, p->vip, PM_ARTNET_PORT, dst, buf, (size_t)n);
    }
}

static void drain_ctl(void)
{
    static char buf[1024];
    static char out[8192];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(S.ctl, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return;
        buf[n] = 0;
        logf_("ctl: %s", buf);
        control(buf, out, sizeof(out));
        sendto(S.ctl, out, strlen(out), 0, (struct sockaddr *)&from, fl);
    }
}

/* ------------------------------------------------------------------------------------------------ main */

static void on_signal(int sig)
{
    (void)sig;
    s_stop = 1;
}

/** "1:3,2:1" -> count per port. */
static bool parse_counts(const char *s, int counts[PM_NUM_PORTS])
{
    while (s && *s) {
        int p, n;
        if (sscanf(s, "%d:%d", &p, &n) != 2 || p < 1 || p > (int)PM_NUM_PORTS || n < 0) return false;
        counts[p - 1] += n;
        s = strchr(s, ',');
        if (s) s++;
    }
    return true;
}

static uint32_t parse_ip(const char *s)
{
    struct in_addr a;
    if (inet_pton(AF_INET, s, &a) != 1) {
        fprintf(stderr, "bad address %s\n", s);
        exit(2);
    }
    return ntohl(a.s_addr);
}

int main(int argc, char **argv)
{
    memset(&S, 0, sizeof(S));
    S.bind_ip = 0x7F000001u;
    S.port = PM_ARTNET_PORT;
    int bport = -1, cport = 0;
    uint32_t node_ip = 0;
    uint8_t mac[6] = {0x02, 0x00, 0x00, 0x12, 0x34, 0x56};
    int tubes[PM_NUM_PORTS] = {0}, fixtures[PM_NUM_PORTS] = {0}, randoms[PM_NUM_PORTS] = {0};
    uint16_t slots = 512;
    bool stdin_open = false;
    S.fw_version = PM_FW_VERSION_ID(0, 1, 0);
    S.rng = 0x9E3779B9u;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        bool ok = true;
        if (strcmp(a, "--log") == 0) {
            S.log = true;
            continue;
        }
        if (strcmp(a, "--stdin-watch") == 0) {
            stdin_open = true;
            continue;
        }
        if (v == NULL) {
            fprintf(stderr, "%s needs a value\n", a);
            return 2;
        }
        i++;
        if (strcmp(a, "--bind") == 0) {
            S.bind_ip = parse_ip(v);
        } else if (strcmp(a, "--port") == 0) {
            S.port = (uint16_t)atoi(v);
        } else if (strcmp(a, "--bcast-port") == 0) {
            bport = atoi(v);
        } else if (strcmp(a, "--ctl-port") == 0) {
            cport = atoi(v);
        } else if (strcmp(a, "--ip") == 0) {
            node_ip = parse_ip(v);
        } else if (strcmp(a, "--mac") == 0) {
            unsigned m[6];
            ok = sscanf(v, "%x:%x:%x:%x:%x:%x", &m[0], &m[1], &m[2], &m[3], &m[4], &m[5]) == 6;
            for (int k = 0; ok && k < 6; k++) mac[k] = (uint8_t)m[k];
        } else if (strcmp(a, "--tubes") == 0) {
            ok = parse_counts(v, tubes);
        } else if (strcmp(a, "--fixtures") == 0) {
            ok = parse_counts(v, fixtures);
        } else if (strcmp(a, "--random") == 0) {
            ok = parse_counts(v, randoms);
        } else if (strcmp(a, "--dmx-slots") == 0) {
            slots = (uint16_t)atoi(v);
        } else if (strcmp(a, "--fw-version") == 0) {
            S.fw_version = (uint32_t)strtoul(v, NULL, 0);
        } else if (strcmp(a, "--seed") == 0) {
            S.rng = (uint32_t)strtoul(v, NULL, 0) | 1u;
        } else {
            ok = false;
        }
        if (!ok) {
            fprintf(stderr, "bad option %s %s\n", a, v);
            return 2;
        }
    }
    snprintf(S.fw_label, sizeof(S.fw_label), "%u.%u.%u", (unsigned)(S.fw_version >> 16) & 0xFFu,
             (unsigned)(S.fw_version >> 8) & 0xFFu, (unsigned)S.fw_version & 0xFFu);
    S.dhcp_ip = node_ip ? node_ip : (S.bind_ip ? S.bind_ip : 0x7F000001u);
    memcpy(S.net.mac, mac, 6);
    S.net.link_up = true;

    S.uni = open_udp(S.bind_ip, S.port, &S.port);
    if (S.uni < 0) {
        perror("bind art-net port");
        return 1;
    }
    uint16_t want_b = bport >= 0 ? (uint16_t)bport : (S.port ? (uint16_t)(S.port + 1u) : 0);
    S.bcast = open_udp(S.bind_ip, want_b, &S.bcast_port);
    if (S.bcast < 0 && bport < 0) S.bcast = open_udp(S.bind_ip, 0, &S.bcast_port);
    S.ctl = open_udp(S.bind_ip, (uint16_t)cport, &S.ctl_port);
    if (S.bcast < 0 || S.ctl < 0) {
        perror("bind broadcast/control port");
        return 1;
    }

    S.uid = pm_uid_node_from_mac(PM_ESTA_MANUFACTURER, mac);
    vn_config_init();
    vn_fw_init(0);
    vn_fw_set_listener(on_fw);
    vn_port_init(&S.uid, slots);
    for (unsigned p = 0; p < PM_NUM_PORTS; p++) {
        for (int k = 1; k <= tubes[p]; k++) {
            vn_port_add((pm_port_idx_t)p, 't', pm_uid_make(PM_ESTA_MANUFACTURER, ((p + 1u) << 24) | (uint32_t)k),
                        0x500u);
        }
        for (int k = 1; k <= fixtures[p]; k++) {
            vn_port_add((pm_port_idx_t)p, 'f', pm_uid_make(0x1234u, ((p + 1u) << 24) | 0x1000u | (uint32_t)k), 0);
        }
        if (randoms[p] > 0) vn_port_add_random((pm_port_idx_t)p, randoms[p]);
    }
    S.mem = aligned_alloc(alignof(max_align_t), (pm_node_size() + alignof(max_align_t) - 1) &
                                                    ~(alignof(max_align_t) - 1));
    if (S.mem == NULL) return 1;
    node_start();

    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGPIPE, SIG_IGN);
    char b[16], u[PM_UID_STR_LEN];
    printf("READY bind=%s port=%u bcast=%u ctl=%u uid=%s\n", ip_str(S.bind_ip, b), S.port, S.bcast_port, S.ctl_port,
           pm_uid_format(&S.uid, u));
    fflush(stdout);

    uint32_t next_fw_poll = vn_now_ms() + 1000u;
    while (!s_stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(S.uni, &rd);
        FD_SET(S.bcast, &rd);
        FD_SET(S.ctl, &rd);
        int maxfd = S.uni > S.bcast ? S.uni : S.bcast;
        if (S.ctl > maxfd) maxfd = S.ctl;
        if (stdin_open) FD_SET(0, &rd);
        bool busy = vn_port_busy() || (S.node && pm_node_busy(S.node));
        struct timeval tv = {.tv_sec = 0, .tv_usec = busy ? 1000 : 5000};
        int r = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (r < 0 && errno != EINTR) break;
        if (r > 0) {
            if (stdin_open && FD_ISSET(0, &rd)) {
                char tmp[256];
                ssize_t n = read(0, tmp, sizeof(tmp));
                if (n <= 0) break; /* parent gone */
            }
            if (FD_ISSET(S.uni, &rd)) drain(S.uni, false);
            if (FD_ISSET(S.bcast, &rd)) drain(S.bcast, true);
            if (FD_ISSET(S.ctl, &rd)) drain_ctl();
        }
        uint32_t now = vn_now_ms();
        pm_image_desc_t inst;
        bool have = false;
        if (vn_fw_take_reboot(&inst, &have)) {
            if (have) {
                S.fw_version = inst.fw_version_id;
                snprintf(S.fw_label, sizeof(S.fw_label), "%.*s", (int)sizeof(inst.version_label),
                         inst.version_label);
            }
            node_reboot();
            continue;
        }
        if (S.reboot_pending && (int32_t)(now - S.reboot_at) >= 0) {
            S.reboot_pending = false;
            node_reboot();
            continue;
        }
        if (S.node == NULL || (int32_t)(now - S.down_until) < 0) continue;
        vn_port_run(S.node);
        vn_config_flush(S.node);
        if (S.net_changed) {
            S.net_changed = false;
            pm_node_net_changed(S.node);
        }
        pm_node_tick(S.node);
        if ((int32_t)(now - next_fw_poll) >= 0) {
            next_fw_poll = now + 1000u;
            vn_fw_poll();
        }
    }
    return 0;
}
