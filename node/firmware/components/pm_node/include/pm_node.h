/*
 * pm_node.h - platform-independent glue of the Pixelmapper Node (pure C11, host-testable).
 *
 * Owns the Art-Net node (pm_artnet) and the node's RDM responder (pm_rdm_responder) and wires them to each other,
 * to the settings store, the port engine, the network and the front panel - all reached through pm_node_ops_t.
 * main.c is the ESP-IDF adapter (pm_config / pm_port / pm_net / pm_ui / pm_ota); a host program can drive the same
 * logic with fakes (virtual node: RAM config, simulated RS-485 bus, RAM firmware sink).
 *
 * Threading: NOT thread-safe, like pm_artnet and the responder. Every pm_node_* call and every ops callback runs in
 * ONE task (the network task on the target). Work from other tasks (settings listener, panel requests, port events)
 * is handed over by the platform (main.c: pm_net_post, coalesced settings masks, pm_port event queue).
 *
 * What it does (protocol/contract.md §5-§7):
 *   - RDM routing: request to the node UID -> responder only (never on the line); to another UID -> DMX line of the
 *     port; broadcast / manufacturer broadcast that includes the node -> both. Same for Art-Net (ArtRdm) and the
 *     front panel. Panel requests carry PM_NODE_UI_TOKEN_FLAG (bit 31) on the line, pm_artnet tokens never do
 *     (PM_ARTNET_RDM_TOKEN_MASK); responses are dispatched on that bit and the panel gets its token back unchanged.
 *   - Port engine events -> pm_artnet (RDM response, TOD changed, input frame, input signal), responder
 *     (BOOT_CATCH -> pm_rdm_responder_boot_catch_event) and panel events.
 *   - Settings: both protocol stacks write through ops->config_modify (atomic read-modify-write of exactly the
 *     fields of their change mask, see pm_node_apply_masked), so a change from the panel that is not yet
 *     distributed is never overwritten with a stale copy. pm_node_settings_changed() fans a change out to the port
 *     engine, the network (IP), pm_artnet, the responder (queued messages) and the panel. IP changes made via RDM
 *     are only stored (E1.37-2: active with INTERFACE_APPLY_CONFIGURATION), ArtIpProg applies after its reply.
 *   - Identify: ArtAddress LED Locate/Normal <-> RDM IDENTIFY_DEVICE <-> panel, without feedback loops.
 *   - Tube updates seen in forwarded ArtRdm (ENTER_BOOTLOADER, BOOT_WRITE_PAGE, BOOT_FINISH; app_end from the
 *     BOOT_INFO response) -> PM_NODE_UI_TUBE_UPDATE percent per port, -1 at the end or after a timeout.
 *   - Live port state for the panel (pm_artnet_port_live) published every PM_NODE_LIVE_PERIOD_MS via
 *     ops->port_live; the platform keeps a thread-safe copy.
 */
#pragma once

#include "pm_artnet.h"
#include "pm_fw.h"
#include "pm_rdm.h"
#include "pm_rdm_responder.h"
#include "pm_settings.h"
#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_NODE_UI_TOKEN_FLAG 0x80000000u       /**< bit 31 of a line token = front-panel request */
#define PM_NODE_LIVE_PERIOD_MS 100u             /**< ops->port_live period */
#define PM_NODE_TUBE_UPDATE_TIMEOUT_MS 10000u   /**< no tube bootloader request for this long -> -1 */
#define PM_NODE_TUBE_APP_END_DEFAULT 0x1800u    /**< tube app end when no BOOT_INFO response was seen */
#define PM_NODE_BUSY_MS 50u                     /**< pm_node_busy() after the last request put on a line */

/* ------------------------------------------------------------------------------------------------ port engine */

typedef enum {
    PM_NODE_PORT_OFF = 0,
    PM_NODE_PORT_OUTPUT,
    PM_NODE_PORT_INPUT,
} pm_node_port_mode_t;

/** Line configuration of one port (mirror of pm_port_cfg_t without ESP-IDF types). */
typedef struct {
    pm_node_port_mode_t mode;
    bool rdm_enabled;
    bool rdm_bg_discovery;
    uint32_t rdm_bg_interval_ms;
    uint16_t break_us;
    uint16_t mab_us;
    uint8_t rate_hz;
    bool continuous;
    uint16_t mbs_us;
} pm_node_port_cfg_t;

/** Derive the line configuration of port `i` from the settings. */
void pm_node_port_cfg_from(const pm_settings_t *s, pm_port_idx_t i, pm_node_port_cfg_t *out);

/** Settings bits that need ops->port_configure for port `i`. */
#define PM_NODE_PORT_CFG_BITS(i)                                                                                 \
    (PM_CHG_PORT(PM_CHG_P_ENABLED | PM_CHG_P_DIRECTION | PM_CHG_P_RDM | PM_CHG_P_OUTPUT | PM_CHG_P_TIMING, (i)) | \
     PM_CHG_RDM_INTERVAL)

typedef enum {
    PM_NODE_PEV_RDM_RESPONSE,   /**< token + data (wire bytes incl. start code) */
    PM_NODE_PEV_TOD_CHANGED,
    PM_NODE_PEV_INPUT_FRAME,    /**< new input frame: read with ops->port_input_read */
    PM_NODE_PEV_INPUT_SIGNAL,   /**< value = 1/0 */
    PM_NODE_PEV_BOOT_CATCH,     /**< value = pm_boot_catch_state_t */
    PM_NODE_PEV_ERROR,          /**< value = platform error code (shown on the panel) */
} pm_node_port_ev_type_t;

typedef struct {
    pm_node_port_ev_type_t type;
    pm_port_idx_t port;
    uint32_t token;
    int32_t value;
    uint16_t len;
    const uint8_t *data;
} pm_node_port_event_t;

/* ------------------------------------------------------------------------------------------------ front panel */

/** Events for the front panel (same meaning and order as pm_ui_event_type_t). */
typedef enum {
    PM_NODE_UI_SETTINGS_CHANGED,  /**< value = pm_change_source_t */
    PM_NODE_UI_LED_MODE,          /**< value = PM_AC_LED_* (ArtAddress) */
    PM_NODE_UI_IDENTIFY,          /**< value = 1/0 (changed by Art-Net or RDM, not sent for the panel's own) */
    PM_NODE_UI_TOD_CHANGED,       /**< port */
    PM_NODE_UI_RDM_RESPONSE,      /**< token (as the panel sent it) + data (wire bytes) */
    PM_NODE_UI_FW_PROGRESS,       /**< value = percent, -1 = failed */
    PM_NODE_UI_BOOT_CATCH,        /**< port + value = pm_boot_catch_state_t */
    PM_NODE_UI_ERROR,             /**< port + value = code */
    PM_NODE_UI_TUBE_UPDATE,       /**< port + value = percent, -1 = finished/aborted */
} pm_node_ui_ev_type_t;

typedef struct {
    pm_node_ui_ev_type_t type;
    pm_port_idx_t port;
    int32_t value;
    uint32_t token;
    uint16_t len;
    const uint8_t *data;          /**< valid during the callback only */
} pm_node_ui_event_t;

/* ------------------------------------------------------------------------------------------------ platform ops */

/** Change callback for ops->config_modify (signature of pm_config_modify_fn). */
typedef void (*pm_node_modify_fn)(void *fctx, pm_settings_t *s);

typedef struct {
    void *ctx;

    uint32_t (*now_ms)(void *ctx);
    uint32_t (*random)(void *ctx);

    /* settings store (pm_config) */
    void (*config_get)(void *ctx, pm_settings_t *out);
    /** Atomic read-modify-write: call fn(fctx, &copy_of_current) under the store's lock, then apply/persist/notify
     *  like an update from `src`. The platform calls pm_node_settings_changed() later (from the node's task). */
    pm_err_t (*config_modify)(void *ctx, pm_node_modify_fn fn, void *fctx, pm_change_source_t src);
    pm_err_t (*config_factory_reset)(void *ctx, pm_change_source_t src);

    /* network */
    pm_err_t (*send_udp)(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len);
    void (*net_info)(void *ctx, pm_artnet_net_info_t *out);
    void (*net_apply_ip)(void *ctx, const pm_ip_settings_t *ip);
    void (*net_renew_dhcp)(void *ctx);

    /* port engine */
    pm_err_t (*port_configure)(void *ctx, pm_port_idx_t port, const pm_node_port_cfg_t *cfg);
    void (*port_output)(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer);
    void (*port_commit)(void *ctx);
    void (*port_clear)(void *ctx, pm_port_idx_t port);
    void (*port_status)(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out);
    uint16_t (*port_input_read)(void *ctx, pm_port_idx_t port, uint8_t *out, uint16_t cap);
    /** Queue an RDM request (wire bytes incl. start code). PM_ERR_FULL = queue full, PM_ERR_STATE = no RDM. */
    pm_err_t (*port_rdm_submit)(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *wire, uint16_t len);
    uint8_t (*port_rdm_queue_free)(void *ctx, pm_port_idx_t port);
    /** Optional: RDM queue size of a port (ArtRdm FifoMax). NULL = PM_RDM_QUEUE_LEN. */
    uint8_t (*port_rdm_queue_len)(void *ctx, pm_port_idx_t port);
    uint16_t (*tod_get)(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap);
    void (*tod_control)(void *ctx, pm_port_idx_t port, uint8_t command);
    pm_err_t (*boot_catch_start)(void *ctx, pm_port_idx_t port, const pm_uid_t *target);
    void (*boot_catch_abort)(void *ctx);
    void (*boot_catch_status)(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out);

    /* system */
    /** RESET_DEVICE (after the response left; 0x01 warm, 0xFF cold). */
    void (*reset)(void *ctx, uint8_t kind);
    /** Front-panel event (may be NULL). */
    void (*ui_event)(void *ctx, const pm_node_ui_event_t *ev);
    /** Live port state, every PM_NODE_LIVE_PERIOD_MS (may be NULL). `live` has PM_NUM_PORTS entries. */
    void (*port_live)(void *ctx, const pm_artnet_port_live_t *live);
    /** Firmware sink for ArtFirmwareMaster and FW_* (NULL = no updates). */
    const pm_fw_sink_t *fw_sink;
} pm_node_ops_t;

typedef struct {
    pm_artnet_identity_t artnet;            /**< uid + num_ports are used by pm_node too */
    pm_rdm_responder_identity_t responder;  /**< uid must equal artnet.uid */
} pm_node_identity_t;

/* ------------------------------------------------------------------------------------------------ node object */

typedef struct pm_node pm_node_t;

/** Bytes for pm_node_init (node + Art-Net node + responder, one block, alignment of max_align_t). */
size_t pm_node_size(void);

/** Initialise `mem` (pm_node_size() bytes, aligned to max_align_t). `settings` = current settings (copied by
 *  pm_artnet). Does not configure the ports (use pm_node_port_cfg_from at start). NULL on bad arguments. */
pm_node_t *pm_node_init(void *mem, const pm_node_identity_t *id, const pm_node_ops_t *ops,
                        const pm_settings_t *settings);

pm_artnet_node_t *pm_node_artnet(pm_node_t *n);
pm_rdm_responder_t *pm_node_responder(pm_node_t *n);

/** One received UDP datagram on 6454 (dst_ip = destination address, 0 = unknown). */
void pm_node_handle_packet(pm_node_t *n, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, const uint8_t *data,
                           size_t len);

/** Periodic work, every 1-20 ms: pm_artnet_tick, pm_rdm_responder_poll, tube update timeouts, live snapshot. */
void pm_node_tick(pm_node_t *n);

/** One event of the port engine. */
void pm_node_port_event(pm_node_t *n, const pm_node_port_event_t *ev);

/** Settings changed (any source; the platform coalesces and calls this from the node's task, also for changes
 *  the node made itself). Reads the current settings via ops->config_get. */
void pm_node_settings_changed(pm_node_t *n, pm_change_mask_t mask, pm_change_source_t src);

/** Link/address changed (unsolicited ArtPollReply from the new address). */
void pm_node_net_changed(pm_node_t *n);

/** Front-panel RDM request (wire bytes incl. start code). `token` must have bit 31 clear (pm_ui uses
 *  PM_UI_RDM_TOKEN_TAG 0x5549xxxx); the response arrives as PM_NODE_UI_RDM_RESPONSE with the same token. A SET to an
 *  attached device is recorded as LOCAL_RDM_SET (contract §7). */
pm_err_t pm_node_ui_rdm_request(pm_node_t *n, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len);

/** Identify toggled on the front panel -> RDM IDENTIFY_DEVICE state + ArtPollReply indicator. */
void pm_node_ui_identify(pm_node_t *n, bool on);

/** Current identify state of the node. */
bool pm_node_identify(const pm_node_t *n);

/** Firmware update progress (pm_ota listener, node's task): panel progress + NodeReport on failure. */
void pm_node_fw_progress(pm_node_t *n, const pm_fw_status_t *st);

/** True while RDM requests were put on a line within PM_NODE_BUSY_MS: the platform should drain port events
 *  more often (responses come back faster than the normal tick). */
bool pm_node_busy(const pm_node_t *n);

/**
 * Copy the fields selected by `mask` (PM_CHG_* bits, per-port bits per port) from `src` to `dst`. This is what
 * both protocol stacks' settings writes do inside ops->config_modify: only the fields they changed are written.
 */
void pm_node_apply_masked(pm_settings_t *dst, const pm_settings_t *src, pm_change_mask_t mask);

#ifdef __cplusplus
}
#endif
