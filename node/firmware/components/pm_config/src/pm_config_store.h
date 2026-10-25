/*
 * pm_config_store.h - NVS blob codec of pm_settings_t with schema migration (pure C11, no ESP-IDF).
 *
 * The struct is never copied to flash as is. Every field is serialized on its own (little-endian) into a
 * self-describing, versioned blob so that any later firmware can read what an older (or newer) one wrote.
 *
 * Blob layout (all integers little-endian):
 *
 *   offset size
 *   0      4    magic: PM_CONFIG_STORE_MAGIC_MAIN ("PMCM") or PM_CONFIG_STORE_MAGIC_SCENE ("PMCS")
 *   4      2    schema version (PM_CONFIG_STORE_SCHEMA when written by this firmware)
 *   6      2    payload length N
 *   8      4    CRC-32 (pm_crc32) over bytes 4..7 (schema + length) followed by the payload
 *   12     N    records: tag u16, len u16, value[len] - repeated until the payload ends
 *
 * Two kinds of blobs:
 *   - "main": everything except the failsafe scenes (<= PM_CONFIG_STORE_MAIN_MAX bytes).
 *   - "scene": one per port, record PM_CFG_TAG_SCENE_DATA (scene_len bytes); header only when scene_len == 0
 *     (<= PM_CONFIG_STORE_SCENE_MAX bytes). Normal changes therefore never rewrite 2 KB of scenes.
 *
 * Decoding starts from pm_settings_defaults(), applies every known tag (length-checked: integers may be 1..4
 * bytes wide, a value that does not fit the field is ignored), skips unknown tags, runs the migration steps from
 * the blob schema up to PM_CONFIG_STORE_SCHEMA and finally pm_settings_sanitize(). A missing tag keeps its
 * default. A broken blob (magic, length, CRC, record overrun) returns an error and leaves defaults.
 *
 * Compatibility rules / how to evolve the format:
 *   - New field: give it a NEW tag. No schema bump. Older firmware skips the tag, newer firmware uses the default
 *     when the tag is missing.
 *   - Never reuse or change the meaning of an existing tag without a schema bump.
 *   - Changed meaning/unit/encoding of an existing tag, a removed tag whose value must move somewhere else, or a
 *     new default that must also apply to old stored data: bump the schema and add a step. To add v2:
 *       1. #define PM_CONFIG_STORE_SCHEMA 2u
 *       2. write `static void migrate_v1_to_v2(pm_settings_t *s, const uint8_t *payload, size_t len)` in
 *          pm_config_store.c. It receives the settings as decoded with the CURRENT tag table and the raw record
 *          payload of the blob (use pm_config_store_find() to read removed/re-defined tags).
 *       3. append it to s_main_steps[] (index = source schema: [1] = v1 -> v2).
 *       4. add a host test with a hand-built v1 blob (see test_config_store.c, "migration v0").
 *   - A blob with a NEWER schema (firmware downgrade) is decoded best effort with the known tags, no steps run;
 *     the result reports `schema > PM_CONFIG_STORE_SCHEMA` so the caller can log it.
 *
 * Schema history:
 *   v0  pre-release prototype builds: DISPLAY_LEVEL stored in percent (0..100), one node-wide failsafe mode
 *       (tag 0x0030) instead of per port.
 *   v1  current.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "pm_settings.h"
#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_CONFIG_STORE_MAGIC_MAIN 0x4D434D50u  /**< "PMCM" as bytes in the blob */
#define PM_CONFIG_STORE_MAGIC_SCENE 0x53434D50u /**< "PMCS" as bytes in the blob */
#define PM_CONFIG_STORE_SCHEMA 1u               /**< schema written by this firmware */
#define PM_CONFIG_STORE_HDR_LEN 12u
#define PM_CONFIG_STORE_REC_HDR_LEN 4u          /**< tag u16 + len u16 */

/** Tags of the main blob. Port tags: PM_CFG_TAG_PORT(port, PM_CFG_PTAG_xxx). Values: integers little-endian,
 *  bools/enums 1 byte, strings without NUL. */
enum {
    PM_CFG_TAG_SHORT_NAME = 0x0001,
    PM_CFG_TAG_LONG_NAME = 0x0002,
    PM_CFG_TAG_IP_DHCP = 0x0010,
    PM_CFG_TAG_IP_ADDR = 0x0011,
    PM_CFG_TAG_IP_MASK = 0x0012,
    PM_CFG_TAG_IP_GATEWAY = 0x0013,
    PM_CFG_TAG_RDM_BG_INTERVAL = 0x0020,
    PM_CFG_TAG_DISPLAY_LEVEL = 0x0021,
    PM_CFG_TAG_DISPLAY_DIM_TIMEOUT = 0x0022,
    PM_CFG_TAG_ADDRESS_FORMAT = 0x0023,
    PM_CFG_TAG_PANEL_LOCK = 0x0024,
    PM_CFG_TAG_HW_REV_OVERRIDE = 0x0025,
    PM_CFG_TAG_V0_FAILSAFE = 0x0030, /**< schema 0 only (node-wide failsafe), read by the v0 -> v1 step */

    PM_CFG_PTAG_ENABLED = 0x01,
    PM_CFG_PTAG_DIRECTION = 0x02,
    PM_CFG_PTAG_ADDRESS = 0x03,
    PM_CFG_PTAG_MERGE = 0x04,
    PM_CFG_PTAG_RDM_ENABLED = 0x05,
    PM_CFG_PTAG_RDM_BG_DISCOVERY = 0x06,
    PM_CFG_PTAG_OUTPUT_STYLE = 0x07,
    PM_CFG_PTAG_FAILSAFE = 0x08,
    PM_CFG_PTAG_DMX_BREAK = 0x09,
    PM_CFG_PTAG_DMX_MAB = 0x0A,
    PM_CFG_PTAG_DMX_RATE = 0x0B,

    PM_CFG_TAG_SCENE_DATA = 0x0001, /**< scene blob: the scene_len bytes of the failsafe scene */
};
/** Tag of per-port field `ptag` of port index `port` (0-based): 0x1000 | port << 8 | ptag. */
#define PM_CFG_TAG_PORT(port, ptag) ((uint16_t)(0x1000u | ((unsigned)(port) << 8) | (unsigned)(ptag)))

/** Upper bound of an encoded main blob (both names at maximum length). */
#define PM_CONFIG_STORE_MAIN_MAX                                                                         \
    (PM_CONFIG_STORE_HDR_LEN + 2u * PM_CONFIG_STORE_REC_HDR_LEN + (PM_SHORT_NAME_LEN - 1u) +           \
     (PM_LONG_NAME_LEN - 1u) + 4u * PM_CONFIG_STORE_REC_HDR_LEN + 1u + 3u * 4u +                        \
     6u * PM_CONFIG_STORE_REC_HDR_LEN + 2u + 1u + 2u + 1u + 1u + 1u +                                   \
     PM_NUM_PORTS * (11u * PM_CONFIG_STORE_REC_HDR_LEN + 8u * 1u + 3u * 2u))
/** Upper bound of an encoded scene blob. */
#define PM_CONFIG_STORE_SCENE_MAX (PM_CONFIG_STORE_HDR_LEN + PM_CONFIG_STORE_REC_HDR_LEN + 512u)

/**
 * Encode everything except the scenes into `buf`.
 * @return number of bytes written, 0 when `cap` is too small (PM_CONFIG_STORE_MAIN_MAX is always enough).
 */
size_t pm_config_store_encode_main(const pm_settings_t *s, uint8_t *buf, size_t cap);

/**
 * Decode a main blob into `out`. `out` is first set to pm_settings_defaults() (scenes empty) - also on error, so
 * the caller can use it as the fallback directly. Runs the migration steps and pm_settings_sanitize().
 * @param schema_out  optional: schema version found in the blob (valid when PM_OK)
 * @return PM_OK, PM_ERR_SHORT (shorter than the header / payload length), PM_ERR_BAD_ID (magic),
 *         PM_ERR_BAD_CHECKSUM (CRC), PM_ERR_BAD_FORMAT (record overruns the payload), PM_ERR_INVALID_ARG.
 */
pm_err_t pm_config_store_decode_main(const uint8_t *buf, size_t len, pm_settings_t *out, uint16_t *schema_out);

/**
 * Encode the failsafe scene of one port (scene_len bytes; header only when scene_len == 0).
 * @return number of bytes written, 0 when `cap` is too small (PM_CONFIG_STORE_SCENE_MAX is always enough).
 */
size_t pm_config_store_encode_scene(const pm_port_settings_t *p, uint8_t *buf, size_t cap);

/**
 * Decode a scene blob into `p->scene_len` / `p->scene` (other fields untouched). On error the scene is empty.
 * @return as pm_config_store_decode_main().
 */
pm_err_t pm_config_store_decode_scene(const uint8_t *buf, size_t len, pm_port_settings_t *p);

/**
 * Find the first record with `tag` in a record payload (bytes after the header). For migration steps.
 * @return true when found; *val / *val_len point into `payload`.
 */
bool pm_config_store_find(const uint8_t *payload, size_t len, uint16_t tag, const uint8_t **val, uint16_t *val_len);

/** Read a 1..4 byte little-endian unsigned record value. Returns false for other lengths. */
bool pm_config_store_get_uint(const uint8_t *val, uint16_t val_len, uint32_t *out);

#ifdef __cplusplus
}
#endif
