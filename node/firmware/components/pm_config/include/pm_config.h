/*
 * pm_config.h - persistent settings (ESP-IDF NVS) and change notification.
 *
 * Holds the one pm_settings_t of the node. Last writer wins (contract §1): every pm_config_update() is applied
 * immediately, saved to NVS (debounced ~1 s to protect the flash), and announced to all listeners with the change
 * mask and the source. Listeners: pm_artnet (unsolicited ArtPollReply), RDM responder (queued messages), pm_ui
 * (redraw, "changed by Pixelmapper" hint), main (pm_port_configure, pm_net_apply_ip).
 *
 * Thread-safe (mutex). Listeners are called in the caller's task - they must only copy/post (pm_net_post).
 *
 * Details of this implementation (Phase 1):
 *   - Listeners run INSIDE the config lock (recursive mutex), one after the other in registration order, so two
 *     concurrent writers can never deliver notifications out of order and `s` stays valid for the whole callback.
 *     Consequence: listeners must not block (no waiting on queues/other tasks that might call pm_config_*).
 *     pm_config_get()/pm_config_update() from inside a listener are allowed: a nested update is applied at once and
 *     announced after the current round has reached all listeners (several nested updates are merged into one
 *     notification with the OR of their masks and the source of the last one).
 *   - NVS: one "main" blob (all but the failsafe scenes) and one blob per port scene, see src/pm_config_store.h.
 *     Writes are debounced: ~1 s after the last change, at the latest ~5 s after the first unsaved change. Only
 *     blobs whose encoding differs from what is in flash are written. The writes run in a low-priority task on
 *     core 0. NVS errors are logged; the node keeps running from RAM.
 *   - Blobs of an older schema are migrated in RAM on load and rewritten with the next change.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "pm_board.h"
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

/** Write pending changes now (before a reboot). Synchronous; callable from any task (also an
 *  esp_register_shutdown_handler handler), not from an ISR. Called from inside a listener it only schedules an
 *  immediate write in the writer task (returns ESP_OK). ESP_ERR_TIMEOUT when NVS stays busy for > 3 s. */
esp_err_t pm_config_flush(void);

typedef void (*pm_config_listener_fn)(void *ctx, const pm_settings_t *s, pm_change_mask_t mask,
                                      pm_change_source_t src);
/** Register a listener (max. 8). */
esp_err_t pm_config_subscribe(pm_config_listener_fn fn, void *ctx);

/** Factory defaults like pm_config_factory_reset(), but keeps the IP settings (pm_settings_t.ip) too
 *  (UI option "keep network"). */
esp_err_t pm_config_factory_reset_keep_network(pm_change_source_t src);

/** True when the current settings equal the factory defaults, ignoring hw_revision_override
 *  (RDM FACTORY_DEFAULTS GET). */
bool pm_config_is_default(void);

/**
 * Power-on key combination for the factory reset (docs/plan.md): configures board->input.enc_push and
 * board->input.back as inputs with pull-up. Returns false at once when not both are pressed (low); otherwise polls
 * for 3 s and returns true only when both stay pressed the whole time. False when a pin is PM_BOARD_PIN_NONE.
 * Blocks up to 3 s; call from app_main before the UI takes over the pins. Independent of pm_config_init().
 */
bool pm_config_reset_keys_held(const pm_board_t *board);

#ifdef __cplusplus
}
#endif
