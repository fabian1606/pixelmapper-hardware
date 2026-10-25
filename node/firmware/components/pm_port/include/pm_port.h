/*
 * pm_port.h - port engine: 4 DMX/RDM ports on core 1.
 *
 * One FreeRTOS task pinned to core 1 runs, per port, a state machine on a UART in RS-485 half-duplex mode:
 *  - output: continuous DMX (break/MAB/rate per pm_port_cfg_t), double buffer per port, ArtSync commit of several
 *    ports at once; RDM transactions between frames, driven by a pm_rdm_ctrl_t per port (discovery, TOD, queue,
 *    BOOT_CATCH). Timing per E1.11/E1.20 (rdm-digest.md §2): controller break 176-352 us, MAB 12-88 us,
 *    response timeout 2.8 ms, DUB window, gaps after broadcasts.
 *  - input: receive DMX (break detection via UART frame error), double buffer, signal-lost detection.
 * Core 0 (network, UI) talks to it only through this API; all functions here are safe to call from core 0
 * (they use lock-free double buffers or FreeRTOS queues). Results come back as pm_port_event_t.
 *
 * UART driver: own implementation on the UART LL/HAL (esp_dmx is not used, docs/plan.md).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pm_board.h"
#include "pm_rdm_ctrl.h"
#include "pm_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PM_PORT_MODE_OFF = 0,
    PM_PORT_MODE_OUTPUT,       /**< DMX out + RDM controller (if rdm_enabled) */
    PM_PORT_MODE_INPUT,        /**< DMX in */
    PM_PORT_MODE_PIXEL,        /**< placeholder: fast pixel protocol for LED matrices (later; one port, several
                                    universes, UART up to 5 Mbit/s - see MEASURE.md). pm_port_configure()
                                    returns ESP_ERR_NOT_SUPPORTED for now */
} pm_port_mode_t;

/** Line configuration of one port (derived from pm_port_settings_t by main/pm_config). */
typedef struct {
    pm_port_mode_t mode;
    bool rdm_enabled;
    bool rdm_bg_discovery;
    uint32_t rdm_bg_interval_ms;
    uint16_t break_us;
    uint16_t mab_us;
    uint8_t rate_hz;
    bool continuous;            /**< false = delta style: send on change + slow refresh */
    uint16_t mbs_us;            /**< extra mark between slots (inter-slot time), 0 = back to back (default).
                                     Experimental: realised with the UART tx_idle_num in 4-us steps, see
                                     MEASURE.md M5. 0..1000 */
} pm_port_cfg_t;

/** Events from core 1 to core 0 (pm_port_event_receive). */
typedef enum {
    PM_PORT_EV_RDM_RESPONSE,    /**< answer to pm_port_rdm_submit (data = wire bytes) */
    PM_PORT_EV_TOD_CHANGED,
    PM_PORT_EV_INPUT_FRAME,     /**< input port: new/changed frame available (pm_port_input_read) */
    PM_PORT_EV_INPUT_SIGNAL,    /**< input signal appeared/lost (value = 1/0) */
    PM_PORT_EV_BOOT_CATCH,      /**< BOOT_CATCH state changed (value = pm_boot_catch_state_t) */
    PM_PORT_EV_ERROR,           /**< UART/driver problem (value = esp_err_t). value = ESP_ERR_NO_MEM with `token`
                                     set: an accepted pm_port_rdm_submit() request was dropped (queue race) */
} pm_port_event_type_t;

typedef struct {
    pm_port_event_type_t type;
    pm_port_idx_t port;
    uint32_t token;             /**< RDM_RESPONSE */
    int32_t value;
    uint16_t len;               /**< RDM_RESPONSE */
    uint8_t data[PM_RDM_MAX_MSG_LEN];
} pm_port_event_t;

/** Counters for the UI (fps) and ArtDiagData. */
typedef struct {
    uint32_t frames_out;
    uint32_t frames_in;
    uint16_t fps;               /**< measured over the last second */
    uint32_t rdm_requests;
    uint32_t rdm_timeouts;
    uint32_t rdm_collisions;
    uint16_t tod_count;
    bool discovery_running;
    bool input_signal;
    uint32_t rx_errors;         /**< framing/parity/overflow events on the line */
    uint32_t events_dropped;    /**< events lost because the core-0 queue was full (whole engine) */
    /* Phase 2 */
    pm_rdm_ctrl_stats_t rdm;    /**< controller counters (pm_rdm_ctrl_get_stats): forwarded requests/responses/
                                     timeouts, DUBs, collisions, full/background discoveries, TOD overflow, corrupt
                                     runs. rdm_requests/rdm_timeouts/rdm_collisions above count line transactions */
} pm_port_stats_t;

/** Configure UARTs/pins from the board table, create the event queue. Does not start the task. */
esp_err_t pm_port_init(const pm_board_t *board, const pm_uid_t *node_uid);
/** Start the engine task on core 1 (highest application priority). */
esp_err_t pm_port_start(void);

/** Apply a line configuration (mode change flushes the TOD and, for output+RDM, starts a full discovery). */
esp_err_t pm_port_configure(pm_port_idx_t port, const pm_port_cfg_t *cfg);

/** Write the next output frame (slots without start code, 0..512). `defer` = keep until pm_port_commit()
 *  (ArtSync). Copies the data; never blocks. */
void pm_port_output_write(pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer);
/** Make all deferred frames active at the same frame boundary. */
void pm_port_commit(void);
/** Clear the output buffer (ArtAddress AcClearOp) / read back the current output (AcFailRecord, UI). */
void pm_port_output_clear(pm_port_idx_t port);
uint16_t pm_port_output_read(pm_port_idx_t port, uint8_t *out, uint16_t cap);

/** Latest input frame (input mode). Returns the length (0 = none yet). */
uint16_t pm_port_input_read(pm_port_idx_t port, uint8_t *out, uint16_t cap);

/** Queue an RDM request (wire bytes incl. start code). ESP_ERR_NO_MEM when the port queue is full,
 *  ESP_ERR_INVALID_STATE when the port has no RDM. The answer arrives as PM_PORT_EV_RDM_RESPONSE (or never). */
esp_err_t pm_port_rdm_submit(pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len);
/** Free RDM queue slots (ArtRdm FifoAvail). */
uint8_t pm_port_rdm_queue_free(pm_port_idx_t port);

/** ArtTodControl: PM_ATC_FLUSH / PM_ATC_END / PM_ATC_INC_ON / PM_ATC_INC_OFF (pm_artnet_packets.h values). */
void pm_port_tod_control(pm_port_idx_t port, uint8_t command);
/** Copy the TOD (sorted). Returns the total count. */
uint16_t pm_port_tod_get(pm_port_idx_t port, pm_uid_t *out, uint16_t cap);

/** BOOT_CATCH (contract §6.4). Only one catch at a time across all ports. */
esp_err_t pm_port_boot_catch_start(pm_port_idx_t port, const pm_uid_t *target);
void pm_port_boot_catch_abort(void);
void pm_port_boot_catch_status(pm_port_idx_t *port, pm_rdm_boot_catch_t *out);

void pm_port_get_stats(pm_port_idx_t port, pm_port_stats_t *out);

/** Wait for the next event (core 0). Returns false on timeout. */
bool pm_port_event_receive(pm_port_event_t *out, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
