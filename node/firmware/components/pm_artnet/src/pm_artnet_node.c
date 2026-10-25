/*
 * pm_artnet_node.c - Art-Net 4 node logic (pure C11): discovery, DMX merge/sync/failsafe, configuration, TOD/RDM,
 * input mode, firmware upload, diagnostics. Behaviour: pm_artnet.h, protocol/contract.md §3-§6, §9.2,
 * protocol/artnet4-digest.md. Single-threaded (pm_net task); ops callbacks may call back into the node
 * (settings_apply -> pm_artnet_settings_changed, rdm_request -> pm_artnet_rdm_response).
 */
#include "pm_artnet.h"

#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------------ constants */

#define TX_BUF_LEN 1300u               /* largest packet we send: ArtTodData with 200 UIDs = 1228 */
#define MAX_PENDING_REPLIES (PM_ARTNET_MAX_CONTROLLERS + 8u)
#define SEQ_MAX_DROPS 8u               /* accept after this many consecutive out-of-order packets (resync) */
#define ALF_LEN_OFFSET 1056u           /* .alf Length3..0 */
#define ALF_OEM_OFFSET 34u
#define ALF_DESC_OFFSET 546u           /* image-format.md §2.5: copy of pm_image_desc_t in Spare */
#define ALF_HEADER_WORDS (PM_ALF_HEADER_LEN / 2u)

#define REPORT_TEXT_LEN 48u
#define RC_POWER_OK 0x0001u
#define RC_FIRMWARE_FAIL 0x000Eu

/* Status bits (digest §2.2) */
#define ST1_INDICATOR_LOCATE 0x40u
#define ST1_INDICATOR_MUTE 0x80u
#define ST1_INDICATOR_NORMAL 0xC0u
#define ST1_PA_BY_NETWORK 0x20u
#define ST1_RDM_CAPABLE 0x02u
#define ST2_RDM_BY_ARTADDRESS 0x80u
#define ST2_STYLE_SWITCHABLE 0x40u
#define ST2_15BIT 0x08u
#define ST2_DHCP_CAPABLE 0x04u
#define ST2_DHCP_ACTIVE 0x02u
#define ST3_FAILSAFE_PROGRAMMABLE 0x20u
#define ST3_DIRECTION_SWITCHABLE 0x08u
#define GI_DATA 0x80u
#define GI_DISABLED 0x08u
#define GOA_DATA 0x80u
#define GOA_MERGING 0x08u
#define GOA_LTP 0x02u
#define GOB_RDM_DISABLED 0x80u
#define GOB_CONTINUOUS 0x40u
#define GOB_DISCOVERY_IDLE 0x20u
#define GOB_BG_DISABLED 0x10u
#define PT_OUTPUT 0x80u
#define PT_INPUT 0x40u

/* ------------------------------------------------------------------------------------------------ state */

typedef struct {
    bool used;
    uint32_t ip;
    uint8_t flags;
    uint8_t diag_priority;
    uint32_t last_ms;
} controller_t;

typedef struct {
    bool used;
    bool await_settings;   /* ArtAddress/ArtInput reply waiting for pm_artnet_settings_changed() */
    uint32_t ip;
    uint8_t pages;         /* bit p = BindIndex p+1 */
    uint32_t due_ms;
} pending_reply_t;

typedef struct {
    bool used;
    uint32_t ip;
    uint8_t ports;         /* TOD subscription per port */
} tod_sub_t;

typedef struct {
    bool used;
    uint32_t ip;
    uint32_t last_ms;
} in_sub_t;

typedef struct {
    bool used;
    uint32_t token;
    uint32_t ip;
    pm_port_idx_t port;
    uint32_t t_ms;
} rdm_pending_t;

typedef struct {
    /* output */
    bool rx_ever;          /* ArtDmx (or local input) received since the port became an output */
    bool data_lost;        /* failsafe applied */
    uint32_t last_rx_ms;
    /* input */
    bool in_has;
    uint8_t in_seq;
    uint16_t in_len;
    uint32_t in_sent_ms;
    uint8_t in_data[512];
    in_sub_t subs[PM_ARTNET_MAX_SUBSCRIBERS];
    /* RDM */
    uint8_t fifo_max_seen;
    /* statistics for pm_artnet_port_live() */
    uint16_t rx_count;     /* frames accepted in the current fps window */
    uint8_t rx_fps;
} port_state_t;

typedef struct {
    bool active;
    uint32_t ip;
    uint8_t next_block;
    uint32_t words;        /* FirmwareLength of the session */
    uint32_t file_off;     /* .alf bytes consumed */
    uint32_t image_len;
    uint32_t written;
    uint32_t alf_len;      /* Length3..0 from the .alf header */
    uint8_t alf_len_bytes;
    uint32_t last_ms;
    bool reboot_pending;
    uint32_t reboot_at;
    bool done;             /* last session finished with FirmAllGood (answer a repeated FirmLast) */
    uint32_t done_ip;
    uint8_t done_block;
} fw_state_t;

struct pm_artnet_node {
    pm_artnet_identity_t id;
    pm_artnet_ops_t ops;
    pm_settings_t settings;
    pm_settings_t work;    /* scratch copy for changes (no big stack objects) */
    pm_artnet_net_info_t net;
    uint32_t now;

    pm_artnet_merge_t merge[PM_NUM_PORTS];
    port_state_t port[PM_NUM_PORTS];

    controller_t ctl[PM_ARTNET_MAX_CONTROLLERS];
    pending_reply_t pend[MAX_PENDING_REPLIES];
    tod_sub_t tod_subs[PM_ARTNET_MAX_SUBSCRIBERS];
    unsigned tod_sub_next;
    rdm_pending_t rdm[PM_ARTNET_MAX_RDM_PENDING];
    uint32_t next_token;

    bool sync_mode;
    uint32_t last_sync_ms;
    uint32_t last_dmx_ip;
    bool last_dmx_valid;
    uint32_t own_poll_ms;
    uint32_t fps_window_ms;

    uint8_t led;           /* PM_AC_LED_NORMAL / MUTE / LOCATE */
    uint16_t report_code;
    uint16_t report_count;
    char report_text[REPORT_TEXT_LEN];
    uint8_t snapshot[PM_NUM_PORTS][PM_POLL_REPLY_LEN];

    fw_state_t fw;

    pm_uid_t tod[PM_ARTNET_TOD_MAX];
    uint8_t frame[512];    /* scratch DMX frame (failsafe, input) */
    uint8_t tx[TX_BUF_LEN];
};

/* ------------------------------------------------------------------------------------------------ helpers */

static bool elapsed(uint32_t now, uint32_t since, uint32_t ms) { return (uint32_t)(now - since) >= ms; }
static bool time_before(uint32_t a, uint32_t b) { return (int32_t)(a - b) < 0; }

static uint32_t now_ms(pm_artnet_node_t *n)
{
    n->now = n->ops.now_ms ? n->ops.now_ms(n->ops.ctx) : 0;
    return n->now;
}

static void refresh_net(pm_artnet_node_t *n)
{
    if (n->ops.net_info) n->ops.net_info(n->ops.ctx, &n->net);
}

static bool net_ready(const pm_artnet_node_t *n) { return n->net.ip != 0 && n->net.link_up; }

/** Directed broadcast of our subnet, 0 (= do not send) while the mask is unknown: never 255.255.255.255. */
static uint32_t bcast_addr(const pm_artnet_node_t *n)
{
    if (n->net.mask == 0 || n->net.mask == 0xFFFFFFFFu) return 0;
    return n->net.ip | ~n->net.mask;
}

static bool is_broadcast(const pm_artnet_node_t *n, uint32_t dst)
{
    if (dst == 0xFFFFFFFFu) return true;
    if (n->net.mask != 0 && n->net.mask != 0xFFFFFFFFu && (dst & ~n->net.mask) == ~n->net.mask) return true;
    return (dst & 0x00FFFFFFu) == 0x00FFFFFFu; /* x.255.255.255 (2.255.255.255, 10.255.255.255) */
}

static void send(pm_artnet_node_t *n, uint32_t ip, const uint8_t *data, int len)
{
    if (len <= 0 || ip == 0 || n->ops.send_udp == NULL) return;
    n->ops.send_udp(n->ops.ctx, ip, PM_ARTNET_PORT, data, (size_t)len);
}

static unsigned num_pages(const pm_artnet_node_t *n) { return n->id.num_ports; }

static const pm_port_settings_t *pset(const pm_artnet_node_t *n, unsigned p) { return &n->settings.port[p]; }

static bool is_output(const pm_artnet_node_t *n, unsigned p) { return pset(n, p)->direction == PM_PORT_DIR_OUTPUT; }

static bool output_active(const pm_artnet_node_t *n, unsigned p) { return is_output(n, p) && pset(n, p)->enabled; }

static bool input_active(const pm_artnet_node_t *n, unsigned p)
{
    return pset(n, p)->direction == PM_PORT_DIR_INPUT && pset(n, p)->enabled;
}

static int page_of_bind(const pm_artnet_node_t *n, uint8_t bind_index)
{
    unsigned p = bind_index == 0 ? 0u : bind_index - 1u;
    return p < num_pages(n) ? (int)p : -1;
}

static void port_status(const pm_artnet_node_t *n, unsigned p, pm_artnet_port_status_t *st)
{
    memset(st, 0, sizeof(*st));
    st->output_active = true;
    st->input_signal = true;
    if (n->ops.port_status) n->ops.port_status(n->ops.ctx, (pm_port_idx_t)p, st);
}

/* ------------------------------------------------------------------------------------------------ merge engine */

void pm_artnet_merge_init(pm_artnet_merge_t *m, pm_merge_mode_t mode)
{
    memset(m, 0, sizeof(*m));
    m->mode = mode;
}

/** Recompute `out`; `latest` = index of the source that just sent (LTP winner). */
static void merge_compute(pm_artnet_merge_t *m, int latest)
{
    int other = latest ^ 1;
    if (!(m->src[0].active && m->src[1].active)) {
        int s = m->src[latest].active ? latest : other;
        if (!m->src[s].active) return;
        m->out_len = m->src[s].len;
        memcpy(m->out, m->src[s].data, m->src[s].len);
        return;
    }
    uint16_t len = m->src[0].len > m->src[1].len ? m->src[0].len : m->src[1].len;
    for (uint16_t i = 0; i < len; i++) {
        uint8_t a = i < m->src[latest].len ? m->src[latest].data[i] : 0;
        uint8_t b = i < m->src[other].len ? m->src[other].data[i] : 0;
        if (m->mode == PM_MERGE_LTP) {
            m->out[i] = i < m->src[latest].len ? a : b;
        } else {
            m->out[i] = a > b ? a : b;
        }
    }
    m->out_len = len;
}

static int merge_find(const pm_artnet_merge_t *m, uint32_t ip, uint8_t physical)
{
    for (int i = 0; i < 2; i++) {
        if (m->src[i].active && m->src[i].ip == ip && m->src[i].physical == physical) return i;
    }
    return -1;
}

pm_err_t pm_artnet_merge_feed(pm_artnet_merge_t *m, uint32_t ip, uint8_t physical, const uint8_t *data,
                              uint16_t len, uint32_t now_ms)
{
    if (m == NULL || (data == NULL && len > 0) || len > 512) return PM_ERR_INVALID_ARG;
    if (m->exclusive && ip != m->exclusive_ip) return PM_ERR_REJECTED;
    int s = merge_find(m, ip, physical);
    if (s < 0) {
        s = !m->src[0].active ? 0 : (!m->src[1].active ? 1 : -1);
        if (s < 0) return PM_ERR_FULL;
        m->src[s].active = true;
        m->src[s].ip = ip;
        m->src[s].physical = physical;
        m->src[s].sequence = 0;
        m->src[s].seq_drops = 0;
    }
    m->src[s].last_ms = now_ms;
    m->src[s].len = len;
    if (len > 0) memcpy(m->src[s].data, data, len);
    if (m->cancel_pending) {
        m->cancel_pending = false;
        m->src[s ^ 1].active = false;
        m->exclusive = true;
        m->exclusive_ip = ip;
    }
    merge_compute(m, s);
    return PM_OK;
}

bool pm_artnet_merge_expire(pm_artnet_merge_t *m, uint32_t now_ms)
{
    bool removed = false;
    for (int i = 0; i < 2; i++) {
        if (m->src[i].active && elapsed(now_ms, m->src[i].last_ms, PM_ARTNET_MERGE_TIMEOUT_MS)) {
            m->src[i].active = false;
            removed = true;
        }
    }
    if (m->exclusive) {
        bool alive = false;
        for (int i = 0; i < 2; i++) alive |= m->src[i].active && m->src[i].ip == m->exclusive_ip;
        if (!alive) m->exclusive = false;
    }
    if (removed && pm_artnet_merge_sources(m) == 1) merge_compute(m, m->src[0].active ? 0 : 1);
    return removed;
}

unsigned pm_artnet_merge_sources(const pm_artnet_merge_t *m)
{
    return (unsigned)m->src[0].active + (unsigned)m->src[1].active;
}

void pm_artnet_merge_cancel(pm_artnet_merge_t *m) { m->cancel_pending = true; }

/** Two sources from different IPs (ArtSync is ignored then, digest §3.3). */
static bool merging_ips(const pm_artnet_merge_t *m)
{
    return m->src[0].active && m->src[1].active && m->src[0].ip != m->src[1].ip;
}

static bool any_merging_ips(const pm_artnet_node_t *n)
{
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (output_active(n, p) && merging_ips(&n->merge[p])) return true;
    }
    return false;
}

/* ------------------------------------------------------------------------------------------------ ArtPollReply */

static uint8_t indicator_bits(uint8_t led)
{
    if (led == PM_AC_LED_LOCATE) return ST1_INDICATOR_LOCATE;
    if (led == PM_AC_LED_MUTE) return ST1_INDICATOR_MUTE;
    return ST1_INDICATOR_NORMAL;
}

static void build_reply(pm_artnet_node_t *n, unsigned p, pm_artpollreply_t *r)
{
    const pm_port_settings_t *ps = pset(n, p);
    pm_artnet_port_status_t st;
    port_status(n, p, &st);
    memset(r, 0, sizeof(*r));
    r->ip = n->net.ip;
    r->vers_info = n->id.vers_info;
    r->net_switch = PM_PA_NET(ps->address);
    r->sub_switch = PM_PA_SUB(ps->address);
    r->oem = n->id.oem;
    r->status1 = indicator_bits(n->led) | ST1_PA_BY_NETWORK | ST1_RDM_CAPABLE;
    r->esta_man = n->id.esta_man;
    snprintf(r->short_name, sizeof(r->short_name), "%s", n->settings.short_name);
    snprintf(r->long_name, sizeof(r->long_name), "%s", n->settings.long_name);
    snprintf(r->node_report, sizeof(r->node_report), "#%04x [%04u] %.*s", n->report_code,
             (unsigned)(n->report_count % 10000u), (int)(REPORT_TEXT_LEN - 1u), n->report_text);
    r->num_ports = 1;
    if (is_output(n, p)) {
        const port_state_t *s = &n->port[p];
        r->port_types[0] = PT_OUTPUT;
        if (ps->enabled && st.output_active && s->rx_ever && !s->data_lost) r->good_output_a[0] |= GOA_DATA;
        if (pm_artnet_merge_sources(&n->merge[p]) == 2) r->good_output_a[0] |= GOA_MERGING;
        if (ps->merge == PM_MERGE_LTP) r->good_output_a[0] |= GOA_LTP;
        r->sw_out[0] = PM_PA_UNI(ps->address);
    } else {
        r->port_types[0] = PT_INPUT;
        if (ps->enabled && st.input_signal) r->good_input[0] |= GI_DATA;
        if (!ps->enabled) r->good_input[0] |= GI_DISABLED;
        r->sw_in[0] = PM_PA_UNI(ps->address);
    }
    memcpy(r->mac, n->net.mac, 6);
    r->bind_ip = n->net.ip;
    r->bind_index = (uint8_t)(p + 1u);
    r->status2 = ST2_RDM_BY_ARTADDRESS | ST2_STYLE_SWITCHABLE | ST2_15BIT | ST2_DHCP_CAPABLE |
                 (n->net.dhcp_active ? ST2_DHCP_ACTIVE : 0u);
    uint8_t gob = 0;
    if (!(is_output(n, p) && ps->rdm_enabled)) gob |= GOB_RDM_DISABLED;
    if (ps->output_style == PM_OUTPUT_CONTINUOUS) gob |= GOB_CONTINUOUS;
    if (!st.rdm_discovery_running) gob |= GOB_DISCOVERY_IDLE;
    if (!ps->rdm_bg_discovery) gob |= GOB_BG_DISABLED;
    r->good_output_b[0] = gob;
    r->status3 = (uint8_t)(((unsigned)ps->failsafe & 3u) << 6) | ST3_FAILSAFE_PROGRAMMABLE | ST3_DIRECTION_SWITCHABLE;
    r->default_resp_uid = n->id.uid;
    r->refresh_rate = 44;
}

static void send_reply(pm_artnet_node_t *n, uint32_t ip, uint8_t pages)
{
    if (!net_ready(n)) return;
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (!(pages & (1u << p))) continue;
        pm_artpollreply_t r;
        n->report_count = (uint16_t)(n->report_count % 9999u + 1u);
        build_reply(n, p, &r);
        send(n, ip, n->tx, pm_artpollreply_encode(&r, n->tx, sizeof(n->tx)));
    }
}

static void queue_reply(pm_artnet_node_t *n, uint32_t ip, uint32_t due, uint8_t pages, bool await_settings)
{
    if (pages == 0 || ip == 0) return;
    pending_reply_t *free_slot = NULL;
    for (unsigned i = 0; i < MAX_PENDING_REPLIES; i++) {
        pending_reply_t *e = &n->pend[i];
        if (e->used && e->ip == ip) {
            e->pages |= pages;
            /* a reply that waits for pm_artnet_settings_changed() is not pulled forward (it would show old values) */
            if (time_before(due, e->due_ms) && !e->await_settings) e->due_ms = due;
            if (await_settings && !e->await_settings) {
                e->await_settings = true;
                e->due_ms = due;
            }
            return;
        }
        if (!e->used && free_slot == NULL) free_slot = e;
    }
    if (free_slot == NULL) { /* table full: answer right away */
        send_reply(n, ip, pages);
        return;
    }
    *free_slot = (pending_reply_t){.used = true, .await_settings = await_settings, .ip = ip, .pages = pages,
                                   .due_ms = due};
}

static void flush_replies(pm_artnet_node_t *n)
{
    for (unsigned i = 0; i < MAX_PENDING_REPLIES; i++) {
        pending_reply_t *e = &n->pend[i];
        if (!e->used || time_before(n->now, e->due_ms)) continue;
        e->used = false;
        send_reply(n, e->ip, e->pages);
    }
}

/** Encoded reply without the volatile parts (NodeReport counter, "discovery running") for change detection. */
static void snapshot_of(pm_artnet_node_t *n, unsigned p, uint8_t out[PM_POLL_REPLY_LEN])
{
    pm_artpollreply_t r;
    build_reply(n, p, &r);
    r.node_report[0] = '\0';
    r.good_output_b[0] &= (uint8_t)~GOB_DISCOVERY_IDLE;
    memset(out, 0, PM_POLL_REPLY_LEN);
    pm_artpollreply_encode(&r, out, PM_POLL_REPLY_LEN);
}

/** Send unsolicited ArtPollReplies for every page whose Art-Net view changed (to pollers with Flags bit 1). */
static void check_changes(pm_artnet_node_t *n)
{
    uint8_t changed = 0;
    uint8_t buf[PM_POLL_REPLY_LEN];
    for (unsigned p = 0; p < num_pages(n); p++) {
        snapshot_of(n, p, buf);
        if (memcmp(buf, n->snapshot[p], sizeof(buf)) != 0) {
            memcpy(n->snapshot[p], buf, sizeof(buf));
            changed |= (uint8_t)(1u << p);
        }
    }
    if (changed == 0) return;
    for (unsigned i = 0; i < PM_ARTNET_MAX_CONTROLLERS; i++) {
        const controller_t *c = &n->ctl[i];
        if (c->used && (c->flags & PM_POLL_FLAG_REPLY_ON_CHANGE) &&
            !elapsed(n->now, c->last_ms, PM_ARTNET_CONTROLLER_KEEP_MS)) {
            queue_reply(n, c->ip, n->now, changed, false);
        }
    }
}

/** Common end of every public entry point. */
static void finish(pm_artnet_node_t *n)
{
    check_changes(n);
    flush_replies(n);
}

/* ------------------------------------------------------------------------------------------------ ArtPoll */

static void remember_controller(pm_artnet_node_t *n, uint32_t ip, const pm_artpoll_t *poll)
{
    controller_t *slot = NULL, *oldest = &n->ctl[0];
    for (unsigned i = 0; i < PM_ARTNET_MAX_CONTROLLERS; i++) {
        controller_t *c = &n->ctl[i];
        if (c->used && c->ip == ip) {
            slot = c;
            break;
        }
        if (!c->used && slot == NULL) slot = c;
        if (time_before(c->last_ms, oldest->last_ms)) oldest = c;
    }
    if (slot == NULL) slot = oldest;
    *slot = (controller_t){.used = true, .ip = ip, .flags = poll->flags, .diag_priority = poll->diag_priority,
                           .last_ms = n->now};
}

static void on_poll(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artpoll_t poll;
    if (pm_artpoll_decode(data, len, &poll) != PM_OK) return;
    remember_controller(n, src, &poll);
    uint8_t pages = 0;
    for (unsigned p = 0; p < num_pages(n); p++) {
        pm_port_address_t pa = pset(n, p)->address;
        if ((poll.flags & PM_POLL_FLAG_TARGETED) && (pa < poll.target_bottom || pa > poll.target_top)) continue;
        pages |= (uint8_t)(1u << p);
    }
    uint32_t delay = n->ops.random ? n->ops.random(n->ops.ctx) % (PM_ARTNET_REPLY_DELAY_MAX_MS + 1u) : 0u;
    queue_reply(n, src, n->now + delay, pages, false);
}

/** ArtPollReply of another node: remember it as subscriber of our input universes (digest §3.1). */
static void on_poll_reply(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artpollreply_t r;
    if (pm_artpollreply_decode(data, len, &r) != PM_OK) return;
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (!input_active(n, p)) continue;
        bool match = false;
        for (unsigned i = 0; i < 4; i++) {
            pm_port_address_t pa = PM_PA_MAKE(r.net_switch, r.sub_switch, r.sw_out[i]);
            if ((r.port_types[i] & PT_OUTPUT) && pa == pset(n, p)->address) match = true;
        }
        if (!match) continue;
        in_sub_t *slot = NULL, *oldest = &n->port[p].subs[0];
        for (unsigned i = 0; i < PM_ARTNET_MAX_SUBSCRIBERS; i++) {
            in_sub_t *s = &n->port[p].subs[i];
            if (s->used && s->ip == src) {
                slot = s;
                break;
            }
            if (!s->used && slot == NULL) slot = s;
            if (time_before(s->last_ms, oldest->last_ms)) oldest = s;
        }
        if (slot == NULL) slot = oldest;
        *slot = (in_sub_t){.used = true, .ip = src, .last_ms = n->now};
    }
}

/* ------------------------------------------------------------------------------------------------ DMX output */

static void output_frame(pm_artnet_node_t *n, unsigned p, const uint8_t *data, uint16_t len, bool defer)
{
    if (n->ops.port_output) n->ops.port_output(n->ops.ctx, (pm_port_idx_t)p, data, len, defer);
}

/** Fill `buf` (512 bytes) with the failsafe frame of port p. Returns its length, -1 = hold (no output). */
static int failsafe_frame(const pm_artnet_node_t *n, unsigned p, uint8_t *buf)
{
    const pm_port_settings_t *ps = pset(n, p);
    switch (ps->failsafe) {
    case PM_FAILSAFE_ZERO:
        memset(buf, 0x00, 512);
        return 512;
    case PM_FAILSAFE_FULL:
        memset(buf, 0xFF, 512);
        return 512;
    case PM_FAILSAFE_SCENE:
        if (ps->scene_len == 0) return -1; /* nothing recorded: hold */
        memcpy(buf, ps->scene, ps->scene_len);
        return ps->scene_len;
    default:
        return -1;
    }
}

static void apply_failsafe(pm_artnet_node_t *n, unsigned p)
{
    int len = failsafe_frame(n, p, n->frame);
    if (len >= 0) output_frame(n, p, n->frame, (uint16_t)len, false);
}

/** Feed one frame into output port p (ArtDmx or local input). Returns true when accepted. */
static bool dmx_to_port(pm_artnet_node_t *n, unsigned p, uint32_t ip, uint8_t physical, uint8_t seq,
                        const uint8_t *data, uint16_t len, bool defer)
{
    pm_artnet_merge_t *m = &n->merge[p];
    int s = merge_find(m, ip, physical);
    if (s >= 0 && seq != 0 && m->src[s].sequence != 0) {
        uint8_t diff = (uint8_t)(seq - m->src[s].sequence);
        if (diff >= 128u && m->src[s].seq_drops < SEQ_MAX_DROPS) { /* older packet (reordered) */
            m->src[s].seq_drops++;
            return false;
        }
    }
    if (pm_artnet_merge_feed(m, ip, physical, data, len, n->now) != PM_OK) return false;
    s = merge_find(m, ip, physical);
    if (s >= 0) {
        m->src[s].sequence = seq;
        m->src[s].seq_drops = 0;
    }
    port_state_t *ps = &n->port[p];
    ps->rx_ever = true;
    ps->data_lost = false;
    ps->last_rx_ms = n->now;
    if (ps->rx_count < 0xFFFFu) ps->rx_count++;
    output_frame(n, p, m->out, m->out_len, defer);
    return true;
}

static void leave_sync(pm_artnet_node_t *n)
{
    if (!n->sync_mode) return;
    n->sync_mode = false;
    if (n->ops.port_commit) n->ops.port_commit(n->ops.ctx);
}

static void on_dmx(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artdmx_t d;
    if (pm_artdmx_decode(data, len, &d) != PM_OK) return;
    bool accepted = false;
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (output_active(n, p) && pset(n, p)->address == d.port_address) {
            accepted |= dmx_to_port(n, p, src, d.physical, d.sequence, d.data, d.length, n->sync_mode);
        }
    }
    if (!accepted) return;
    n->last_dmx_ip = src;
    n->last_dmx_valid = true;
    if (n->sync_mode && any_merging_ips(n)) leave_sync(n); /* syncs are ignored while merging: no frozen output */
}

static void on_sync(pm_artnet_node_t *n, uint32_t src, uint32_t dst, const uint8_t *data, size_t len)
{
    if (pm_artsync_decode(data, len) != PM_OK) return;
    if (dst != 0 && !is_broadcast(n, dst)) return; /* unicast ArtSync is not allowed */
    if (!n->last_dmx_valid || src != n->last_dmx_ip) return;
    if (any_merging_ips(n)) return;
    n->sync_mode = true;
    n->last_sync_ms = n->now;
    if (n->ops.port_commit) n->ops.port_commit(n->ops.ctx);
}

/* ------------------------------------------------------------------------------------------------ settings */

static void begin_change(pm_artnet_node_t *n) { n->work = n->settings; }

/** Hand n->work to the platform. The reply to `reply_ip` waits for pm_artnet_settings_changed(). */
static void commit_change(pm_artnet_node_t *n, pm_change_mask_t mask, pm_change_source_t src, uint32_t reply_ip,
                          uint8_t reply_pages)
{
    if (mask == 0 || n->ops.settings_apply == NULL) {
        queue_reply(n, reply_ip, n->now, reply_pages, false);
        return;
    }
    queue_reply(n, reply_ip, n->now + PM_ARTNET_ADDRESS_REPLY_WAIT_MS, reply_pages, true);
    if (n->ops.settings_apply(n->ops.ctx, &n->work, mask, src) != PM_OK) {
        for (unsigned i = 0; i < MAX_PENDING_REPLIES; i++) {
            if (n->pend[i].used && n->pend[i].ip == reply_ip) n->pend[i].due_ms = n->now;
        }
    }
}

/** Current output of port p (AcFailRecord). */
static uint16_t current_output(pm_artnet_node_t *n, unsigned p, uint8_t *buf)
{
    if (n->port[p].data_lost) {
        int len = failsafe_frame(n, p, buf);
        if (len >= 0) return (uint16_t)len;
    }
    memcpy(buf, n->merge[p].out, n->merge[p].out_len);
    return n->merge[p].out_len;
}

static void clear_subscribers(pm_artnet_node_t *n, unsigned p)
{
    memset(n->port[p].subs, 0, sizeof(n->port[p].subs));
}

/** ArtAddress programming rule for Net/Sub/SwIn/SwOut: bit 7 = program, 0x00 = reset, else no change. */
static bool prog_value(uint8_t v, uint8_t mask_bits, uint8_t def, uint8_t *out)
{
    if (v & 0x80u) {
        *out = v & mask_bits;
        return true;
    }
    if (v == 0) {
        *out = def;
        return true;
    }
    return false;
}

static void on_address(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artaddress_t a;
    if (pm_artaddress_decode(data, len, &a) != PM_OK) return;
    int pi = page_of_bind(n, a.bind_index);
    if (pi < 0) return;
    unsigned p = (unsigned)pi;
    pm_change_mask_t mask = 0;

    pm_settings_defaults(&n->work);
    const pm_port_address_t def_pa = n->work.port[p].address;
    begin_change(n);
    pm_port_settings_t *w = &n->work.port[p];

    if (a.short_name[0] && strcmp(a.short_name, n->work.short_name) != 0) {
        snprintf(n->work.short_name, sizeof(n->work.short_name), "%s", a.short_name);
        mask |= PM_CHG_NAMES;
    }
    if (a.long_name[0] && strcmp(a.long_name, n->work.long_name) != 0) {
        snprintf(n->work.long_name, sizeof(n->work.long_name), "%s", a.long_name);
        mask |= PM_CHG_NAMES;
    }
    uint8_t net = PM_PA_NET(w->address), sub = PM_PA_SUB(w->address), uni = PM_PA_UNI(w->address);
    prog_value(a.net_switch, 0x7Fu, PM_PA_NET(def_pa), &net);
    prog_value(a.sub_switch, 0x0Fu, PM_PA_SUB(def_pa), &sub);
    /* one port per BindIndex: SwOut[0] for an output, SwIn[0] for an input; a programmed (bit 7) value in the
       other field is accepted too, a 0x00 there is not taken as reset */
    uint8_t own = w->direction == PM_PORT_DIR_OUTPUT ? a.sw_out[0] : a.sw_in[0];
    uint8_t other = w->direction == PM_PORT_DIR_OUTPUT ? a.sw_in[0] : a.sw_out[0];
    if (!prog_value(own, 0x0Fu, PM_PA_UNI(def_pa), &uni) && (other & 0x80u)) uni = other & 0x0Fu;
    pm_port_address_t pa = PM_PA_MAKE(net, sub, uni);
    if (pa != w->address) {
        w->address = pa;
        mask |= PM_CHG_PORT(PM_CHG_P_ADDRESS, p);
    }

    uint8_t cmd = a.command;
    switch (cmd) {
    case PM_AC_CANCEL_MERGE:
        pm_artnet_merge_cancel(&n->merge[p]);
        break;
    case PM_AC_LED_NORMAL:
    case PM_AC_LED_MUTE:
    case PM_AC_LED_LOCATE:
        n->led = cmd;
        if (n->ops.indicator) n->ops.indicator(n->ops.ctx, cmd);
        break;
    case PM_AC_FAIL_HOLD:
    case PM_AC_FAIL_ZERO:
    case PM_AC_FAIL_FULL:
    case PM_AC_FAIL_SCENE:
        if (w->failsafe != (pm_failsafe_t)(cmd - PM_AC_FAIL_HOLD)) {
            w->failsafe = (pm_failsafe_t)(cmd - PM_AC_FAIL_HOLD);
            mask |= PM_CHG_PORT(PM_CHG_P_OUTPUT, p);
        }
        break;
    case PM_AC_FAIL_RECORD:
        w->scene_len = current_output(n, p, w->scene);
        mask |= PM_CHG_PORT(PM_CHG_P_OUTPUT, p);
        break;
    case PM_AC_MERGE_LTP0:
    case PM_AC_MERGE_HTP0: {
        pm_merge_mode_t mm = cmd == PM_AC_MERGE_LTP0 ? PM_MERGE_LTP : PM_MERGE_HTP;
        if (w->merge != mm) {
            w->merge = mm;
            mask |= PM_CHG_PORT(PM_CHG_P_MERGE, p);
        }
        break;
    }
    case PM_AC_DIRECTION_TX0:
    case PM_AC_DIRECTION_RX0: {
        pm_port_dir_t dir = cmd == PM_AC_DIRECTION_TX0 ? PM_PORT_DIR_OUTPUT : PM_PORT_DIR_INPUT;
        if (dir == PM_PORT_DIR_INPUT) clear_subscribers(n, p); /* spec: AcDirectionRx clears the subscriber list */
        if (w->direction != dir) {
            w->direction = dir;
            mask |= PM_CHG_PORT(PM_CHG_P_DIRECTION, p);
        }
        break;
    }
    case PM_AC_CLEAR_OP0:
        memset(n->merge[p].out, 0, sizeof(n->merge[p].out));
        for (int i = 0; i < 2; i++) memset(n->merge[p].src[i].data, 0, sizeof(n->merge[p].src[i].data));
        if (n->ops.port_clear) n->ops.port_clear(n->ops.ctx, (pm_port_idx_t)p);
        break;
    case PM_AC_STYLE_DELTA0:
    case PM_AC_STYLE_CONST0: {
        pm_output_style_t os = cmd == PM_AC_STYLE_DELTA0 ? PM_OUTPUT_DELTA : PM_OUTPUT_CONTINUOUS;
        if (w->output_style != os) {
            w->output_style = os;
            mask |= PM_CHG_PORT(PM_CHG_P_OUTPUT, p);
        }
        break;
    }
    case PM_AC_RDM_ENABLE0:
    case PM_AC_RDM_DISABLE0: {
        bool en = cmd == PM_AC_RDM_ENABLE0;
        if (w->rdm_enabled != en) {
            w->rdm_enabled = en;
            mask |= PM_CHG_PORT(PM_CHG_P_RDM, p);
        }
        break;
    }
    default: /* AcNone, AcResetRxFlags, AcAnalysis*, AcArtNetSel (always), AcAcnSel (no sACN), AcBqp, deprecated
                port 1-3 commands (one port per BindIndex) */
        break;
    }
    commit_change(n, mask, PM_SRC_ARTNET, src, (uint8_t)(1u << p));
}

static void on_input(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artinput_t in;
    if (pm_artinput_decode(data, len, &in) != PM_OK) return;
    int pi = page_of_bind(n, in.bind_index);
    if (pi < 0) return;
    unsigned p = (unsigned)pi;
    pm_change_mask_t mask = 0;
    begin_change(n);
    if (n->work.port[p].direction == PM_PORT_DIR_INPUT) { /* ArtInput only affects DMX inputs (digest §4.2) */
        bool en = !(in.input[0] & 0x01u);
        if (n->work.port[p].enabled != en) {
            n->work.port[p].enabled = en;
            mask |= PM_CHG_PORT(PM_CHG_P_ENABLED, p);
        }
    }
    commit_change(n, mask, PM_SRC_ARTNET, src, (uint8_t)(1u << p));
}

static void on_ip_prog(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artipprog_t req;
    if (pm_artipprog_decode(data, len, &req) != PM_OK) return;
    pm_settings_defaults(&n->work);
    const pm_ip_settings_t def = n->work.ip;
    begin_change(n);
    pm_ip_settings_t *w = &n->work.ip;
    bool enquiry = !(req.command & PM_IPPROG_ENABLE);
    if (!enquiry) {
        if (req.command & PM_IPPROG_DHCP) {
            w->dhcp = true; /* lower bits ignored */
        } else {
            if (req.command & PM_IPPROG_RESET) *w = def;
            /* invalid values (IP 0/255.255.255.255, non-contiguous or empty mask) are ignored */
            if ((req.command & PM_IPPROG_PROG_IP) && req.ip != 0 && req.ip != 0xFFFFFFFFu) {
                w->static_ip = req.ip;
                w->dhcp = false;
            }
            if ((req.command & PM_IPPROG_PROG_MASK) && req.mask != 0 && ((~req.mask + 1u) & ~req.mask) == 0) {
                w->static_mask = req.mask;
            }
            if (req.command & PM_IPPROG_PROG_GATEWAY) w->gateway = req.gateway;
        }
    }
    const pm_ip_settings_t *o = &n->settings.ip;
    bool changed = w->dhcp != o->dhcp || w->static_ip != o->static_ip || w->static_mask != o->static_mask ||
                   w->gateway != o->gateway;
    pm_ip_settings_t new_ip = *w;
    if (changed && (n->ops.settings_apply == NULL ||
                    n->ops.settings_apply(n->ops.ctx, &n->work, PM_CHG_IP, PM_SRC_IPPROG) != PM_OK)) {
        changed = false; /* not stored: report the unchanged configuration */
        new_ip = n->settings.ip;
    }

    /* Reply from the old address with the new values (contract §5). */
    pm_artipprogreply_t r = {.ip = n->net.ip, .mask = n->net.mask, .gateway = n->net.gateway};
    if (enquiry) {
        r.status = n->settings.ip.dhcp ? 0x40u : 0u;
    } else if (new_ip.dhcp) {
        r.status = 0x40u;
    } else {
        if (new_ip.static_ip != 0) r.ip = new_ip.static_ip;
        r.mask = new_ip.static_mask;
        r.gateway = new_ip.gateway;
    }
    send(n, src, n->tx, pm_artipprogreply_encode(&r, n->tx, sizeof(n->tx)));
    if (changed && n->ops.ip_apply) n->ops.ip_apply(n->ops.ctx);
}

/* ------------------------------------------------------------------------------------------------ TOD / RDM */

static bool rdm_port(const pm_artnet_node_t *n, unsigned p) { return output_active(n, p) && pset(n, p)->rdm_enabled; }

static void send_tod(pm_artnet_node_t *n, uint32_t ip, unsigned p, bool nak)
{
    pm_port_address_t pa = pset(n, p)->address;
    pm_arttoddata_t t = {.rdm_ver = 1, .port = 1, .bind_index = (uint8_t)(p + 1u), .net = PM_PA_NET(pa),
                         .address = (uint8_t)(pa & 0xFFu), .uids = n->tod};
    if (nak || !rdm_port(n, p) || n->ops.tod_get == NULL) { /* no RDM / discovery restarted: TOD not available */
        t.command_response = PM_TOD_NAK;
        send(n, ip, n->tx, pm_arttoddata_encode(&t, n->tx, sizeof(n->tx)));
        return;
    }
    uint16_t total = n->ops.tod_get(n->ops.ctx, (pm_port_idx_t)p, n->tod, PM_ARTNET_TOD_MAX);
    uint16_t cnt = total > PM_ARTNET_TOD_MAX ? (uint16_t)PM_ARTNET_TOD_MAX : total;
    t.command_response = PM_TOD_FULL;
    t.uid_total = cnt;
    uint16_t off = 0;
    do {
        uint16_t k = (uint16_t)(cnt - off);
        if (k > PM_TOD_DATA_MAX_UIDS) k = PM_TOD_DATA_MAX_UIDS;
        t.uid_count = (uint8_t)k;
        t.uids = &n->tod[off];
        send(n, ip, n->tx, pm_arttoddata_encode(&t, n->tx, sizeof(n->tx)));
        t.block_count++;
        off = (uint16_t)(off + k);
    } while (off < cnt);
}

static void tod_subscribe(pm_artnet_node_t *n, uint32_t ip, unsigned p)
{
    tod_sub_t *slot = NULL;
    for (unsigned i = 0; i < PM_ARTNET_MAX_SUBSCRIBERS; i++) {
        tod_sub_t *s = &n->tod_subs[i];
        if (s->used && s->ip == ip) {
            slot = s;
            break;
        }
        if (!s->used && slot == NULL) slot = s;
    }
    if (slot == NULL) slot = &n->tod_subs[n->tod_sub_next++ % PM_ARTNET_MAX_SUBSCRIBERS]; /* full: replace one */
    if (!slot->used || slot->ip != ip) *slot = (tod_sub_t){.used = true, .ip = ip};
    slot->ports |= (uint8_t)(1u << p);
}

static bool tod_addressed(const pm_artnet_node_t *n, unsigned p, uint8_t net, uint8_t address)
{
    pm_port_address_t pa = pset(n, p)->address;
    return PM_PA_NET(pa) == (net & 0x7Fu) && (pa & 0xFFu) == address;
}

static void on_tod_request(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_arttodrequest_t r;
    if (pm_arttodrequest_decode(data, len, &r) != PM_OK || r.command != 0x00) return;
    for (unsigned p = 0; p < num_pages(n); p++) {
        for (unsigned i = 0; i < r.adcount; i++) {
            if (tod_addressed(n, p, r.net, r.address[i])) {
                tod_subscribe(n, src, p);
                send_tod(n, src, p, false);
                break;
            }
        }
    }
}

static void on_tod_control(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_arttodcontrol_t c;
    if (pm_arttodcontrol_decode(data, len, &c) != PM_OK) return;
    pm_change_mask_t mask = 0;
    begin_change(n);
    uint8_t ports = 0;
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (!tod_addressed(n, p, c.net, c.address)) continue;
        ports |= (uint8_t)(1u << p);
        if (c.command == PM_ATC_NONE || c.command > PM_ATC_INC_OFF) continue;
        if (rdm_port(n, p) && n->ops.tod_control) n->ops.tod_control(n->ops.ctx, (pm_port_idx_t)p, c.command);
        if (c.command == PM_ATC_INC_ON || c.command == PM_ATC_INC_OFF) {
            bool on = c.command == PM_ATC_INC_ON;
            if (n->work.port[p].rdm_bg_discovery != on) {
                n->work.port[p].rdm_bg_discovery = on;
                mask |= PM_CHG_PORT(PM_CHG_P_RDM, p);
            }
        }
    }
    if (mask && n->ops.settings_apply) n->ops.settings_apply(n->ops.ctx, &n->work, mask, PM_SRC_ARTNET);
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (!(ports & (1u << p))) continue;
        tod_subscribe(n, src, p);
        send_tod(n, src, p, c.command == PM_ATC_FLUSH); /* the new TOD follows via pm_artnet_tod_changed() */
    }
}

static uint8_t fifo_avail(pm_artnet_node_t *n, unsigned p)
{
    uint8_t a = n->ops.rdm_fifo_avail ? n->ops.rdm_fifo_avail(n->ops.ctx, (pm_port_idx_t)p) : 0;
    if (a > n->port[p].fifo_max_seen) n->port[p].fifo_max_seen = a;
    return a;
}

static uint8_t fifo_max(pm_artnet_node_t *n, unsigned p)
{
    if (n->ops.rdm_fifo_max) return n->ops.rdm_fifo_max(n->ops.ctx, (pm_port_idx_t)p);
    return n->port[p].fifo_max_seen;
}

static void on_rdm(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artrdm_t r;
    if (pm_artrdm_decode(data, len, &r) != PM_OK || r.command != 0x00 || n->ops.rdm_request == NULL) return;
    pm_port_address_t pa = (pm_port_address_t)(((r.net & 0x7Fu) << 8) | r.address);
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (pset(n, p)->address != pa) continue;
        /* Every UID is forwarded on an RDM port (routing node UID / line / broadcast is the platform's job). A port
           without RDM (input, RDM disabled, disabled) only passes requests the node responder handles. */
        if (!rdm_port(n, p)) {
            pm_uid_t dest;
            if (r.rdm_len < 8) continue;
            memcpy(dest.b, r.rdm + 2, 6);
            if (!pm_uid_addresses(&dest, &n->id.uid)) continue;
        }
        rdm_pending_t *slot = &n->rdm[0];
        for (unsigned i = 0; i < PM_ARTNET_MAX_RDM_PENDING; i++) {
            if (!n->rdm[i].used) {
                slot = &n->rdm[i];
                break;
            }
            if (time_before(n->rdm[i].t_ms, slot->t_ms)) slot = &n->rdm[i];
        }
        /* bit 31 stays clear: the platform tags its own requests (front panel) with it */
        n->next_token = (n->next_token + 1u) & PM_ARTNET_RDM_TOKEN_MASK;
        if (n->next_token == 0) n->next_token = 1;
        uint32_t token = n->next_token;
        *slot = (rdm_pending_t){.used = true, .token = token, .ip = src, .port = (pm_port_idx_t)p, .t_ms = n->now};
        const pm_artnet_rdm_request_t req = {.port = (pm_port_idx_t)p, .token = token, .rdm = r.rdm,
                                             .rdm_len = r.rdm_len};
        if (n->ops.rdm_request(n->ops.ctx, &req) != PM_OK && slot->used && slot->token == token) slot->used = false;
    }
}

/* ------------------------------------------------------------------------------------------------ firmware */

static void fw_reply(pm_artnet_node_t *n, uint32_t ip, uint8_t type)
{
    send(n, ip, n->tx, pm_artfirmwarereply_encode(type, n->tx, sizeof(n->tx)));
}

/** A FirmFirst was rejected before sink->begin(): record the reason in the sink (FW_INFO.last_error) via
 *  abort(), which pm_fw.h allows while no session runs. A session of another source (RDM FW_*) is left alone. */
static void fw_report_rejected(pm_artnet_node_t *n, pm_fw_error_t reason)
{
    const pm_fw_sink_t *k = n->ops.fw_sink;
    if (k == NULL || k->abort == NULL) return;
    if (k->status) {
        pm_fw_status_t st;
        k->status(k->ctx, &st);
        if (st.state == PM_FW_STATE_RECEIVING || st.state == PM_FW_STATE_VERIFYING || st.state == PM_FW_STATE_READY) {
            return;
        }
    }
    k->abort(k->ctx, reason);
}

static void fw_abort(pm_artnet_node_t *n, pm_fw_error_t reason)
{
    if (n->fw.active && n->ops.fw_sink && n->ops.fw_sink->abort) n->ops.fw_sink->abort(n->ops.fw_sink->ctx, reason);
    n->fw.active = false;
    pm_artnet_set_node_report(n, RC_FIRMWARE_FAIL, "Firmware upload failed");
}

/** Consume one 1024-byte block of the .alf file: header Length field + image bytes to the sink. */
static pm_fw_error_t fw_consume(pm_artnet_node_t *n, const uint8_t *data)
{
    fw_state_t *f = &n->fw;
    const uint32_t start = f->file_off, end = f->file_off + PM_FIRM_BLOCK_BYTES;
    for (uint32_t o = start; o < end; o++) {
        if (o >= ALF_LEN_OFFSET && o < PM_ALF_HEADER_LEN) {
            f->alf_len = (f->alf_len << 8) | data[o - start];
            if (++f->alf_len_bytes == 4 && f->alf_len != (f->image_len + 1u) / 2u) return PM_FW_ERROR_DESC_MISMATCH;
        }
    }
    uint32_t img_end = PM_ALF_HEADER_LEN + f->image_len;
    uint32_t a = start > PM_ALF_HEADER_LEN ? start : PM_ALF_HEADER_LEN;
    uint32_t b = end < img_end ? end : img_end;
    if (a < b) {
        pm_fw_error_t e = n->ops.fw_sink->write(n->ops.fw_sink->ctx, a - PM_ALF_HEADER_LEN, data + (a - start),
                                                 b - a);
        if (e != PM_FW_ERROR_NONE) return e;
        f->written += b - a;
    }
    f->file_off = end;
    return PM_FW_ERROR_NONE;
}

/** FirmFirst: OEM list + description copy (image-format.md §2.5) -> sink->begin. `*begun` = sink->begin() was
 *  called (from then on the sink itself records errors). */
static pm_fw_error_t fw_first(pm_artnet_node_t *n, uint32_t src, const pm_artfirmwaremaster_t *fm, bool *begun)
{
    *begun = false;
    if (fm->block_id != 0) return PM_FW_ERROR_SEQUENCE;
    bool oem_ok = false;
    for (unsigned i = 0; i < 256; i++) oem_ok |= pm_get_be16(fm->data + ALF_OEM_OFFSET + 2u * i) == n->id.oem;
    if (!oem_ok) return PM_FW_ERROR_OEM;
    pm_image_desc_t desc;
    pm_image_desc_decode(fm->data + ALF_DESC_OFFSET, PM_IMAGE_DESC_SIZE, &desc);
    if (desc.magic != PM_NODE_IMAGE_MAGIC) return PM_FW_ERROR_DESC_MAGIC;
    if (desc.image_length > 0x7FFFFFFFu ||
        fm->firmware_length_words != ALF_HEADER_WORDS + (desc.image_length + 1u) / 2u) {
        return PM_FW_ERROR_DESC_MISMATCH; /* FirmwareLength must match image_length (image-format.md §2.5) */
    }
    const pm_fw_sink_t *sink = n->ops.fw_sink;
    *begun = true;
    pm_fw_error_t e = sink->begin(sink->ctx, &desc); /* steps 1-5 of image-format.md §2.3 */
    if (e != PM_FW_ERROR_NONE) return e;
    n->fw = (fw_state_t){.active = true, .ip = src, .next_block = 1, .words = fm->firmware_length_words,
                         .image_len = desc.image_length, .last_ms = n->now};
    return fw_consume(n, fm->data);
}

static void on_firmware(pm_artnet_node_t *n, uint32_t src, const uint8_t *data, size_t len)
{
    pm_artfirmwaremaster_t fm;
    if (pm_artfirmwaremaster_decode(data, len, &fm) != PM_OK) return;
    const pm_fw_sink_t *sink = n->ops.fw_sink;
    fw_state_t *f = &n->fw;
    if (sink == NULL || fm.type > PM_FIRM_LAST || (f->active && f->ip != src)) {
        /* no upload support / UBEA / a second source while a session runs */
        fw_reply(n, src, PM_FIRM_FAIL);
        return;
    }
    if (f->done && f->done_ip == src && fm.type == PM_FIRM_LAST && fm.block_id == f->done_block) {
        fw_reply(n, src, PM_FIRM_ALL_GOOD); /* our FirmAllGood got lost */
        return;
    }
    if (f->reboot_pending) { /* a new image is about to boot */
        fw_reply(n, src, PM_FIRM_FAIL);
        return;
    }
    pm_fw_error_t e;
    if (fm.type == PM_FIRM_FIRST) {
        if (f->active) fw_abort(n, PM_FW_ERROR_SEQUENCE); /* restart by the same controller */
        bool begun;
        e = fw_first(n, src, &fm, &begun);
        if (e != PM_FW_ERROR_NONE) {
            if (!begun) fw_report_rejected(n, e); /* .alf OEM list, FirmwareLength, description block */
            fw_abort(n, e);
            fw_reply(n, src, PM_FIRM_FAIL);
        } else {
            fw_reply(n, src, PM_FIRM_BLOCK_GOOD);
        }
        return;
    }
    if (!f->active) {
        fw_reply(n, src, PM_FIRM_FAIL);
        return;
    }
    if (fm.type == PM_FIRM_CONT && fm.block_id == (uint8_t)(f->next_block - 1u)) { /* retransmission */
        f->last_ms = n->now;
        fw_reply(n, src, PM_FIRM_BLOCK_GOOD);
        return;
    }
    e = PM_FW_ERROR_NONE;
    if (fm.block_id != f->next_block || fm.firmware_length_words != f->words) e = PM_FW_ERROR_SEQUENCE;
    if (e == PM_FW_ERROR_NONE) e = fw_consume(n, fm.data);
    if (e == PM_FW_ERROR_NONE && fm.type == PM_FIRM_LAST && (f->written != f->image_len || f->alf_len_bytes != 4)) {
        e = PM_FW_ERROR_SEQUENCE; /* FirmLast before all image bytes arrived */
    }
    if (e != PM_FW_ERROR_NONE) {
        fw_abort(n, e);
        fw_reply(n, src, PM_FIRM_FAIL);
        return;
    }
    f->next_block++;
    f->last_ms = n->now;
    if (fm.type != PM_FIRM_LAST) {
        fw_reply(n, src, PM_FIRM_BLOCK_GOOD);
        return;
    }
    f->active = false;
    if (sink->finish(sink->ctx) != PM_FW_ERROR_NONE) { /* steps 6-7: the sink keeps last_error */
        pm_artnet_set_node_report(n, RC_FIRMWARE_FAIL, "Firmware verify failed");
        fw_reply(n, src, PM_FIRM_FAIL);
        return;
    }
    fw_reply(n, src, PM_FIRM_ALL_GOOD);
    f->done = true;
    f->done_ip = src;
    f->done_block = fm.block_id;
    f->reboot_pending = true;
    f->reboot_at = n->now + PM_ARTNET_REBOOT_DELAY_MS;
}

/* ------------------------------------------------------------------------------------------------ input mode */

static void send_input(pm_artnet_node_t *n, unsigned p)
{
    port_state_t *s = &n->port[p];
    s->in_sent_ms = n->now;
    /* local loopback: own output ports on the same universe (our own ArtDmx never comes back, digest §3.2) */
    for (unsigned q = 0; q < num_pages(n); q++) {
        if (q != p && output_active(n, q) && pset(n, q)->address == pset(n, p)->address) {
            dmx_to_port(n, q, n->net.ip, (uint8_t)p, 0, s->in_data, s->in_len, false); /* not part of a sync */
        }
    }
    if (!net_ready(n)) return;
    uint16_t len = s->in_len;
    memcpy(n->frame, s->in_data, len);
    if (len & 1u) n->frame[len++] = 0; /* ArtDmx length should be even */
    if (++s->in_seq == 0) s->in_seq = 1;
    const pm_artdmx_t d = {.sequence = s->in_seq, .physical = (uint8_t)p, .port_address = pset(n, p)->address,
                           .length = len, .data = n->frame};
    int plen = pm_artdmx_encode(&d, n->tx, sizeof(n->tx));
    uint32_t sent[PM_ARTNET_MAX_SUBSCRIBERS + PM_ARTNET_MAX_CONTROLLERS];
    unsigned ns = 0;
    for (unsigned i = 0; i < PM_ARTNET_MAX_SUBSCRIBERS + PM_ARTNET_MAX_CONTROLLERS; i++) {
        uint32_t ip = 0;
        if (i < PM_ARTNET_MAX_SUBSCRIBERS) {
            const in_sub_t *sub = &s->subs[i];
            if (sub->used && !elapsed(n->now, sub->last_ms, PM_ARTNET_SUBSCRIBER_TIMEOUT_MS)) ip = sub->ip;
        } else {
            const controller_t *c = &n->ctl[i - PM_ARTNET_MAX_SUBSCRIBERS];
            if (c->used && !elapsed(n->now, c->last_ms, PM_ARTNET_POLLER_TIMEOUT_MS)) ip = c->ip;
        }
        if (ip == 0 || ip == n->net.ip) continue;
        bool dup = false;
        for (unsigned k = 0; k < ns; k++) dup |= sent[k] == ip;
        if (dup) continue;
        sent[ns++] = ip;
        send(n, ip, n->tx, plen); /* unicast only: broadcast of ArtDmx is forbidden (digest §3.1) */
    }
}

void pm_artnet_input_frame(pm_artnet_node_t *n, pm_port_idx_t port, const uint8_t *data, uint16_t len)
{
    if (n == NULL || port >= num_pages(n) || !input_active(n, port) || data == NULL || len == 0) return;
    now_ms(n);
    if (len > 512) len = 512;
    port_state_t *s = &n->port[port];
    bool changed = !s->in_has || len != s->in_len || memcmp(s->in_data, data, len) != 0;
    memcpy(s->in_data, data, len);
    s->in_len = len;
    s->in_has = true;
    if (changed) send_input(n, port);
    finish(n);
}

static void own_poll(pm_artnet_node_t *n)
{
    bool any = false;
    for (unsigned p = 0; p < num_pages(n); p++) any |= input_active(n, p);
    if (!any || !net_ready(n) || !elapsed(n->now, n->own_poll_ms, PM_ARTNET_OWN_POLL_MS)) return;
    n->own_poll_ms = n->now;
    const pm_artpoll_t poll = {.flags = 0, .diag_priority = 0x10, .esta_man = n->id.esta_man, .oem = n->id.oem};
    send(n, bcast_addr(n), n->tx, pm_artpoll_encode(&poll, n->tx, sizeof(n->tx)));
}

/* ------------------------------------------------------------------------------------------------ public API */

size_t pm_artnet_node_size(void) { return sizeof(struct pm_artnet_node); }

pm_artnet_node_t *pm_artnet_node_init(void *mem, const pm_artnet_identity_t *id, const pm_artnet_ops_t *ops,
                                      const pm_settings_t *settings)
{
    if (mem == NULL || id == NULL || ops == NULL || settings == NULL || id->num_ports > PM_NUM_PORTS) return NULL;
    pm_artnet_node_t *n = mem;
    memset(n, 0, sizeof(*n));
    n->id = *id;
    n->ops = *ops;
    n->settings = *settings;
    n->led = PM_AC_LED_NORMAL;
    n->report_code = RC_POWER_OK;
    snprintf(n->report_text, sizeof(n->report_text), "%s", "Power On Tests successful");
    now_ms(n);
    refresh_net(n);
    n->own_poll_ms = n->now - PM_ARTNET_OWN_POLL_MS; /* first own poll on the first tick */
    n->fps_window_ms = n->now;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        pm_artnet_merge_init(&n->merge[i], settings->port[i].merge);
        n->port[i].last_rx_ms = n->now;
    }
    for (unsigned p = 0; p < num_pages(n); p++) {
        fifo_avail(n, p);
        snapshot_of(n, p, n->snapshot[p]);
    }
    return n;
}

void pm_artnet_handle_packet_ex(pm_artnet_node_t *n, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                                const uint8_t *data, size_t len)
{
    (void)src_port;
    uint16_t op;
    if (n == NULL || data == NULL || pm_artnet_peek_opcode(data, len, &op) != PM_OK) return;
    if (src_ip == 0 || src_ip == n->net.ip) return; /* our own broadcasts */
    now_ms(n);
    switch (op) {
    case PM_OP_POLL: on_poll(n, src_ip, data, len); break;
    case PM_OP_POLL_REPLY: on_poll_reply(n, src_ip, data, len); break;
    case PM_OP_DMX: on_dmx(n, src_ip, data, len); break;
    case PM_OP_SYNC: on_sync(n, src_ip, dst_ip, data, len); break;
    case PM_OP_ADDRESS: on_address(n, src_ip, data, len); break;
    case PM_OP_INPUT: on_input(n, src_ip, data, len); break;
    case PM_OP_IP_PROG: on_ip_prog(n, src_ip, data, len); break;
    case PM_OP_TOD_REQUEST: on_tod_request(n, src_ip, data, len); break;
    case PM_OP_TOD_CONTROL: on_tod_control(n, src_ip, data, len); break;
    case PM_OP_RDM: on_rdm(n, src_ip, data, len); break;
    case PM_OP_FIRMWARE_MASTER: on_firmware(n, src_ip, data, len); break;
    default: break; /* ArtRdmSub (not supported, contract §6.2), ArtNzs, ... */
    }
    finish(n);
}

void pm_artnet_handle_packet(pm_artnet_node_t *n, uint32_t src_ip, uint16_t src_port, const uint8_t *data,
                             size_t len)
{
    pm_artnet_handle_packet_ex(n, src_ip, src_port, 0, data, len);
}

void pm_artnet_tick(pm_artnet_node_t *n)
{
    if (n == NULL) return;
    now_ms(n);
    refresh_net(n);
    for (unsigned p = 0; p < num_pages(n); p++) {
        port_state_t *s = &n->port[p];
        if (output_active(n, p)) {
            pm_artnet_merge_t *m = &n->merge[p];
            if (pm_artnet_merge_expire(m, n->now) && pm_artnet_merge_sources(m) == 1 && !s->data_lost) {
                output_frame(n, p, m->out, m->out_len, n->sync_mode);
            }
            if (!s->data_lost && elapsed(n->now, s->last_rx_ms, PM_ARTNET_DATA_LOSS_MS)) {
                s->data_lost = true;
                apply_failsafe(n, p);
            }
        } else if (input_active(n, p) && s->in_has && elapsed(n->now, s->in_sent_ms, PM_ARTNET_INPUT_KEEPALIVE_MS)) {
            pm_artnet_port_status_t st;
            port_status(n, p, &st);
            if (st.input_signal) send_input(n, p); /* a failed input stops sending */
        }
    }
    if (n->sync_mode && elapsed(n->now, n->last_sync_ms, PM_ARTNET_SYNC_TIMEOUT_MS)) leave_sync(n);
    uint32_t window = n->now - n->fps_window_ms;
    if (window >= 1000u) {
        for (unsigned p = 0; p < PM_NUM_PORTS; p++) {
            port_state_t *s = &n->port[p];
            uint32_t fps = window < 2000u ? ((uint32_t)s->rx_count * 1000u + window / 2u) / window : 0u;
            s->rx_fps = (uint8_t)(fps > 255u ? 255u : fps);
            s->rx_count = 0;
        }
        n->fps_window_ms = n->now;
    }
    own_poll(n);
    if (n->fw.active && elapsed(n->now, n->fw.last_ms, PM_ARTNET_FIRMWARE_TIMEOUT_MS)) {
        fw_abort(n, PM_FW_ERROR_TIMEOUT);
    }
    if (n->fw.reboot_pending && !time_before(n->now, n->fw.reboot_at)) {
        n->fw.reboot_pending = false;
        if (n->ops.fw_sink && n->ops.fw_sink->reboot) n->ops.fw_sink->reboot(n->ops.fw_sink->ctx);
    }
    for (unsigned i = 0; i < PM_ARTNET_MAX_RDM_PENDING; i++) {
        if (n->rdm[i].used && elapsed(n->now, n->rdm[i].t_ms, PM_ARTNET_RDM_TIMEOUT_MS)) n->rdm[i].used = false;
    }
    /* drop expired entries, so they cannot come back after the 2^32 ms wrap-around */
    for (unsigned i = 0; i < PM_ARTNET_MAX_CONTROLLERS; i++) {
        if (n->ctl[i].used && elapsed(n->now, n->ctl[i].last_ms, PM_ARTNET_CONTROLLER_KEEP_MS)) n->ctl[i].used = false;
    }
    for (unsigned p = 0; p < PM_NUM_PORTS; p++) {
        for (unsigned i = 0; i < PM_ARTNET_MAX_SUBSCRIBERS; i++) {
            in_sub_t *sub = &n->port[p].subs[i];
            if (sub->used && elapsed(n->now, sub->last_ms, PM_ARTNET_SUBSCRIBER_TIMEOUT_MS)) sub->used = false;
        }
    }
    finish(n);
}

void pm_artnet_settings_changed(pm_artnet_node_t *n, const pm_settings_t *s, pm_change_mask_t mask)
{
    (void)mask; /* the comparison with the previous copy is authoritative */
    if (n == NULL || s == NULL) return;
    now_ms(n);
    bool failsafe_changed[PM_NUM_PORTS];
    for (unsigned p = 0; p < PM_NUM_PORTS; p++) {
        const pm_port_settings_t *o = &n->settings.port[p], *x = &s->port[p];
        failsafe_changed[p] = o->failsafe != x->failsafe || o->scene_len != x->scene_len ||
                              memcmp(o->scene, x->scene, x->scene_len <= 512 ? x->scene_len : 512) != 0;
        if (o->direction != x->direction || o->address != x->address || o->enabled != x->enabled) {
            pm_artnet_merge_init(&n->merge[p], x->merge);
            port_state_t *ps = &n->port[p];
            ps->rx_ever = false;
            ps->data_lost = false;
            ps->last_rx_ms = n->now;
            ps->in_has = false;
            if (o->direction != x->direction || o->address != x->address) clear_subscribers(n, p);
        }
        n->merge[p].mode = x->merge;
    }
    if (s != &n->settings) n->settings = *s;
    for (unsigned p = 0; p < num_pages(n); p++) {
        if (failsafe_changed[p] && output_active(n, p) && n->port[p].data_lost) apply_failsafe(n, p);
    }
    for (unsigned i = 0; i < MAX_PENDING_REPLIES; i++) {
        if (n->pend[i].used && n->pend[i].await_settings) {
            n->pend[i].await_settings = false;
            n->pend[i].due_ms = n->now;
        }
    }
    finish(n);
}

void pm_artnet_net_changed(pm_artnet_node_t *n)
{
    if (n == NULL) return;
    now_ms(n);
    refresh_net(n);
    finish(n);
}

void pm_artnet_port_status_changed(pm_artnet_node_t *n, pm_port_idx_t port)
{
    (void)port;
    if (n == NULL) return;
    now_ms(n);
    finish(n);
}

void pm_artnet_tod_changed(pm_artnet_node_t *n, pm_port_idx_t port)
{
    if (n == NULL || port >= num_pages(n)) return;
    now_ms(n);
    for (unsigned i = 0; i < PM_ARTNET_MAX_SUBSCRIBERS; i++) {
        if (n->tod_subs[i].used && (n->tod_subs[i].ports & (1u << port))) send_tod(n, n->tod_subs[i].ip, port, false);
    }
    finish(n);
}

void pm_artnet_rdm_response(pm_artnet_node_t *n, uint32_t token, const uint8_t *rdm, size_t rdm_len)
{
    if (n == NULL || rdm == NULL || rdm_len == 0) return;
    for (unsigned i = 0; i < PM_ARTNET_MAX_RDM_PENDING; i++) {
        rdm_pending_t *e = &n->rdm[i];
        if (!e->used || e->token != token) continue;
        e->used = false;
        pm_port_address_t pa = pset(n, e->port)->address;
        const pm_artrdm_t r = {.rdm_ver = 1, .fifo_avail = fifo_avail(n, e->port), .fifo_max = fifo_max(n, e->port),
                               .net = PM_PA_NET(pa), .command = 0, .address = (uint8_t)(pa & 0xFFu), .rdm = rdm,
                               .rdm_len = rdm_len};
        send(n, e->ip, n->tx, pm_artrdm_encode(&r, n->tx, sizeof(n->tx)));
        return;
    }
}

void pm_artnet_diag(pm_artnet_node_t *n, uint8_t priority, const char *text)
{
    if (n == NULL || text == NULL || !net_ready(n)) return;
    now_ms(n);
    unsigned count = 0;
    uint8_t min_prio = 0xFF;
    const controller_t *one = NULL;
    for (unsigned i = 0; i < PM_ARTNET_MAX_CONTROLLERS; i++) {
        const controller_t *c = &n->ctl[i];
        if (!c->used || !(c->flags & PM_POLL_FLAG_DIAG_ENABLE) ||
            elapsed(n->now, c->last_ms, PM_ARTNET_CONTROLLER_KEEP_MS)) {
            continue;
        }
        count++;
        one = c;
        if (c->diag_priority < min_prio) min_prio = c->diag_priority;
    }
    if (count == 0 || priority < min_prio) return;
    const pm_artdiagdata_t d = {.priority = priority, .logical_port = 0, .text = text};
    int len = pm_artdiagdata_encode(&d, n->tx, sizeof(n->tx));
    /* several requesters -> broadcast (digest §2.1); one -> as it asked */
    uint32_t ip = (count == 1 && (one->flags & PM_POLL_FLAG_DIAG_UNICAST)) ? one->ip : bcast_addr(n);
    send(n, ip, n->tx, len);
}

void pm_artnet_set_node_report(pm_artnet_node_t *n, uint16_t code, const char *text)
{
    if (n == NULL) return;
    n->report_code = code;
    snprintf(n->report_text, sizeof(n->report_text), "%s", text ? text : "");
}

/* ------------------------------------------------------------------------------------------------ Phase 2 additions */

void pm_artnet_set_indicator(pm_artnet_node_t *n, uint8_t ac_led_command)
{
    if (n == NULL || n->led == ac_led_command) return;
    if (ac_led_command != PM_AC_LED_NORMAL && ac_led_command != PM_AC_LED_MUTE && ac_led_command != PM_AC_LED_LOCATE) {
        return;
    }
    now_ms(n);
    n->led = ac_led_command;
    finish(n); /* Status1 indicator bits changed: unsolicited ArtPollReply */
}

uint8_t pm_artnet_indicator(const pm_artnet_node_t *n) { return n != NULL ? n->led : PM_AC_LED_NORMAL; }

void pm_artnet_port_live(const pm_artnet_node_t *n, pm_port_idx_t port, pm_artnet_port_live_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    if (n == NULL || port >= num_pages(n) || !output_active(n, port)) return;
    const port_state_t *s = &n->port[port];
    const pm_artnet_merge_t *m = &n->merge[port];
    out->rx_active = s->rx_ever && !elapsed(n->now, s->last_rx_ms, PM_ARTNET_RX_ACTIVE_MS);
    out->rx_fps = out->rx_active ? s->rx_fps : 0;
    out->merge_sources = (uint8_t)pm_artnet_merge_sources(m);
    out->merging = out->merge_sources == 2;
    out->data_loss = s->data_lost;
    out->synced = n->sync_mode;
}
