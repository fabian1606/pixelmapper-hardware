/*
 * pm_config.c - settings store: NVS persistence (debounced, per-blob), change notification, factory reset.
 *
 * Locks (always taken in this order, never the other way round):
 *   s_nvs_lock  mutex, serializes the NVS writes (writer task, pm_config_flush) and guards s_snap/s_buf/s_saved.
 *   s_lock      recursive mutex, guards the settings, the dirty state and the listener list. Held during
 *               notification. Never held while waiting for s_nvs_lock (see pm_config_flush).
 */
#include "pm_config.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pm_config_store.h"

#define PM_CONFIG_MAX_LISTENERS 8
#define PM_CONFIG_NAMESPACE "pm_cfg"
#define PM_CONFIG_SAVE_DELAY_US (1000 * 1000LL)     /* write ~1 s after the last change ... */
#define PM_CONFIG_SAVE_MAX_DELAY_US (5000 * 1000LL) /* ... but at the latest ~5 s after the first unsaved one */
#define PM_CONFIG_FLUSH_TIMEOUT_MS 3000
#define PM_CONFIG_MAX_BLOB_LEN 4096 /* larger stored blobs are treated as corrupt */
#define PM_CONFIG_NUM_BLOBS (1 + PM_NUM_PORTS) /* [0] main, [1 + i] scene of port i */
#define PM_CONFIG_TASK_STACK 4096
#define PM_CONFIG_TASK_PRIO (tskIDLE_PRIORITY + 1)
#define PM_CONFIG_KEY_HOLD_MS 3000
#define PM_CONFIG_KEY_POLL_MS 20

static const char *TAG = "pm_config";
static const char *const s_keys[PM_CONFIG_NUM_BLOBS] = {"main", "scene0", "scene1", "scene2", "scene3"};
_Static_assert(PM_NUM_PORTS == 4, "s_keys needs one scene key per port");

/* ---- guarded by s_lock */
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;
static pm_settings_t s_settings;
static pm_settings_t s_next;        /* update scratch (large: not on the caller's stack) */
static pm_settings_t s_notify_snap; /* what listeners see during one notification round */
static pm_settings_t s_reset;       /* factory reset / is_default scratch */
static struct {
    pm_config_listener_fn fn;
    void *ctx;
} s_listeners[PM_CONFIG_MAX_LISTENERS];
static bool s_notifying;
static pm_change_mask_t s_pending_mask;
static pm_change_source_t s_pending_src;
static bool s_dirty;
static int64_t s_first_dirty_us; /* first unsaved change */
static int64_t s_last_dirty_us;  /* last unsaved change */
static TaskHandle_t s_task;

/* ---- guarded by s_nvs_lock */
static SemaphoreHandle_t s_nvs_lock;
static StaticSemaphore_t s_nvs_lock_buf;
static nvs_handle_t s_nvs;
static bool s_nvs_open;
static pm_settings_t s_snap; /* settings being written */
static uint8_t s_buf[PM_CONFIG_STORE_SCENE_MAX > PM_CONFIG_STORE_MAIN_MAX ? PM_CONFIG_STORE_SCENE_MAX
                                                                          : PM_CONFIG_STORE_MAIN_MAX];
static struct {
    bool valid;   /* flash content known */
    uint32_t crc; /* pm_crc32 over the whole blob in flash */
    size_t len;
} s_saved[PM_CONFIG_NUM_BLOBS];

static inline void lock(void) { xSemaphoreTakeRecursive(s_lock, portMAX_DELAY); }
static inline void unlock(void) { xSemaphoreGiveRecursive(s_lock); }

/* ------------------------------------------------------------------------------------------------ persistence */

static size_t encode_blob(unsigned k, const pm_settings_t *s)
{
    return k == 0 ? pm_config_store_encode_main(s, s_buf, sizeof(s_buf))
                  : pm_config_store_encode_scene(&s->port[k - 1], s_buf, sizeof(s_buf));
}

/** Write all blobs whose encoding differs from flash. Caller must NOT hold s_lock (lock order). */
static esp_err_t persist(void)
{
    if (xSemaphoreTake(s_nvs_lock, pdMS_TO_TICKS(PM_CONFIG_FLUSH_TIMEOUT_MS)) != pdTRUE) return ESP_ERR_TIMEOUT;
    lock();
    s_snap = s_settings;
    s_dirty = false;
    unlock();

    esp_err_t ret = ESP_OK;
    if (!s_nvs_open) {
        ret = ESP_ERR_INVALID_STATE;
    } else {
        bool written[PM_CONFIG_NUM_BLOBS] = {false};
        uint32_t crcs[PM_CONFIG_NUM_BLOBS] = {0};
        size_t lens[PM_CONFIG_NUM_BLOBS] = {0};
        bool any = false;
        for (unsigned k = 0; k < PM_CONFIG_NUM_BLOBS; k++) {
            size_t n = encode_blob(k, &s_snap);
            if (n == 0) continue; /* cannot happen: s_buf >= max size */
            uint32_t crc = pm_crc32(0, s_buf, n);
            if (s_saved[k].valid && s_saved[k].crc == crc && s_saved[k].len == n) continue;
            esp_err_t err = nvs_set_blob(s_nvs, s_keys[k], s_buf, n);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "write %s: %s", s_keys[k], esp_err_to_name(err));
                s_saved[k].valid = false;
                ret = err;
                continue;
            }
            written[k] = true;
            crcs[k] = crc;
            lens[k] = n;
            any = true;
        }
        if (any) {
            esp_err_t err = nvs_commit(s_nvs);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "commit: %s", esp_err_to_name(err));
                ret = err;
            }
            for (unsigned k = 0; k < PM_CONFIG_NUM_BLOBS; k++) {
                if (!written[k]) continue;
                s_saved[k].valid = err == ESP_OK;
                s_saved[k].crc = crcs[k];
                s_saved[k].len = lens[k];
                if (err == ESP_OK) ESP_LOGI(TAG, "saved %s (%u bytes)", s_keys[k], (unsigned)lens[k]);
            }
        }
    }
    xSemaphoreGive(s_nvs_lock);
    return ret;
}

/** Writer task: waits until the debounce deadline of the pending changes, then persists. */
static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        bool due = false;
        lock();
        if (s_dirty) {
            int64_t deadline = s_last_dirty_us + PM_CONFIG_SAVE_DELAY_US;
            if (deadline > s_first_dirty_us + PM_CONFIG_SAVE_MAX_DELAY_US) {
                deadline = s_first_dirty_us + PM_CONFIG_SAVE_MAX_DELAY_US;
            }
            int64_t now = esp_timer_get_time();
            if (now >= deadline) {
                due = true;
            } else {
                wait = pdMS_TO_TICKS((uint32_t)((deadline - now + 999) / 1000));
                if (wait == 0) wait = 1;
            }
        }
        unlock();
        if (due) {
            persist(); /* errors are logged; retried with the next change or flush */
            continue;
        }
        ulTaskNotifyTake(pdTRUE, wait); /* new change (recompute deadline) or deadline reached */
    }
}

/** Remember an unsaved change and wake the writer task. Caller holds s_lock. */
static void mark_dirty(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_dirty) {
        s_dirty = true;
        s_first_dirty_us = now;
    }
    s_last_dirty_us = now;
    if (s_task != NULL) xTaskNotifyGive(s_task);
}

/* ------------------------------------------------------------------------------------------------ loading */

/** Read blob `k` into a heap buffer (init only). Returns NULL when missing or unreadable. */
static uint8_t *read_blob(unsigned k, size_t *len)
{
    size_t n = 0;
    esp_err_t err = nvs_get_blob(s_nvs, s_keys[k], NULL, &n);
    if (err == ESP_ERR_NVS_NOT_FOUND) return NULL;
    if (err != ESP_OK || n == 0 || n > PM_CONFIG_MAX_BLOB_LEN) {
        ESP_LOGW(TAG, "%s unreadable (%s, %u bytes)", s_keys[k], esp_err_to_name(err), (unsigned)n);
        return NULL;
    }
    uint8_t *buf = malloc(n);
    if (buf == NULL) return NULL;
    err = nvs_get_blob(s_nvs, s_keys[k], buf, &n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", s_keys[k], esp_err_to_name(err));
        free(buf);
        return NULL;
    }
    *len = n;
    return buf;
}

static void remember_saved(unsigned k, const uint8_t *blob, size_t n)
{
    s_saved[k].valid = true;
    s_saved[k].crc = pm_crc32(0, blob, n);
    s_saved[k].len = n;
}

/** Load everything from NVS into s_settings and remember what is in flash (s_saved). */
static void load(void)
{
    pm_settings_defaults(&s_settings);
    if (!s_nvs_open) return;
    size_t n = 0;
    uint8_t *buf = read_blob(0, &n);
    if (buf == NULL) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
    } else {
        uint16_t schema = 0;
        pm_err_t err = pm_config_store_decode_main(buf, n, &s_settings, &schema);
        if (err != PM_OK) {
            ESP_LOGW(TAG, "main blob corrupt (%s), using defaults", pm_err_name(err));
        } else if (schema < PM_CONFIG_STORE_SCHEMA) {
            ESP_LOGI(TAG, "migrated settings schema %u -> %u", (unsigned)schema, PM_CONFIG_STORE_SCHEMA);
        } else if (schema > PM_CONFIG_STORE_SCHEMA) {
            ESP_LOGW(TAG, "settings schema %u is newer than %u: known fields only", (unsigned)schema,
                     PM_CONFIG_STORE_SCHEMA);
        }
        remember_saved(0, buf, n); /* a migrated/repaired blob differs from the next encoding -> rewritten then */
        free(buf);
    }
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        unsigned k = 1 + i;
        buf = read_blob(k, &n);
        if (buf == NULL) {
            /* missing = empty scene, which then needs no write */
            s_settings.port[i].scene_len = 0;
            n = encode_blob(k, &s_settings);
            remember_saved(k, s_buf, n);
            continue;
        }
        pm_err_t err = pm_config_store_decode_scene(buf, n, &s_settings.port[i]);
        if (err != PM_OK) ESP_LOGW(TAG, "%s corrupt (%s), scene cleared", s_keys[k], pm_err_name(err));
        remember_saved(k, buf, n);
        free(buf);
    }
    pm_settings_sanitize(&s_settings);
}

/* ------------------------------------------------------------------------------------------------ API */

esp_err_t pm_config_init(bool factory_reset)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    s_nvs_lock = xSemaphoreCreateMutexStatic(&s_nvs_lock_buf);

    esp_err_t err = nvs_open(PM_CONFIG_NAMESPACE, NVS_READWRITE, &s_nvs);
    s_nvs_open = err == ESP_OK;
    if (!s_nvs_open) ESP_LOGE(TAG, "nvs_open: %s - settings are not persistent", esp_err_to_name(err));

    load();
    pm_config_store_normalize(&s_settings);
    if (factory_reset) {
        uint8_t hw = s_settings.hw_revision_override;
        pm_settings_defaults(&s_settings);
        s_settings.hw_revision_override = hw;
        ESP_LOGW(TAG, "factory reset");
        if (s_nvs_open) {
            err = nvs_erase_all(s_nvs); /* also drops keys of newer firmware */
            if (err != ESP_OK) ESP_LOGE(TAG, "erase: %s", esp_err_to_name(err));
            memset(s_saved, 0, sizeof(s_saved));
        }
        persist();
    }

    if (xTaskCreatePinnedToCore(writer_task, "pm_config", PM_CONFIG_TASK_STACK, NULL, PM_CONFIG_TASK_PRIO, &s_task,
                                0) != pdPASS) {
        ESP_LOGE(TAG, "writer task not created - only pm_config_flush() saves");
        s_task = NULL;
    }
    return ESP_OK;
}

void pm_config_get(pm_settings_t *out)
{
    if (s_lock == NULL) {
        pm_settings_defaults(out);
        return;
    }
    lock();
    *out = s_settings;
    unlock();
}

/** Commit s_next (filled by the caller) and notify. Caller holds s_lock. */
static void commit_next(pm_change_source_t src)
{
    pm_change_mask_t mask = pm_config_store_prepare(&s_next, &s_settings);
    if (mask != 0) {
        s_settings = s_next;
        mark_dirty();
        s_pending_mask |= mask;
        s_pending_src = src;
        if (!s_notifying) {
            /* outermost call: deliver this change and any made by listeners meanwhile, round by round */
            s_notifying = true;
            while (s_pending_mask != 0) {
                pm_change_mask_t m = s_pending_mask;
                pm_change_source_t from = s_pending_src;
                s_pending_mask = 0;
                s_notify_snap = s_settings;
                for (int i = 0; i < PM_CONFIG_MAX_LISTENERS; i++) {
                    if (s_listeners[i].fn) s_listeners[i].fn(s_listeners[i].ctx, &s_notify_snap, m, from);
                }
            }
            s_notifying = false;
        }
    }
}

esp_err_t pm_config_update(const pm_settings_t *s, pm_change_source_t src)
{
    if (s == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    lock();
    s_next = *s;
    commit_next(src);
    unlock();
    return ESP_OK;
}

esp_err_t pm_config_modify(pm_config_modify_fn fn, void *ctx, pm_change_source_t src)
{
    if (fn == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    lock();
    s_next = s_settings; /* read-modify-write under the lock: no change of another writer is lost */
    fn(ctx, &s_next);
    commit_next(src);
    unlock();
    return ESP_OK;
}

/** Defaults with hw_revision_override kept (and the IP settings when `keep_ip`), applied as one update. */
static esp_err_t reset_to_defaults(pm_change_source_t src, bool keep_ip)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    lock();
    pm_settings_defaults(&s_reset);
    s_reset.hw_revision_override = s_settings.hw_revision_override;
    if (keep_ip) s_reset.ip = s_settings.ip;
    esp_err_t err = pm_config_update(&s_reset, src); /* copies s_reset before calling anything */
    unlock();
    return err;
}

esp_err_t pm_config_factory_reset(pm_change_source_t src) { return reset_to_defaults(src, false); }

esp_err_t pm_config_factory_reset_keep_network(pm_change_source_t src) { return reset_to_defaults(src, true); }

bool pm_config_is_default(void)
{
    if (s_lock == NULL) return true;
    lock();
    pm_settings_defaults(&s_reset);
    s_reset.hw_revision_override = s_settings.hw_revision_override;
    bool equal = pm_settings_diff(&s_reset, &s_settings) == 0;
    unlock();
    return equal;
}

esp_err_t pm_config_flush(void)
{
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreGetMutexHolder(s_lock) == xTaskGetCurrentTaskHandle()) {
        /* called from a listener: waiting for s_nvs_lock while holding s_lock could deadlock with the writer task.
         * Make the pending change due now instead. */
        lock();
        if (s_dirty) s_first_dirty_us = s_last_dirty_us = -PM_CONFIG_SAVE_MAX_DELAY_US;
        if (s_task != NULL) xTaskNotifyGive(s_task);
        unlock();
        return ESP_OK;
    }
    return persist();
}

esp_err_t pm_config_subscribe(pm_config_listener_fn fn, void *ctx)
{
    if (fn == NULL) return ESP_ERR_INVALID_ARG;
    if (s_lock == NULL) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_ERR_NO_MEM;
    lock();
    for (int i = 0; i < PM_CONFIG_MAX_LISTENERS; i++) {
        if (s_listeners[i].fn == NULL) {
            s_listeners[i].fn = fn;
            s_listeners[i].ctx = ctx;
            err = ESP_OK;
            break;
        }
    }
    unlock();
    return err;
}

/* ------------------------------------------------------------------------------------------------ reset keys */

static bool both_pressed(int a, int b) { return gpio_get_level(a) == 0 && gpio_get_level(b) == 0; }

bool pm_config_reset_keys_held(const pm_board_t *board)
{
    if (board == NULL) return false;
    int push = board->input.enc_push;
    int back = board->input.back;
    if (push == PM_BOARD_PIN_NONE || back == PM_BOARD_PIN_NONE) return false;
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << push) | (1ULL << back),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) return false;
    vTaskDelay(pdMS_TO_TICKS(PM_CONFIG_KEY_POLL_MS)); /* let the pull-ups settle */
    if (!both_pressed(push, back)) return false;
    ESP_LOGI(TAG, "reset keys pressed, hold for %d s", PM_CONFIG_KEY_HOLD_MS / 1000);
    for (int t = 0; t < PM_CONFIG_KEY_HOLD_MS; t += PM_CONFIG_KEY_POLL_MS) {
        vTaskDelay(pdMS_TO_TICKS(PM_CONFIG_KEY_POLL_MS));
        if (!both_pressed(push, back)) {
            ESP_LOGI(TAG, "reset keys released early");
            return false;
        }
    }
    return true;
}
