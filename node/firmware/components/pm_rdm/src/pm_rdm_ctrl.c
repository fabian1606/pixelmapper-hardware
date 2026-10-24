/*
 * pm_rdm_ctrl.c - per-port RDM controller. Phase 0 stub: init + queue + TOD accessors; discovery and
 * BOOT_CATCH are implemented in Phase 1 (see pm_rdm_ctrl.h).
 */
#include "pm_rdm_ctrl.h"

#include <string.h>

#include "pm_pids.h"

void pm_rdm_ctrl_init(pm_rdm_ctrl_t *c, pm_port_idx_t port, const pm_uid_t *self, const pm_rdm_ctrl_cb_t *cb)
{
    memset(c, 0, sizeof(*c));
    c->port = port;
    c->self = *self;
    if (cb) c->cb = *cb;
    c->bg_interval_ms = 3000;
    c->background = true;
}

void pm_rdm_ctrl_set_enabled(pm_rdm_ctrl_t *c, bool enabled, uint32_t now_ms)
{
    (void)now_ms;
    c->enabled = enabled;
}

void pm_rdm_ctrl_set_background(pm_rdm_ctrl_t *c, bool on, uint32_t interval_ms)
{
    c->background = on;
    c->bg_interval_ms = interval_ms;
}

void pm_rdm_ctrl_flush(pm_rdm_ctrl_t *c, uint32_t now_ms)
{
    (void)now_ms;
    c->tod_count = 0;
}

pm_err_t pm_rdm_ctrl_submit(pm_rdm_ctrl_t *c, const pm_rdm_request_t *req)
{
    if (req == NULL || req->len > PM_RDM_MAX_MSG_LEN) return PM_ERR_INVALID_ARG;
    if (c->q_count >= PM_RDM_QUEUE_LEN) return PM_ERR_FULL;
    c->queue[(c->q_head + c->q_count) % PM_RDM_QUEUE_LEN] = *req;
    c->q_count++;
    return PM_OK;
}

uint8_t pm_rdm_ctrl_queue_free(const pm_rdm_ctrl_t *c) { return (uint8_t)(PM_RDM_QUEUE_LEN - c->q_count); }

bool pm_rdm_ctrl_next(pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_txn_t *out)
{
    (void)c; (void)now_ms; (void)out;
    return false; /* Phase 1 */
}

void pm_rdm_ctrl_done(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_rx_t *rx)
{
    (void)c; (void)now_ms; (void)rx;
}

uint16_t pm_rdm_ctrl_tod(const pm_rdm_ctrl_t *c, pm_uid_t *out, uint16_t cap)
{
    uint16_t n = c->tod_count < cap ? c->tod_count : cap;
    if (out && n) memcpy(out, c->tod, n * sizeof(pm_uid_t));
    return c->tod_count;
}

bool pm_rdm_ctrl_discovery_running(const pm_rdm_ctrl_t *c) { return c->disc_mode != PM_RDM_DISC_IDLE; }

pm_err_t pm_rdm_ctrl_boot_catch_start(pm_rdm_ctrl_t *c, const pm_uid_t *target, uint32_t now_ms)
{
    (void)c; (void)target; (void)now_ms;
    return PM_ERR_NOT_IMPLEMENTED;
}

void pm_rdm_ctrl_boot_catch_abort(pm_rdm_ctrl_t *c) { (void)c; }

void pm_rdm_ctrl_boot_catch_status(const pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_boot_catch_t *out)
{
    (void)now_ms;
    *out = c->boot_catch;
}
