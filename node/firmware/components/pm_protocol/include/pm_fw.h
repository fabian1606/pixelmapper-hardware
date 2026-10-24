/*
 * pm_fw.h - node image description block and the firmware sink interface (pure C11).
 *
 * Format: protocol/image-format.md §2. Both update paths (ArtFirmwareMaster in pm_artnet, FW_* PIDs in the
 * pm_rdm responder) feed one pm_fw_sink_t, implemented by pm_ota on the target and by a RAM fake in host tests.
 */
#pragma once

#include "pm_pids.h"
#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_IMAGE_DESC_SIZE 64u
#define PM_IMAGE_DESC_OFFSET 288u /**< offset of the block in the ESP-IDF app binary (24 + 8 + 256) */

/** Node image description block, little-endian, packed layout = wire layout (64 bytes). */
typedef struct __attribute__((packed)) {
    uint32_t magic;            /**< PM_NODE_IMAGE_MAGIC ("PMIM") */
    uint8_t format;            /**< PM_NODE_IMAGE_FORMAT */
    uint8_t size;              /**< 64 */
    uint16_t reserved;         /**< 0 */
    uint16_t esta_manufacturer;
    uint16_t device_model_id;  /**< PM_MODEL_NODE */
    uint32_t fw_version_id;    /**< 0x00MMmmpp */
    uint32_t hw_rev_mask;      /**< bit n = hardware revision n supported */
    uint32_t image_length;     /**< 0 inside the image; filled in FW_BEGIN / .alf */
    uint32_t image_crc32;      /**< 0 inside the image; CRC-32 of the whole binary */
    char version_label[32];    /**< NUL padded */
    uint32_t desc_crc32;       /**< CRC-32 of bytes 0..59; may be 0 inside the image */
} pm_image_desc_t;

_Static_assert(sizeof(pm_image_desc_t) == PM_IMAGE_DESC_SIZE, "pm_image_desc_t must be 64 bytes");

/** Decode 64 bytes (little-endian wire format) into `out` (portable, no alignment needed). */
pm_err_t pm_image_desc_decode(const uint8_t *buf, size_t len, pm_image_desc_t *out);
/** Encode into 64 bytes. `fill_desc_crc` recomputes desc_crc32. */
void pm_image_desc_encode(const pm_image_desc_t *d, bool fill_desc_crc, uint8_t out[PM_IMAGE_DESC_SIZE]);

/**
 * Check an announced description (FW_BEGIN payload / .alf header copy) against this device.
 * Order and result codes as image-format.md §2.3 steps 1-5. `max_image_len` = size of the inactive OTA partition.
 * Returns PM_FW_ERROR_NONE when the image may be written.
 */
pm_fw_error_t pm_image_desc_check(const pm_image_desc_t *announced, uint16_t own_esta, uint8_t own_hw_rev,
                                  uint32_t max_image_len);

/** Step 6: compare the block found at PM_IMAGE_DESC_OFFSET of the image with the announced copy
 *  (ignoring image_length, image_crc32, desc_crc32). */
bool pm_image_desc_matches(const pm_image_desc_t *in_image, const pm_image_desc_t *announced);

/* ------------------------------------------------------------------------------------------------ sink */

/** Progress/state as reported by FW_INFO. */
typedef struct {
    pm_fw_state_t state;
    pm_fw_error_t last_error;
    uint32_t received;
    uint32_t total;
} pm_fw_status_t;

/**
 * Firmware sink. One session at a time (RDM or Art-Net). All calls from the network task (core 0).
 * The sink performs image-format.md §2.3 steps 1-8 itself, so callers only move bytes:
 *   begin(desc)  -> checks 1-5, opens the inactive OTA partition (may block for the erase: up to a few seconds)
 *   write(off)   -> sequential bytes of the ESP-IDF binary; off < received is accepted as retransmission
 *   finish()     -> length/CRC/esp_ota_end/set boot partition; on success the caller answers and then calls
 *                   reboot() ~500 ms later
 *   abort()      -> drop the session (timeout, new session)
 */
typedef struct pm_fw_sink {
    void *ctx;
    pm_fw_error_t (*begin)(void *ctx, const pm_image_desc_t *announced);
    pm_fw_error_t (*write)(void *ctx, uint32_t offset, const uint8_t *data, size_t len);
    pm_fw_error_t (*finish)(void *ctx);
    void (*abort)(void *ctx, pm_fw_error_t reason);
    void (*status)(void *ctx, pm_fw_status_t *out);
    void (*reboot)(void *ctx);
} pm_fw_sink_t;

#ifdef __cplusplus
}
#endif
