/*
 * pm_port_line.c - line state machine of one DMX/RDM port (see pm_port_line.h). Pure C11.
 */
#include "pm_port_line.h"

#include <string.h>

/* ------------------------------------------------------------------------------------------------ scheduler */

PM_PORT_IRAM pm_port_sched_t pm_port_sched_decide(const pm_port_sched_in_t *in, uint32_t *wake_us)
{
    if (!in->dmx_started) {
        *wake_us = in->now;
        return PM_PORT_SCHED_DMX; /* the first frame always comes first; it anchors the rate */
    }
    uint32_t floor_period = 1000000u / PM_PORT_DMX_FLOOR_HZ;
    if (in->period_us > floor_period) floor_period = in->period_us;
    uint32_t floor_at = in->last_dmx_start + floor_period;

    uint32_t dmx_due;
    if (in->continuous || in->dmx_fresh) {
        dmx_due = in->last_dmx_start + in->period_us;
    } else {
        dmx_due = in->last_dmx_start + PM_PORT_DELTA_REFRESH_US;
    }

    if (in->rdm_pending) {
        if (in->frames_since_rdm >= PM_PORT_RDM_STARVE_FRAMES) return PM_PORT_SCHED_RDM;
        if (pm_port_t_ge(floor_at, in->now + in->rdm_est_us)) return PM_PORT_SCHED_RDM;
    }
    if (pm_port_t_ge(in->now, dmx_due)) return PM_PORT_SCHED_DMX;

    *wake_us = dmx_due;
    if (!in->continuous && !in->dmx_fresh && pm_port_t_ge(dmx_due, in->now + PM_PORT_DELTA_POLL_US)) {
        *wake_us = in->now + PM_PORT_DELTA_POLL_US; /* delta: look for new data again soon */
    }
    return PM_PORT_SCHED_WAIT;
}

PM_PORT_IRAM uint32_t pm_port_rdm_estimate_us(const pm_rdm_txn_t *t, uint16_t break_us, uint16_t mab_us)
{
    uint32_t us = (uint32_t)break_us + mab_us + (uint32_t)t->len * PM_PORT_US_PER_SLOT;
    switch (t->kind) {
    case PM_RDM_TXN_BROADCAST:
        us += PM_PORT_RDM_GAP_US;
        break;
    case PM_RDM_TXN_DUB:
        us += PM_PORT_RDM_DUB_BACKOFF_US;
        break;
    case PM_RDM_TXN_REQUEST:
    default: {
        /* responder delay up to 2 ms + typical response, or the no-response backoff - whichever is longer */
        uint32_t resp = 2000u + PM_PORT_RDM_EXPECT_RESP_SLOTS * PM_PORT_US_PER_SLOT + PM_PORT_RDM_GAP_US;
        us += resp > PM_PORT_RDM_NORESP_BACKOFF_US ? resp : PM_PORT_RDM_NORESP_BACKOFF_US;
        break;
    }
    }
    return us;
}

/* ------------------------------------------------------------------------------------------------ helpers */

__attribute__((always_inline)) static inline uint16_t clamp16(uint16_t v, uint16_t lo, uint16_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static PM_PORT_IRAM void set_deadline(pm_port_line_t *l, uint32_t t)
{
    l->has_deadline = true;
    l->deadline = t;
}

static PM_PORT_IRAM void enter(pm_port_line_t *l, pm_port_line_state_t s)
{
    l->state = s;
    l->has_deadline = false;
}

static PM_PORT_IRAM void start_packet(pm_port_line_t *l, uint32_t now, bool rdm)
{
    l->cur_rdm = rdm;
    if (rdm) {
        l->cur_data = l->offer->data;
        l->cur_len = l->offer->len;
    } else {
        l->cur_len = l->ops.dmx_frame(l->ops.ctx, &l->cur_data);
        if (l->cur_len == 0 || l->cur_len > PM_PORT_FRAME_MAX) {
            l->cur_len = 0;
        }
        l->dmx_started = true;
        l->last_dmx_start = now;
        if (l->offer && !l->result_ready) {
            if (l->frames_since_rdm < 255u) l->frames_since_rdm++;
        }
    }
    l->ops.set_de(l->ops.ctx, true);
    l->ops.set_break(l->ops.ctx, true);
    enter(l, PM_PORT_LINE_BREAK);
    set_deadline(l, now + l->timing.break_us);
}

/** Output line idle: ask the scheduler what comes next. */
static PM_PORT_IRAM void schedule(pm_port_line_t *l, uint32_t now)
{
    bool rdm_pending = l->offer != NULL && !l->result_ready;
    pm_port_sched_in_t in = {
        .now = now,
        .dmx_started = l->dmx_started,
        .last_dmx_start = l->last_dmx_start,
        .period_us = l->period_us,
        .continuous = l->timing.continuous,
        .dmx_fresh = l->timing.continuous ? true : l->ops.dmx_fresh(l->ops.ctx),
        .rdm_pending = rdm_pending,
        .rdm_est_us = rdm_pending ? pm_port_rdm_estimate_us(l->offer, l->timing.break_us, l->timing.mab_us) : 0,
        .frames_since_rdm = l->frames_since_rdm,
    };
    uint32_t wake = now;
    switch (pm_port_sched_decide(&in, &wake)) {
    case PM_PORT_SCHED_DMX:
        start_packet(l, now, false);
        break;
    case PM_PORT_SCHED_RDM:
        start_packet(l, now, true);
        break;
    case PM_PORT_SCHED_WAIT:
    default:
        enter(l, PM_PORT_LINE_GAP);
        set_deadline(l, wake);
        break;
    }
}

static PM_PORT_IRAM void backoff(pm_port_line_t *l, uint32_t until)
{
    enter(l, PM_PORT_LINE_BACKOFF);
    set_deadline(l, until);
}

/** Finish the RDM transaction in flight. */
static PM_PORT_IRAM void rdm_finish(pm_port_line_t *l, pm_rdm_rx_status_t st)
{
    l->result.status = st;
    l->result.len = st == PM_RDM_RX_TIMEOUT ? 0 : l->rx_len;
    l->result.data = l->rx;
    l->result_ready = true;
    l->frames_since_rdm = 0;
    if (st == PM_RDM_RX_TIMEOUT && l->offer->kind != PM_RDM_TXN_BROADCAST) l->stats.rdm_timeouts++;
    if (st == PM_RDM_RX_GARBAGE) l->stats.rdm_garbage++;
    l->ops.rdm_done(l->ops.ctx);
}

static PM_PORT_IRAM uint32_t later(uint32_t a, uint32_t b) { return pm_port_t_ge(a, b) ? a : b; }

/* ------------------------------------------------------------------------------------------------ input */

static PM_PORT_IRAM void input_finish(pm_port_line_t *l, uint32_t now)
{
    if (l->in_len > 0) {
        if (l->in[0] == 0x00) {
            l->stats.frames_in++;
            l->last_in_us = now;
            l->ops.input_frame(l->ops.ctx, l->in, l->in_len);
        } else {
            l->stats.frames_in_other_sc++;
        }
    }
    l->in_len = 0;
}

/* ------------------------------------------------------------------------------------------------ API */

void pm_port_line_init(pm_port_line_t *l, const pm_port_line_ops_t *ops)
{
    memset(l, 0, sizeof(*l));
    l->ops = *ops;
    l->timing.break_us = PM_PORT_BREAK_DEFAULT_US;
    l->timing.mab_us = PM_PORT_MAB_DEFAULT_US;
    l->timing.rate_hz = PM_PORT_RATE_DEFAULT_HZ;
    l->timing.continuous = true;
    l->period_us = 1000000u / PM_PORT_RATE_DEFAULT_HZ;
}

PM_PORT_IRAM void pm_port_line_set_timing(pm_port_line_t *l, const pm_port_line_timing_t *t)
{
    l->timing.break_us = clamp16(t->break_us, PM_PORT_BREAK_MIN_US, PM_PORT_BREAK_MAX_US);
    l->timing.mab_us = clamp16(t->mab_us, PM_PORT_MAB_MIN_US, PM_PORT_MAB_MAX_US);
    l->timing.rate_hz = (uint8_t)clamp16(t->rate_hz, PM_PORT_RATE_MIN_HZ, PM_PORT_RATE_MAX_HZ);
    l->timing.continuous = t->continuous;
    l->period_us = 1000000u / l->timing.rate_hz;
}

PM_PORT_IRAM void pm_port_line_configure(pm_port_line_t *l, pm_port_line_mode_t mode,
                                         const pm_port_line_timing_t *timing, uint32_t now)
{
    if (l->offer && !l->result_ready) {
        l->rx_len = 0;
        rdm_finish(l, PM_RDM_RX_TIMEOUT);
    }
    if (timing) pm_port_line_set_timing(l, timing);
    l->ops.set_break(l->ops.ctx, false);
    l->mode = mode;
    l->dmx_started = false;
    l->frames_since_rdm = 0;
    l->in_len = 0;
    l->in_sync = false;
    l->rx_len = 0;
    l->rx_err = false;
    switch (mode) {
    case PM_PORT_LINE_MODE_OUTPUT:
        l->ops.set_de(l->ops.ctx, true);
        /* mark before the first break: one gap */
        backoff(l, now + PM_PORT_RDM_GAP_US);
        break;
    case PM_PORT_LINE_MODE_INPUT:
        l->ops.set_de(l->ops.ctx, false);
        l->ops.rx_flush(l->ops.ctx);
        enter(l, PM_PORT_LINE_INPUT);
        break;
    case PM_PORT_LINE_MODE_OFF:
    default:
        l->ops.set_de(l->ops.ctx, false);
        enter(l, PM_PORT_LINE_OFF);
        break;
    }
}

PM_PORT_IRAM bool pm_port_line_rdm_offer(pm_port_line_t *l, const pm_rdm_txn_t *t, uint32_t now)
{
    if (l->mode != PM_PORT_LINE_MODE_OUTPUT || pm_port_line_rdm_busy(l) || t == NULL || t->len == 0 ||
        t->len > PM_RDM_MAX_MSG_LEN) {
        return false;
    }
    l->offer = t;
    l->result_ready = false;
    l->frames_since_rdm = 0;
    if (l->state == PM_PORT_LINE_GAP) schedule(l, now);
    return true;
}

PM_PORT_IRAM bool pm_port_line_rdm_busy(const pm_port_line_t *l) { return l->offer != NULL; }

PM_PORT_IRAM bool pm_port_line_rdm_take(pm_port_line_t *l, pm_rdm_rx_t *rx)
{
    if (!l->offer || !l->result_ready) return false;
    *rx = l->result;
    l->offer = NULL;
    l->result_ready = false;
    return true;
}

PM_PORT_IRAM void pm_port_line_poke(pm_port_line_t *l, uint32_t now)
{
    if (l->state == PM_PORT_LINE_GAP) schedule(l, now);
}

PM_PORT_IRAM void pm_port_line_timer(pm_port_line_t *l, uint32_t now)
{
    if (!l->has_deadline || !pm_port_t_ge(now, l->deadline)) return;
    l->has_deadline = false;

    switch (l->state) {
    case PM_PORT_LINE_GAP:
        schedule(l, now);
        break;
    case PM_PORT_LINE_BREAK:
        l->ops.set_break(l->ops.ctx, false);
        enter(l, PM_PORT_LINE_MAB);
        set_deadline(l, now + l->timing.mab_us);
        break;
    case PM_PORT_LINE_MAB:
        enter(l, PM_PORT_LINE_TX);
        if (l->cur_len == 0) {
            pm_port_line_tx_done(l, now);
            break;
        }
        /* watchdog: twice the nominal time plus 2 ms */
        set_deadline(l, now + 2u * l->cur_len * PM_PORT_US_PER_SLOT + 2000u);
        l->ops.tx_start(l->ops.ctx, l->cur_data, l->cur_len);
        break;
    case PM_PORT_LINE_TX:
        l->stats.tx_watchdog++;
        pm_port_line_tx_done(l, now);
        break;
    case PM_PORT_LINE_RDM_WAIT:
        /* nothing (valid) came: timeout; framing errors only = garbage */
        rdm_finish(l, l->rx_err ? PM_RDM_RX_GARBAGE : PM_RDM_RX_TIMEOUT);
        backoff(l, later(now, l->tx_end + (l->offer->kind == PM_RDM_TXN_DUB ? PM_PORT_RDM_DUB_BACKOFF_US
                                                                             : PM_PORT_RDM_NORESP_BACKOFF_US)));
        break;
    case PM_PORT_LINE_RDM_RX:
        if (l->offer->kind == PM_RDM_TXN_DUB) {
            /* DUB response is over (silence) */
            rdm_finish(l, l->rx_err ? PM_RDM_RX_GARBAGE : PM_RDM_RX_OK);
            backoff(l, later(now + PM_PORT_RDM_GAP_US, l->tx_end + PM_PORT_RDM_DUB_BACKOFF_US));
        } else {
            /* inter-slot timeout inside a response: incomplete */
            rdm_finish(l, PM_RDM_RX_GARBAGE);
            backoff(l, later(now + PM_PORT_RDM_GAP_US, l->tx_end + PM_PORT_RDM_NORESP_BACKOFF_US));
        }
        break;
    case PM_PORT_LINE_BACKOFF:
        if (l->mode == PM_PORT_LINE_MODE_OUTPUT) {
            l->ops.set_de(l->ops.ctx, true);
            schedule(l, now);
        }
        break;
    case PM_PORT_LINE_OFF:
    case PM_PORT_LINE_INPUT:
    default:
        break;
    }
}

PM_PORT_IRAM void pm_port_line_tx_done(pm_port_line_t *l, uint32_t now)
{
    if (l->state != PM_PORT_LINE_TX) return;
    l->has_deadline = false;
    l->tx_end = now;
    if (!l->cur_rdm) {
        l->stats.frames_out++;
        backoff(l, now + PM_PORT_RDM_GAP_US);
        return;
    }
    l->stats.rdm_txns++;
    l->rx_len = 0;
    l->rx_err = false;
    if (l->offer->kind == PM_RDM_TXN_BROADCAST) {
        rdm_finish(l, PM_RDM_RX_TIMEOUT);
        backoff(l, now + PM_PORT_RDM_GAP_US);
        return;
    }
    /* turnaround: release the line immediately, the responder may answer after 176 us */
    l->ops.set_de(l->ops.ctx, false);
    l->ops.rx_flush(l->ops.ctx);
    enter(l, PM_PORT_LINE_RDM_WAIT);
    set_deadline(l, now + (l->offer->kind == PM_RDM_TXN_DUB ? PM_PORT_RDM_DUB_WINDOW_US
                                                            : PM_PORT_RDM_RESPONSE_TIMEOUT_US));
}

static PM_PORT_IRAM void rx_append(pm_port_line_t *l, const uint8_t *data, uint16_t len, uint16_t cap)
{
    for (uint16_t i = 0; i < len; i++) {
        if (l->rx_len < cap) {
            l->rx[l->rx_len++] = data[i];
        } else {
            l->rx_err = true; /* longer than any valid packet */
        }
    }
}

PM_PORT_IRAM void pm_port_line_rx_bytes(pm_port_line_t *l, uint32_t now, const uint8_t *data, uint16_t len)
{
    if (len == 0) return;
    switch (l->state) {
    case PM_PORT_LINE_INPUT:
        if (!l->in_sync) return;
        for (uint16_t i = 0; i < len; i++) {
            l->in[l->in_len++] = data[i];
            if (l->in_len == PM_PORT_FRAME_MAX) {
                /* full frame: deliver now instead of at the next break; drop the rest */
                input_finish(l, now);
                l->in_sync = false;
                return;
            }
        }
        break;
    case PM_PORT_LINE_RDM_WAIT:
        if (l->offer->kind == PM_RDM_TXN_DUB) {
            rx_append(l, data, len, PM_PORT_DUB_MAX_BYTES);
            enter(l, PM_PORT_LINE_RDM_RX);
            set_deadline(l, now + PM_PORT_RDM_DUB_IDLE_END_US);
        } else {
            l->rx_err = true; /* bytes without a break before the response: noise */
        }
        break;
    case PM_PORT_LINE_RDM_RX:
        if (l->offer->kind == PM_RDM_TXN_DUB) {
            rx_append(l, data, len, PM_PORT_DUB_MAX_BYTES);
            set_deadline(l, now + PM_PORT_RDM_DUB_IDLE_END_US);
            break;
        }
        rx_append(l, data, len, PM_RDM_MAX_MSG_LEN);
        set_deadline(l, now + PM_PORT_RDM_INTERSLOT_TIMEOUT_US);
        if (l->rx_len >= 1 && l->rx[0] != PM_RDM_START_CODE) {
            rdm_finish(l, PM_RDM_RX_GARBAGE);
            backoff(l, later(now + PM_PORT_RDM_GAP_US, l->tx_end + PM_PORT_RDM_NORESP_BACKOFF_US));
        } else if (l->rx_len >= 3 && l->rx_len >= (uint16_t)l->rx[2] + 2u) {
            l->rx_len = (uint16_t)l->rx[2] + 2u; /* message length excludes the checksum */
            rdm_finish(l, l->rx_err ? PM_RDM_RX_GARBAGE : PM_RDM_RX_OK);
            backoff(l, now + PM_PORT_RDM_GAP_US);
        }
        break;
    default:
        break; /* output idle / transmitting: own echo or noise, ignored */
    }
}

PM_PORT_IRAM void pm_port_line_rx_error(pm_port_line_t *l, uint32_t now)
{
    (void)now;
    l->stats.rx_errors++;
    /* Only DUB uses framing errors as the collision signal. A response with a break also produces a framing
     * error for the break itself (the order of the two interrupts is not defined), so for normal responses
     * the erroneous bytes are simply missing (discarded by the UART) and length/checksum catch the damage. */
    if ((l->state == PM_PORT_LINE_RDM_WAIT || l->state == PM_PORT_LINE_RDM_RX) &&
        l->offer->kind == PM_RDM_TXN_DUB) {
        l->rx_err = true;
    }
}

PM_PORT_IRAM void pm_port_line_rx_break(pm_port_line_t *l, uint32_t now)
{
    switch (l->state) {
    case PM_PORT_LINE_INPUT:
        if (l->in_sync) input_finish(l, now);
        l->in_len = 0;
        l->in_sync = true;
        break;
    case PM_PORT_LINE_RDM_WAIT:
        if (l->offer->kind == PM_RDM_TXN_DUB) {
            l->rx_err = true; /* DUB responses have no break: something else is talking */
        } else {
            l->rx_len = 0;
            l->rx_err = false; /* noise before the response break does not matter */
            enter(l, PM_PORT_LINE_RDM_RX);
            set_deadline(l, now + PM_PORT_RDM_INTERSLOT_TIMEOUT_US);
        }
        break;
    case PM_PORT_LINE_RDM_RX:
        l->rx_err = true;
        if (l->offer->kind != PM_RDM_TXN_DUB) {
            /* a second break inside a response: two responders or a broken one */
            rdm_finish(l, PM_RDM_RX_GARBAGE);
            backoff(l, later(now + PM_PORT_RDM_GAP_US, l->tx_end + PM_PORT_RDM_NORESP_BACKOFF_US));
        }
        break;
    default:
        break;
    }
}
