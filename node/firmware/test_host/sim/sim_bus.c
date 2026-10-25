/* sim_bus.c - simulated RS-485 RDM bus (see sim_bus.h). */
#include "sim_bus.h"

#include <string.h>

#include "pm_pids.h"
#include "sim_model.h"

#define TUBE_WINDOW_START_MS 26u  /* tube README: rescue window 26..225 ms after reset */
#define TUBE_WINDOW_END_MS 225u

void sim_init(sim_bus_t *b, uint32_t seed)
{
    memset(b, 0, sizeof(*b));
    b->rng = seed ? seed : 1u;
}

uint32_t sim_rand(sim_bus_t *b)
{
    uint32_t x = b->rng; /* xorshift32 */
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    b->rng = x;
    return x;
}

int sim_find(const sim_bus_t *b, const pm_uid_t *uid)
{
    for (int i = 0; i < b->n; i++) {
        if (pm_uid_equal(&b->dev[i].uid, uid)) return i;
    }
    return -1;
}

int sim_add(sim_bus_t *b, pm_uid_t uid)
{
    if (b->n >= SIM_MAX_DEVS || sim_find(b, &uid) >= 0) return -1; /* UIDs are unique */
    sim_dev_t *d = &b->dev[b->n];
    memset(d, 0, sizeof(*d));
    d->uid = uid;
    d->present = true;
    d->app_valid = true;
    d->preamble = (uint8_t)(sim_rand(b) % 8u);
    d->resp_delay_us = (uint16_t)(176u + sim_rand(b) % (PM_RDM_RESPONSE_TIMEOUT_US - 176u + 1u));
    d->break_us = (uint16_t)(176u + sim_rand(b) % 177u);
    d->mab_us = (uint16_t)(12u + sim_rand(b) % 77u);
    return b->n++;
}

void sim_add_random(sim_bus_t *b, int n)
{
    for (int k = 0; k < n;) {
        uint32_t r = sim_rand(b);
        uint16_t man;
        uint32_t dev;
        switch (r % 4u) {
        case 0: /* our manufacturer, clustered device IDs (neighbours collide deep in the tree) */
            man = PM_ESTA_MANUFACTURER;
            dev = 0x12340000u + (sim_rand(b) % 64u);
            break;
        case 1:
            man = PM_ESTA_MANUFACTURER;
            dev = sim_rand(b);
            break;
        default:
            man = (uint16_t)(1u + sim_rand(b) % 0x7FFEu);
            dev = sim_rand(b);
            break;
        }
        pm_uid_t u = pm_uid_make(man, dev);
        if (pm_uid_is_vendorcast(&u) || sim_find(b, &u) >= 0) continue;
        sim_add(b, u);
        k++;
    }
}

void sim_power_cycle(sim_bus_t *b, int idx, uint32_t off_ms)
{
    sim_dev_t *d = &b->dev[idx];
    d->present = false;
    d->power_on_ms = b->now_ms + off_ms;
}

/* Lazy device clock: power-on, rescue window. Returns false while the device cannot answer. */
static bool dev_update(sim_bus_t *b, sim_dev_t *d)
{
    if (!d->present) {
        if (d->power_on_ms == 0 || (int32_t)(b->now_ms - d->power_on_ms) < 0) return false;
        d->present = true;
        d->muted = false;
        d->mode = d->tube ? SIM_BOOT_WINDOW : SIM_APP;
        if (d->model) sim_model_power_on(b, d);
    }
    if (d->mode == SIM_BOOT_WINDOW) {
        uint32_t t = b->now_ms - d->power_on_ms;
        if (t < TUBE_WINDOW_START_MS) return false;
        if (t > TUBE_WINDOW_END_MS) {
            d->muted = false; /* reset into the app */
            if (d->model) {
                sim_model_window_end(b, d);
            } else {
                d->mode = d->app_valid ? SIM_APP : SIM_BOOT;
            }
        }
    }
    if (d->model) return sim_model_update(b, d);
    return true;
}

static int encode_resp(const sim_dev_t *d, const pm_rdm_msg_t *req, uint8_t rt, const uint8_t *pd, uint8_t pdl,
                       uint8_t *out)
{
    pm_rdm_msg_t r;
    pm_rdm_make_response(req, &d->uid, rt, 0, &r);
    r.pd = pd;
    r.pdl = pdl;
    return pm_rdm_encode(&r, out, PM_RDM_MAX_MSG_LEN);
}

static int nack(const sim_dev_t *d, const pm_rdm_msg_t *req, uint16_t reason, uint8_t *out)
{
    uint8_t pd[2];
    pm_put_be16(pd, reason);
    return encode_resp(d, req, PM_RDM_RESP_NACK_REASON, pd, 2, out);
}

/* One device's answer to `m` (0 = silent). */
static int dev_respond(sim_bus_t *b, sim_dev_t *d, const pm_rdm_msg_t *m, uint8_t *out)
{
    if (!dev_update(b, d) || !pm_uid_addresses(&m->dest, &d->uid)) return 0;
    bool bcast = !pm_uid_equal(&m->dest, &d->uid);
    bool boot = d->mode != SIM_APP;
    if (m->cc == PM_RDM_CC_DISCOVERY) {
        if (m->sub_device != 0) return 0;
        if (m->pid == PM_RDM_PID_DISC_UNIQUE_BRANCH) {
            if (m->pdl != 12 || (d->muted && !d->ignore_mute)) return 0;
            pm_uid_t lo, hi;
            memcpy(lo.b, m->pd, 6);
            memcpy(hi.b, m->pd + 6, 6);
            uint64_t u = pm_uid_to_u64(&d->uid);
            if (!d->ignore_range && (u < pm_uid_to_u64(&lo) || u > pm_uid_to_u64(&hi))) return 0;
            return pm_rdm_dub_encode(&d->uid, d->preamble, out, 32);
        }
        if ((m->pid != PM_RDM_PID_DISC_MUTE && m->pid != PM_RDM_PID_DISC_UN_MUTE) || m->pdl != 0) return 0;
        if (d->never_ack_mute) return 0;
        d->muted = m->pid == PM_RDM_PID_DISC_MUTE;
        if (bcast) return 0;
        uint8_t pd[8];
        uint16_t cf = d->control;
        if (boot && !d->app_valid) cf |= PM_RDM_MUTE_BOOT_LOADER;
        pm_put_be16(pd, cf);
        memcpy(pd + 2, d->uid.b, 6);
        return encode_resp(d, m, PM_RDM_RESP_ACK, pd, d->binding ? 8 : 2, out);
    }
    if (m->cc != PM_RDM_CC_GET && m->cc != PM_RDM_CC_SET) return 0;
    if (bcast && m->cc == PM_RDM_CC_GET) return 0;
    d->requests++;
    d->last_pid_hi = (uint8_t)(m->pid >> 8);
    d->last_pid_lo = (uint8_t)m->pid;
    if (d->model) return sim_model_respond(b, d, m, bcast, out);
    int n;
    if (m->pid == PM_PID_BOOT_INFO && m->cc == PM_RDM_CC_GET) {
        d->boot_info_gets++;
        if (!d->tube || !boot) {
            n = nack(d, m, PM_NR_UNKNOWN_PID, out);
        } else {
            d->mode = SIM_BOOT; /* caught: stays in the bootloader */
            uint8_t pd[19] = {PM_BOOT_RDM_VERSION, PM_BOOT_RDM_PAGE_SIZE, 0x18, 0x00, 0, 0};
            pm_put_be16(pd + 6, PM_ESTA_MANUFACTURER);
            pm_put_be16(pd + 8, PM_MODEL_TUBE);
            pm_put_be32(pd + 10, PM_BOOT_RDM_SW_VERSION_ID);
            pd[14] = d->app_valid;
            pm_put_be32(pd + 15, 0x500);
            n = encode_resp(d, m, PM_RDM_RESP_ACK, pd, sizeof(pd), out);
        }
    } else if (m->pid == PM_RDM_PID_DEVICE_INFO && m->cc == PM_RDM_CC_GET) {
        uint8_t pd[19] = {0x01, 0x00};
        pm_put_be16(pd + 2, PM_MODEL_TUBE);
        pm_put_be32(pd + 6, boot ? PM_BOOT_RDM_SW_VERSION_ID : 0x500);
        n = encode_resp(d, m, PM_RDM_RESP_ACK, pd, sizeof(pd), out);
    } else if (m->pid == PM_RDM_PID_IDENTIFY_DEVICE && m->cc == PM_RDM_CC_SET) {
        n = encode_resp(d, m, PM_RDM_RESP_ACK, NULL, 0, out);
    } else {
        n = nack(d, m, PM_NR_UNKNOWN_PID, out);
    }
    return bcast ? 0 : n;
}

void sim_transact(sim_bus_t *b, const pm_rdm_txn_t *t, pm_rdm_rx_t *rx)
{
    pm_rdm_msg_t m;
    uint8_t one[PM_RDM_MAX_MSG_LEN];
    int count = 0, len = 0;
    uint32_t slowest = 0;
    b->txns++;
    uint32_t req_us = 176u + 16u + (uint32_t)t->len * PM_RDM_BYTE_US; /* sim_run uses 176/16 too */
    if (t->kind == PM_RDM_TXN_DUB) b->last_us = req_us + PM_RDM_DUB_GAP_US;
    else if (t->kind == PM_RDM_TXN_BROADCAST) b->last_us = req_us + PM_RDM_BROADCAST_GAP_US;
    else b->last_us = req_us + PM_RDM_NO_RESPONSE_GAP_US;
    if (t->kind == PM_RDM_TXN_DUB) b->dubs++;
    if (t->kind == PM_RDM_TXN_BROADCAST) b->broadcasts++;
    rx->status = PM_RDM_RX_TIMEOUT;
    rx->len = 0;
    rx->data = b->rx;
    if (pm_rdm_decode(t->data, t->len, &m) != PM_OK) return;
    memset(b->rx, 0xFF, sizeof(b->rx));
    for (int i = 0; i < b->n; i++) {
        int n = dev_respond(b, &b->dev[i], &m, one);
        if (n <= 0) continue;
        for (int k = 0; k < n; k++) b->rx[k] &= one[k]; /* wired AND */
        if (n > len) len = n;
        count++;
        const sim_dev_t *d = &b->dev[i];
        uint32_t us = d->resp_delay_us + d->break_us + d->mab_us + (uint32_t)n * PM_RDM_BYTE_US;
        if (us > slowest) slowest = us;
    }
    if (t->kind == PM_RDM_TXN_BROADCAST || count == 0) return; /* nothing listened for / nothing came */
    rx->len = (uint16_t)len;
    rx->status = PM_RDM_RX_OK;
    if (t->kind == PM_RDM_TXN_REQUEST && req_us + slowest + PM_RDM_BROADCAST_GAP_US > b->last_us) {
        b->last_us = req_us + slowest + PM_RDM_BROADCAST_GAP_US;
    }
    if (count > 1) {
        b->collisions++;
        if (sim_rand(b) % 100u < b->garbage_percent) rx->status = PM_RDM_RX_GARBAGE;
    } else if (b->noise_permille && sim_rand(b) % 1000u < b->noise_permille) {
        b->rx[sim_rand(b) % (unsigned)len] ^= (uint8_t)(1u << (sim_rand(b) % 8u));
    }
}

void sim_run(sim_bus_t *b, pm_rdm_ctrl_t *c, uint32_t until_ms, uint16_t slots, bool (*stop)(void *), void *ctx,
             sim_port_stats_t *st)
{
    const uint32_t frame_us = 176u + 16u + (1u + slots) * PM_RDM_BYTE_US;
    const uint32_t min_period_us = 25000u;            /* configured 40 Hz */
    const uint32_t max_period_us = 1000000u / 30u;    /* >= 30 Hz */
    while ((int32_t)(b->now_ms - until_ms) < 0 && !(stop && stop(ctx))) {
        uint64_t t0 = b->now_us;
        b->now_us += frame_us; /* DMX frame */
        b->now_ms = (uint32_t)(b->now_us / 1000u);
        if (st) st->frames++;
        uint32_t budget = max_period_us > frame_us ? max_period_us - frame_us : 0;
        bool first = true;
        pm_rdm_txn_t t;
        while (pm_rdm_ctrl_next_max(c, b->now_ms, first ? UINT32_MAX : budget, &t)) {
            pm_rdm_rx_t rx;
            sim_transact(b, &t, &rx);
            if (st && b->last_us > t.est_us) st->est_violations++;
            b->now_us += b->last_us;
            b->now_ms = (uint32_t)(b->now_us / 1000u);
            pm_rdm_ctrl_done(c, b->now_ms, &rx);
            budget = budget > b->last_us ? budget - b->last_us : 0;
            first = false;
            if (st) st->rdm_txns++;
        }
        if (b->now_us - t0 < min_period_us) b->now_us = t0 + min_period_us;
        b->now_ms = (uint32_t)(b->now_us / 1000u);
        uint32_t period = (uint32_t)(b->now_us - t0);
        if (st && period > st->max_period_us) st->max_period_us = period;
    }
}
