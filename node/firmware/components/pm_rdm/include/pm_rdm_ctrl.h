/*
 * pm_rdm_ctrl.h - RDM controller of one DMX port (pure C11, host-testable).
 *
 * Contents: discovery state machine (full + background with presence check), TOD (up to 512 UIDs), request queue,
 * BOOT_CATCH. Behaviour: protocol/contract.md §6.1, §6.2, §6.4; algorithm: rdm-digest.md §3.3.
 *
 * The controller does NOT touch the line. It is a state machine driven by the port engine (pm_port, core 1):
 *
 *     for (;;) {                                        // pm_port task, between DMX frames
 *         pm_rdm_txn_t t;
 *         if (pm_rdm_ctrl_next(&c, now_ms, &t)) {       // what to send now?
 *             ... send t.data on the line, wait as t.kind says ...
 *             pm_rdm_ctrl_done(&c, now_ms, &result);    // what came back
 *         }
 *     }
 *
 * Priority inside next(): queued requests (ArtRdm, responder-forwarded, UI) > BOOT_CATCH tick > discovery step.
 * Discovery never starves requests; a full discovery is split into single transactions so DMX keeps >= 30 Hz.
 *
 * Threading: not thread-safe. All calls from the port engine task. Requests from core 0 go through pm_port's
 * cross-core queue and are inserted with pm_rdm_ctrl_submit() on core 1. Results leave through the callbacks
 * (called from inside pm_rdm_ctrl_done()/next(); they must only copy/queue).
 */
#pragma once

#include "pm_rdm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_RDM_TOD_MAX 512u
#define PM_RDM_QUEUE_LEN 8u            /**< requests per port (ArtRdm FifoMax) */
#define PM_RDM_PRESENCE_MAX_MISSES 3u  /**< DISC_MUTE misses before a device leaves the TOD */
#define PM_RDM_BROADCAST_GAP_US 176u   /**< min. gap after a broadcast request before the next packet */

/** What the line must do with a transaction. Timing values: rdm-digest.md §2. */
typedef enum {
    PM_RDM_TXN_REQUEST,     /**< send, then receive a response (2.8 ms timeout) */
    PM_RDM_TXN_BROADCAST,   /**< send, no response expected */
    PM_RDM_TXN_DUB,         /**< send DISC_UNIQUE_BRANCH, then collect raw bytes (no break) for the DUB window */
} pm_rdm_txn_kind_t;

typedef struct {
    pm_rdm_txn_kind_t kind;
    uint16_t len;
    uint8_t data[PM_RDM_MAX_MSG_LEN]; /**< wire bytes incl. start code */
} pm_rdm_txn_t;

typedef enum {
    PM_RDM_RX_OK,           /**< a complete packet with valid framing was received (may still fail the checksum) */
    PM_RDM_RX_TIMEOUT,      /**< nothing received in time */
    PM_RDM_RX_GARBAGE,      /**< bytes received, but framing/break errors (collision on DUB) */
} pm_rdm_rx_status_t;

typedef struct {
    pm_rdm_rx_status_t status;
    uint16_t len;
    const uint8_t *data;    /**< response wire bytes (start code first) or raw DUB bytes */
} pm_rdm_rx_t;

/** A queued request from outside (ArtRdm, node UI). */
typedef struct {
    uint32_t token;         /**< echoed in on_response */
    uint16_t len;
    uint8_t data[PM_RDM_MAX_MSG_LEN]; /**< wire bytes incl. start code; TN/src/port id are kept as given */
} pm_rdm_request_t;

typedef struct pm_rdm_ctrl pm_rdm_ctrl_t;

typedef struct {
    void *ctx;
    /** Response (wire bytes) for a submitted request. Not called on timeout or for broadcasts (contract §6.2). */
    void (*on_response)(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len);
    /** TOD changed (device added/removed, flush, discovery finished). */
    void (*on_tod_changed)(void *ctx, pm_port_idx_t port);
    /** BOOT_CATCH state changed (pm_boot_catch_state_t from pm_pids.h). */
    void (*on_boot_catch)(void *ctx, pm_port_idx_t port, uint8_t state);
} pm_rdm_ctrl_cb_t;

typedef enum {
    PM_RDM_DISC_IDLE,
    PM_RDM_DISC_FULL,        /**< un-mute all, binary search over the whole range */
    PM_RDM_DISC_BACKGROUND,  /**< presence check of known UIDs + DUB without un-mute (finds new devices) */
} pm_rdm_disc_mode_t;

typedef struct {
    uint8_t state;           /**< pm_boot_catch_state_t */
    pm_uid_t target;
    uint32_t started_ms;
    uint32_t elapsed_ms;
    uint16_t attempts;
} pm_rdm_boot_catch_t;

/** Controller object. Fields are private to pm_rdm_ctrl.c; the struct is public only for static allocation. */
struct pm_rdm_ctrl {
    pm_port_idx_t port;
    pm_uid_t self;                 /**< source UID of discovery/BOOT_CATCH packets (node UID) */
    pm_rdm_ctrl_cb_t cb;
    bool enabled;
    bool background;
    uint32_t bg_interval_ms;
    uint32_t next_bg_ms;
    pm_rdm_disc_mode_t disc_mode;
    uint8_t tn;
    /* TOD, sorted by UID */
    uint16_t tod_count;
    pm_uid_t tod[PM_RDM_TOD_MAX];
    uint8_t tod_misses[PM_RDM_TOD_MAX];
    /* discovery: explicit stack of branches (binary search) */
    uint16_t stack_depth;
    struct { uint64_t lo, hi; } stack[49];
    uint8_t disc_step;
    pm_uid_t pending_uid;
    /* request queue (ring) */
    uint8_t q_head, q_count;
    pm_rdm_request_t queue[PM_RDM_QUEUE_LEN];
    uint8_t inflight;              /**< what the last next() handed out (private enum) */
    uint32_t inflight_token;
    pm_rdm_boot_catch_t boot_catch;
    uint32_t next_catch_ms;
};

void pm_rdm_ctrl_init(pm_rdm_ctrl_t *c, pm_port_idx_t port, const pm_uid_t *self, const pm_rdm_ctrl_cb_t *cb);

/** RDM on/off for the port (ArtAddress AcRdmEnable/Disable, port direction, port enabled). Off: TOD cleared
 *  (on_tod_changed), queue answered with nothing (requests dropped), BOOT_CATCH aborted. On: full discovery. */
void pm_rdm_ctrl_set_enabled(pm_rdm_ctrl_t *c, bool enabled, uint32_t now_ms);

/** Background discovery on/off + interval (PORTn_RDM_BG_DISCOVERY, RDM_BG_DISCOVERY_INTERVAL). */
void pm_rdm_ctrl_set_background(pm_rdm_ctrl_t *c, bool on, uint32_t interval_ms);

/** ArtTodControl AtcFlush: clear TOD, start full discovery. */
void pm_rdm_ctrl_flush(pm_rdm_ctrl_t *c, uint32_t now_ms);

/** Queue a request. PM_ERR_FULL when PM_RDM_QUEUE_LEN requests are waiting. */
pm_err_t pm_rdm_ctrl_submit(pm_rdm_ctrl_t *c, const pm_rdm_request_t *req);
/** Free queue slots (ArtRdm FifoAvail). */
uint8_t pm_rdm_ctrl_queue_free(const pm_rdm_ctrl_t *c);

/** Next transaction to put on the line, or false if idle now. */
bool pm_rdm_ctrl_next(pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_txn_t *out);
/** Result of the transaction returned by the last next(). For PM_RDM_TXN_BROADCAST pass PM_RDM_RX_TIMEOUT. */
void pm_rdm_ctrl_done(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_rx_t *rx);

/** TOD access (copy, sorted). Returns the total count. */
uint16_t pm_rdm_ctrl_tod(const pm_rdm_ctrl_t *c, pm_uid_t *out, uint16_t cap);
bool pm_rdm_ctrl_discovery_running(const pm_rdm_ctrl_t *c);

/** BOOT_CATCH (contract §6.4): GET BOOT_INFO to `target` every PM_BOOT_CATCH_PERIOD_MS for
 *  PM_BOOT_CATCH_DURATION_MS. Replaces a running catch. PM_ERR_STATE if RDM is disabled on this port,
 *  PM_ERR_INVALID_ARG for broadcast/vendorcast UIDs. */
pm_err_t pm_rdm_ctrl_boot_catch_start(pm_rdm_ctrl_t *c, const pm_uid_t *target, uint32_t now_ms);
void pm_rdm_ctrl_boot_catch_abort(pm_rdm_ctrl_t *c);
void pm_rdm_ctrl_boot_catch_status(const pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_boot_catch_t *out);

#ifdef __cplusplus
}
#endif
