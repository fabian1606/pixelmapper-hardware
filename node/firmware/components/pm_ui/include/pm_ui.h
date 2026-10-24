/*
 * pm_ui.h - front panel: ST7735S 128x160 TFT (SPI, landscape), rotary encoder with push, BACK button.
 *
 * Concept: docs/ui-konzept.md (English UI texts, status bar replaces LEDs, home screen = short name + IP/mode +
 * 4 port lines, menus 6 lines x 20 chars, strip-buffer drawing of dirty regions only). Runs as the lowest-priority
 * task on core 0. Reads state through pm_ui_model_t callbacks and pm_config, writes settings with
 * pm_config_update(PM_SRC_LOCAL_UI). Never blocks other tasks; events from elsewhere arrive via pm_ui_notify().
 *
 * Sub-modules (Phase 1, inside this component): st7735 driver (pm_ui_lcd_*), fonts (F-M 6x12, F-N 8x16, F-L 16x24),
 * input debouncing/acceleration (pm_ui_input_*), screens/menus (pm_ui_screen_*).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pm_board.h"
#include "pm_port.h"
#include "pm_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Read-only live data the UI shows (implemented by main). All callbacks must be fast and thread-safe. */
typedef struct {
    void *ctx;
    void (*net_status)(void *ctx, uint32_t *ip, uint32_t *mask, uint8_t *mode, bool *link_up); /**< mode: pm_net_addr_mode_t */
    void (*port_stats)(void *ctx, pm_port_idx_t port, pm_port_stats_t *out);
    /** Port state for the status bar: data loss/failsafe active, merging, sync mode. */
    void (*port_flags)(void *ctx, pm_port_idx_t port, bool *data_loss, bool *merging, bool *synced);
    uint16_t (*tod_get)(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap);
    /** RDM request from the panel to a device (identify, address, personality, label). The response arrives via
     *  pm_ui_notify(PM_UI_EV_RDM_RESPONSE). main also records SETs with pm_rdm_responder_local_set(). */
    esp_err_t (*rdm_request)(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len);
} pm_ui_model_t;

typedef enum {
    PM_UI_EV_SETTINGS_CHANGED,  /**< value = pm_change_source_t; remote changes highlight the value (orange, 3 s) */
    PM_UI_EV_LED_MODE,          /**< value = PM_AC_LED_NORMAL / MUTE / LOCATE (mute = display off) */
    PM_UI_EV_IDENTIFY,          /**< value = 1/0 (RDM IDENTIFY_DEVICE of the node) */
    PM_UI_EV_TOD_CHANGED,       /**< port */
    PM_UI_EV_RDM_RESPONSE,      /**< token + data */
    PM_UI_EV_FW_PROGRESS,       /**< value = percent, -1 = failed; full-screen during a node update */
    PM_UI_EV_BOOT_CATCH,        /**< port + value = pm_boot_catch_state_t */
    PM_UI_EV_ERROR,             /**< value = code, shown as toast */
} pm_ui_event_type_t;

typedef struct {
    pm_ui_event_type_t type;
    pm_port_idx_t port;
    int32_t value;
    uint32_t token;
    uint16_t len;
    const uint8_t *data;        /**< copied by pm_ui_notify */
} pm_ui_event_t;

/** Init display/backlight/inputs from the board table. Returns quickly; shows the boot screen. */
esp_err_t pm_ui_init(const pm_board_t *board, const pm_ui_model_t *model);
/** Start the UI task (core 0, lowest application priority). */
esp_err_t pm_ui_start(void);
/** Post an event (any task, never blocks; dropped when the queue is full). */
void pm_ui_notify(const pm_ui_event_t *ev);

/** True when BACK + push were held at power-on for the factory reset (checked in pm_ui_init). */
bool pm_ui_factory_reset_requested(void);

#ifdef __cplusplus
}
#endif
