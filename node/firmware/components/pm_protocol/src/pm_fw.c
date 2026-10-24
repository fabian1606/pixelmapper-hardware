/* pm_fw.c - node image description block (protocol/image-format.md §2), pure C11. */
#include "pm_fw.h"

#include <string.h>

pm_err_t pm_image_desc_decode(const uint8_t *buf, size_t len, pm_image_desc_t *out)
{
    if (buf == NULL || out == NULL) return PM_ERR_INVALID_ARG;
    if (len < PM_IMAGE_DESC_SIZE) return PM_ERR_SHORT;
    out->magic = pm_get_le32(buf + 0);
    out->format = buf[4];
    out->size = buf[5];
    out->reserved = pm_get_le16(buf + 6);
    out->esta_manufacturer = pm_get_le16(buf + 8);
    out->device_model_id = pm_get_le16(buf + 10);
    out->fw_version_id = pm_get_le32(buf + 12);
    out->hw_rev_mask = pm_get_le32(buf + 16);
    out->image_length = pm_get_le32(buf + 20);
    out->image_crc32 = pm_get_le32(buf + 24);
    memcpy(out->version_label, buf + 28, sizeof(out->version_label));
    out->desc_crc32 = pm_get_le32(buf + 60);
    return PM_OK;
}

void pm_image_desc_encode(const pm_image_desc_t *d, bool fill_desc_crc, uint8_t out[PM_IMAGE_DESC_SIZE])
{
    pm_put_le32(out + 0, d->magic);
    out[4] = d->format;
    out[5] = d->size;
    pm_put_le16(out + 6, d->reserved);
    pm_put_le16(out + 8, d->esta_manufacturer);
    pm_put_le16(out + 10, d->device_model_id);
    pm_put_le32(out + 12, d->fw_version_id);
    pm_put_le32(out + 16, d->hw_rev_mask);
    pm_put_le32(out + 20, d->image_length);
    pm_put_le32(out + 24, d->image_crc32);
    memcpy(out + 28, d->version_label, sizeof(d->version_label));
    pm_put_le32(out + 60, fill_desc_crc ? pm_crc32(0, out, 60) : d->desc_crc32);
}

pm_fw_error_t pm_image_desc_check(const pm_image_desc_t *a, uint16_t own_esta, uint8_t own_hw_rev,
                                  uint32_t max_image_len)
{
    uint8_t raw[PM_IMAGE_DESC_SIZE];
    pm_image_desc_encode(a, false, raw);
    if (a->magic != PM_NODE_IMAGE_MAGIC || a->format != PM_NODE_IMAGE_FORMAT || a->size != PM_IMAGE_DESC_SIZE ||
        a->desc_crc32 != pm_crc32(0, raw, 60)) {
        return PM_FW_ERROR_DESC_MAGIC;
    }
    if (a->esta_manufacturer != own_esta) return PM_FW_ERROR_DESC_MANUFACTURER;
    if (a->device_model_id != PM_MODEL_NODE) return PM_FW_ERROR_DESC_MODEL;
    if (own_hw_rev >= 32 || !(a->hw_rev_mask & (1u << own_hw_rev))) return PM_FW_ERROR_DESC_HW_REV;
    if (a->image_length == 0 || a->image_length > max_image_len) return PM_FW_ERROR_TOO_LARGE;
    return PM_FW_ERROR_NONE;
}

bool pm_image_desc_matches(const pm_image_desc_t *in_image, const pm_image_desc_t *announced)
{
    uint8_t x[PM_IMAGE_DESC_SIZE], y[PM_IMAGE_DESC_SIZE];
    pm_image_desc_encode(in_image, false, x);
    pm_image_desc_encode(announced, false, y);
    return memcmp(x, y, 20) == 0 && memcmp(x + 28, y + 28, 32) == 0;
}
