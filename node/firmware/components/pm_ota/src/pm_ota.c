/*
 * pm_ota.c - OTA sink. Phase 0: description block placed in .rodata_custom_desc; sink functions are stubs.
 */
#include "pm_ota.h"

#include <string.h>

#include "esp_ota_ops.h"
#include "esp_system.h"
#include "pm_pids.h"

#define STR2(x) #x
#define STR(x) STR2(x)

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

static pm_fw_error_t sink_begin(void *ctx, const pm_image_desc_t *announced)
{
    (void)ctx;
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL) return PM_FW_ERROR_FLASH;
    return pm_image_desc_check(announced, PM_ESTA_MANUFACTURER, s_hw_rev, next->size);
    /* Phase 1: esp_ota_begin(), session state */
}

static pm_fw_error_t sink_write(void *ctx, uint32_t offset, const uint8_t *data, size_t len)
{
    (void)ctx; (void)offset; (void)data; (void)len;
    return PM_FW_ERROR_SEQUENCE;
}

static pm_fw_error_t sink_finish(void *ctx)
{
    (void)ctx;
    return PM_FW_ERROR_SEQUENCE;
}

static void sink_abort(void *ctx, pm_fw_error_t reason) { (void)ctx; (void)reason; }

static void sink_status(void *ctx, pm_fw_status_t *out)
{
    (void)ctx;
    memset(out, 0, sizeof(*out));
}

static void sink_reboot(void *ctx)
{
    (void)ctx;
    esp_restart();
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

esp_err_t pm_ota_init(uint8_t hw_revision)
{
    s_hw_rev = hw_revision;
    return ESP_OK;
}

const pm_fw_sink_t *pm_ota_sink(void) { return &s_sink; }

bool pm_ota_pending_verify(void)
{
    esp_ota_img_states_t st;
    const esp_partition_t *run = esp_ota_get_running_partition();
    return esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t pm_ota_mark_valid(void) { return esp_ota_mark_app_valid_cancel_rollback(); }
