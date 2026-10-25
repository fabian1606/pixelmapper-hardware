/*
 * ui_core.h - screens, menus, editors and status bar of the front panel (pure C11, no ESP-IDF/FreeRTOS).
 *
 * Runs in one task (the UI task on the target, main() in the host renderer). Everything that touches hardware or
 * other components goes through ui_platform_t + pm_ui_model_t, so the same code renders PNGs on the Mac
 * (components/pm_ui/host). Concept: docs/ui-konzept.md.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pm_ui.h"
#include "ui_input.h"

/** Static node facts for info pages and RDM requests. */
typedef struct {
    pm_uid_t uid;
    uint8_t mac[6];
    char fw_version[32];
    uint8_t hw_revision;
} ui_node_info_t;

typedef struct {
    void *ctx;
    uint32_t (*now_ms)(void *ctx);
    void (*settings_get)(void *ctx, pm_settings_t *out);
    /** Write the settings with source PM_SRC_LOCAL_UI. */
    void (*settings_set)(void *ctx, const pm_settings_t *s);
    /** Backlight 0..255; `panel_on` false = panel off (LED Mute). */
    void (*backlight)(void *ctx, uint8_t level, bool panel_on);
    void (*restart)(void *ctx);
    void (*factory_reset)(void *ctx, bool keep_network);
    /** Copy the current output of `port` into its failsafe scene (pm_port_output_read) and store it. */
    void (*record_scene)(void *ctx, pm_port_idx_t port);
    /** Full RDM discovery (ArtTodControl flush) of a port. */
    void (*tod_flush)(void *ctx, pm_port_idx_t port);
    /** Panel identify on/off (local identify; same state as RDM IDENTIFY_DEVICE of the node). */
    void (*identify)(void *ctx, bool on);
} ui_platform_t;

void ui_core_init(const ui_platform_t *plat, const pm_ui_model_t *model, const ui_node_info_t *info);
void ui_core_set_info(const ui_node_info_t *info);
/** Event from pm_ui_notify (event data already copied). */
void ui_core_event(const pm_ui_event_t *ev);
/** Feed the input state (call every ~5 ms). */
void ui_core_input(int detents, bool push, bool back);
/** Process keys/timers and draw (call every ~20-50 ms). Returns pixels sent to the display. */
uint32_t ui_core_tick(void);
/** Force a full redraw on the next tick (after the panel was powered up). */
void ui_core_invalidate(void);
