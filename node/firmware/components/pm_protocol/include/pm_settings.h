/*
 * pm_settings.h - the complete persistent configuration of the node (pure C11).
 *
 * One struct for everything that can be changed via Art-Net, RDM or the front panel. pm_config (ESP-IDF) stores it
 * in NVS and distributes changes; pm_artnet, the RDM responder (pm_rdm) and pm_ui read/modify copies of it and hand
 * changes back with a change mask. Last writer wins (protocol/contract.md §1).
 *
 * Defaults: pm_settings_defaults(). Ranges: protocol/pids.toml ([pid.pd] min/max) and Art-Net 4.
 */
#pragma once

#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_SHORT_NAME_LEN 18 /**< Art-Net PortName incl. NUL (17 chars) - also RDM DEVICE_LABEL */
#define PM_LONG_NAME_LEN 64  /**< Art-Net LongName incl. NUL (63 chars) */

typedef enum {
    PM_PORT_DIR_OUTPUT = 0, /**< Art-Net -> DMX (+ RDM controller) */
    PM_PORT_DIR_INPUT = 1,  /**< DMX -> Art-Net (no RDM) */
} pm_port_dir_t;

typedef enum {
    PM_MERGE_HTP = 0, /**< default */
    PM_MERGE_LTP = 1,
} pm_merge_mode_t;

/** Art-Net failsafe modes (ArtAddress AcFailHold..AcFailScene, ArtPollReply Status3 bits 7-6). */
typedef enum {
    PM_FAILSAFE_HOLD = 0, /**< default */
    PM_FAILSAFE_ZERO = 1,
    PM_FAILSAFE_FULL = 2,
    PM_FAILSAFE_SCENE = 3,
} pm_failsafe_t;

typedef enum {
    PM_OUTPUT_CONTINUOUS = 0, /**< default: DMX is refreshed at dmx_rate_hz all the time */
    PM_OUTPUT_DELTA = 1,      /**< frames only when data changes (plus a slow refresh) */
} pm_output_style_t;

/** Per-port settings. */
typedef struct {
    bool enabled;                  /**< PORTn_ENABLED, ArtInput (inputs only). Default true */
    pm_port_dir_t direction;       /**< ArtAddress AcDirectionTx/Rx. Default output */
    pm_port_address_t address;     /**< 15-bit Port-Address. Default 1..4 (0 is deprecated) */
    pm_merge_mode_t merge;         /**< ArtAddress AcMergeLtp/Htp */
    bool rdm_enabled;              /**< ArtAddress AcRdmEnable/AcRdmDisable. Default true */
    bool rdm_bg_discovery;         /**< PORTn_RDM_BG_DISCOVERY / ArtTodControl AtcIncOn/Off. Default true */
    pm_output_style_t output_style;
    pm_failsafe_t failsafe;        /**< Art-Net failsafe is per node in ArtPollReply; kept per port for the
                                        BindIndex-per-port model (all ports normally equal) */
    uint16_t dmx_break_us;         /**< PORTn_DMX_BREAK, 176..352, default 176 */
    uint16_t dmx_mab_us;           /**< PORTn_DMX_MAB, 12..88, default 16 */
    uint8_t dmx_rate_hz;           /**< PORTn_DMX_RATE, 1..44, default 40 */
    uint16_t scene_len;            /**< length of the recorded failsafe scene (0 = none) */
    uint8_t scene[512];            /**< failsafe scene (ArtAddress AcFailRecord) */
} pm_port_settings_t;

/** IPv4 configuration (ArtIpProg, E1.37-2). Addresses in host byte order (0x02000001 = 2.0.0.1). */
typedef struct {
    bool dhcp;              /**< default true; without a lease after 5 s the Art-Net fallback address is used */
    uint32_t static_ip;     /**< 0 = derive the Art-Net default 2.B.C.D from MAC + OEM */
    uint32_t static_mask;   /**< default 255.0.0.0 */
    uint32_t gateway;       /**< 0 = none */
} pm_ip_settings_t;

typedef struct {
    uint32_t version;                       /**< layout version of this struct in NVS */
    char short_name[PM_SHORT_NAME_LEN];     /**< default "PM Node" */
    char long_name[PM_LONG_NAME_LEN];       /**< default "Pixelmapper Node" */
    pm_ip_settings_t ip;
    pm_port_settings_t port[PM_NUM_PORTS];
    uint16_t rdm_bg_interval_s;             /**< RDM_BG_DISCOVERY_INTERVAL, default 3 */
    uint8_t display_level;                  /**< E1.20 DISPLAY_LEVEL 0..255, default 255 */
    uint16_t display_dim_timeout_s;         /**< DISPLAY_DIM_TIMEOUT, default 60, 0 = never */
    uint8_t address_format;                 /**< pm_address_format_t (pm_pids.h), default Net:Sub:Uni */
    bool panel_lock;                        /**< PANEL_LOCK */
    uint8_t hw_revision_override;           /**< prototype only: 0xFF = auto-detect (pm_board) */
} pm_settings_t;

#define PM_SETTINGS_VERSION 1u

/** Bits of a change mask: which part of pm_settings_t changed. Port-specific bits are shifted by
 *  PM_CHG_PORT_SHIFT(port). */
enum {
    PM_CHG_NAMES = 1u << 0,
    PM_CHG_IP = 1u << 1,
    PM_CHG_RDM_INTERVAL = 1u << 2,
    PM_CHG_DISPLAY = 1u << 3, /**< level, dim timeout, address format */
    PM_CHG_PANEL_LOCK = 1u << 4,
    PM_CHG_HW_REV = 1u << 5,
    /* per port: (PM_CHG_P_xxx << PM_CHG_PORT_SHIFT(port)) */
    PM_CHG_P_ENABLED = 1u << 0,
    PM_CHG_P_DIRECTION = 1u << 1,
    PM_CHG_P_ADDRESS = 1u << 2,
    PM_CHG_P_MERGE = 1u << 3,
    PM_CHG_P_RDM = 1u << 4, /**< rdm_enabled, rdm_bg_discovery */
    PM_CHG_P_OUTPUT = 1u << 5, /**< output_style, failsafe, scene */
    PM_CHG_P_TIMING = 1u << 6, /**< break, mab, rate */
};
typedef uint64_t pm_change_mask_t;
#define PM_CHG_PORT_SHIFT(port) (8u + 7u * (unsigned)(port)) /* 4 ports -> bits 8..35 */
/** Change bit `pbit` (PM_CHG_P_xxx) of port index `port` (0-based). */
#define PM_CHG_PORT(pbit, port) ((pm_change_mask_t)(pbit) << PM_CHG_PORT_SHIFT(port))

/** Who made a change (for the UI hint "changed by Pixelmapper" and for queued messages). */
typedef enum {
    PM_SRC_LOCAL_UI = 0,
    PM_SRC_ARTNET = 1,   /**< ArtAddress, ArtInput, ArtTodControl */
    PM_SRC_IPPROG = 2,   /**< ArtIpProg */
    PM_SRC_RDM = 3,      /**< RDM to the node UID */
    PM_SRC_SYSTEM = 4,   /**< factory reset, migration */
} pm_change_source_t;

/** Fill `s` with factory defaults (names, Port-Address 1..4, DHCP, HTP, Hold, 176/16 us, 40 Hz ...). */
void pm_settings_defaults(pm_settings_t *s);

/** Clamp all values into their valid ranges (after loading from NVS or before applying a remote SET).
 *  Returns true when something had to be changed. */
bool pm_settings_sanitize(pm_settings_t *s);

/** Compare two settings and return the change mask (0 = equal). */
pm_change_mask_t pm_settings_diff(const pm_settings_t *a, const pm_settings_t *b);

#ifdef __cplusplus
}
#endif
