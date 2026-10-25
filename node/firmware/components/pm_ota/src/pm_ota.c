/*
 * pm_ota.c - firmware sink on ESP-IDF OTA (image-format.md §2.3) + the description block of this firmware.
 *
 * The session logic (checks, sequence, CRC, in-image block) lives in pm_ota_session.c (host-tested); this file
 * provides the esp_ota backend, the pm_fw_sink_t glue, a thread-safe status copy and the progress listener.
 * All sink calls come from the network task; only pm_ota_get_status()/sink.status may run in other tasks.
 */
#include "pm_ota.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "pm_ota_session.h"
#include "pm_pids.h"

#define STR2(x) #x
#define STR(x) STR2(x)

static const char *TAG = "pm_ota";

/* image-format.md §2.2. image_length, image_crc32 and desc_crc32 stay 0 inside the image. */
const pm_image_desc_t pm_image_desc __attribute__((section(".rodata_custom_desc"), used, aligned(4))) = {
    .magic = PM_NODE_IMAGE_MAGIC,
    .format = PM_NODE_IMAGE_FORMAT,
    .size = PM_IMAGE_DESC_SIZE,
    .esta_manufacturer = PM_ESTA_MANUFACTURER,
    .device_model_id = PM_MODEL_NODE,
    .fw_version_id = PM_FW_VERSION_ID(PM_FW_VERSION_MAJOR, PM_FW_VERSION_MINOR, PM_FW_VERSION_PATCH),
    .hw_rev_mask = 1u << 0, /* keep in sync with pm_board_supported_revisions() */
    .version_label = STR(PM_FW_VERSION_MAJOR) "." STR(PM_FW_VERSION_MINOR) "." STR(PM_FW_VERSION_PATCH),
};

static uint8_t s_hw_rev;
static const esp_partition_t *s_part; /* inactive slot, looked up again at each begin */
static esp_ota_handle_t s_handle;
static pm_ota_session_t s_session;
static pm_ota_listener_fn s_listener;
static void *s_listener_ctx;

static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static pm_fw_status_t s_status_pub; /* snapshot for readers in other tasks */

/* ------------------------------------------------------------------------------------------------ backend */

static pm_fw_error_t be_begin(void *ctx, uint32_t image_len)
{
    (void)ctx;
    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) return PM_FW_ERROR_FLASH;
    if (image_len > s_part->size) return PM_FW_ERROR_TOO_LARGE;
    /* Erase sector by sector while writing: the network task is not blocked for the whole-partition erase. */
    esp_err_t err = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin(%s): %s", s_part->label, esp_err_to_name(err));
        s_handle = 0;
        return PM_FW_ERROR_FLASH;
    }
    ESP_LOGI(TAG, "update started: %u bytes -> %s", (unsigned)image_len, s_part->label);
    return PM_FW_ERROR_NONE;
}

static pm_fw_error_t be_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    esp_err_t err = esp_ota_write(s_handle, data, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
        return PM_FW_ERROR_FLASH;
    }
    return PM_FW_ERROR_NONE;
}

static pm_fw_error_t be_end(void *ctx)
{
    (void)ctx;
    esp_err_t err = esp_ota_end(s_handle); /* frees the handle in any case */
    s_handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? PM_FW_ERROR_IMAGE_INVALID : PM_FW_ERROR_FLASH;
    }
    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return err == ESP_ERR_OTA_VALIDATE_FAILED ? PM_FW_ERROR_IMAGE_INVALID : PM_FW_ERROR_FLASH;
    }
    ESP_LOGI(TAG, "update complete, boot partition %s", s_part->label);
    return PM_FW_ERROR_NONE;
}

static void be_abort(void *ctx)
{
    (void)ctx;
    if (s_handle != 0) esp_ota_abort(s_handle);
    s_handle = 0;
}

static uint32_t be_now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static const pm_ota_backend_t s_backend = {
    .ctx = NULL,
    .begin = be_begin,
    .write = be_write,
    .end = be_end,
    .abort = be_abort,
    .now_ms = be_now_ms,
};

/* ------------------------------------------------------------------------------------------------ status */

static void publish(void)
{
    pm_fw_status_t st;
    pm_ota_session_status(&s_session, &st);
    taskENTER_CRITICAL(&s_status_lock);
    s_status_pub = st;
    taskEXIT_CRITICAL(&s_status_lock);
}

static void on_session_event(void *ctx, const pm_fw_status_t *st)
{
    (void)ctx;
    if (st->state == PM_FW_STATE_FAILED) ESP_LOGW(TAG, "update failed: error %d", (int)st->last_error);
    publish();
    pm_ota_listener_fn fn = s_listener;
    if (fn != NULL) fn(s_listener_ctx, st);
}

/* ------------------------------------------------------------------------------------------------ sink */

static pm_fw_error_t sink_begin(void *ctx, const pm_image_desc_t *announced)
{
    (void)ctx;
    pm_fw_error_t err = pm_ota_session_begin(&s_session, announced, PM_ESTA_MANUFACTURER, s_hw_rev,
                                             pm_ota_max_image_size());
    publish();
    return err;
}

static pm_fw_error_t sink_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    (void)ctx;
    pm_fw_error_t err = pm_ota_session_write(&s_session, offset, data, len);
    publish();
    return err;
}

static pm_fw_error_t sink_finish(void *ctx)
{
    (void)ctx;
    pm_fw_error_t err = pm_ota_session_finish(&s_session);
    publish();
    return err;
}

static void sink_abort(void *ctx, pm_fw_error_t reason)
{
    (void)ctx;
    pm_ota_session_abort(&s_session, reason);
    publish();
}

static void sink_status(void *ctx, pm_fw_status_t *out)
{
    (void)ctx;
    pm_ota_get_status(out);
}

static void sink_reboot(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "rebooting into the new firmware");
    esp_restart(); /* registered shutdown handlers (config flush) run first */
}

static const pm_fw_sink_t s_sink = {
    .ctx = NULL,
    .begin = sink_begin,
    .write = sink_write,
    .finish = sink_finish,
    .abort = sink_abort,
    .status = sink_status,
    .reboot = sink_reboot,
};

/* ------------------------------------------------------------------------------------------------ public */

esp_err_t pm_ota_init(uint8_t hw_revision)
{
    s_hw_rev = hw_revision;
    pm_ota_session_init(&s_session, &s_backend, on_session_event, NULL);
    publish();
    s_part = esp_ota_get_next_update_partition(NULL);
    if (s_part == NULL) {
        ESP_LOGW(TAG, "no OTA partition: firmware update disabled");
    } else {
        ESP_LOGI(TAG, "update slot %s, %u bytes", s_part->label, (unsigned)s_part->size);
    }
    return ESP_OK;
}

const pm_fw_sink_t *pm_ota_sink(void) { return &s_sink; }

esp_err_t pm_ota_set_listener(pm_ota_listener_fn fn, void *ctx)
{
    s_listener_ctx = ctx;
    s_listener = fn;
    return ESP_OK;
}

uint32_t pm_ota_max_image_size(void)
{
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    return p != NULL ? (uint32_t)p->size : 0;
}

void pm_ota_poll(void)
{
    pm_ota_session_poll(&s_session);
    publish();
}

void pm_ota_get_status(pm_fw_status_t *out)
{
    taskENTER_CRITICAL(&s_status_lock);
    *out = s_status_pub;
    taskEXIT_CRITICAL(&s_status_lock);
}

bool pm_ota_pending_verify(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t pm_ota_mark_valid(void) { return esp_ota_mark_app_valid_cancel_rollback(); }
