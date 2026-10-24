/*
 * pm_ota.h - firmware sink on ESP-IDF OTA + the image description block of this firmware.
 *
 * Implements pm_fw_sink_t (pm_fw.h) for both update paths: checks the announced description block
 * (image-format.md §2.3 steps 1-5), writes the inactive OTA partition, compares the in-image block at offset 288,
 * checks length + CRC-32, esp_ota_end/esp_ota_set_boot_partition, reboot. Rollback: the new image must call
 * pm_ota_mark_valid() (done by main once Ethernet and the port engine run for 30 s); otherwise the bootloader
 * returns to the previous slot (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
 */
#pragma once

#include "esp_err.h"
#include "pm_fw.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The description block linked into this firmware (.rodata_custom_desc, image offset 288). */
extern const pm_image_desc_t pm_image_desc;

/** Prepare the sink (finds the inactive partition). `hw_revision` = pm_board_revision(). */
esp_err_t pm_ota_init(uint8_t hw_revision);

/** The sink to hand to pm_artnet (ops.fw_sink) and the RDM responder. */
const pm_fw_sink_t *pm_ota_sink(void);

/** True when the running image is still in the "pending verify" state after an update. */
bool pm_ota_pending_verify(void);

/** Mark the running image valid (cancel rollback). Idempotent. */
esp_err_t pm_ota_mark_valid(void);

/**
 * Listener for update progress (e.g. the UI). Called in the caller's task of the sink (= network task) on every
 * state change and per whole percent of progress. Keep it short and non-blocking.
 */
typedef void (*pm_ota_listener_fn)(void *ctx, const pm_fw_status_t *st);

/** Register (or with fn = NULL remove) the single progress listener. Call before the network task starts. */
esp_err_t pm_ota_set_listener(pm_ota_listener_fn fn, void *ctx);

/** Size of the inactive OTA partition in bytes (= maximum image_length), 0 before pm_ota_init()/without OTA. */
uint32_t pm_ota_max_image_size(void);

/**
 * Drop a session idle for more than 30 s (FW_INFO.last_error = TIMEOUT, image-format.md §2.4/§2.5). Call
 * periodically (e.g. once per second) from the network task, the same task that uses the sink.
 */
void pm_ota_poll(void);

/** Thread-safe copy of the current update status (any task). */
void pm_ota_get_status(pm_fw_status_t *out);

#ifdef __cplusplus
}
#endif
