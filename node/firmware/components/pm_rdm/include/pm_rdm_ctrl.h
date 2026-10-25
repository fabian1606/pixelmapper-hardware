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
 * Discovery: full = TOD cleared, DISC_UN_MUTE x3, DUB binary search; background (every bg interval) = DISC_UN_MUTE
 * x3 + presence DISC_MUTE of every TOD entry + DUB search for the rest (PM_RDM_DISC_BACKGROUND). pm_port drives it
 * with pm_rdm_ctrl_next_max() and a per-gap time budget (pm_port_rdm_max_us) so discovery keeps DMX >= 30 Hz.
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

#define PM_RDM_UNMUTE_REPEATS 3u        /**< broadcast DISC_UN_MUTE repeats at the start of a full discovery (OLA) */
#define PM_RDM_MUTE_ATTEMPTS 5u         /**< DISC_MUTE attempts to verify a UID found by DUB (OLA MAX_MUTE_ATTEMPTS) */
#define PM_RDM_BRANCH_MAX_FAILURES 5u   /**< collisions on a single-UID branch before it is given up (OLA) */
#define PM_RDM_BRANCH_MAX_ATTEMPTS 5u   /**< DUBs of one branch without a new device before it is given up (OLA) */
#define PM_RDM_DISC_STACK 72u           /**< branch stack depth (48 binary levels + split-around-bad-UID + margin) */
#define PM_RDM_BAD_UIDS 16u             /**< UIDs that answered DUB but not DISC_MUTE (phantoms, broken responders) */
#define PM_RDM_TOD_NOTIFY_MS 1000u      /**< min. interval of on_tod_changed while a full discovery runs */
#define PM_RDM_BG_INTERVAL_MIN_MS 100u

/* Line timing used for pm_rdm_txn_t.est_us (rdm-digest.md §2). */
#define PM_RDM_BYTE_US 44u              /**< 250 kbit/s, 11 bits per slot */
#define PM_RDM_RESPONSE_TIMEOUT_US 2800u /**< request end -> response start, incl. inline proxy delay */
#define PM_RDM_NO_RESPONSE_GAP_US 3000u /**< request end -> next packet when no response came */
#define PM_RDM_DUB_WINDOW_US 2900u      /**< raw bytes of a DUB answer arrive within this window after the request */
#define PM_RDM_DUB_GAP_US 5800u         /**< DUB request end -> next packet */
#define PM_RDM_RESP_DELAY_MAX_US 2000u  /**< responder turnaround */

/** Flag bit in a TOD entry's control field (pm_rdm_ctrl_tod_find): not a wire bit, set for UIDs whose
 *  DISC_MUTE response carried a binding UID. */
#define PM_RDM_TOD_HAS_BINDING_UID 0x8000u

/** What the line must do with a transaction. Timing values: rdm-digest.md §2. */
typedef enum {
    PM_RDM_TXN_REQUEST,     /**< send, then receive a response (PM_RDM_RESPONSE_TIMEOUT_US) */
    PM_RDM_TXN_BROADCAST,   /**< send, no response expected; keep >= PM_RDM_BROADCAST_GAP_US before the next packet */
    PM_RDM_TXN_DUB,         /**< send DISC_UNIQUE_BRANCH, then collect raw bytes (no break) for PM_RDM_DUB_WINDOW_US,
                                 next packet not before PM_RDM_DUB_GAP_US after the request */
} pm_rdm_txn_kind_t;

typedef struct {
    pm_rdm_txn_kind_t kind;
    uint16_t len;
    uint32_t est_us;        /**< worst-case line time from the start of the break until the next packet may start
                                 (assumes the break/MAB of pm_rdm_ctrl_set_timing and the largest expected response) */
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
    /** Response (wire bytes) for a submitted request. Not called on timeout, for broadcasts/vendorcasts, for
     *  forwarded DISC_UNIQUE_BRANCH (raw DUB bytes are no RDM packet) or for responses that fail the checksum or do
     *  not come from the addressed UID with the request's TN (contract §6.2: no response -> no message). */
    void (*on_response)(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len);
    /** TOD changed (device added/removed, flush, discovery finished). */
    void (*on_tod_changed)(void *ctx, pm_port_idx_t port);
    /** BOOT_CATCH state changed (pm_boot_catch_state_t from pm_pids.h). */
    void (*on_boot_catch)(void *ctx, pm_port_idx_t port, uint8_t state);
} pm_rdm_ctrl_cb_t;

typedef enum {
    PM_RDM_DISC_IDLE,
    PM_RDM_DISC_FULL,        /**< un-mute all, binary search over the whole range */
    PM_RDM_DISC_BACKGROUND,  /**< incremental (OLA): DISC_UN_MUTE broadcast x PM_RDM_UNMUTE_REPEATS, presence
                                  check = DISC_MUTE to every known UID (PM_RDM_PRESENCE_MAX_MISSES misses drop it),
                                  then the DUB binary search: only devices not muted by the presence check answer
                                  (new ones, and dropped ones that are still connected). The TOD is not cleared */
} pm_rdm_disc_mode_t;

typedef struct {
    uint8_t state;           /**< pm_boot_catch_state_t */
    pm_uid_t target;
    uint32_t started_ms;
    uint32_t elapsed_ms;
    uint16_t attempts;
} pm_rdm_boot_catch_t;

/** Counters (UI, ArtDiagData, pm_port_stats_t). */
typedef struct {
    uint32_t requests;       /**< forwarded requests put on the line */
    uint32_t responses;      /**< forwarded responses delivered via on_response */
    uint32_t timeouts;       /**< forwarded unicast requests without a valid response */
    uint32_t dubs;
    uint32_t collisions;     /**< DUB answers that could not be decoded */
    uint32_t full_discoveries;
    uint32_t bg_discoveries;
    uint32_t tod_overflow;   /**< devices found while the TOD was full (muted, not listed; counted once per run) */
    uint32_t corrupt_runs;   /**< runs ended by the DUB cap or with branches given up (OLA "tree corrupt") */
} pm_rdm_ctrl_stats_t;

/** Branch of the binary search (private). */
typedef struct {
    uint64_t lo, hi;
    uint8_t failures;
    uint8_t attempts;
} pm_rdm_branch_t;

/** Controller object. Fields are private to pm_rdm_ctrl.c; the struct is public only for static allocation. */
struct pm_rdm_ctrl {
    pm_port_idx_t port;
    pm_uid_t self;                 /**< source UID of discovery/BOOT_CATCH packets (node UID) */
    pm_rdm_ctrl_cb_t cb;
    bool enabled;
    bool background;
    bool bg_schedule;              /**< next_bg_ms must be (re)computed from the next now_ms */
    uint32_t bg_interval_ms;
    uint32_t next_bg_ms;
    uint16_t break_us, mab_us;
    pm_rdm_disc_mode_t disc_mode;
    uint8_t disc_step;
    uint8_t tn;
    uint8_t unmute_left;
    uint8_t mute_attempts;
    uint16_t disc_gen;             /**< changes whenever discovery is restarted/stopped */
    uint32_t run_dubs;             /**< DUBs in the current run (cap: see pm_rdm_ctrl.c) */
    bool run_corrupt;
    /* TOD, sorted by UID */
    uint16_t tod_count;
    pm_uid_t tod[PM_RDM_TOD_MAX];
    uint16_t tod_flags[PM_RDM_TOD_MAX]; /**< DISC_MUTE control field (+ private bits) */
    bool tod_dirty;
    uint32_t tod_notify_ms;
    /* discovery */
    uint16_t stack_depth;
    pm_rdm_branch_t stack[PM_RDM_DISC_STACK];
    pm_uid_t pending_uid;          /**< UID being muted (verify or presence check) */
    bool pending_outside;          /**< pending_uid came from outside the DUB branch */
    uint16_t presence_idx;
    uint8_t bad_count;
    pm_uid_t bad[PM_RDM_BAD_UIDS];
    /* request queue (ring) */
    uint8_t q_head, q_count;
    pm_rdm_request_t queue[PM_RDM_QUEUE_LEN];
    /* transaction handed out by next() */
    uint8_t inflight;              /**< private enum */
    uint8_t inflight_tn;
    uint16_t inflight_gen;
    uint32_t inflight_token;
    pm_uid_t inflight_dest;
    pm_uid_t inflight_src;
    uint8_t inflight_cc;
    uint16_t inflight_pid;
    pm_rdm_boot_catch_t boot_catch;
    uint32_t next_catch_ms;
    uint32_t last_now_ms;          /**< last now_ms seen (for calls without a clock) */
    pm_rdm_ctrl_stats_t stats;
};

void pm_rdm_ctrl_init(pm_rdm_ctrl_t *c, pm_port_idx_t port, const pm_uid_t *self, const pm_rdm_ctrl_cb_t *cb);

/** RDM on/off for the port (ArtAddress AcRdmEnable/Disable, port direction, port enabled). Off: TOD cleared
 *  (on_tod_changed), queue answered with nothing (requests dropped), BOOT_CATCH aborted. On: full discovery. */
void pm_rdm_ctrl_set_enabled(pm_rdm_ctrl_t *c, bool enabled, uint32_t now_ms);

/** Background discovery on/off + interval (PORTn_RDM_BG_DISCOVERY, RDM_BG_DISCOVERY_INTERVAL). */
void pm_rdm_ctrl_set_background(pm_rdm_ctrl_t *c, bool on, uint32_t interval_ms);

/** ArtTodControl AtcFlush: clear TOD, start full discovery. */
void pm_rdm_ctrl_flush(pm_rdm_ctrl_t *c, uint32_t now_ms);

/**
 * ArtTodControl AtcEnd: end a running discovery (full or background) now. The TOD keeps what was found so far
 * (on_tod_changed if it changed); the background search, if on, starts again one interval later.
 * Mapping of the other ArtTodControl commands: AtcFlush -> pm_rdm_ctrl_flush(), AtcIncOn/AtcIncOff ->
 * pm_rdm_ctrl_set_background(c, true/false, interval).
 */
void pm_rdm_ctrl_discovery_stop(pm_rdm_ctrl_t *c, uint32_t now_ms);

/** Queue a request. PM_ERR_FULL when PM_RDM_QUEUE_LEN requests are waiting. */
pm_err_t pm_rdm_ctrl_submit(pm_rdm_ctrl_t *c, const pm_rdm_request_t *req);
/** Free queue slots (ArtRdm FifoAvail). */
uint8_t pm_rdm_ctrl_queue_free(const pm_rdm_ctrl_t *c);

/** Next transaction to put on the line, or false if idle now. */
bool pm_rdm_ctrl_next(pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_txn_t *out);
/** Result of the transaction returned by the last next(). For PM_RDM_TXN_BROADCAST pass PM_RDM_RX_TIMEOUT.
 *  Every transaction handed out must be finished with exactly one done(); if the port could not put it on the line
 *  (driver error, port reconfigured), pass PM_RDM_RX_TIMEOUT (rx may be NULL) - it is then handled like a missing
 *  answer (forwarded request: no on_response; discovery: empty branch / missed DISC_MUTE). */
void pm_rdm_ctrl_done(pm_rdm_ctrl_t *c, uint32_t now_ms, const pm_rdm_rx_t *rx);

/** TOD access (copy, sorted). Returns the total count. */
uint16_t pm_rdm_ctrl_tod(const pm_rdm_ctrl_t *c, pm_uid_t *out, uint16_t cap);
bool pm_rdm_ctrl_discovery_running(const pm_rdm_ctrl_t *c);

/** Break/MAB the port uses (only for pm_rdm_txn_t.est_us). Default 176/16 us. */
void pm_rdm_ctrl_set_timing(pm_rdm_ctrl_t *c, uint16_t break_us, uint16_t mab_us);

/**
 * Like pm_rdm_ctrl_next(), but hands out the next transaction only if its est_us <= max_us; otherwise returns false
 * and leaves it pending (nothing is consumed). Lets the port engine fit RDM into the gap between DMX frames:
 *
 *     budget = 1e6 / 30 Hz - dmx_frame_us;                      // keep DMX >= 30 Hz
 *     first = true;
 *     while (pm_rdm_ctrl_next_max(&c, now, first ? UINT32_MAX : budget, &t)) { budget -= t.est_us; first = false; }
 *
 * (one transaction per gap is always allowed so large ArtRdm requests are never starved; every discovery and
 * BOOT_CATCH transaction is <= ~8 ms, so discovery alone keeps a 512-slot universe above 30 Hz).
 */
bool pm_rdm_ctrl_next_max(pm_rdm_ctrl_t *c, uint32_t now_ms, uint32_t max_us, pm_rdm_txn_t *out);

/** Look up a UID in the TOD. `control_field` (may be NULL) receives its DISC_MUTE control field
 *  (PM_RDM_MUTE_* bits, e.g. BOOT_LOADER for a tube without valid app). */
bool pm_rdm_ctrl_tod_find(const pm_rdm_ctrl_t *c, const pm_uid_t *uid, uint16_t *control_field);

void pm_rdm_ctrl_get_stats(const pm_rdm_ctrl_t *c, pm_rdm_ctrl_stats_t *out);

/** BOOT_CATCH (contract §6.4): GET BOOT_INFO to `target` every PM_BOOT_CATCH_PERIOD_MS for
 *  PM_BOOT_CATCH_DURATION_MS. Replaces a running catch. PM_ERR_STATE if RDM is disabled on this port,
 *  PM_ERR_INVALID_ARG for broadcast/vendorcast UIDs. */
pm_err_t pm_rdm_ctrl_boot_catch_start(pm_rdm_ctrl_t *c, const pm_uid_t *target, uint32_t now_ms);
void pm_rdm_ctrl_boot_catch_abort(pm_rdm_ctrl_t *c);
void pm_rdm_ctrl_boot_catch_status(const pm_rdm_ctrl_t *c, uint32_t now_ms, pm_rdm_boot_catch_t *out);

#ifdef __cplusplus
}
#endif
