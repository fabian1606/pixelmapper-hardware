/*
 * pm_port_line.h - line state machine of one DMX/RDM port (pure C11, host-testable, no ESP-IDF).
 *
 * The line FSM owns everything that happens on the wire of one port: DMX frames (break, MAB, slots), RDM
 * transactions inserted between DMX frames (turnaround, response window, DUB collection, backoffs) and the DMX
 * receiver (break detection, slot counting). It never touches hardware itself: the platform layer (pm_port.c on
 * the ESP32-P4, a simulator in test_host/) feeds it events and executes its requests through pm_port_line_ops_t.
 *
 *   events in : pm_port_line_timer()    deadline reached (pm_port_line_deadline())
 *               pm_port_line_tx_done()  last stop bit of the bytes handed to ops.tx_start() has left the UART
 *               pm_port_line_rx_bytes() received bytes (bytes with framing errors are NOT passed - discarded by HW)
 *               pm_port_line_rx_error() framing/parity error or FIFO overflow seen
 *               pm_port_line_rx_break() break detected; all bytes received before it were passed already
 *   actions   : ops.set_de / set_break / tx_start / rx_flush, ops.dmx_frame (fetch frame to send),
 *               ops.rdm_done (result of an offered RDM transaction ready), ops.input_frame (received DMX frame)
 *
 * Time is a free-running 32-bit microsecond counter (wraps after ~71 min; all comparisons are wrap-safe).
 * Timing values: protocol/rdm-digest.md §2 (E1.20 tables via Ja Rule), contract §6.
 *
 * Threading: not thread-safe. On the target all calls happen on core 1 inside one critical section (ISRs and the
 * port task share a spinlock), so every function is short and IRAM-placed (PM_PORT_IRAM).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pm_rdm_ctrl.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define PM_PORT_IRAM IRAM_ATTR
#else
#define PM_PORT_IRAM
#endif

/* ------------------------------------------------------------------------------------------------ constants */

#define PM_PORT_DMX_BAUD 250000u
#define PM_PORT_US_PER_SLOT 44u                /**< 1 start + 8 data + 2 stop bits at 4 us */
#define PM_PORT_FRAME_MAX 513u                 /**< start code + 512 slots */

#define PM_PORT_BREAK_MIN_US 176u              /**< contract §6.3 PORTn_DMX_BREAK range = E1.20 controller break */
#define PM_PORT_BREAK_MAX_US 352u
#define PM_PORT_BREAK_DEFAULT_US 176u
#define PM_PORT_MAB_MIN_US 12u
#define PM_PORT_MAB_MAX_US 88u
#define PM_PORT_MAB_DEFAULT_US 16u
#define PM_PORT_RATE_MIN_HZ 1u
#define PM_PORT_RATE_MAX_HZ 44u
#define PM_PORT_RATE_DEFAULT_HZ 40u

#define PM_PORT_RDM_RESPONSE_TIMEOUT_US 2800u  /**< end of request -> start of response (digest §2) */
#define PM_PORT_RDM_INTERSLOT_TIMEOUT_US 2100u /**< max gap between two bytes of a response */
#define PM_PORT_RDM_DUB_WINDOW_US 2900u        /**< end of DUB -> first byte of the DUB response */
#define PM_PORT_RDM_DUB_IDLE_END_US 200u       /**< DUB response is over after this silence (> 4 slots) */
#define PM_PORT_RDM_DUB_BACKOFF_US 5800u       /**< end of DUB -> next packet */
#define PM_PORT_RDM_NORESP_BACKOFF_US 3000u    /**< end of request without response -> next packet */
#define PM_PORT_RDM_GAP_US 176u                /**< after response / broadcast / DMX frame before the next packet */
#define PM_PORT_RDM_EXPECT_RESP_SLOTS 64u      /**< response size assumed for scheduling (not a limit) */
#define PM_PORT_DUB_MAX_BYTES 32u              /**< 7 preamble + separator + 16 EUID/ECS, with slack */
#define PM_PORT_RX_LATENCY_US 100u             /**< byte on the wire -> rx_bytes() (UART RX FIFO threshold 1 +
                                                    ISR); added to RDM receive deadlines */

#define PM_PORT_DMX_FLOOR_HZ 30u               /**< DMX rate never drops below this for RDM (docs/plan.md) */
#define PM_PORT_RDM_STARVE_FRAMES 4u           /**< ...except: an RDM transaction is forced after this many frames */
#define PM_PORT_DELTA_REFRESH_US 800000u       /**< delta (non-continuous) output: refresh unchanged data */
#define PM_PORT_DELTA_POLL_US 1000u            /**< delta output: check for new data this often */

/** Wrap-safe "a is at or after b". (always_inline: used from IRAM code) */
__attribute__((always_inline)) static inline bool pm_port_t_ge(uint32_t a, uint32_t b) { return (int32_t)(a - b) >= 0; }

/* ------------------------------------------------------------------------------------------------ scheduler */

typedef enum {
    PM_PORT_SCHED_WAIT = 0, /**< nothing now; re-decide at *wake_us */
    PM_PORT_SCHED_DMX,      /**< start a DMX frame now */
    PM_PORT_SCHED_RDM,      /**< start the pending RDM transaction now */
} pm_port_sched_t;

/** Inputs of the frame scheduler (one decision point on an idle output line). */
typedef struct {
    uint32_t now;
    bool dmx_started;           /**< at least one DMX frame was sent since the port was configured */
    uint32_t last_dmx_start;    /**< start (break) of the last DMX frame */
    uint32_t period_us;         /**< 1e6 / rate */
    bool continuous;            /**< false = delta: send only new data (+ refresh) */
    bool dmx_fresh;             /**< new output data was published since the last frame */
    bool rdm_pending;
    uint32_t rdm_est_us;        /**< expected line time of the pending RDM transaction incl. backoff */
    uint8_t frames_since_rdm;   /**< DMX frames sent while an RDM transaction was waiting */
} pm_port_sched_in_t;

/**
 * Decide what to put on the line. Policy: DMX at its rate; an RDM transaction goes in front of the next DMX
 * frame only when it ends before the 30 Hz floor deadline (last_dmx_start + max(period, 1/30 s)), so DMX stays
 * >= 30 Hz while RDM runs; after PM_PORT_RDM_STARVE_FRAMES frames without a fit it is forced once.
 */
pm_port_sched_t pm_port_sched_decide(const pm_port_sched_in_t *in, uint32_t *wake_us);

/** Expected line time of an RDM transaction (break + MAB + request + response window/backoff). */
uint32_t pm_port_rdm_estimate_us(const pm_rdm_txn_t *t, uint16_t break_us, uint16_t mab_us, uint16_t mbs_us);

#define PM_PORT_RDM_GATE_STARVE_MS 133u        /**< discovery step not handed out for this long (4 frames at the
                                                    30 Hz floor): hand out the next one without a budget */

/**
 * RDM line time that fits into one gap between two DMX frames of `frame_len` bytes (start code + slots) without
 * the DMX rate dropping below PM_PORT_DMX_FLOOR_HZ (or below the configured rate if that is lower):
 * max(period, 1/30 s) - (break + MAB + frame_len * (44 us + MBS) + PM_PORT_RDM_GAP_US). 0 if nothing fits.
 */
uint32_t pm_port_rdm_budget_us(uint16_t break_us, uint16_t mab_us, uint16_t mbs_us, uint8_t rate_hz,
                               uint16_t frame_len);

/**
 * max_us for pm_rdm_ctrl_next_max() (docs/plan.md: "Die DMX-Rate bleibt über ca. 30 Hz"):
 *  - a forwarded request is waiting (ArtRdm, panel): UINT32_MAX - requests are never held back by the node; the
 *    line scheduler fits them into a gap or forces them after PM_PORT_RDM_STARVE_FRAMES frames.
 *  - otherwise (discovery, presence check, BOOT_CATCH): `budget_us`, so these steps only go out when they fit
 *    next to the DMX frame; after PM_PORT_RDM_GATE_STARVE_MS without a hand-out (budget too small for any step,
 *    e.g. long MBS) UINT32_MAX, so discovery still progresses (slowly, with the line's forced slot).
 */
uint32_t pm_port_rdm_max_us(uint32_t budget_us, bool request_queued, uint32_t ms_since_handout);

/* ------------------------------------------------------------------------------------------------ line FSM */

typedef enum {
    PM_PORT_LINE_MODE_OFF = 0,
    PM_PORT_LINE_MODE_OUTPUT,   /**< DMX out + RDM transactions */
    PM_PORT_LINE_MODE_INPUT,    /**< DMX in */
} pm_port_line_mode_t;

typedef enum {
    PM_PORT_LINE_OFF = 0,
    PM_PORT_LINE_GAP,           /**< output idle (mark, DE on), waiting for the scheduler */
    PM_PORT_LINE_BREAK,
    PM_PORT_LINE_MAB,
    PM_PORT_LINE_TX,            /**< bytes in the UART */
    PM_PORT_LINE_RDM_WAIT,      /**< DE off, waiting for response break (request) or first byte (DUB) */
    PM_PORT_LINE_RDM_RX,        /**< receiving response bytes */
    PM_PORT_LINE_BACKOFF,       /**< mandatory pause before the next packet */
    PM_PORT_LINE_INPUT,         /**< DMX receiver */
} pm_port_line_state_t;

typedef struct {
    void *ctx;
    void (*set_de)(void *ctx, bool on);          /**< RS-485 driver enable (+ /RE) */
    void (*set_break)(void *ctx, bool on);       /**< hold TX low (break) */
    void (*tx_start)(void *ctx, const uint8_t *data, uint16_t len); /**< send bytes; tx_done() at the end */
    /** Drop everything in the RX FIFO, clear error flags. per_byte = deliver every byte at once (RDM response
     *  window, low latency); false = bulk delivery (DMX input). */
    void (*rx_start)(void *ctx, bool per_byte);
    void (*tx_abort)(void *ctx);                 /**< stop sending: flush TX FIFO, no tx_done() will follow */
    /** Frame to send now (start code + slots, len 1..513); the buffer must stay valid until tx_done. */
    uint16_t (*dmx_frame)(void *ctx, const uint8_t **frame);
    bool (*dmx_fresh)(void *ctx);                /**< new data published since the last dmx_frame() */
    void (*rdm_done)(void *ctx);                 /**< result of the offered transaction is ready */
    void (*input_frame)(void *ctx, const uint8_t *frame, uint16_t len); /**< received DMX frame, SC 0 */
} pm_port_line_ops_t;

typedef struct {
    uint16_t break_us;
    uint16_t mab_us;
    uint8_t rate_hz;
    bool continuous;
    uint16_t mbs_us;                /**< mark between slots realised by the platform (only used for timeouts) */
} pm_port_line_timing_t;

typedef struct {
    uint32_t frames_out;
    uint32_t frames_in;
    uint32_t frames_in_other_sc;    /**< received frames with a start code != 0 (ignored) */
    uint32_t rx_errors;             /**< framing/parity/overflow events */
    uint32_t rdm_txns;
    uint32_t rdm_timeouts;
    uint32_t rdm_garbage;           /**< collisions / framing errors / incomplete responses */
    uint32_t tx_watchdog;           /**< tx_done never came (driver problem) */
} pm_port_line_stats_t;

typedef struct {
    pm_port_line_ops_t ops;
    pm_port_line_mode_t mode;
    pm_port_line_timing_t timing;
    uint32_t period_us;

    pm_port_line_state_t state;
    bool has_deadline;
    uint32_t deadline;

    bool dmx_started;
    uint32_t last_dmx_start;
    uint16_t last_dmx_len;          /**< bytes of the last DMX frame (start code + slots), for the RDM budget */
    uint8_t frames_since_rdm;
    bool cur_rdm;                   /**< packet in flight is the RDM transaction */
    const uint8_t *cur_data;
    uint16_t cur_len;
    uint32_t tx_end;

    const pm_rdm_txn_t *offer;      /**< pending / in-flight RDM transaction (owned by the caller) */
    bool result_ready;
    pm_rdm_rx_t result;

    uint8_t rx[PM_RDM_MAX_MSG_LEN];
    uint16_t rx_len;
    bool rx_err;

    uint8_t in[PM_PORT_FRAME_MAX];
    uint16_t in_len;
    bool in_sync;                   /**< a break was seen; bytes belong to a frame */
    uint32_t last_in_us;
    bool in_seen;                   /**< a valid input frame arrived since configure */

    pm_port_line_stats_t stats;
} pm_port_line_t;

void pm_port_line_init(pm_port_line_t *l, const pm_port_line_ops_t *ops);

/** Switch mode. Aborts anything in flight (an offered RDM transaction completes with PM_RDM_RX_TIMEOUT). */
void pm_port_line_configure(pm_port_line_t *l, pm_port_line_mode_t mode, const pm_port_line_timing_t *timing,
                            uint32_t now);
/** Change break/MAB/rate/continuous without restarting (takes effect with the next packet). Values are clamped. */
void pm_port_line_set_timing(pm_port_line_t *l, const pm_port_line_timing_t *timing);

/** Hand over an RDM transaction (kept by pointer until rdm_done + pm_port_line_rdm_take()). False if the line is
 *  not an output or still busy with the previous one. */
bool pm_port_line_rdm_offer(pm_port_line_t *l, const pm_rdm_txn_t *t, uint32_t now);
/** True while a transaction is offered, in flight, or its result has not been taken. */
bool pm_port_line_rdm_busy(const pm_port_line_t *l);
/** Take the result (rx->data points into the line, valid until the next offer). */
bool pm_port_line_rdm_take(pm_port_line_t *l, pm_rdm_rx_t *rx);

/** pm_port_rdm_budget_us() for the current timing and the last DMX frame length (513 before the first frame). */
uint32_t pm_port_line_rdm_budget_us(const pm_port_line_t *l);

/** Re-run the scheduler now (call after new output data in delta mode or other external changes). */
void pm_port_line_poke(pm_port_line_t *l, uint32_t now);

/**
 * Input signal present: a valid frame within `lost_us` before `now`. Once lost, the "seen" state is cleared, so the
 * 32-bit microsecond clock cannot revive an old frame when it wraps (every 71.6 min). Call regularly (pm_port: every
 * engine loop) - the wrap protection needs one call within 2^32 - lost_us after the last frame.
 */
bool pm_port_line_input_signal(pm_port_line_t *l, uint32_t now, uint32_t lost_us);

void pm_port_line_timer(pm_port_line_t *l, uint32_t now);
void pm_port_line_tx_done(pm_port_line_t *l, uint32_t now);
void pm_port_line_rx_bytes(pm_port_line_t *l, uint32_t now, const uint8_t *data, uint16_t len);
void pm_port_line_rx_error(pm_port_line_t *l, uint32_t now);
void pm_port_line_rx_break(pm_port_line_t *l, uint32_t now);

/** Next deadline (absolute us). False = no timer needed. */
__attribute__((always_inline)) static inline bool pm_port_line_deadline(const pm_port_line_t *l, uint32_t *t)
{
    *t = l->deadline;
    return l->has_deadline;
}

#ifdef __cplusplus
}
#endif
