/*
 * pm_rdm_responder.h - the node's own RDM responder (pure C11, host-testable).
 *
 * Reached via ArtRdm to the node UID (never on the DMX line). Answers the standard PIDs of
 * pids.toml [node.standard_pids] and all node manufacturer PIDs of pm_pids.h incl. PARAMETER_DESCRIPTION
 * (PM_NODE_PID_DESC_LIST), keeps the queued-message/status-message queue, and implements the request side of
 * BOOT_CATCH and FW_* (contract §6.3, §6.4, §9.2).
 *
 * Rules (rdm-digest.md §7, OLA responder test): required PIDs are not in SUPPORTED_PARAMETERS; unknown PID ->
 * NACK UNKNOWN_PID; wrong CC -> UNSUPPORTED_COMMAND_CLASS; wrong PDL -> FORMAT_ERROR; out of range ->
 * DATA_OUT_OF_RANGE; sub-device != 0 -> SUB_DEVICE_OUT_OF_RANGE (0xFFFF with GET too); broadcast/vendorcast SET is
 * executed but never answered; GET to broadcast is ignored.
 *
 * Threading: not thread-safe, all calls from the network task (core 0).
 */
#pragma once

#include "pm_fw.h"
#include "pm_rdm.h"
#include "pm_rdm_ctrl.h"
#include "pm_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_RDM_RESP_QUEUE_LEN 16u   /**< queued messages (changed PIDs) */
#define PM_RDM_STATUS_QUEUE_LEN 8u  /**< status messages */

/** Network state for E1.37-2 GETs. */
typedef struct {
    uint32_t ip, mask, gateway;     /**< host order, current */
    bool dhcp_active;
    uint8_t mac[6];
} pm_rdm_net_info_t;

typedef struct {
    void *ctx;
    uint32_t (*now_ms)(void *ctx);
    /** Current settings (copy). */
    void (*settings_get)(void *ctx, pm_settings_t *out);
    /** Persist + distribute a change (source PM_SRC_RDM). The platform calls pm_rdm_responder_settings_changed()
     *  for every change from any source, which creates the queued messages. */
    pm_err_t (*settings_apply)(void *ctx, const pm_settings_t *s, pm_change_mask_t mask);
    void (*net_info)(void *ctx, pm_rdm_net_info_t *out);
    /** E1.37-2 INTERFACE_APPLY_CONFIGURATION / INTERFACE_RENEW_DHCP. */
    void (*net_apply)(void *ctx);
    void (*net_renew_dhcp)(void *ctx);
    /** IDENTIFY_DEVICE on/off (same as ArtAddress LED Locate/Normal). */
    void (*identify)(void *ctx, bool on);
    /** RESET_DEVICE: 0x01 warm, 0xFF cold (after the response has been sent). */
    void (*reset)(void *ctx, uint8_t kind);
    /** FACTORY_DEFAULTS SET. */
    void (*factory_defaults)(void *ctx);
    /** BOOT_CATCH on a port (forwarded to pm_port / pm_rdm_ctrl on core 1). port 0-based. */
    pm_err_t (*boot_catch_start)(void *ctx, pm_port_idx_t port, const pm_uid_t *target);
    void (*boot_catch_abort)(void *ctx);
    /** Status of the last/current catch (any port). */
    void (*boot_catch_status)(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out);
    /** Firmware sink for FW_* PIDs. */
    const pm_fw_sink_t *fw_sink;
} pm_rdm_responder_ops_t;

/** Static identity. */
typedef struct {
    pm_uid_t uid;
    uint16_t model_id;              /**< PM_MODEL_NODE */
    uint32_t sw_version_id;         /**< 0x00MMmmpp */
    const char *sw_version_label;   /**< "0.1.0" */
    uint32_t boot_sw_version_id;    /**< ESP-IDF bootloader version (or 0) */
    const char *boot_sw_version_label;
    const char *model_description;  /**< "Pixelmapper Node" */
    const char *manufacturer_label; /**< "Prototype" (until an ESTA ID exists) */
    uint8_t hw_revision;
} pm_rdm_responder_identity_t;

typedef struct pm_rdm_responder pm_rdm_responder_t;

size_t pm_rdm_responder_size(void);
pm_rdm_responder_t *pm_rdm_responder_init(void *mem, const pm_rdm_responder_identity_t *id,
                                          const pm_rdm_responder_ops_t *ops);

/**
 * Handle one request (wire bytes incl. start code). Writes the response (wire bytes) to `out`.
 * Returns the response length, 0 = no response (broadcast, not addressed to us), negative pm_err_t on a
 * malformed request (also no response).
 */
int pm_rdm_responder_handle(pm_rdm_responder_t *r, const uint8_t *req, size_t len, uint8_t *out, size_t cap);

/** A setting changed (any source): queue the GET responses of the affected PIDs that are not visible in
 *  ArtPollReply (contract §6.3 "Änderungsmeldungen per RDM"). */
void pm_rdm_responder_settings_changed(pm_rdm_responder_t *r, pm_change_mask_t mask, pm_change_source_t src);

/** Queue the GET response of `pid` as a queued message (e.g. PM_PID_BOOT_CATCH_STATUS, PM_PID_LOCAL_RDM_SET). */
void pm_rdm_responder_queue_pid(pm_rdm_responder_t *r, uint16_t pid);

/** Add a status message (STATUS_MESSAGES): status_id from pm_pids.h PM_STATUS_*, type PM_RDM_STATUS_*. */
void pm_rdm_responder_status(pm_rdm_responder_t *r, uint16_t status_id, uint8_t type, int16_t data1, int16_t data2);

/** Record an RDM SET that the front panel sent to an attached device (contract §7) -> LOCAL_RDM_SET. */
void pm_rdm_responder_local_set(pm_rdm_responder_t *r, pm_port_idx_t port, const pm_uid_t *uid,
                                uint16_t sub_device, uint16_t pid);

/* ------------------------------------------------------------------------------------------------ Phase 1 additions */

/** FW_* session timeout: without FW_WRITE for this long the session is aborted (PM_FW_ERROR_TIMEOUT). */
#define PM_RDM_RESP_FW_TIMEOUT_MS 30000u
/** Delay between the ACK of FW_FINISH and fw_sink->reboot(). */
#define PM_RDM_RESP_REBOOT_DELAY_MS 500u
/** Delay between the ACK of RESET_DEVICE and ops->reset() (the response must leave the node first). */
#define PM_RDM_RESP_RESET_DELAY_MS 100u
/** E1.37-2 interface ID of the single Ethernet interface. */
#define PM_RDM_RESP_INTERFACE_ID 1u
/** E1.37-2 NR_ACTION_NOT_SUPPORTED (rdm-digest.md §4.2 [ungeprüft]); INTERFACE_RENEW_DHCP while DHCP is inactive. */
#define PM_RDM_RESP_NR_ACTION_NOT_SUPPORTED 0x000Bu

/**
 * Periodic work (call every few ms from the network task): runs RESET_DEVICE and the post-FW_FINISH reboot after
 * their delays and aborts an FW_* session after PM_RDM_RESP_FW_TIMEOUT_MS without FW_WRITE. Uses ops->now_ms.
 */
void pm_rdm_responder_poll(pm_rdm_responder_t *r);

/**
 * BOOT_CATCH state change on `port` (0-based; wire it to pm_rdm_ctrl_cb_t.on_boot_catch). Queues the GET response
 * of BOOT_CATCH_STATUS; on CAUGHT / TIMEOUT also adds status PM_STATUS_BOOT_CATCH_CAUGHT (advisory) /
 * PM_STATUS_BOOT_CATCH_TIMEOUT (warning) with data1 = port 1-based.
 */
void pm_rdm_responder_boot_catch_event(pm_rdm_responder_t *r, pm_port_idx_t port, uint8_t state);

/**
 * Message Count as sent in every response: queued PIDs + pending status messages, capped at 255. Status messages
 * count because they are fetched through QUEUED_MESSAGE too (when no queued PID is left, QUEUED_MESSAGE returns the
 * STATUS_MESSAGES response), so a controller polling until the count is 0 also collects them.
 */
uint8_t pm_rdm_responder_message_count(const pm_rdm_responder_t *r);

/* ------------------------------------------------------------------------------------------------ Phase 2 additions */

/**
 * Identify state changed elsewhere (ArtAddress LED Locate/Normal, front panel): set the IDENTIFY_DEVICE state
 * without calling ops->identify (no loop). A real change queues IDENTIFY_DEVICE as a queued message.
 */
void pm_rdm_responder_set_identify(pm_rdm_responder_t *r, bool on);

/** Current IDENTIFY_DEVICE state. */
bool pm_rdm_responder_identify(const pm_rdm_responder_t *r);

#ifdef __cplusplus
}
#endif
