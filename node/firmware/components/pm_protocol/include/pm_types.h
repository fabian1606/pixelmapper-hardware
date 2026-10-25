/*
 * pm_types.h - common types of the Pixelmapper Node firmware (pure C11, no ESP-IDF).
 *
 * Shared by pm_artnet, pm_rdm (host-testable) and all ESP-IDF components.
 * Numbers that Pixelmapper must know live in pm_pids.h (generated from protocol/pids.toml).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ errors */

/** Error codes of all pm_* components (0 = success, negative = error). ESP-IDF components map esp_err_t
 *  results to PM_ERR_IO and log the original code. */
typedef enum {
    PM_OK = 0,
    PM_ERR_INVALID_ARG = -1,     /**< argument out of range / NULL */
    PM_ERR_SHORT = -2,           /**< packet shorter than the minimum length */
    PM_ERR_BAD_ID = -3,          /**< wrong Art-Net ID / RDM start code / unknown opcode */
    PM_ERR_BAD_CHECKSUM = -4,    /**< RDM / DUB checksum wrong */
    PM_ERR_BAD_FORMAT = -5,      /**< length fields inconsistent, protocol version too old, ... */
    PM_ERR_NO_SPACE = -6,        /**< output buffer too small */
    PM_ERR_FULL = -7,            /**< queue / table full */
    PM_ERR_NOT_FOUND = -8,
    PM_ERR_STATE = -9,           /**< call not allowed in the current state */
    PM_ERR_UNSUPPORTED = -10,
    PM_ERR_NOT_IMPLEMENTED = -11,/**< Phase 0 stub */
    PM_ERR_IO = -12,             /**< platform / driver error */
    PM_ERR_TIMEOUT = -13,
    PM_ERR_REJECTED = -14,       /**< content check failed (image description, OEM, ...) */
} pm_err_t;

/** Short constant name of an error code ("PM_ERR_SHORT"), never NULL. */
const char *pm_err_name(pm_err_t err);

/* ------------------------------------------------------------------------------------------------ ports */

/** Number of DMX ports of hardware revision 0. Arrays are sized with this. */
#define PM_NUM_PORTS 4u

/** Port index 0..PM_NUM_PORTS-1 inside the firmware. User-visible port numbers (RDM PIDs, UI) are 1-based. */
typedef uint8_t pm_port_idx_t;

/* ------------------------------------------------------------------------------------------------ Port-Address */

/** Art-Net 15-bit Port-Address: bit 14-8 Net, 7-4 Sub-Net, 3-0 Universe. 0 is deprecated (Art-Net 4). */
typedef uint16_t pm_port_address_t;

#define PM_PA_MAX 0x7FFFu
#define PM_PA_MAKE(net, sub, uni) \
    ((pm_port_address_t)((((net) & 0x7Fu) << 8) | (((sub) & 0x0Fu) << 4) | ((uni) & 0x0Fu)))
#define PM_PA_NET(pa) ((uint8_t)(((pa) >> 8) & 0x7Fu))
#define PM_PA_SUB(pa) ((uint8_t)(((pa) >> 4) & 0x0Fu))
#define PM_PA_UNI(pa) ((uint8_t)((pa) & 0x0Fu))

/* ------------------------------------------------------------------------------------------------ RDM UID */

/** RDM UID, 6 bytes, network order (manufacturer hi, lo, device id MSB..LSB). */
typedef struct {
    uint8_t b[6];
} pm_uid_t;

#define PM_UID_STR_LEN 14 /**< "MMMM:DDDDDDDD" + NUL */

pm_uid_t pm_uid_make(uint16_t manufacturer, uint32_t device);
uint16_t pm_uid_manufacturer(const pm_uid_t *uid);
uint32_t pm_uid_device(const pm_uid_t *uid);
/** 48-bit value (manufacturer << 32 | device) - for ordering and discovery ranges. */
uint64_t pm_uid_to_u64(const pm_uid_t *uid);
pm_uid_t pm_uid_from_u64(uint64_t v);
bool pm_uid_equal(const pm_uid_t *a, const pm_uid_t *b);
/** FFFF:FFFFFFFF */
bool pm_uid_is_broadcast(const pm_uid_t *uid);
/** mmmm:FFFFFFFF (manufacturer broadcast, "vendorcast"), including FFFF:FFFFFFFF. */
bool pm_uid_is_vendorcast(const pm_uid_t *uid);
/** True when a message to `dest` must be processed by the device `self` (exact, broadcast or own vendorcast). */
bool pm_uid_addresses(const pm_uid_t *dest, const pm_uid_t *self);
/** Format as "MMMM:DDDDDDDD" (upper case). buf must hold PM_UID_STR_LEN bytes. Returns buf. */
char *pm_uid_format(const pm_uid_t *uid, char *buf);
/** Parse "MMMM:DDDDDDDD" (case-insensitive). */
pm_err_t pm_uid_parse(const char *s, pm_uid_t *out);

/** Node UID from the Ethernet MAC: ESTA:(0x80000000 | lower 28 bits of the MAC). */
pm_uid_t pm_uid_node_from_mac(uint16_t esta, const uint8_t mac[6]);

/** Art-Net 4 fallback address 2.B.C.D (host order): B = MAC[3] + OemHi + OemLo (mod 256), C = MAC[4], D = MAC[5]
 *  (contract §2). The address a static configuration with static_ip = 0 uses. */
uint32_t pm_ip_artnet_fallback(const uint8_t mac[6], uint16_t oem);

/* ------------------------------------------------------------------------------------------------ big-endian helpers */

static inline uint16_t pm_get_be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t pm_get_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint16_t pm_get_le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t pm_get_le32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void pm_put_be16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline void pm_put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static inline void pm_put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void pm_put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* ------------------------------------------------------------------------------------------------ checksums */

/** CRC-32/ISO-HDLC (zlib): reflected poly 0x04C11DB7, init/xorout 0xFFFFFFFF. Chainable: pass the previous
 *  result as `crc` (start with 0). */
uint32_t pm_crc32(uint32_t crc, const void *data, size_t len);
/** CRC16-CCITT as used by the tube trailer / BOOT_APP_CRC: poly 0x1021, not reflected. Start with 0xFFFF. */
uint16_t pm_crc16_ccitt(uint16_t crc, const void *data, size_t len);

/* ------------------------------------------------------------------------------------------------ firmware version */

/** FW_VERSION_ID 0x00MMmmpp (same scheme as the tubes; DEVICE_INFO software version, image description). */
#define PM_FW_VERSION_ID(major, minor, patch) \
    ((uint32_t)(((major) & 0xFFu) << 16) | (((minor) & 0xFFu) << 8) | ((patch) & 0xFFu))
/** Art-Net VersInfo = (major << 8) | minor. */
#define PM_FW_VERSINFO(id) ((uint16_t)(((id) >> 8) & 0xFFFFu))

#ifdef __cplusplus
}
#endif
