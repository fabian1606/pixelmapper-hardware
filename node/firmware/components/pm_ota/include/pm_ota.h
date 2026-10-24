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

#ifdef __cplusplus
}
#endif
