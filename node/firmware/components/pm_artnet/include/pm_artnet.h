/*
 * pm_artnet.h - Art-Net 4 node logic (pure C11, no ESP-IDF, host-testable).
 *
 * Behaviour: protocol/contract.md §2-§6, §9.2; byte layouts: pm_artnet_packets.h.
 *
 * Threading: NOT thread-safe. Everything (packets, tick, events from the port engine) is called from ONE task
 * (pm_net's network task on core 0). Cross-core events are queued by the caller.
 *
 * Model: one Art-Net "page" per DMX port (BindIndex = port index + 1, contract §3). The node object owns:
 *   - ArtPoll handling, ArtPollReply building (4 pages), unsolicited replies to controllers with Flags bit 1
 *   - ArtDmx receive, merge HTP/LTP with max. 2 sources (IP + Physical), source timeout 10 s, data loss -> failsafe
 *     after 4 s, ArtSync (4 s timeout, ignored while merging / from a foreign IP)
 *   - input ports: ArtDmx unicast to subscribers (own ArtPoll every ~3 s) and to controllers that polled within
 *     10 s; on change, keepalive >= 1/s
 *   - ArtAddress / ArtIpProg / ArtInput -> settings changes (ops->settings_apply)
 *   - ArtTodRequest / ArtTodControl / ArtTodData (subscribers = IPs that sent ArtTodRequest), ArtRdm forwarding
 *   - ArtFirmwareMaster session -> pm_fw_sink_t
 *   - ArtDiagData (optional)
 * The platform (time, UDP, settings storage, port engine, RDM, firmware sink) is reached only through
 * pm_artnet_ops_t.
 */
#pragma once

#include "pm_artnet_packets.h"
#include "pm_fw.h"
#include "pm_settings.h"
#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ timing */

#define PM_ARTNET_MERGE_TIMEOUT_MS 10000u  /**< a merge source expires (spec) */
#define PM_ARTNET_DATA_LOSS_MS 4000u       /**< failsafe after this (our definition, contract §4) */
#define PM_ARTNET_SYNC_TIMEOUT_MS 4000u    /**< back to immediate output (spec) */
#define PM_ARTNET_INPUT_KEEPALIVE_MS 1000u /**< input ArtDmx at least this often */
#define PM_ARTNET_POLLER_TIMEOUT_MS 10000u /**< controller counts as "polling" for input data / unsolicited replies */
#define PM_ARTNET_OWN_POLL_MS 3000u        /**< node's own ArtPoll to find input subscribers */
#define PM_ARTNET_REPLY_DELAY_MAX_MS 1000u /**< random ArtPollReply delay (spec) */
#define PM_ARTNET_FIRMWARE_TIMEOUT_MS 30000u
#define PM_ARTNET_CONTROLLER_KEEP_MS 30000u /**< a poller with Flags bit 1/2 gets unsolicited replies / diagnostics
                                                 this long after its last ArtPoll (our definition) */
#define PM_ARTNET_ADDRESS_REPLY_WAIT_MS 200u /**< reply to ArtAddress/ArtInput waits this long at most for the
                                                  platform's pm_artnet_settings_changed() */
#define PM_ARTNET_REBOOT_DELAY_MS 500u     /**< FirmAllGood -> fw_sink->reboot() */
#define PM_ARTNET_RDM_TIMEOUT_MS 10000u    /**< forget a forwarded ArtRdm request without answer */
#define PM_ARTNET_SUBSCRIBER_TIMEOUT_MS 10000u /**< input subscriber (output node) not seen in own polls */

#define PM_ARTNET_MAX_CONTROLLERS 16u      /**< remembered pollers (reply-on-change, input targets) */
#define PM_ARTNET_MAX_SUBSCRIBERS 32u      /**< remembered output nodes per input port / ArtTodData subscribers */
#define PM_ARTNET_MAX_RDM_PENDING 32u      /**< forwarded ArtRdm requests waiting for an answer */
#define PM_ARTNET_TOD_MAX 512u             /**< UIDs per port that are reported in ArtTodData */
#define PM_ARTNET_RX_ACTIVE_MS 2000u       /**< pm_artnet_port_live().rx_active: ArtDmx within this time */
/** Tokens of pm_artnet_rdm_request_t never have bits outside this mask (bit 31 is free for the platform, e.g. to
 *  tag its own front-panel requests on the same port queue). */
#define PM_ARTNET_RDM_TOKEN_MASK 0x7FFFFFFFu

/* ------------------------------------------------------------------------------------------------ platform ops */

/** Live state of the network interface, filled by ops->net_info. */
typedef struct {
    uint32_t ip;          /**< host order */
    uint32_t mask;
    uint32_t gateway;
    uint8_t mac[6];
    bool dhcp_active;     /**< address came from DHCP (Status2 bit 1) */
    bool link_up;
} pm_artnet_net_info_t;

/** Live state of one DMX port (from the port engine), filled by ops->port_status. */
typedef struct {
    bool output_active;   /**< DMX is being sent (GoodOutputA bit 7) */
    bool input_signal;    /**< DMX received recently (GoodInput bit 7) */
    bool rdm_discovery_running; /**< GoodOutputB bit 5 inverted */
    uint16_t tod_count;
} pm_artnet_port_status_t;

/** RDM request handed to the platform: forwarded to the DMX line of `port` or answered by the node responder
 *  when the destination is the node UID (routing is the platform's job). The answer comes back via
 *  pm_artnet_rdm_response() with the same `token`. */
typedef struct {
    pm_port_idx_t port;
    uint32_t token;              /**< opaque, echo in pm_artnet_rdm_response(); never 0, bit 31 always clear */
    const uint8_t *rdm;          /**< RDM message without start code */
    size_t rdm_len;
} pm_artnet_rdm_request_t;

typedef struct {
    void *ctx;

    /** Monotonic milliseconds. */
    uint32_t (*now_ms)(void *ctx);
    /** Random number for the ArtPollReply delay. */
    uint32_t (*random)(void *ctx);
    /** Send one UDP datagram from port 6454. ip in host order; 0xFFFFFFFF never used (directed broadcast only). */
    pm_err_t (*send_udp)(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len);
    void (*net_info)(void *ctx, pm_artnet_net_info_t *out);

    /** Persist + distribute a settings change made via Art-Net (ArtAddress, ArtInput, ArtIpProg, ArtTodControl).
     *  The platform calls pm_artnet_settings_changed() back (also for its own changes), so the node never assumes
     *  its write succeeded. */
    pm_err_t (*settings_apply)(void *ctx, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src);
    /** Apply IP settings now (ArtIpProg). Called after settings_apply(PM_CHG_IP), the reply is sent first. */
    void (*ip_apply)(void *ctx);

    /** Hand a merged frame to the port engine (double buffer). `defer` = ArtSync mode: keep it until
     *  port_commit(). len 0..512. */
    void (*port_output)(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer);
    /** ArtSync: make all deferred frames visible at the same time. */
    void (*port_commit)(void *ctx);
    void (*port_status)(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out);
    /** ArtAddress AcClearOp / AcFailRecord helpers: clear output buffer, record the current output as scene. */
    void (*port_clear)(void *ctx, pm_port_idx_t port);

    /** Copy the TOD of a port (max `cap` UIDs), return the total count. */
    uint16_t (*tod_get)(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap);
    /** ArtTodControl: PM_ATC_FLUSH (full discovery), PM_ATC_END, PM_ATC_INC_ON/OFF. */
    void (*tod_control)(void *ctx, pm_port_idx_t port, uint8_t command);
    /** Queue an RDM request. PM_ERR_FULL = port queue full (request dropped, no answer). */
    pm_err_t (*rdm_request)(void *ctx, const pm_artnet_rdm_request_t *req);
    /** Free request slots of a port (ArtRdm FifoAvail). */
    uint8_t (*rdm_fifo_avail)(void *ctx, pm_port_idx_t port);
    /** Optional: size of the RDM request queue of a port (ArtRdm FifoMax). NULL = the largest FifoAvail seen.
     *  The platform should set it (pm_port: PM_RDM_QUEUE_LEN) so FifoMax is right from the first ArtRdm. */
    uint8_t (*rdm_fifo_max)(void *ctx, pm_port_idx_t port);

    /** LED/indicator mode changed: PM_AC_LED_NORMAL / MUTE / LOCATE (display off / identify). */
    void (*indicator)(void *ctx, uint8_t ac_led_command);

    /** Firmware sink for ArtFirmwareMaster (NULL = firmware upload not supported). */
    const pm_fw_sink_t *fw_sink;
} pm_artnet_ops_t;

/** Static identity, set once at init. */
typedef struct {
    uint16_t esta_man;           /**< PM_ESTA_MANUFACTURER */
    uint16_t oem;                /**< PM_ARTNET_OEM_NODE */
    uint16_t vers_info;          /**< PM_FW_VERSINFO(fw_version_id) */
    pm_uid_t uid;                /**< node RDM UID -> DefaultRespUID */
    uint8_t num_ports;           /**< <= PM_NUM_PORTS */
} pm_artnet_identity_t;

/* ------------------------------------------------------------------------------------------------ node object */

typedef struct pm_artnet_node pm_artnet_node_t;

/** Size of the node object for static allocation (opaque contents, see pm_artnet_node.c). */
size_t pm_artnet_node_size(void);

/** Initialise `mem` (pm_artnet_node_size() bytes, any alignment of max_align_t) as a node.
 *  `settings` is copied. Returns the node or NULL on bad arguments. */
pm_artnet_node_t *pm_artnet_node_init(void *mem, const pm_artnet_identity_t *id, const pm_artnet_ops_t *ops,
                                      const pm_settings_t *settings);

/** Handle one received UDP datagram (any port 6454 traffic, also our own broadcasts - they are ignored). */
void pm_artnet_handle_packet(pm_artnet_node_t *n, uint32_t src_ip, uint16_t src_port, const uint8_t *data,
                             size_t len);

/** Same with the destination address of the datagram (host order, 0 = unknown). Needed to accept ArtSync only
 *  as broadcast (spec: "Unicast Transmit Not Allowed"); with 0 every ArtSync is treated as broadcast. pm_net should
 *  use this variant (IP_PKTINFO). */
void pm_artnet_handle_packet_ex(pm_artnet_node_t *n, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip,
                                const uint8_t *data, size_t len);

/** Periodic work: delayed poll replies, merge/sync/data-loss timeouts, input keepalive, own polls, firmware
 *  timeout. Call every 5-20 ms. */
void pm_artnet_tick(pm_artnet_node_t *n);

/** Settings changed (from any source, including the node's own settings_apply). Sends unsolicited
 *  ArtPollReplies when the Art-Net view changed. */
void pm_artnet_settings_changed(pm_artnet_node_t *n, const pm_settings_t *s, pm_change_mask_t mask);

/** Network address changed (DHCP lease, fallback, ArtIpProg applied): unsolicited replies from the new IP. */
void pm_artnet_net_changed(pm_artnet_node_t *n);

/** Port status changed (signal lost/found, failsafe active ...): unsolicited reply if visible in ArtPollReply. */
void pm_artnet_port_status_changed(pm_artnet_node_t *n, pm_port_idx_t port);

/** A DMX frame was received on an input port (data = slots without start code). */
void pm_artnet_input_frame(pm_artnet_node_t *n, pm_port_idx_t port, const uint8_t *data, uint16_t len);

/** The TOD of a port changed: send ArtTodData to all TOD subscribers. */
void pm_artnet_tod_changed(pm_artnet_node_t *n, pm_port_idx_t port);

/** Answer for a pm_artnet_rdm_request_t (rdm without start code). Sent as ArtRdm unicast to the requester. */
void pm_artnet_rdm_response(pm_artnet_node_t *n, uint32_t token, const uint8_t *rdm, size_t rdm_len);

/** Diagnostics text (ArtDiagData) to controllers that asked for it, if priority >= their DiagPriority. */
void pm_artnet_diag(pm_artnet_node_t *n, uint8_t priority, const char *text);

/** Node report text for ArtPollReply NodeReport ("#xxxx [cnt] text", the counter is added by the node). */
void pm_artnet_set_node_report(pm_artnet_node_t *n, uint16_t code, const char *text);

/* ------------------------------------------------------------------------------------------------ Phase 2 additions */

/** Indicator state changed elsewhere (RDM IDENTIFY_DEVICE, front panel): PM_AC_LED_NORMAL / MUTE / LOCATE.
 *  Updates ArtPollReply Status1 (unsolicited replies) without calling ops->indicator (no loop). */
void pm_artnet_set_indicator(pm_artnet_node_t *n, uint8_t ac_led_command);

/** Current indicator state (PM_AC_LED_*). */
uint8_t pm_artnet_indicator(const pm_artnet_node_t *n);

/** Live Art-Net state of one output port (UI). All false/0 for input/disabled ports. */
typedef struct {
    bool rx_active;        /**< ArtDmx accepted within PM_ARTNET_RX_ACTIVE_MS */
    uint8_t rx_fps;        /**< accepted ArtDmx frames per second (last full 1-s window), 0 when !rx_active */
    uint8_t merge_sources; /**< 0..2 active merge sources */
    bool merging;          /**< two sources (GoodOutputA bit 3) */
    bool data_loss;        /**< no ArtDmx for PM_ARTNET_DATA_LOSS_MS, failsafe applied */
    bool synced;           /**< ArtSync mode active */
} pm_artnet_port_live_t;

/** Copy the live state of `port`. Not thread-safe like everything here: call from the node's task and hand a copy
 *  to other tasks. Values are as of the last call into the node (tick). */
void pm_artnet_port_live(const pm_artnet_node_t *n, pm_port_idx_t port, pm_artnet_port_live_t *out);

/* ------------------------------------------------------------------------------------------------ merge (exposed for tests) */

/** Merge engine of one port: up to 2 sources identified by IP + Physical. */
typedef struct {
    struct {
        bool active;
        uint32_t ip;
        uint8_t physical;
        uint32_t last_ms;
        uint8_t sequence;        /**< last accepted ArtDmx Sequence (node logic, 0 = none) */
        uint8_t seq_drops;       /**< consecutive out-of-order drops (node logic) */
        uint16_t len;
        uint8_t data[512];
    } src[2];
    pm_merge_mode_t mode;
    bool cancel_pending;         /**< AcCancelMerge received: the next ArtDmx ends the merge */
    bool exclusive;              /**< after AcCancelMerge: only exclusive_ip is accepted until it expires */
    uint32_t exclusive_ip;
    uint16_t out_len;
    uint8_t out[512];
} pm_artnet_merge_t;

void pm_artnet_merge_init(pm_artnet_merge_t *m, pm_merge_mode_t mode);
/** Feed one ArtDmx. Returns PM_OK (out updated), PM_ERR_FULL (third source dropped), PM_ERR_REJECTED (other IP
 *  after AcCancelMerge), PM_ERR_INVALID_ARG. HTP: highest value per slot; LTP: the latest packet wins (slots
 *  beyond its length keep the other source). */
pm_err_t pm_artnet_merge_feed(pm_artnet_merge_t *m, uint32_t ip, uint8_t physical, const uint8_t *data,
                              uint16_t len, uint32_t now_ms);
/** Expire sources older than PM_ARTNET_MERGE_TIMEOUT_MS. Returns true if a source was removed. When one source
 *  remains, `out` is recomputed from it; when none remains, `out` keeps the last merge result. */
bool pm_artnet_merge_expire(pm_artnet_merge_t *m, uint32_t now_ms);
/** Number of active sources (2 = merging -> GoodOutputA bit 3). */
unsigned pm_artnet_merge_sources(const pm_artnet_merge_t *m);
/** AcCancelMerge: the next ArtDmx ends the merge (the sending source wins). */
void pm_artnet_merge_cancel(pm_artnet_merge_t *m);

#ifdef __cplusplus
}
#endif
