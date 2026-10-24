/*
 * pm_config.h - persistent settings (ESP-IDF NVS) and change notification.
 *
 * Holds the one pm_settings_t of the node. Last writer wins (contract §1): every pm_config_update() is applied
 * immediately, saved to NVS (debounced ~1 s to protect the flash), and announced to all listeners with the change
 * mask and the source. Listeners: pm_artnet (unsolicited ArtPollReply), RDM responder (queued messages), pm_ui
 * (redraw, "changed by Pixelmapper" hint), main (pm_port_configure, pm_net_apply_ip).
 *
 * Thread-safe (mutex). Listeners are called in the caller's task - they must only copy/post (pm_net_post).
 */
#pragma once

#include "esp_err.h"
#include "pm_settings.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Load from NVS (namespace "pm_cfg"), fall back to defaults, sanitize. Factory reset when `factory_reset`
 *  (power-on key combination, docs/plan.md). */
esp_err_t pm_config_init(bool factory_reset);

/** Copy of the current settings. */
void pm_config_get(pm_settings_t *out);

/**
 * Replace the settings with `s` (sanitized), compute the diff (pm_settings_diff), persist and notify the
 * listeners with that mask. Callers modify a copy from pm_config_get(). Returns ESP_OK also when nothing changed
 * (then no notification).
 */
esp_err_t pm_config_update(const pm_settings_t *s, pm_change_source_t src);

/** Factory defaults (keeps hw_revision_override), source PM_SRC_SYSTEM or the requester. */
esp_err_t pm_config_factory_reset(pm_change_source_t src);

/** Write pending changes now (before a reboot). */
esp_err_t pm_config_flush(void);

typedef void (*pm_config_listener_fn)(void *ctx, const pm_settings_t *s, pm_change_mask_t mask,
                                      pm_change_source_t src);
/** Register a listener (max. 8). */
esp_err_t pm_config_subscribe(pm_config_listener_fn fn, void *ctx);

#ifdef __cplusplus
}
#endif
