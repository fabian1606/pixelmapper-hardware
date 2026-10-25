/*
 * pm_rdm_responder.c - the node's own RDM responder (pm_rdm_responder.h).
 *
 * Table driven: one row per PID {pid, count, allowed CCs, PDL range for GET/SET, handler}. A lookup gives the
 * UNKNOWN_PID / UNSUPPORTED_COMMAND_CLASS / FORMAT_ERROR checks in the order OLA expects (rdm-digest.md §8);
 * handlers only check values (DATA_OUT_OF_RANGE) and build the PD.
 *
 * Queued messages store only PIDs (deduplicated, last writer wins); the content is the GET response at the time
 * it is fetched (contract §6.3, §7).
 */
#include "pm_rdm_responder.h"

#include <string.h>

#include "pm_pids.h"

#define RDM_PROTOCOL_VERSION 0x0100u
#define HW_TYPE_ETHERNET 0x0001u   /* E1.37-2 hardware type (ARP hardware type 1) */
#define LABEL_MAX 32u              /* E1.20 label/description length */
#define MANUFACTURER_PID_FIRST 0x8000u
#define STATUS_ENTRY_LEN 9u
#define STATUS_MAX_PER_RESPONSE (PM_RDM_MAX_PDL / STATUS_ENTRY_LEN) /* 25 */
#define ALL_SUB_DEVICES 0xFFFFu
#define INTERFACE_LABEL "eth0"

#define CC_GET 0x01u               /* same bits as PARAMETER_DESCRIPTION command_class */
#define CC_SET 0x02u
#define RES_ACK (-1)               /* handler result: ACK; >= 0 = NACK reason */

typedef struct {
    uint16_t id;
    uint8_t type;
    int16_t data1, data2;
} status_t;

enum { LAST_NONE = 0, LAST_PID, LAST_STATUS };

struct pm_rdm_responder {
    pm_rdm_responder_identity_t id;
    pm_rdm_responder_ops_t ops;
    bool identify;
    /* queued messages: PIDs, oldest first */
    uint8_t q_count;
    uint16_t queue[PM_RDM_RESP_QUEUE_LEN];
    /* status messages, oldest first */
    uint8_t st_count;
    status_t status[PM_RDM_STATUS_QUEUE_LEN];
    /* what QUEUED_MESSAGE / STATUS_MESSAGES delivered last (GET_LAST_MESSAGE) */
    uint8_t last_kind;
    uint16_t last_pid;
    uint8_t last_st_count;
    status_t last_status[PM_RDM_STATUS_QUEUE_LEN];
    /* LOCAL_RDM_SET */
    struct {
        uint16_t seq;
        uint8_t port;              /* 1-based, 0 = none yet */
        pm_uid_t uid;
        uint16_t sub_device;
        uint16_t pid;
    } local_set;
    /* deferred actions */
    bool reset_pending;
    uint8_t reset_kind;
    uint32_t reset_due;
    bool reboot_pending;
    uint32_t reboot_due;
    bool fw_active;
    uint32_t fw_last_ms;
    /* settings: last known copy (change detection) + scratch buffers (kept off the stack, ~2 KB each).
     * `work` is used by request handlers, `tmp` by settings_changed (re-entered from ops->settings_apply). */
    pm_settings_t last, work, tmp;
};

typedef struct {
    const pm_rdm_msg_t *req;
    uint16_t resp_pid;             /* PID of the response (differs for QUEUED_MESSAGE) */
    uint8_t out_len;
    uint8_t out[PM_RDM_MAX_PDL];
} ctx_t;

typedef int (*handler_t)(pm_rdm_responder_t *r, ctx_t *c);

typedef struct {
    uint16_t pid;
    uint8_t count;                 /* consecutive PIDs (port-indexed rows) */
    uint8_t cc;                    /* CC_GET | CC_SET */
    uint8_t get_min, get_max;      /* request PDL range */
    uint8_t set_min, set_max;
    handler_t fn;
} pid_entry_t;

/* ------------------------------------------------------------------------------------------------ descriptions */

typedef struct {
    uint16_t pid;
    uint8_t pdl, type, cc, unit, prefix;
    int32_t min, max, def;
    const char *text;
} desc_t;

#define DESC_ROW(p, pdl_, type_, cc_, unit_, prefix_, min_, max_, def_, text_) \
    {p, pdl_, type_, cc_, unit_, prefix_, min_, max_, def_, text_},
static const desc_t DESCS[] = {PM_NODE_PID_DESC_LIST(DESC_ROW)};
#undef DESC_ROW

#define STATUS_ROW(id_, text_) {id_, text_},
static const struct {
    uint16_t id;
    const char *text;
} STATUS_DESCS[] = {PM_STATUS_ID_DESC_LIST(STATUS_ROW)};
#undef STATUS_ROW

static const uint16_t STD_LISTED[] = {PM_NODE_STD_PIDS_LISTED};

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

static const desc_t *desc_find(uint16_t pid)
{
    for (size_t i = 0; i < ARRAY_LEN(DESCS); i++) {
        if (DESCS[i].pid == pid) return &DESCS[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ helpers */

static uint32_t now_ms(pm_rdm_responder_t *r) { return r->ops.now_ms ? r->ops.now_ms(r->ops.ctx) : 0; }

static bool time_reached(uint32_t now, uint32_t due) { return (int32_t)(now - due) >= 0; }

static void settings_get(pm_rdm_responder_t *r, pm_settings_t *s)
{
    if (r->ops.settings_get) {
        r->ops.settings_get(r->ops.ctx, s);
    } else {
        pm_settings_defaults(s);
    }
}

/* Persist a modified copy. Must be the last use of `s` in a handler: the platform re-enters
 * pm_rdm_responder_settings_changed() from inside settings_apply. */
static int settings_apply(pm_rdm_responder_t *r, const pm_settings_t *s, pm_change_mask_t mask)
{
    if (r->ops.settings_apply == NULL) return PM_NR_HARDWARE_FAULT;
    return r->ops.settings_apply(r->ops.ctx, s, mask) == PM_OK ? RES_ACK : PM_NR_HARDWARE_FAULT;
}

static uint8_t hw_revision(const pm_rdm_responder_t *r, const pm_settings_t *s)
{
    return s->hw_revision_override != 0xFFu ? s->hw_revision_override : r->id.hw_revision;
}

static int put_label(ctx_t *c, const char *s)
{
    size_t n = s ? strnlen(s, LABEL_MAX) : 0;
    if (n) memcpy(c->out, s, n);
    c->out_len = (uint8_t)n;
    return RES_ACK;
}

static int put_u8(ctx_t *c, uint8_t v)
{
    c->out[0] = v;
    c->out_len = 1;
    return RES_ACK;
}

static uint8_t mask_to_cidr(uint32_t mask)
{
    uint8_t n = 0;
    while (n < 32u && (mask & (0x80000000u >> n))) n++;
    return n;
}

static uint32_t cidr_to_mask(uint8_t cidr) { return cidr == 0 ? 0 : 0xFFFFFFFFu << (32u - cidr); }

static bool interface_ok(const ctx_t *c) { return pm_get_be32(c->req->pd) == PM_RDM_RESP_INTERFACE_ID; }

/* ------------------------------------------------------------------------------------------------ queues */

void pm_rdm_responder_queue_pid(pm_rdm_responder_t *r, uint16_t pid)
{
    if (r == NULL) return;
    /* dedupe: an existing entry is removed and the PID goes to the end (content = GET at fetch time) */
    for (uint8_t i = 0; i < r->q_count; i++) {
        if (r->queue[i] == pid) {
            memmove(&r->queue[i], &r->queue[i + 1], (size_t)(r->q_count - i - 1u) * sizeof(r->queue[0]));
            r->q_count--;
            break;
        }
    }
    if (r->q_count >= PM_RDM_RESP_QUEUE_LEN) { /* full: drop the oldest */
        memmove(&r->queue[0], &r->queue[1], (PM_RDM_RESP_QUEUE_LEN - 1u) * sizeof(r->queue[0]));
        r->q_count--;
    }
    r->queue[r->q_count++] = pid;
}

static uint8_t severity(uint8_t type) { return type & 0x0Fu; } /* 0x12..0x14 (cleared) -> 2..4 */

void pm_rdm_responder_status(pm_rdm_responder_t *r, uint16_t status_id, uint8_t type, int16_t data1, int16_t data2)
{
    if (r == NULL) return;
    uint8_t sev = severity(type);
    if (sev < PM_RDM_STATUS_ADVISORY || sev > PM_RDM_STATUS_ERROR || (type >> 4) > 1u) return;
    if (r->st_count >= PM_RDM_STATUS_QUEUE_LEN) { /* full: drop the oldest */
        memmove(&r->status[0], &r->status[1], (PM_RDM_STATUS_QUEUE_LEN - 1u) * sizeof(r->status[0]));
        r->st_count--;
    }
    r->status[r->st_count++] = (status_t){status_id, type, data1, data2};
}

uint8_t pm_rdm_responder_message_count(const pm_rdm_responder_t *r)
{
    if (r == NULL) return 0;
    unsigned n = (unsigned)r->q_count + r->st_count;
    return (uint8_t)(n > 255u ? 255u : n);
}

static void put_status_list(ctx_t *c, const status_t *list, uint8_t n)
{
    uint8_t *p = c->out;
    for (uint8_t i = 0; i < n && i < STATUS_MAX_PER_RESPONSE; i++) {
        pm_put_be16(p, 0); /* sub-device: root */
        p[2] = list[i].type;
        pm_put_be16(p + 3, list[i].id);
        pm_put_be16(p + 5, (uint16_t)list[i].data1);
        pm_put_be16(p + 7, (uint16_t)list[i].data2);
        p += STATUS_ENTRY_LEN;
    }
    c->out_len = (uint8_t)(p - c->out);
}

/* Deliver (and remove) the status messages of at least severity `type`. */
static int deliver_status(pm_rdm_responder_t *r, ctx_t *c, uint8_t type)
{
    uint8_t keep = 0, n = 0;
    for (uint8_t i = 0; i < r->st_count; i++) {
        if (severity(r->status[i].type) >= type && n < STATUS_MAX_PER_RESPONSE) {
            r->last_status[n++] = r->status[i];
        } else {
            r->status[keep++] = r->status[i];
        }
    }
    r->st_count = keep;
    r->last_st_count = n;
    r->last_kind = LAST_STATUS;
    put_status_list(c, r->last_status, n);
    return RES_ACK;
}

/* ------------------------------------------------------------------------------------------------ handlers */

static const pid_entry_t *pid_find(uint16_t pid);

/* Run the GET handler of `pid` with an empty request (QUEUED_MESSAGE delivery). */
static int run_get(pm_rdm_responder_t *r, ctx_t *c, uint16_t pid)
{
    static const uint8_t none[1] = {0};
    pm_rdm_msg_t q;
    memset(&q, 0, sizeof(q));
    q.cc = PM_RDM_CC_GET;
    q.pid = pid;
    q.pd = none;
    c->resp_pid = pid;
    const pid_entry_t *e = pid_find(pid);
    if (e == NULL) return PM_NR_UNKNOWN_PID;
    if (!(e->cc & CC_GET)) return PM_NR_UNSUPPORTED_COMMAND_CLASS;
    const pm_rdm_msg_t *saved = c->req;
    c->req = &q;
    int res = e->fn(r, c);
    c->req = saved;
    return res;
}

/* ---- E1.20 status / queue ---- */

static int h_queued_message(pm_rdm_responder_t *r, ctx_t *c)
{
    uint8_t type = c->req->pd[0];
    if (type < PM_RDM_STATUS_GET_LAST_MESSAGE || type > PM_RDM_STATUS_ERROR) return PM_NR_DATA_OUT_OF_RANGE;
    if (type == PM_RDM_STATUS_GET_LAST_MESSAGE) {
        if (r->last_kind == LAST_PID) return run_get(r, c, r->last_pid);
        c->resp_pid = PM_RDM_PID_STATUS_MESSAGES;
        if (r->last_kind == LAST_STATUS) put_status_list(c, r->last_status, r->last_st_count);
        return RES_ACK;
    }
    if (r->q_count > 0) {
        uint16_t pid = r->queue[0];
        memmove(&r->queue[0], &r->queue[1], (size_t)(r->q_count - 1u) * sizeof(r->queue[0]));
        r->q_count--;
        r->last_kind = LAST_PID;
        r->last_pid = pid;
        return run_get(r, c, pid);
    }
    c->resp_pid = PM_RDM_PID_STATUS_MESSAGES;
    return deliver_status(r, c, type);
}

static int h_status_messages(pm_rdm_responder_t *r, ctx_t *c)
{
    uint8_t type = c->req->pd[0];
    if (type == PM_RDM_STATUS_NONE) return RES_ACK; /* empty list */
    if (type > PM_RDM_STATUS_ERROR) return PM_NR_DATA_OUT_OF_RANGE;
    if (type == PM_RDM_STATUS_GET_LAST_MESSAGE) {
        put_status_list(c, r->last_status, r->last_st_count);
        return RES_ACK;
    }
    return deliver_status(r, c, type);
}

static int h_status_id_description(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)r;
    uint16_t id = pm_get_be16(c->req->pd);
    for (size_t i = 0; i < ARRAY_LEN(STATUS_DESCS); i++) {
        if (STATUS_DESCS[i].id == id) return put_label(c, STATUS_DESCS[i].text);
    }
    return PM_NR_DATA_OUT_OF_RANGE;
}

static int h_clear_status_id(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)c;
    r->st_count = 0;
    return RES_ACK;
}

/* ---- E1.20 information ---- */

static int h_supported_parameters(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)r;
    uint8_t *p = c->out;
    for (size_t i = 0; i < ARRAY_LEN(STD_LISTED); i++, p += 2) pm_put_be16(p, STD_LISTED[i]);
    for (size_t i = 0; i < ARRAY_LEN(DESCS); i++, p += 2) pm_put_be16(p, DESCS[i].pid);
    c->out_len = (uint8_t)(p - c->out);
    return RES_ACK;
}

static int h_parameter_description(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)r;
    uint16_t pid = pm_get_be16(c->req->pd);
    const desc_t *d = pid >= MANUFACTURER_PID_FIRST ? desc_find(pid) : NULL;
    if (d == NULL) return PM_NR_DATA_OUT_OF_RANGE;
    uint8_t *p = c->out;
    pm_put_be16(p, d->pid);
    p[2] = d->pdl;
    p[3] = d->type;
    p[4] = d->cc;
    p[5] = 0; /* type: always 0 */
    p[6] = d->unit;
    p[7] = d->prefix;
    pm_put_be32(p + 8, (uint32_t)d->min);
    pm_put_be32(p + 12, (uint32_t)d->max);
    pm_put_be32(p + 16, (uint32_t)d->def);
    size_t n = strnlen(d->text, LABEL_MAX);
    memcpy(p + 20, d->text, n);
    c->out_len = (uint8_t)(20u + n);
    return RES_ACK;
}

static int h_device_info(pm_rdm_responder_t *r, ctx_t *c)
{
    uint8_t *p = c->out;
    pm_put_be16(p + 0, RDM_PROTOCOL_VERSION);
    pm_put_be16(p + 2, r->id.model_id);
    pm_put_be16(p + 4, PM_NODE_PRODUCT_CATEGORY);
    pm_put_be32(p + 6, r->id.sw_version_id);
    pm_put_be16(p + 10, 0);      /* footprint */
    p[12] = 0;                   /* current personality */
    p[13] = 0;                   /* personality count */
    pm_put_be16(p + 14, 0xFFFF); /* start address: none (footprint 0) */
    pm_put_be16(p + 16, 0);      /* sub-devices */
    p[18] = 0;                   /* sensors */
    c->out_len = 19;
    return RES_ACK;
}

static int h_product_detail(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)r;
    pm_put_be16(c->out, PM_NODE_PRODUCT_DETAIL);
    c->out_len = 2;
    return RES_ACK;
}

static int h_model_description(pm_rdm_responder_t *r, ctx_t *c) { return put_label(c, r->id.model_description); }
static int h_manufacturer_label(pm_rdm_responder_t *r, ctx_t *c) { return put_label(c, r->id.manufacturer_label); }
static int h_sw_version_label(pm_rdm_responder_t *r, ctx_t *c) { return put_label(c, r->id.sw_version_label); }
static int h_boot_sw_version_label(pm_rdm_responder_t *r, ctx_t *c)
{
    return put_label(c, r->id.boot_sw_version_label);
}

static int h_boot_sw_version_id(pm_rdm_responder_t *r, ctx_t *c)
{
    pm_put_be32(c->out, r->id.boot_sw_version_id);
    c->out_len = 4;
    return RES_ACK;
}

static int h_device_label(pm_rdm_responder_t *r, ctx_t *c)
{
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    if (c->req->cc == PM_RDM_CC_GET) {
        size_t n = strnlen(s->short_name, PM_SHORT_NAME_LEN - 1u);
        memcpy(c->out, s->short_name, n);
        c->out_len = (uint8_t)n;
        return RES_ACK;
    }
    /* longer labels are cut to the Art-Net short name (17 chars), a NUL ends the text */
    size_t n = 0;
    while (n < c->req->pdl && n < PM_SHORT_NAME_LEN - 1u && c->req->pd[n] != 0) n++;
    memset(s->short_name, 0, sizeof(s->short_name));
    memcpy(s->short_name, c->req->pd, n);
    return settings_apply(r, s, PM_CHG_NAMES);
}

static int h_factory_defaults(pm_rdm_responder_t *r, ctx_t *c)
{
    if (c->req->cc == PM_RDM_CC_SET) {
        if (r->ops.factory_defaults == NULL) return PM_NR_HARDWARE_FAULT;
        r->ops.factory_defaults(r->ops.ctx);
        return RES_ACK;
    }
    pm_settings_t *s = &r->work, *d = &r->tmp;
    settings_get(r, s);
    pm_settings_defaults(d);
    d->version = s->version;
    d->hw_revision_override = s->hw_revision_override;
    return put_u8(c, (pm_settings_diff(s, d) & ~(pm_change_mask_t)PM_CHG_HW_REV) == 0 ? 1 : 0);
}

/* ---- E1.20 control ---- */

static int h_display_level(pm_rdm_responder_t *r, ctx_t *c)
{
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    if (c->req->cc == PM_RDM_CC_GET) return put_u8(c, s->display_level);
    s->display_level = c->req->pd[0];
    return settings_apply(r, s, PM_CHG_DISPLAY);
}

static int h_identify(pm_rdm_responder_t *r, ctx_t *c)
{
    if (c->req->cc == PM_RDM_CC_GET) return put_u8(c, r->identify ? 1 : 0);
    if (c->req->pd[0] > 1) return PM_NR_DATA_OUT_OF_RANGE;
    r->identify = c->req->pd[0] != 0;
    if (r->ops.identify) r->ops.identify(r->ops.ctx, r->identify);
    return RES_ACK;
}

static int h_reset_device(pm_rdm_responder_t *r, ctx_t *c)
{
    uint8_t kind = c->req->pd[0];
    if (kind != 0x01 && kind != 0xFF) return PM_NR_DATA_OUT_OF_RANGE;
    r->reset_pending = true;
    r->reset_kind = kind;
    r->reset_due = now_ms(r) + PM_RDM_RESP_RESET_DELAY_MS; /* after the response has left */
    return RES_ACK;
}

/* ---- E1.37-2 ---- */

static void net_info(pm_rdm_responder_t *r, pm_rdm_net_info_t *n)
{
    memset(n, 0, sizeof(*n));
    if (r->ops.net_info) r->ops.net_info(r->ops.ctx, n);
}

static int h_list_interfaces(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)r;
    pm_put_be32(c->out, PM_RDM_RESP_INTERFACE_ID);
    pm_put_be16(c->out + 4, HW_TYPE_ETHERNET);
    c->out_len = 6;
    return RES_ACK;
}

static int h_interface_label(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)r;
    if (!interface_ok(c)) return PM_NR_DATA_OUT_OF_RANGE;
    pm_put_be32(c->out, PM_RDM_RESP_INTERFACE_ID);
    size_t n = strlen(INTERFACE_LABEL);
    memcpy(c->out + 4, INTERFACE_LABEL, n);
    c->out_len = (uint8_t)(4u + n);
    return RES_ACK;
}

static int h_hw_address(pm_rdm_responder_t *r, ctx_t *c)
{
    if (!interface_ok(c)) return PM_NR_DATA_OUT_OF_RANGE;
    pm_rdm_net_info_t n;
    net_info(r, &n);
    pm_put_be32(c->out, PM_RDM_RESP_INTERFACE_ID);
    memcpy(c->out + 4, n.mac, 6);
    c->out_len = 10;
    return RES_ACK;
}

/* DHCP / static / route SETs are stored at once (settings_apply, PM_CHG_IP) and take effect with
 * INTERFACE_APPLY_CONFIGURATION (ops->net_apply). */
static int h_dhcp_mode(pm_rdm_responder_t *r, ctx_t *c)
{
    if (!interface_ok(c)) return PM_NR_DATA_OUT_OF_RANGE;
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    if (c->req->cc == PM_RDM_CC_GET) {
        pm_put_be32(c->out, PM_RDM_RESP_INTERFACE_ID);
        c->out[4] = s->ip.dhcp ? 1 : 0;
        c->out_len = 5;
        return RES_ACK;
    }
    if (c->req->pd[4] > 1) return PM_NR_DATA_OUT_OF_RANGE;
    s->ip.dhcp = c->req->pd[4] != 0;
    return settings_apply(r, s, PM_CHG_IP);
}

static int h_current_address(pm_rdm_responder_t *r, ctx_t *c)
{
    if (!interface_ok(c)) return PM_NR_DATA_OUT_OF_RANGE;
    pm_rdm_net_info_t n;
    net_info(r, &n);
    pm_put_be32(c->out, PM_RDM_RESP_INTERFACE_ID);
    pm_put_be32(c->out + 4, n.ip);
    c->out[8] = mask_to_cidr(n.mask);
    c->out[9] = n.dhcp_active ? 1 : 0; /* dhcp_status: 0 inactive, 1 active */
    c->out_len = 10;
    return RES_ACK;
}

static int h_static_address(pm_rdm_responder_t *r, ctx_t *c)
{
    if (!interface_ok(c)) return PM_NR_DATA_OUT_OF_RANGE;
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    if (c->req->cc == PM_RDM_CC_GET) {
        /* static_ip 0 = the Art-Net fallback 2.B.C.D: report the address a static configuration uses */
        uint32_t ip = s->ip.static_ip;
        if (ip == 0) {
            pm_rdm_net_info_t n;
            net_info(r, &n);
            ip = pm_ip_artnet_fallback(n.mac, PM_ARTNET_OEM_NODE);
        }
        pm_put_be32(c->out, PM_RDM_RESP_INTERFACE_ID);
        pm_put_be32(c->out + 4, ip);
        c->out[8] = mask_to_cidr(s->ip.static_mask);
        c->out_len = 9;
        return RES_ACK;
    }
    uint8_t cidr = c->req->pd[8];
    if (cidr > 32u) return PM_NR_DATA_OUT_OF_RANGE;
    s->ip.static_ip = pm_get_be32(c->req->pd + 4);
    s->ip.static_mask = cidr_to_mask(cidr);
    return settings_apply(r, s, PM_CHG_IP);
}

static int h_renew_dhcp(pm_rdm_responder_t *r, ctx_t *c)
{
    if (!interface_ok(c)) return PM_NR_DATA_OUT_OF_RANGE;
    pm_rdm_net_info_t n;
    net_info(r, &n);
    if (!n.dhcp_active) return PM_RDM_RESP_NR_ACTION_NOT_SUPPORTED;
    if (r->ops.net_renew_dhcp) r->ops.net_renew_dhcp(r->ops.ctx);
    return RES_ACK;
}

static int h_apply_configuration(pm_rdm_responder_t *r, ctx_t *c)
{
    if (!interface_ok(c)) return PM_NR_DATA_OUT_OF_RANGE;
    if (r->ops.net_apply) r->ops.net_apply(r->ops.ctx);
    return RES_ACK;
}

static int h_default_route(pm_rdm_responder_t *r, ctx_t *c)
{
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    if (c->req->cc == PM_RDM_CC_GET) {
        /* interface 0 + address 0 = no default route */
        pm_put_be32(c->out, s->ip.gateway ? PM_RDM_RESP_INTERFACE_ID : 0u);
        pm_put_be32(c->out + 4, s->ip.gateway);
        c->out_len = 8;
        return RES_ACK;
    }
    uint32_t ifc = pm_get_be32(c->req->pd);
    if (ifc != 0 && ifc != PM_RDM_RESP_INTERFACE_ID) return PM_NR_DATA_OUT_OF_RANGE;
    s->ip.gateway = ifc == 0 ? 0u : pm_get_be32(c->req->pd + 4); /* interface 0 = no default route */
    return settings_apply(r, s, PM_CHG_IP);
}

/* ---- manufacturer: numeric settings (ranges from PM_NODE_PID_DESC_LIST) ---- */

static bool in_port_range(uint16_t pid, uint16_t first, unsigned count, pm_port_idx_t *port)
{
    if (pid < first || pid >= first + count) return false;
    *port = (pm_port_idx_t)(pid - first);
    return *port < PM_NUM_PORTS;
}

static uint32_t num_get(const pm_rdm_responder_t *r, const pm_settings_t *s, uint16_t pid)
{
    pm_port_idx_t p;
    if (in_port_range(pid, PM_PID_PORT_ENABLED(1u), PM_PID_PORT_ENABLED_COUNT, &p)) return s->port[p].enabled;
    if (in_port_range(pid, PM_PID_PORT_DMX_BREAK(1u), PM_PID_PORT_DMX_BREAK_COUNT, &p)) return s->port[p].dmx_break_us;
    if (in_port_range(pid, PM_PID_PORT_DMX_MAB(1u), PM_PID_PORT_DMX_MAB_COUNT, &p)) return s->port[p].dmx_mab_us;
    if (in_port_range(pid, PM_PID_PORT_DMX_RATE(1u), PM_PID_PORT_DMX_RATE_COUNT, &p)) return s->port[p].dmx_rate_hz;
    if (in_port_range(pid, PM_PID_PORT_DMX_MBS(1u), PM_PID_PORT_DMX_MBS_COUNT, &p)) return s->port[p].dmx_mbs_us;
    if (in_port_range(pid, PM_PID_PORT_RDM_BG_DISCOVERY(1u), PM_PID_PORT_RDM_BG_DISCOVERY_COUNT, &p)) {
        return s->port[p].rdm_bg_discovery;
    }
    switch (pid) {
    case PM_PID_NODE_HW_REVISION: return hw_revision(r, s);
    case PM_PID_RDM_BG_DISCOVERY_INTERVAL: return s->rdm_bg_interval_s;
    case PM_PID_DISPLAY_DIM_TIMEOUT: return s->display_dim_timeout_s;
    case PM_PID_DISPLAY_ADDRESS_FORMAT: return s->address_format;
    case PM_PID_PANEL_LOCK: return s->panel_lock;
    default: return 0;
    }
}

/* Store `v` (already range checked); returns the change mask, 0 = not a settable numeric PID. */
static pm_change_mask_t num_set(pm_settings_t *s, uint16_t pid, uint32_t v)
{
    pm_port_idx_t p;
    if (in_port_range(pid, PM_PID_PORT_ENABLED(1u), PM_PID_PORT_ENABLED_COUNT, &p)) {
        s->port[p].enabled = v != 0;
        return PM_CHG_PORT(PM_CHG_P_ENABLED, p);
    }
    if (in_port_range(pid, PM_PID_PORT_DMX_BREAK(1u), PM_PID_PORT_DMX_BREAK_COUNT, &p)) {
        s->port[p].dmx_break_us = (uint16_t)v;
        return PM_CHG_PORT(PM_CHG_P_TIMING, p);
    }
    if (in_port_range(pid, PM_PID_PORT_DMX_MAB(1u), PM_PID_PORT_DMX_MAB_COUNT, &p)) {
        s->port[p].dmx_mab_us = (uint16_t)v;
        return PM_CHG_PORT(PM_CHG_P_TIMING, p);
    }
    if (in_port_range(pid, PM_PID_PORT_DMX_RATE(1u), PM_PID_PORT_DMX_RATE_COUNT, &p)) {
        s->port[p].dmx_rate_hz = (uint8_t)v;
        return PM_CHG_PORT(PM_CHG_P_TIMING, p);
    }
    if (in_port_range(pid, PM_PID_PORT_DMX_MBS(1u), PM_PID_PORT_DMX_MBS_COUNT, &p)) {
        s->port[p].dmx_mbs_us = (uint16_t)v;
        return PM_CHG_PORT(PM_CHG_P_TIMING, p);
    }
    if (in_port_range(pid, PM_PID_PORT_RDM_BG_DISCOVERY(1u), PM_PID_PORT_RDM_BG_DISCOVERY_COUNT, &p)) {
        s->port[p].rdm_bg_discovery = v != 0;
        return PM_CHG_PORT(PM_CHG_P_RDM, p);
    }
    switch (pid) {
    case PM_PID_RDM_BG_DISCOVERY_INTERVAL: s->rdm_bg_interval_s = (uint16_t)v; return PM_CHG_RDM_INTERVAL;
    case PM_PID_DISPLAY_DIM_TIMEOUT: s->display_dim_timeout_s = (uint16_t)v; return PM_CHG_DISPLAY;
    case PM_PID_DISPLAY_ADDRESS_FORMAT: s->address_format = (uint8_t)v; return PM_CHG_DISPLAY;
    case PM_PID_PANEL_LOCK: s->panel_lock = v != 0; return PM_CHG_PANEL_LOCK;
    default: return 0;
    }
}

static int h_num(pm_rdm_responder_t *r, ctx_t *c)
{
    const desc_t *d = desc_find(c->req->pid);
    if (d == NULL || (d->pdl != 1 && d->pdl != 2)) return PM_NR_HARDWARE_FAULT; /* table error */
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    if (c->req->cc == PM_RDM_CC_GET) {
        uint32_t v = num_get(r, s, c->req->pid);
        if (d->pdl == 2) {
            pm_put_be16(c->out, (uint16_t)v);
        } else {
            c->out[0] = (uint8_t)v;
        }
        c->out_len = d->pdl;
        return RES_ACK;
    }
    int32_t v = d->pdl == 2 ? (int32_t)pm_get_be16(c->req->pd) : (int32_t)c->req->pd[0];
    if (v < d->min || v > d->max) return PM_NR_DATA_OUT_OF_RANGE;
    pm_change_mask_t m = num_set(s, c->req->pid, (uint32_t)v);
    if (m == 0) return PM_NR_UNSUPPORTED_COMMAND_CLASS;
    return settings_apply(r, s, m);
}

/* ---- manufacturer: BOOT_CATCH ---- */

static int h_boot_catch_start(pm_rdm_responder_t *r, ctx_t *c)
{
    uint8_t port = c->req->pd[0];
    pm_uid_t target;
    memcpy(target.b, c->req->pd + 1, 6);
    if (port < 1 || port > PM_NODE_PORTS || port > PM_NUM_PORTS) return PM_NR_DATA_OUT_OF_RANGE;
    if (pm_uid_is_vendorcast(&target)) return PM_NR_DATA_OUT_OF_RANGE; /* includes FFFF:FFFFFFFF */
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    const pm_port_settings_t *ps = &s->port[port - 1u];
    if (!ps->enabled || ps->direction != PM_PORT_DIR_OUTPUT || !ps->rdm_enabled) return PM_NR_DATA_OUT_OF_RANGE;
    if (r->ops.boot_catch_start == NULL) return PM_NR_HARDWARE_FAULT;
    if (r->ops.boot_catch_start(r->ops.ctx, (pm_port_idx_t)(port - 1u), &target) != PM_OK) {
        return PM_NR_DATA_OUT_OF_RANGE;
    }
    return RES_ACK;
}

static int h_boot_catch_status(pm_rdm_responder_t *r, ctx_t *c)
{
    pm_rdm_boot_catch_t bc;
    pm_port_idx_t port = 0;
    memset(&bc, 0, sizeof(bc));
    if (r->ops.boot_catch_status) r->ops.boot_catch_status(r->ops.ctx, &port, &bc);
    uint8_t *p = c->out;
    p[0] = bc.state;
    p[1] = bc.state == PM_BOOT_CATCH_STATE_IDLE ? 0 : (uint8_t)(port + 1u); /* 1-based, 0 = never started */
    memcpy(p + 2, bc.target.b, 6);
    pm_put_be32(p + 8, bc.elapsed_ms);
    pm_put_be16(p + 12, bc.attempts);
    c->out_len = PM_PDL_BOOT_CATCH_STATUS_GET_RESPONSE;
    return RES_ACK;
}

static int h_boot_catch_abort(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)c;
    if (r->ops.boot_catch_abort) r->ops.boot_catch_abort(r->ops.ctx);
    return RES_ACK;
}

static int h_local_rdm_set(pm_rdm_responder_t *r, ctx_t *c)
{
    uint8_t *p = c->out;
    pm_put_be16(p, r->local_set.seq);
    p[2] = r->local_set.port;
    memcpy(p + 3, r->local_set.uid.b, 6);
    pm_put_be16(p + 9, r->local_set.sub_device);
    pm_put_be16(p + 11, r->local_set.pid);
    c->out_len = PM_PDL_LOCAL_RDM_SET_GET_RESPONSE;
    return RES_ACK;
}

/* ---- manufacturer: FW_* ---- */

static void fw_status(const pm_rdm_responder_t *r, pm_fw_status_t *st)
{
    memset(st, 0, sizeof(*st));
    const pm_fw_sink_t *k = r->ops.fw_sink;
    if (k && k->status) k->status(k->ctx, st);
}

/* Sink error -> NACK: flash problems are hardware faults, everything else is wrong data (details in
 * FW_INFO.last_error). */
static int fw_nack(pm_fw_error_t e)
{
    return e == PM_FW_ERROR_FLASH ? PM_NR_HARDWARE_FAULT : PM_NR_DATA_OUT_OF_RANGE;
}

static int h_fw_info(pm_rdm_responder_t *r, ctx_t *c)
{
    pm_fw_status_t st;
    fw_status(r, &st);
    pm_settings_t *s = &r->work;
    settings_get(r, s);
    uint8_t *p = c->out;
    pm_put_be16(p + 0, pm_uid_manufacturer(&r->id.uid));
    pm_put_be16(p + 2, r->id.model_id);
    p[4] = hw_revision(r, s);
    pm_put_be32(p + 5, r->id.sw_version_id);
    p[9] = (uint8_t)st.state;
    p[10] = (uint8_t)st.last_error;
    p[11] = PM_NODE_FW_MAX_BLOCK;
    pm_put_be32(p + 12, st.received);
    pm_put_be32(p + 16, st.total);
    c->out_len = PM_PDL_FW_INFO_GET_RESPONSE;
    return RES_ACK;
}

static int h_fw_begin(pm_rdm_responder_t *r, ctx_t *c)
{
    const pm_fw_sink_t *k = r->ops.fw_sink;
    if (k == NULL || k->begin == NULL) return PM_NR_HARDWARE_FAULT;
    pm_image_desc_t d;
    if (pm_image_desc_decode(c->req->pd, c->req->pdl, &d) != PM_OK) return PM_NR_FORMAT_ERROR;
    r->fw_active = false;
    pm_fw_error_t e = k->begin(k->ctx, &d); /* restarts a running session */
    if (e != PM_FW_ERROR_NONE) return fw_nack(e);
    r->fw_active = true;
    r->fw_last_ms = now_ms(r);
    return RES_ACK;
}

static int h_fw_write(pm_rdm_responder_t *r, ctx_t *c)
{
    const pm_fw_sink_t *k = r->ops.fw_sink;
    if (k == NULL || k->write == NULL) return PM_NR_HARDWARE_FAULT;
    if (!r->fw_active) return PM_NR_DATA_OUT_OF_RANGE;
    uint32_t off = pm_get_be32(c->req->pd);
    size_t len = (size_t)c->req->pdl - 4u;
    pm_fw_status_t st;
    fw_status(r, &st);
    if (off > st.received) return PM_NR_DATA_OUT_OF_RANGE; /* gap */
    r->fw_last_ms = now_ms(r);
    if ((uint64_t)off + len <= st.received) return RES_ACK; /* retransmission: already written */
    pm_fw_error_t e = k->write(k->ctx, off, c->req->pd + 4, len);
    if (e != PM_FW_ERROR_NONE) {
        fw_status(r, &st);
        if (st.state != PM_FW_STATE_RECEIVING) r->fw_active = false;
        return fw_nack(e);
    }
    return RES_ACK;
}

static int h_fw_finish(pm_rdm_responder_t *r, ctx_t *c)
{
    (void)c;
    const pm_fw_sink_t *k = r->ops.fw_sink;
    if (k == NULL || k->finish == NULL) return PM_NR_HARDWARE_FAULT;
    if (!r->fw_active) return PM_NR_DATA_OUT_OF_RANGE;
    r->fw_active = false;
    pm_fw_error_t e = k->finish(k->ctx);
    if (e != PM_FW_ERROR_NONE) return fw_nack(e); /* old firmware stays active */
    r->reboot_pending = true;
    r->reboot_due = now_ms(r) + PM_RDM_RESP_REBOOT_DELAY_MS;
    return RES_ACK;
}

/* ------------------------------------------------------------------------------------------------ PID table */

#define G(min, max) CC_GET, (min), (max), 0, 0
#define S(min, max) CC_SET, 0, 0, (min), (max)
#define GS(gmin, gmax, smin, smax) CC_GET | CC_SET, (gmin), (gmax), (smin), (smax)
/* manufacturer GET/SET PIDs: GET request PDL 0, SET request PDL from pm_pids.h */
#define MGS(name) GS(PM_PDL_##name##_GET_REQUEST, PM_PDL_##name##_GET_REQUEST, \
                     PM_PDL_##name##_SET_REQUEST, PM_PDL_##name##_SET_REQUEST)
#define MG(name) G(PM_PDL_##name##_GET_REQUEST, PM_PDL_##name##_GET_REQUEST)
#define MS(name) S(PM_PDL_##name##_SET_REQUEST, PM_PDL_##name##_SET_REQUEST)

static const pid_entry_t PIDS[] = {
    /* E1.20 */
    {PM_RDM_PID_QUEUED_MESSAGE, 1, G(1, 1), h_queued_message},
    {PM_RDM_PID_STATUS_MESSAGES, 1, G(1, 1), h_status_messages},
    {PM_RDM_PID_STATUS_ID_DESCRIPTION, 1, G(2, 2), h_status_id_description},
    {PM_RDM_PID_CLEAR_STATUS_ID, 1, S(0, 0), h_clear_status_id},
    {PM_RDM_PID_SUPPORTED_PARAMETERS, 1, G(0, 0), h_supported_parameters},
    {PM_RDM_PID_PARAMETER_DESCRIPTION, 1, G(2, 2), h_parameter_description},
    {PM_RDM_PID_DEVICE_INFO, 1, G(0, 0), h_device_info},
    {PM_RDM_PID_PRODUCT_DETAIL_ID_LIST, 1, G(0, 0), h_product_detail},
    {PM_RDM_PID_DEVICE_MODEL_DESCRIPTION, 1, G(0, 0), h_model_description},
    {PM_RDM_PID_MANUFACTURER_LABEL, 1, G(0, 0), h_manufacturer_label},
    {PM_RDM_PID_DEVICE_LABEL, 1, GS(0, 0, 0, LABEL_MAX), h_device_label},
    {PM_RDM_PID_FACTORY_DEFAULTS, 1, GS(0, 0, 0, 0), h_factory_defaults},
    {PM_RDM_PID_SOFTWARE_VERSION_LABEL, 1, G(0, 0), h_sw_version_label},
    {PM_RDM_PID_BOOT_SOFTWARE_VERSION_ID, 1, G(0, 0), h_boot_sw_version_id},
    {PM_RDM_PID_BOOT_SOFTWARE_VERSION_LABEL, 1, G(0, 0), h_boot_sw_version_label},
    {PM_RDM_PID_DISPLAY_LEVEL, 1, GS(0, 0, 1, 1), h_display_level},
    {PM_RDM_PID_IDENTIFY_DEVICE, 1, GS(0, 0, 1, 1), h_identify},
    {PM_RDM_PID_RESET_DEVICE, 1, S(1, 1), h_reset_device},
    /* E1.37-2 */
    {PM_RDM_PID_LIST_INTERFACES, 1, G(0, 0), h_list_interfaces},
    {PM_RDM_PID_INTERFACE_LABEL, 1, G(4, 4), h_interface_label},
    {PM_RDM_PID_INTERFACE_HARDWARE_ADDRESS_TYPE1, 1, G(4, 4), h_hw_address},
    {PM_RDM_PID_IPV4_DHCP_MODE, 1, GS(4, 4, 5, 5), h_dhcp_mode},
    {PM_RDM_PID_IPV4_CURRENT_ADDRESS, 1, G(4, 4), h_current_address},
    {PM_RDM_PID_IPV4_STATIC_ADDRESS, 1, GS(4, 4, 9, 9), h_static_address},
    {PM_RDM_PID_INTERFACE_RENEW_DHCP, 1, S(4, 4), h_renew_dhcp},
    {PM_RDM_PID_INTERFACE_APPLY_CONFIGURATION, 1, S(4, 4), h_apply_configuration},
    {PM_RDM_PID_IPV4_DEFAULT_ROUTE, 1, GS(0, 0, 8, 8), h_default_route},
    /* manufacturer (pids.toml) */
    {PM_PID_NODE_HW_REVISION, 1, MG(NODE_HW_REVISION), h_num},
    {PM_PID_PORT_ENABLED(1u), PM_PID_PORT_ENABLED_COUNT, MGS(PORT_ENABLED), h_num},
    {PM_PID_PORT_DMX_BREAK(1u), PM_PID_PORT_DMX_BREAK_COUNT, MGS(PORT_DMX_BREAK), h_num},
    {PM_PID_PORT_DMX_MAB(1u), PM_PID_PORT_DMX_MAB_COUNT, MGS(PORT_DMX_MAB), h_num},
    {PM_PID_PORT_DMX_RATE(1u), PM_PID_PORT_DMX_RATE_COUNT, MGS(PORT_DMX_RATE), h_num},
    {PM_PID_PORT_DMX_MBS(1u), PM_PID_PORT_DMX_MBS_COUNT, MGS(PORT_DMX_MBS), h_num},
    {PM_PID_PORT_RDM_BG_DISCOVERY(1u), PM_PID_PORT_RDM_BG_DISCOVERY_COUNT, MGS(PORT_RDM_BG_DISCOVERY), h_num},
    {PM_PID_RDM_BG_DISCOVERY_INTERVAL, 1, MGS(RDM_BG_DISCOVERY_INTERVAL), h_num},
    {PM_PID_DISPLAY_DIM_TIMEOUT, 1, MGS(DISPLAY_DIM_TIMEOUT), h_num},
    {PM_PID_DISPLAY_ADDRESS_FORMAT, 1, MGS(DISPLAY_ADDRESS_FORMAT), h_num},
    {PM_PID_PANEL_LOCK, 1, MGS(PANEL_LOCK), h_num},
    {PM_PID_BOOT_CATCH_START, 1, MS(BOOT_CATCH_START), h_boot_catch_start},
    {PM_PID_BOOT_CATCH_STATUS, 1, MG(BOOT_CATCH_STATUS), h_boot_catch_status},
    {PM_PID_BOOT_CATCH_ABORT, 1, MS(BOOT_CATCH_ABORT), h_boot_catch_abort},
    {PM_PID_LOCAL_RDM_SET, 1, MG(LOCAL_RDM_SET), h_local_rdm_set},
    {PM_PID_FW_INFO, 1, MG(FW_INFO), h_fw_info},
    {PM_PID_FW_BEGIN, 1, MS(FW_BEGIN), h_fw_begin},
    {PM_PID_FW_WRITE, 1, S(4u, PM_PDL_FW_WRITE_SET_REQUEST), h_fw_write}, /* offset u32 + 0..224 bytes */
    {PM_PID_FW_FINISH, 1, MS(FW_FINISH), h_fw_finish},
};

#undef G
#undef S
#undef GS
#undef MGS
#undef MG
#undef MS

static const pid_entry_t *pid_find(uint16_t pid)
{
    for (size_t i = 0; i < ARRAY_LEN(PIDS); i++) {
        if (pid >= PIDS[i].pid && pid < PIDS[i].pid + PIDS[i].count) return &PIDS[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ API */

size_t pm_rdm_responder_size(void) { return sizeof(struct pm_rdm_responder); }

pm_rdm_responder_t *pm_rdm_responder_init(void *mem, const pm_rdm_responder_identity_t *id,
                                          const pm_rdm_responder_ops_t *ops)
{
    if (mem == NULL || id == NULL || ops == NULL) return NULL;
    pm_rdm_responder_t *r = mem;
    memset(r, 0, sizeof(*r));
    r->id = *id;
    r->ops = *ops;
    settings_get(r, &r->last);
    return r;
}

/* Request -> NACK reason or RES_ACK, in the order of rdm-digest.md §8. */
static int dispatch(pm_rdm_responder_t *r, ctx_t *c)
{
    const pm_rdm_msg_t *q = c->req;
    bool is_set = q->cc == PM_RDM_CC_SET;
    if (q->sub_device != 0 && !(is_set && q->sub_device == ALL_SUB_DEVICES)) return PM_NR_SUB_DEVICE_OUT_OF_RANGE;
    const pid_entry_t *e = pid_find(q->pid);
    if (e == NULL) return PM_NR_UNKNOWN_PID;
    if (!(e->cc & (is_set ? CC_SET : CC_GET))) return PM_NR_UNSUPPORTED_COMMAND_CLASS;
    uint8_t lo = is_set ? e->set_min : e->get_min, hi = is_set ? e->set_max : e->get_max;
    if (q->pdl < lo || q->pdl > hi) return PM_NR_FORMAT_ERROR;
    return e->fn(r, c);
}

int pm_rdm_responder_handle(pm_rdm_responder_t *r, const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    if (r == NULL || req == NULL || out == NULL) return PM_ERR_INVALID_ARG;
    pm_rdm_msg_t q;
    pm_err_t err = pm_rdm_decode(req, len, &q);
    if (err != PM_OK) return err;
    if (!pm_uid_addresses(&q.dest, &r->id.uid)) return 0; /* not for us (also foreign vendorcast) */
    if (q.cc != PM_RDM_CC_GET && q.cc != PM_RDM_CC_SET) return 0; /* DISCOVERY: the node is never on a line */
    bool unicast = pm_uid_equal(&q.dest, &r->id.uid);
    if (!unicast && q.cc == PM_RDM_CC_GET) return 0;
    if (pm_uid_is_broadcast(&q.dest) && q.pid >= MANUFACTURER_PID_FIRST) return 0; /* E1.20 6.2.10.2 */

    ctx_t c;
    c.req = &q;
    c.resp_pid = q.pid;
    c.out_len = 0;
    int res = dispatch(r, &c);
    if (!unicast) return 0; /* SET executed, broadcasts are never answered */

    pm_rdm_msg_t resp;
    uint8_t nack[2];
    pm_rdm_make_response(&q, &r->id.uid, res == RES_ACK ? PM_RDM_RESP_ACK : PM_RDM_RESP_NACK_REASON,
                         pm_rdm_responder_message_count(r), &resp);
    resp.pid = c.resp_pid;
    if (res == RES_ACK) {
        resp.pdl = c.out_len;
        resp.pd = c.out;
    } else {
        pm_put_be16(nack, (uint16_t)res);
        resp.pdl = 2;
        resp.pd = nack;
    }
    return pm_rdm_encode(&resp, out, cap);
}

void pm_rdm_responder_settings_changed(pm_rdm_responder_t *r, pm_change_mask_t mask, pm_change_source_t src)
{
    (void)mask; /* exact per-field comparison with the cached copy instead */
    if (r == NULL) return;
    pm_settings_t *n = &r->tmp, *o = &r->last;
    settings_get(r, n);
    /* only values that ArtPollReply does not show (names, IP, Port-Address, direction, merge, rdm_enabled and
     * failsafe are visible there); PORTn_ENABLED is queued anyway */
    for (unsigned p = 0; p < PM_NUM_PORTS; p++) {
        const pm_port_settings_t *a = &o->port[p], *b = &n->port[p];
        uint16_t port1 = (uint16_t)(p + 1u);
        if (a->enabled != b->enabled) pm_rdm_responder_queue_pid(r, PM_PID_PORT_ENABLED(port1));
        if (a->dmx_break_us != b->dmx_break_us) pm_rdm_responder_queue_pid(r, PM_PID_PORT_DMX_BREAK(port1));
        if (a->dmx_mab_us != b->dmx_mab_us) pm_rdm_responder_queue_pid(r, PM_PID_PORT_DMX_MAB(port1));
        if (a->dmx_rate_hz != b->dmx_rate_hz) pm_rdm_responder_queue_pid(r, PM_PID_PORT_DMX_RATE(port1));
        if (a->dmx_mbs_us != b->dmx_mbs_us) pm_rdm_responder_queue_pid(r, PM_PID_PORT_DMX_MBS(port1));
        if (a->rdm_bg_discovery != b->rdm_bg_discovery) {
            pm_rdm_responder_queue_pid(r, PM_PID_PORT_RDM_BG_DISCOVERY(port1));
        }
    }
    if (o->rdm_bg_interval_s != n->rdm_bg_interval_s) pm_rdm_responder_queue_pid(r, PM_PID_RDM_BG_DISCOVERY_INTERVAL);
    if (o->display_level != n->display_level) pm_rdm_responder_queue_pid(r, PM_RDM_PID_DISPLAY_LEVEL);
    if (o->display_dim_timeout_s != n->display_dim_timeout_s) {
        pm_rdm_responder_queue_pid(r, PM_PID_DISPLAY_DIM_TIMEOUT);
    }
    if (o->address_format != n->address_format) pm_rdm_responder_queue_pid(r, PM_PID_DISPLAY_ADDRESS_FORMAT);
    if (hw_revision(r, o) != hw_revision(r, n)) pm_rdm_responder_queue_pid(r, PM_PID_NODE_HW_REVISION);
    if (o->panel_lock != n->panel_lock) {
        pm_rdm_responder_queue_pid(r, PM_PID_PANEL_LOCK);
        if (o->panel_lock && !n->panel_lock && src == PM_SRC_LOCAL_UI) {
            pm_rdm_responder_status(r, PM_STATUS_PANEL_UNLOCKED_LOCAL, PM_RDM_STATUS_ADVISORY, 0, 0);
        }
    }
    *o = *n;
}

void pm_rdm_responder_local_set(pm_rdm_responder_t *r, pm_port_idx_t port, const pm_uid_t *uid,
                                uint16_t sub_device, uint16_t pid)
{
    if (r == NULL || uid == NULL) return;
    r->local_set.seq++;
    r->local_set.port = (uint8_t)(port + 1u);
    r->local_set.uid = *uid;
    r->local_set.sub_device = sub_device;
    r->local_set.pid = pid;
    pm_rdm_responder_queue_pid(r, PM_PID_LOCAL_RDM_SET);
    pm_rdm_responder_status(r, PM_STATUS_LOCAL_RDM_SET, PM_RDM_STATUS_ADVISORY, (int16_t)(port + 1u), (int16_t)pid);
}

void pm_rdm_responder_boot_catch_event(pm_rdm_responder_t *r, pm_port_idx_t port, uint8_t state)
{
    if (r == NULL) return;
    pm_rdm_responder_queue_pid(r, PM_PID_BOOT_CATCH_STATUS);
    if (state == PM_BOOT_CATCH_STATE_CAUGHT) {
        pm_rdm_responder_status(r, PM_STATUS_BOOT_CATCH_CAUGHT, PM_RDM_STATUS_ADVISORY, (int16_t)(port + 1u), 0);
    } else if (state == PM_BOOT_CATCH_STATE_TIMEOUT) {
        pm_rdm_responder_status(r, PM_STATUS_BOOT_CATCH_TIMEOUT, PM_RDM_STATUS_WARNING, (int16_t)(port + 1u), 0);
    }
}

void pm_rdm_responder_poll(pm_rdm_responder_t *r)
{
    if (r == NULL) return;
    uint32_t now = now_ms(r);
    if (r->reset_pending && time_reached(now, r->reset_due)) {
        r->reset_pending = false;
        if (r->ops.reset) r->ops.reset(r->ops.ctx, r->reset_kind);
    }
    const pm_fw_sink_t *k = r->ops.fw_sink;
    if (r->reboot_pending && time_reached(now, r->reboot_due)) {
        r->reboot_pending = false;
        if (k && k->reboot) k->reboot(k->ctx);
    }
    if (r->fw_active && now - r->fw_last_ms >= PM_RDM_RESP_FW_TIMEOUT_MS) {
        r->fw_active = false;
        if (k && k->abort) k->abort(k->ctx, PM_FW_ERROR_TIMEOUT);
    }
}

void pm_rdm_responder_set_identify(pm_rdm_responder_t *r, bool on)
{
    if (r == NULL || r->identify == on) return;
    r->identify = on;
    pm_rdm_responder_queue_pid(r, PM_RDM_PID_IDENTIFY_DEVICE);
}

bool pm_rdm_responder_identify(const pm_rdm_responder_t *r)
{
    return r != NULL && r->identify;
}
