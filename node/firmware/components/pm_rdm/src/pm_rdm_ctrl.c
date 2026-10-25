/*
 * pm_rdm_ctrl.c - per-port RDM controller: discovery (full + background), TOD, request queue, BOOT_CATCH.
 *
 * Discovery follows OLA's DiscoveryAgent (rdm-digest.md §3.3):
 *  - full: TOD cleared, DISC_UN_MUTE to broadcast 3x, binary search with DISC_UNIQUE_BRANCH from (0, FFFF:FFFFFFFF).
 *  - background (incremental, OLA): DISC_UN_MUTE to broadcast 3x, DISC_MUTE to every known UID (presence check:
 *    PM_RDM_PRESENCE_MAX_MISSES consecutive misses remove it), then the same binary search: only devices that are not
 *    in the TOD answer (new ones, and ones that were dropped although they are still connected and muted - without
 *    the un-mute those would never come back until a flush). The TOD is not cleared.
 *  - every run is capped at DUB_CAP() DUBs (protection against responders that answer every DUB).
 *  - DUB answer that decodes: verify with DISC_MUTE (PM_RDM_MUTE_ATTEMPTS); ACK -> TOD, then DUB the same branch
 *    again. No ACK -> the UID is "bad" (phantom from a collision or broken responder).
 *  - DUB answer that does not decode (collision): split the branch in the middle. A UID that was already muted in
 *    this run (or is bad) and still answers: split the branch around it (OLA SplitAroundBadUID).
 *  - a branch is given up after PM_RDM_BRANCH_MAX_FAILURES collisions on a single UID or PM_RDM_BRANCH_MAX_ATTEMPTS
 *    DUBs without a new device.
 * Proxy / sub-device flags of the DISC_MUTE control field are stored with the TOD entry but do not change the
 * algorithm (OLA does the same: managed proxies answer DUB for their proxied devices themselves).
 *
 * The controller never waits: every step is one transaction handed out by next() and finished by done().
 */
#include "pm_rdm_ctrl.h"

#include <string.h>

#include "pm_pids.h"

#define UID_MAX_U64 0xFFFFFFFFFFFFull

enum { /* inflight */
    INF_NONE,
    INF_REQUEST,        /* forwarded unicast request, response goes to on_response */
    INF_REQUEST_NORESP, /* forwarded broadcast / vendorcast / DUB: nothing is forwarded */
    INF_UNMUTE,
    INF_DUB,
    INF_MUTE,           /* verify a UID found by DUB */
    INF_PRESENCE,       /* DISC_MUTE to a known UID (background) */
    INF_CATCH,
};

enum { /* disc_step */
    STEP_UNMUTE,
    STEP_PRESENCE,
    STEP_DUB,
    STEP_MUTE,
};

/* private TOD flag: device answered DISC_MUTE in the current discovery run */
#define TOD_SEEN 0x4000u
#define TOD_PUBLIC_MASK ((uint16_t)(0x00FFu | PM_RDM_TOD_HAS_BINDING_UID))

#define MUTE_RESP_MAX (PM_RDM_HEADER_LEN + 8u + 2u)

/* DUB cap of one run: a clean binary search needs < 2 * 48 DUBs per device */
#define DUB_CAP(c) (2u * 48u * ((uint32_t)(c)->tod_count + (c)->bad_count + 1u) + 64u)

static const pm_uid_t UID_ALL = {{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};

/* ------------------------------------------------------------------------------------------------ helpers */

static bool time_reached(uint32_t now, uint32_t t) { return (int32_t)(now - t) >= 0; }

static int tod_index(const pm_rdm_ctrl_t *c, const pm_uid_t *uid, bool *found)
{
    uint64_t v = pm_uid_to_u64(uid);
    int lo = 0, hi = (int)c->tod_count;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        uint64_t m = pm_uid_to_u64(&c->tod[mid]);
        if (m == v) {
            *found = true;
            return mid;
        }
        if (m < v) lo = mid + 1;
        else hi = mid;
    }
    *found = false;
    return lo;
}

/* Report a TOD change. During a full discovery reports are throttled (plan()) and sent at the end. */
static void tod_changed(pm_rdm_ctrl_t *c, uint32_t now_ms, bool force)
{
    c->tod_dirty = true;
    if (!force && c->disc_mode == PM_RDM_DISC_FULL) return;
    c->tod_dirty = false;
    c->tod_notify_ms = now_ms;
    if (c->cb.on_tod_changed) c->cb.on_tod_changed(c->cb.ctx, c->port);
}

/* Add or update; false when the TOD is full. */
static bool tod_add(pm_rdm_ctrl_t *c, const pm_uid_t *uid, uint16_t flags, uint32_t now_ms)
{
    bool found;
    int i = tod_index(c, uid, &found);
    if (found) {
        if ((c->tod_flags[i] & TOD_PUBLIC_MASK) != (flags & TOD_PUBLIC_MASK)) tod_changed(c, now_ms, false);
        c->tod_flags[i] = flags;
        return true;
    }
    if (c->tod_count >= PM_RDM_TOD_MAX) {
        c->stats.tod_overflow++;
        return false;
    }
    size_t n = (size_t)(c->tod_count - i);
    memmove(&c->tod[i + 1], &c->tod[i], n * sizeof(pm_uid_t));
    memmove(&c->tod_flags[i + 1], &c->tod_flags[i], n * sizeof(uint16_t));
    c->tod[i] = *uid;
    c->tod_flags[i] = flags;
    c->tod_count++;
    tod_changed(c, now_ms, false);
    return true;
}

static void tod_remove_at(pm_rdm_ctrl_t *c, int i, uint32_t now_ms)
{
    size_t n = (size_t)(c->tod_count - i - 1);
    memmove(&c->tod[i], &c->tod[i + 1], n * sizeof(pm_uid_t));
    memmove(&c->tod_flags[i], &c->tod_flags[i + 1], n * sizeof(uint16_t));
    c->tod_count--;
    tod_changed(c, now_ms, false);
}

static void tod_clear(pm_rdm_ctrl_t *c, uint32_t now_ms)
{
    bool had = c->tod_count > 0;
    c->tod_count = 0;
    if (had) tod_changed(c, now_ms, true);
}

static bool is_bad(const pm_rdm_ctrl_t *c, const pm_uid_t *uid)
{
    for (unsigned i = 0; i < c->bad_count; i++) {
        if (pm_uid_equal(&c->bad[i], uid)) return true;
    }
    return false;
}

static void push(pm_rdm_ctrl_t *c, uint64_t lo, uint64_t hi)
{
    if (c->stack_depth >= PM_RDM_DISC_STACK) return; /* callers check the room first */
    pm_rdm_branch_t *b = &c->stack[c->stack_depth++];
    b->lo = lo;
    b->hi = hi;
    b->failures = 0;
    b->attempts = 0;
}

static void boot_catch_set(pm_rdm_ctrl_t *c, uint8_t state, uint32_t now_ms)
{
    if (c->boot_catch.state == PM_BOOT_CATCH_STATE_RUNNING) {
        c->boot_catch.elapsed_ms = now_ms - c->boot_catch.started_ms;
    }
    c->boot_catch.state = state;
    if (c->cb.on_boot_catch) c->cb.on_boot_catch(c->cb.ctx, c->port, state);
}

/* ------------------------------------------------------------------------------------------------ discovery control */

static void disc_stop(pm_rdm_ctrl_t *c)
{
    c->disc_mode = PM_RDM_DISC_IDLE;
    c->stack_depth = 0;
    c->disc_gen++;
}

static void disc_begin(pm_rdm_ctrl_t *c, pm_rdm_disc_mode_t mode)
{
    disc_stop(c);
    c->disc_mode = mode;
    c->bad_count = 0;
    c->mute_attempts = 0;
    c->presence_idx = 0;
    for (unsigned i = 0; i < c->tod_count; i++) c->tod_flags[i] &= (uint16_t)~TOD_SEEN;
    c->run_dubs = 0;
    c->run_corrupt = false;
    push(c, 0, UID_MAX_U64);
    if (mode == PM_RDM_DISC_FULL) c->stats.full_discoveries++;
    else c->stats.bg_discoveries++;
    c->unmute_left = PM_RDM_UNMUTE_REPEATS;
    c->disc_step = STEP_UNMUTE;
}

static void disc_finish(pm_rdm_ctrl_t *c, uint32_t now_ms)
{
    bool full = c->disc_mode == PM_RDM_DISC_FULL;
    if (c->run_corrupt) c->stats.corrupt_runs++;
    disc_stop(c);
    if (full || c->tod_dirty) tod_changed(c, now_ms, true);
    c->bg_schedule = true; /* next background run one interval after this one ended */
}

static void start_full(pm_rdm_ctrl_t *c, uint32_t now_ms)
{
    tod_clear(c, now_ms);
    disc_begin(c, PM_RDM_DISC_FULL);
}

/* ------------------------------------------------------------------------------------------------ public: setup */

void pm_rdm_ctrl_init(pm_rdm_ctrl_t *c, pm_port_idx_t port, const pm_uid_t *self, const pm_rdm_ctrl_cb_t *cb)
{
    memset(c, 0, sizeof(*c));
    c->port = port;
    c->self = *self;
    if (cb) c->cb = *cb;
    c->bg_interval_ms = 3000;
    c->background = true;
    c->bg_schedule = true;
    c->break_us = 176;
    c->mab_us = 16;
}

void pm_rdm_ctrl_set_enabled(pm_rdm_ctrl_t *c, bool enabled, uint32_t now_ms)
{
    c->last_now_ms = now_ms;
    if (enabled == c->enabled) return;
    c->enabled = enabled;
    c->inflight = INF_NONE;
    if (enabled) {
        c->bg_schedule = true;
        start_full(c, now_ms);
        return;
    }
    disc_stop(c);
    c->q_count = 0;
    c->q_head = 0;
    tod_clear(c, now_ms);
    if (c->boot_catch.state == PM_BOOT_CATCH_STATE_RUNNING) boot_catch_set(c, PM_BOOT_CATCH_STATE_ABORTED, now_ms);
}

void pm_rdm_ctrl_set_background(pm_rdm_ctrl_t *c, bool on, uint32_t interval_ms)
{
    if (interval_ms < PM_RDM_BG_INTERVAL_MIN_MS) interval_ms = PM_RDM_BG_INTERVAL_MIN_MS;
    if (on != c->background || interval_ms != c->bg_interval_ms) c->bg_schedule = true;
    c->background = on;
    c->bg_interval_ms = interval_ms;
    if (!on && c->disc_mode == PM_RDM_DISC_BACKGROUND) disc_stop(c);
}

void pm_rdm_ctrl_set_timing(pm_rdm_ctrl_t *c, uint16_t break_us, uint16_t mab_us)
{
    c->break_us = break_us;
    c->mab_us = mab_us;
}

void pm_rdm_ctrl_flush(pm_rdm_ctrl_t *c, uint32_t now_ms)
{
    c->last_now_ms = now_ms;
    if (!c->enabled) {
        tod_clear(c, now_ms);
        return;
    }
    start_full(c, now_ms);
}

void pm_rdm_ctrl_discovery_stop(pm_rdm_ctrl_t *c, uint32_t now_ms)
{
    c->last_now_ms = now_ms;
    if (c->disc_mode == PM_RDM_DISC_IDLE) return;
    bool full = c->disc_mode == PM_RDM_DISC_FULL;
    disc_stop(c);
    if (full || c->tod_dirty) tod_changed(c, now_ms, true);
    c->bg_schedule = true;
}

pm_err_t pm_rdm_ctrl_submit(pm_rdm_ctrl_t *c, const pm_rdm_request_t *req)
{
    if (req == NULL || req->len < PM_RDM_HEADER_LEN + 2u || req->len > PM_RDM_MAX_MSG_LEN ||
        req->data[0] != PM_RDM_START_CODE) {
        return PM_ERR_INVALID_ARG;
    }
    if (!c->enabled) return PM_ERR_STATE;
    if (c->q_count >= PM_RDM_QUEUE_LEN) return PM_ERR_FULL;
    c->queue[(c->q_head + c->q_count) % PM_RDM_QUEUE_LEN] = *req;
    c->q_count++;
    return PM_OK;
}

uint8_t pm_rdm_ctrl_queue_free(const pm_rdm_ctrl_t *c) { return (uint8_t)(PM_RDM_QUEUE_LEN - c->q_count); }

/* ------------------------------------------------------------------------------------------------ building packets */

static uint32_t est_us(const pm_rdm_ctrl_t *c, pm_rdm_txn_kind_t kind, uint16_t len, uint16_t max_resp_len)
{
    uint32_t req = (uint32_t)c->break_us + c->mab_us + (uint32_t)len * PM_RDM_BYTE_US;
    if (kind == PM_RDM_TXN_DUB) return req + PM_RDM_DUB_GAP_US;
    if (kind == PM_RDM_TXN_BROADCAST) return req + PM_RDM_BROADCAST_GAP_US;
    /* response: turnaround + max. break/MAB + bytes + gap before our next packet */
    uint32_t resp = PM_RDM_RESPONSE_TIMEOUT_US + 352u + 88u + (uint32_t)max_resp_len * PM_RDM_BYTE_US +
                    PM_RDM_BROADCAST_GAP_US;
    return req + (resp > PM_RDM_NO_RESPONSE_GAP_US ? resp : PM_RDM_NO_RESPONSE_GAP_US);
}

static void build(const pm_rdm_ctrl_t *c, pm_rdm_txn_t *t, pm_rdm_txn_kind_t kind, const pm_uid_t *dest, uint8_t cc,
                  uint16_t pid, const uint8_t *pd, uint8_t pdl, uint16_t max_resp_len)
{
    pm_rdm_msg_t m;
    memset(&m, 0, sizeof(m));
    m.dest = *dest;
    m.src = c->self;
    m.tn = c->tn;
    m.port_or_resp = (uint8_t)(c->port + 1u);
    m.cc = cc;
    m.pid = pid;
    m.pdl = pdl;
    m.pd = pd;
    int n = pm_rdm_encode(&m, t->data, sizeof(t->data));
    t->kind = kind;
    t->len = n > 0 ? (uint16_t)n : 0;
    t->est_us = est_us(c, kind, t->len, max_resp_len);
}

/* Decide what to send next without consuming anything. Time-driven state changes (catch timeout, start of a
 * background run, throttled TOD report) happen here; they do not depend on whether the transaction is taken. */
static bool plan(pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_txn_t *t, uint8_t *inflight)
{
    if (!c->enabled) return false;

    if (c->tod_dirty && c->disc_mode == PM_RDM_DISC_FULL &&
        (uint32_t)(now_ms - c->tod_notify_ms) >= PM_RDM_TOD_NOTIFY_MS) {
        tod_changed(c, now_ms, true);
    }
    if (c->boot_catch.state == PM_BOOT_CATCH_STATE_RUNNING &&
        (uint32_t)(now_ms - c->boot_catch.started_ms) >= PM_BOOT_CATCH_DURATION_MS) {
        boot_catch_set(c, PM_BOOT_CATCH_STATE_TIMEOUT, now_ms);
    }
    if (c->bg_schedule) {
        c->bg_schedule = false;
        c->next_bg_ms = now_ms + c->bg_interval_ms;
    }

    /* 1. forwarded requests (every UID, also outside the TOD) */
    if (c->q_count) {
        const pm_rdm_request_t *r = &c->queue[c->q_head];
        pm_uid_t dest;
        memcpy(dest.b, r->data + 3, 6);
        uint8_t cc = r->data[20];
        uint16_t pid = pm_get_be16(r->data + 21);
        t->len = r->len;
        memcpy(t->data, r->data, r->len);
        if (cc == PM_RDM_CC_DISCOVERY && pid == PM_RDM_PID_DISC_UNIQUE_BRANCH) {
            t->kind = PM_RDM_TXN_DUB;
            *inflight = INF_REQUEST_NORESP;
        } else if (pm_uid_is_vendorcast(&dest)) { /* broadcast + vendorcast: never answered */
            t->kind = PM_RDM_TXN_BROADCAST;
            *inflight = INF_REQUEST_NORESP;
        } else {
            t->kind = PM_RDM_TXN_REQUEST;
            *inflight = INF_REQUEST;
        }
        t->est_us = est_us(c, t->kind, t->len, PM_RDM_MAX_MSG_LEN);
        return true;
    }

    /* 2. BOOT_CATCH */
    bool catching = c->boot_catch.state == PM_BOOT_CATCH_STATE_RUNNING;
    if (catching && time_reached(now_ms, c->next_catch_ms)) {
        build(c, t, PM_RDM_TXN_REQUEST, &c->boot_catch.target, PM_RDM_CC_GET, PM_PID_BOOT_INFO, NULL, 0,
              PM_RDM_HEADER_LEN + PM_PDL_BOOT_INFO_GET_RESPONSE + 2u);
        *inflight = INF_CATCH;
        return true;
    }

    /* 3. discovery (no background start while a catch runs: it must hit the tube's ~200 ms window) */
    if (c->disc_mode == PM_RDM_DISC_IDLE) {
        if (!c->background || catching || !time_reached(now_ms, c->next_bg_ms)) return false;
        disc_begin(c, PM_RDM_DISC_BACKGROUND);
    }
    for (;;) {
        switch (c->disc_step) {
        case STEP_UNMUTE:
            if (c->unmute_left == 0) {
                c->disc_step = c->disc_mode == PM_RDM_DISC_BACKGROUND ? STEP_PRESENCE : STEP_DUB;
                continue;
            }
            build(c, t, PM_RDM_TXN_BROADCAST, &UID_ALL, PM_RDM_CC_DISCOVERY, PM_RDM_PID_DISC_UN_MUTE, NULL, 0, 0);
            *inflight = INF_UNMUTE;
            return true;
        case STEP_PRESENCE:
            if (c->presence_idx >= c->tod_count) {
                c->disc_step = STEP_DUB;
                continue;
            }
            build(c, t, PM_RDM_TXN_REQUEST, &c->tod[c->presence_idx], PM_RDM_CC_DISCOVERY, PM_RDM_PID_DISC_MUTE,
                  NULL, 0, MUTE_RESP_MAX);
            *inflight = INF_PRESENCE;
            return true;
        case STEP_MUTE:
            build(c, t, PM_RDM_TXN_REQUEST, &c->pending_uid, PM_RDM_CC_DISCOVERY, PM_RDM_PID_DISC_MUTE, NULL, 0,
                  MUTE_RESP_MAX);
            *inflight = INF_MUTE;
            return true;
        case STEP_DUB:
        default: {
            if (c->stack_depth && c->run_dubs >= DUB_CAP(c)) {
                c->stack_depth = 0; /* broken responder on the line: end the run with what we have */
                c->run_corrupt = true;
            }
            if (c->stack_depth == 0) {
                disc_finish(c, now_ms);
                return false;
            }
            const pm_rdm_branch_t *b = &c->stack[c->stack_depth - 1];
            uint8_t pd[12];
            pm_uid_t u = pm_uid_from_u64(b->lo);
            memcpy(pd, u.b, 6);
            u = pm_uid_from_u64(b->hi);
            memcpy(pd + 6, u.b, 6);
            build(c, t, PM_RDM_TXN_DUB, &UID_ALL, PM_RDM_CC_DISCOVERY, PM_RDM_PID_DISC_UNIQUE_BRANCH, pd, 12, 0);
            *inflight = INF_DUB;
            return true;
        }
        }
    }
}

static void commit(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_txn_t *t, uint8_t inflight)
{
    c->inflight = inflight;
    c->inflight_gen = c->disc_gen;
    c->inflight_tn = t->data[15];
    memcpy(c->inflight_dest.b, t->data + 3, 6);
    memcpy(c->inflight_src.b, t->data + 9, 6);
    c->inflight_cc = t->data[20];
    c->inflight_pid = pm_get_be16(t->data + 21);
    switch (inflight) {
    case INF_REQUEST:
    case INF_REQUEST_NORESP:
        c->inflight_token = c->queue[c->q_head].token;
        c->q_head = (uint8_t)((c->q_head + 1u) % PM_RDM_QUEUE_LEN);
        c->q_count--;
        c->stats.requests++;
        return; /* TN of forwarded requests is the sender's */
    case INF_CATCH:
        c->boot_catch.attempts++;
        c->next_catch_ms = now_ms + PM_BOOT_CATCH_PERIOD_MS;
        break;
    case INF_UNMUTE:
        c->unmute_left--;
        break;
    case INF_DUB:
        c->stats.dubs++;
        c->run_dubs++;
        break;
    default:
        break;
    }
    c->tn++;
}

bool pm_rdm_ctrl_next_max(pm_rdm_ctrl_t *c, uint32_t now_ms, uint32_t max_us, pm_rdm_txn_t *out)
{
    uint8_t inflight = INF_NONE;
    c->last_now_ms = now_ms;
    if (!plan(c, now_ms, out, &inflight)) return false;
    if (out->est_us > max_us) return false;
    commit(c, now_ms, out, inflight);
    return true;
}

bool pm_rdm_ctrl_next(pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_txn_t *out)
{
    return pm_rdm_ctrl_next_max(c, now_ms, UINT32_MAX, out);
}

/* ------------------------------------------------------------------------------------------------ results */

/* Decoded response from the UID we addressed, to the request's source, with the request's TN. */
static bool valid_reply(const pm_rdm_ctrl_t *c, const pm_rdm_rx_t *rx, pm_rdm_msg_t *m)
{
    if (rx == NULL || rx->status != PM_RDM_RX_OK || rx->data == NULL) return false;
    if (pm_rdm_decode(rx->data, rx->len, m) != PM_OK) return false;
    if (!pm_uid_equal(&m->src, &c->inflight_dest) || !pm_uid_equal(&m->dest, &c->inflight_src) ||
        m->tn != c->inflight_tn) {
        return false;
    }
    /* QUEUED_MESSAGE is answered with the PID and CC of the queued response (OLA RDMCommand.cpp) */
    if (c->inflight_pid == PM_RDM_PID_QUEUED_MESSAGE) {
        return m->cc == PM_RDM_CC_GET_RESPONSE || m->cc == PM_RDM_CC_SET_RESPONSE;
    }
    return m->cc == (uint8_t)(c->inflight_cc + 1u) && m->pid == c->inflight_pid;
}

static bool mute_ack(const pm_rdm_ctrl_t *c, const pm_rdm_rx_t *rx, uint16_t *flags)
{
    pm_rdm_msg_t m;
    if (!valid_reply(c, rx, &m)) return false;
    if (m.cc != PM_RDM_CC_DISCOVERY_RESPONSE || m.pid != PM_RDM_PID_DISC_MUTE || m.port_or_resp != PM_RDM_RESP_ACK ||
        m.pdl < 2) {
        return false;
    }
    *flags = (uint16_t)(pm_get_be16(m.pd) & 0x00FFu);
    if (m.pdl >= 8) *flags |= PM_RDM_TOD_HAS_BINDING_UID;
    return true;
}

static void branch_no_progress(pm_rdm_ctrl_t *c);

static void add_bad(pm_rdm_ctrl_t *c, const pm_uid_t *uid)
{
    if (is_bad(c, uid)) return;
    if (c->bad_count < PM_RDM_BAD_UIDS) c->bad[c->bad_count++] = *uid;
    else c->bad[PM_RDM_BAD_UIDS - 1u] = *uid;
}

static void branch_pop(pm_rdm_ctrl_t *c)
{
    if (c->stack_depth) c->stack_depth--;
}

static void branch_split_mid(pm_rdm_ctrl_t *c)
{
    pm_rdm_branch_t b = c->stack[c->stack_depth - 1];
    if (b.lo >= b.hi) {
        if (++c->stack[c->stack_depth - 1].failures >= PM_RDM_BRANCH_MAX_FAILURES) {
            c->run_corrupt = true;
            branch_pop(c);
        }
        return;
    }
    if (c->stack_depth >= PM_RDM_DISC_STACK) { /* no room for two halves: count it as a failure instead */
        c->run_corrupt = true;
        if (++c->stack[c->stack_depth - 1].failures >= PM_RDM_BRANCH_MAX_FAILURES) branch_pop(c);
        return;
    }
    uint64_t mid = b.lo + (b.hi - b.lo) / 2u;
    branch_pop(c);
    push(c, mid + 1u, b.hi);
    push(c, b.lo, mid); /* lower half first */
}

static void branch_split_around(pm_rdm_ctrl_t *c, uint64_t u)
{
    pm_rdm_branch_t b = c->stack[c->stack_depth - 1];
    if (c->stack_depth >= PM_RDM_DISC_STACK && u > b.lo && u < b.hi) {
        c->run_corrupt = true;
        branch_no_progress(c);
        return;
    }
    branch_pop(c);
    if (u < b.hi) push(c, u + 1u, b.hi);
    if (u > b.lo) push(c, b.lo, u - 1u);
}

/* A DUB of the top branch brought no new device. */
static void branch_no_progress(pm_rdm_ctrl_t *c)
{
    if (c->stack_depth && ++c->stack[c->stack_depth - 1].attempts >= PM_RDM_BRANCH_MAX_ATTEMPTS) {
        c->run_corrupt = true;
        branch_pop(c);
    }
}

static void done_dub(pm_rdm_ctrl_t *c, const pm_rdm_rx_t *rx)
{
    if (c->stack_depth == 0) return;
    const pm_rdm_branch_t *b = &c->stack[c->stack_depth - 1];
    if (rx == NULL || rx->status == PM_RDM_RX_TIMEOUT || rx->len == 0) {
        branch_pop(c); /* nobody un-muted in this branch */
        return;
    }
    pm_uid_t uid;
    if (rx->status != PM_RDM_RX_OK || pm_rdm_dub_decode(rx->data, rx->len, &uid) != PM_OK) {
        c->stats.collisions++;
        branch_split_mid(c);
        return;
    }
    uint64_t u = pm_uid_to_u64(&uid);
    bool found;
    int i = tod_index(c, &uid, &found);
    bool seen = found && (c->tod_flags[i] & TOD_SEEN);
    if (u < b->lo || u > b->hi) {
        /* Clean answer with a UID outside the branch: either a phantom from a collision (checksum matched by chance)
         * or a broken responder that answers every DUB. A known broken responder means nobody else answered:
         * the branch is empty. A phantom: split like a collision. Unknown: verify with DISC_MUTE first. */
        c->run_corrupt = true;
        if (seen) {
            branch_pop(c);
        } else if (is_bad(c, &uid)) {
            c->stats.collisions++;
            branch_split_mid(c);
        } else {
            c->pending_uid = uid;
            c->pending_outside = true;
            c->mute_attempts = 0;
            c->disc_step = STEP_MUTE;
        }
        return;
    }
    if (is_bad(c, &uid) || seen) {
        /* muted in this run (or never mutable) but still answering: broken responder, search around it */
        branch_split_around(c, u);
        return;
    }
    c->pending_uid = uid;
    c->pending_outside = false;
    c->mute_attempts = 0;
    c->disc_step = STEP_MUTE;
}

static void done_mute(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_rx_t *rx)
{
    uint16_t flags;
    if (mute_ack(c, rx, &flags)) {
        c->disc_step = STEP_DUB; /* same branch again */
        if (tod_add(c, &c->pending_uid, (uint16_t)(flags | TOD_SEEN), now_ms)) {
            if (c->stack_depth) c->stack[c->stack_depth - 1].attempts = 0;
        } else { /* TOD full: muted but not listed; remember it so it is counted/handled only once */
            add_bad(c, &c->pending_uid);
            branch_no_progress(c);
        }
        return;
    }
    if (++c->mute_attempts < PM_RDM_MUTE_ATTEMPTS) return; /* try again */
    add_bad(c, &c->pending_uid);
    if (c->pending_outside && c->stack_depth) {
        c->stats.collisions++; /* phantom from a collision */
        branch_split_mid(c);
    } else {
        branch_no_progress(c);
    }
    c->disc_step = STEP_DUB;
}

static void done_presence(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_rx_t *rx)
{
    uint16_t flags;
    if (c->presence_idx >= c->tod_count) return;
    if (mute_ack(c, rx, &flags)) {
        if ((c->tod_flags[c->presence_idx] & TOD_PUBLIC_MASK) != flags) c->tod_dirty = true; /* e.g. boot flag */
        c->tod_flags[c->presence_idx] = (uint16_t)(flags | TOD_SEEN);
        c->presence_idx++;
        c->mute_attempts = 0;
        return;
    }
    if (++c->mute_attempts < PM_RDM_PRESENCE_MAX_MISSES) return;
    c->mute_attempts = 0;
    tod_remove_at(c, c->presence_idx, now_ms); /* the next entry moves to presence_idx */
}

static void done_catch(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_rx_t *rx)
{
    pm_rdm_msg_t m;
    if (c->boot_catch.state != PM_BOOT_CATCH_STATE_RUNNING ||
        !pm_uid_equal(&c->inflight_dest, &c->boot_catch.target) || !valid_reply(c, rx, &m)) {
        return;
    }
    /* only an ACK counts: the app answers BOOT_INFO with NACK UNKNOWN_PID */
    if (m.cc == PM_RDM_CC_GET_RESPONSE && m.pid == PM_PID_BOOT_INFO && m.port_or_resp == PM_RDM_RESP_ACK) {
        boot_catch_set(c, PM_BOOT_CATCH_STATE_CAUGHT, now_ms);
    }
}

void pm_rdm_ctrl_done(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_rx_t *rx)
{
    uint8_t inf = c->inflight;
    c->inflight = INF_NONE;
    c->last_now_ms = now_ms;
    if (!c->enabled) return;
    switch (inf) {
    case INF_REQUEST: {
        pm_rdm_msg_t m;
        if (valid_reply(c, rx, &m)) {
            c->stats.responses++;
            if (c->cb.on_response) {
                c->cb.on_response(c->cb.ctx, c->port, c->inflight_token, rx->data,
                                  (uint16_t)(PM_RDM_HEADER_LEN + m.pdl + 2u));
            }
        } else {
            c->stats.timeouts++;
        }
        return;
    }
    case INF_CATCH:
        done_catch(c, now_ms, rx);
        return;
    case INF_DUB:
    case INF_MUTE:
    case INF_PRESENCE:
        if (c->inflight_gen != c->disc_gen || c->disc_mode == PM_RDM_DISC_IDLE) return; /* restarted meanwhile */
        if (inf == INF_DUB) done_dub(c, rx);
        else if (inf == INF_MUTE) done_mute(c, now_ms, rx);
        else done_presence(c, now_ms, rx);
        return;
    default:
        return;
    }
}

/* ------------------------------------------------------------------------------------------------ queries */

uint16_t pm_rdm_ctrl_tod(const pm_rdm_ctrl_t *c, pm_uid_t *out, uint16_t cap)
{
    uint16_t n = c->tod_count < cap ? c->tod_count : cap;
    if (out && n) memcpy(out, c->tod, n * sizeof(pm_uid_t));
    return c->tod_count;
}

bool pm_rdm_ctrl_tod_find(const pm_rdm_ctrl_t *c, const pm_uid_t *uid, uint16_t *control_field)
{
    bool found;
    int i = tod_index(c, uid, &found);
    if (found && control_field) *control_field = (uint16_t)(c->tod_flags[i] & TOD_PUBLIC_MASK);
    return found;
}

bool pm_rdm_ctrl_discovery_running(const pm_rdm_ctrl_t *c) { return c->disc_mode != PM_RDM_DISC_IDLE; }

void pm_rdm_ctrl_get_stats(const pm_rdm_ctrl_t *c, pm_rdm_ctrl_stats_t *out) { *out = c->stats; }

/* ------------------------------------------------------------------------------------------------ BOOT_CATCH */

pm_err_t pm_rdm_ctrl_boot_catch_start(pm_rdm_ctrl_t *c, const pm_uid_t *target, uint32_t now_ms)
{
    if (target == NULL || pm_uid_is_vendorcast(target)) return PM_ERR_INVALID_ARG;
    if (!c->enabled) return PM_ERR_STATE;
    c->last_now_ms = now_ms;
    if (c->inflight == INF_CATCH) c->inflight = INF_NONE; /* the answer of a replaced catch is ignored */
    memset(&c->boot_catch, 0, sizeof(c->boot_catch));
    c->boot_catch.target = *target;
    c->boot_catch.started_ms = now_ms;
    c->next_catch_ms = now_ms;
    boot_catch_set(c, PM_BOOT_CATCH_STATE_RUNNING, now_ms);
    return PM_OK;
}

void pm_rdm_ctrl_boot_catch_abort(pm_rdm_ctrl_t *c)
{
    if (c->boot_catch.state == PM_BOOT_CATCH_STATE_RUNNING) {
        boot_catch_set(c, PM_BOOT_CATCH_STATE_ABORTED, c->last_now_ms);
    }
}

void pm_rdm_ctrl_boot_catch_status(const pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_boot_catch_t *out)
{
    *out = c->boot_catch;
    if (out->state == PM_BOOT_CATCH_STATE_RUNNING) out->elapsed_ms = now_ms - out->started_ms;
}
