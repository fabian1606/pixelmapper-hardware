/*
 * ui_rdm.h - RDM device cache of the front panel (pure C11).
 *
 * The panel talks to attached devices through pm_ui_model_t.rdm_request (one request in flight, 1 s timeout).
 * It keeps a small LRU cache of DEVICE_INFO + DEVICE_LABEL for the rows that are visible, and the personality
 * descriptions of the one device page that is open (docs/ui-konzept.md 6.7).
 * Requests are built here (E1.20 framing incl. checksum) so the panel does not depend on the codec of pm_rdm.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pm_types.h"

#define UI_RDM_CACHE 32
#define UI_RDM_PERS_MAX 16
#define UI_RDM_LABEL_LEN 33
#define UI_RDM_TIMEOUT_MS 1000u

enum {
    UI_RDM_HAVE_INFO = 1u << 0,
    UI_RDM_HAVE_LABEL = 1u << 1,
    UI_RDM_FAILED = 1u << 2,   /**< last request got no ACK */
    UI_RDM_IDENTIFY = 1u << 3, /**< identify switched on from the panel */
};

typedef struct {
    pm_uid_t uid;
    uint8_t port;
    uint8_t flags;
    uint16_t model;
    uint16_t footprint;
    uint16_t start;          /**< DMX start address, 0xFFFF = none */
    uint8_t pers, pers_count;
    uint32_t sw_version;
    char label[UI_RDM_LABEL_LEN];
    uint32_t used;           /**< LRU stamp */
    uint32_t fetched_ms;
} ui_rdm_dev_t;

typedef struct {
    uint16_t footprint;
    char name[UI_RDM_LABEL_LEN];
    bool valid;
} ui_rdm_pers_t;

/** Send function = pm_ui_model_t.rdm_request (returns 0 on success). */
typedef int (*ui_rdm_send_fn)(void *ctx, uint8_t port, uint32_t token, const uint8_t *data, uint16_t len);

typedef enum {
    UI_RDM_RES_NONE = 0,
    UI_RDM_RES_OK,
    UI_RDM_RES_NACK,
    UI_RDM_RES_TIMEOUT,
} ui_rdm_res_t;

void ui_rdm_init(const pm_uid_t *self, ui_rdm_send_fn send, void *ctx);
void ui_rdm_set_self(const pm_uid_t *self);

/** Cache entry for (port, uid); fetches DEVICE_INFO/DEVICE_LABEL when missing or older than 30 s. Never NULL. */
ui_rdm_dev_t *ui_rdm_dev(uint8_t port, const pm_uid_t *uid);
/** Cache slot i (0..UI_RDM_CACHE-1), NULL when empty. */
const ui_rdm_dev_t *ui_rdm_cache_at(int i);
/** Lookup without fetching (NULL when not cached). */
ui_rdm_dev_t *ui_rdm_find(uint8_t port, const pm_uid_t *uid);

/** SETs from the panel (async; the cache is updated on ACK and DEVICE_INFO is re-read). */
void ui_rdm_set_identify(ui_rdm_dev_t *d, bool on);
void ui_rdm_set_start(ui_rdm_dev_t *d, uint16_t start);
void ui_rdm_set_personality(ui_rdm_dev_t *d, uint8_t pers);
void ui_rdm_set_label(ui_rdm_dev_t *d, const char *label);
/** Load the personality descriptions of `d` into the page buffer (async). */
void ui_rdm_load_personalities(ui_rdm_dev_t *d);
const ui_rdm_pers_t *ui_rdm_personality(uint8_t pers); /**< 1-based; NULL if unknown */

/** True while a SET for d (any PID) is queued or in flight. */
bool ui_rdm_busy(const ui_rdm_dev_t *d, uint16_t pid);

void ui_rdm_on_response(uint32_t token, const uint8_t *data, uint16_t len);
void ui_rdm_tick(uint32_t now_ms);
/** Result of the last SET (consumed by the call): pid and outcome. */
ui_rdm_res_t ui_rdm_take_result(uint16_t *pid);
/** Forget everything of a port (TOD flush, mode change). 0xFF = all ports. */
void ui_rdm_forget(uint8_t port);
