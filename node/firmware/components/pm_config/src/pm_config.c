/*
 * pm_config.c - settings store. Phase 0 stub: RAM only (defaults), notification implemented, no NVS yet.
 */
#include "pm_config.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define PM_CONFIG_MAX_LISTENERS 8

static pm_settings_t s_settings;
static SemaphoreHandle_t s_lock;
static struct {
    pm_config_listener_fn fn;
    void *ctx;
} s_listeners[PM_CONFIG_MAX_LISTENERS];

esp_err_t pm_config_init(bool factory_reset)
{
    (void)factory_reset;
    s_lock = xSemaphoreCreateMutex();
    if (s_lock == NULL) return ESP_ERR_NO_MEM;
    pm_settings_defaults(&s_settings);
    return ESP_OK;
}

void pm_config_get(pm_settings_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_settings;
    xSemaphoreGive(s_lock);
}

esp_err_t pm_config_update(const pm_settings_t *s, pm_change_source_t src)
{
    static pm_settings_t next; /* large struct: not on the caller's stack; guarded by s_lock */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    next = *s;
    pm_settings_sanitize(&next);
    pm_change_mask_t mask = pm_settings_diff(&s_settings, &next);
    s_settings = next;
    xSemaphoreGive(s_lock);
    if (mask == 0) return ESP_OK;
    for (int i = 0; i < PM_CONFIG_MAX_LISTENERS; i++) {
        if (s_listeners[i].fn) s_listeners[i].fn(s_listeners[i].ctx, &next, mask, src);
    }
    return ESP_OK;
}

esp_err_t pm_config_factory_reset(pm_change_source_t src)
{
    static pm_settings_t d;
    pm_settings_defaults(&d);
    d.hw_revision_override = s_settings.hw_revision_override;
    return pm_config_update(&d, src);
}

esp_err_t pm_config_flush(void) { return ESP_OK; }

esp_err_t pm_config_subscribe(pm_config_listener_fn fn, void *ctx)
{
    for (int i = 0; i < PM_CONFIG_MAX_LISTENERS; i++) {
        if (s_listeners[i].fn == NULL) {
            s_listeners[i].fn = fn;
            s_listeners[i].ctx = ctx;
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}
