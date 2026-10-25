/* pm_types.c - UID helpers, error names, checksums (pure C11). */
#include "pm_types.h"

#include <stdio.h>
#include <string.h>

const char *pm_err_name(pm_err_t err)
{
    switch (err) {
    case PM_OK: return "PM_OK";
    case PM_ERR_INVALID_ARG: return "PM_ERR_INVALID_ARG";
    case PM_ERR_SHORT: return "PM_ERR_SHORT";
    case PM_ERR_BAD_ID: return "PM_ERR_BAD_ID";
    case PM_ERR_BAD_CHECKSUM: return "PM_ERR_BAD_CHECKSUM";
    case PM_ERR_BAD_FORMAT: return "PM_ERR_BAD_FORMAT";
    case PM_ERR_NO_SPACE: return "PM_ERR_NO_SPACE";
    case PM_ERR_FULL: return "PM_ERR_FULL";
    case PM_ERR_NOT_FOUND: return "PM_ERR_NOT_FOUND";
    case PM_ERR_STATE: return "PM_ERR_STATE";
    case PM_ERR_UNSUPPORTED: return "PM_ERR_UNSUPPORTED";
    case PM_ERR_NOT_IMPLEMENTED: return "PM_ERR_NOT_IMPLEMENTED";
    case PM_ERR_IO: return "PM_ERR_IO";
    case PM_ERR_TIMEOUT: return "PM_ERR_TIMEOUT";
    case PM_ERR_REJECTED: return "PM_ERR_REJECTED";
    }
    return "PM_ERR_?";
}

pm_uid_t pm_uid_make(uint16_t manufacturer, uint32_t device)
{
    pm_uid_t u;
    pm_put_be16(u.b, manufacturer);
    pm_put_be32(u.b + 2, device);
    return u;
}

uint16_t pm_uid_manufacturer(const pm_uid_t *uid) { return pm_get_be16(uid->b); }
uint32_t pm_uid_device(const pm_uid_t *uid) { return pm_get_be32(uid->b + 2); }

uint64_t pm_uid_to_u64(const pm_uid_t *uid)
{
    return ((uint64_t)pm_uid_manufacturer(uid) << 32) | pm_uid_device(uid);
}

pm_uid_t pm_uid_from_u64(uint64_t v) { return pm_uid_make((uint16_t)(v >> 32), (uint32_t)v); }

bool pm_uid_equal(const pm_uid_t *a, const pm_uid_t *b) { return memcmp(a->b, b->b, 6) == 0; }

bool pm_uid_is_broadcast(const pm_uid_t *uid)
{
    return pm_uid_manufacturer(uid) == 0xFFFFu && pm_uid_device(uid) == 0xFFFFFFFFu;
}

bool pm_uid_is_vendorcast(const pm_uid_t *uid) { return pm_uid_device(uid) == 0xFFFFFFFFu; }

bool pm_uid_addresses(const pm_uid_t *dest, const pm_uid_t *self)
{
    if (pm_uid_equal(dest, self) || pm_uid_is_broadcast(dest)) {
        return true;
    }
    return pm_uid_is_vendorcast(dest) && pm_uid_manufacturer(dest) == pm_uid_manufacturer(self);
}

char *pm_uid_format(const pm_uid_t *uid, char *buf)
{
    snprintf(buf, PM_UID_STR_LEN, "%04X:%08lX", (unsigned)pm_uid_manufacturer(uid),
             (unsigned long)pm_uid_device(uid));
    return buf;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

pm_err_t pm_uid_parse(const char *s, pm_uid_t *out)
{
    if (s == NULL || out == NULL || strlen(s) != 13 || s[4] != ':') {
        return PM_ERR_INVALID_ARG;
    }
    uint64_t v = 0;
    for (int i = 0; i < 13; i++) {
        if (i == 4) continue;
        int h = hexval(s[i]);
        if (h < 0) return PM_ERR_INVALID_ARG;
        v = (v << 4) | (uint64_t)h;
    }
    *out = pm_uid_from_u64(v);
    return PM_OK;
}

uint32_t pm_ip_artnet_fallback(const uint8_t mac[6], uint16_t oem)
{
    /* Art-Net 4 p. 13: B = MAC[3] + OemHi + OemLo; the spec does not say "mod 256", a byte can only hold that. */
    uint8_t b = (uint8_t)(mac[3] + (oem >> 8) + (oem & 0xFFu));
    return (2u << 24) | ((uint32_t)b << 16) | ((uint32_t)mac[4] << 8) | mac[5];
}

pm_uid_t pm_uid_node_from_mac(uint16_t esta, const uint8_t mac[6])
{
    uint32_t low28 = ((uint32_t)(mac[2] & 0x0Fu) << 24) | ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    return pm_uid_make(esta, 0x80000000u | low28);
}

uint32_t pm_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

uint16_t pm_crc16_ccitt(uint16_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    while (len--) {
        crc ^= (uint16_t)(*p++ << 8);
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
