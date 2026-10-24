/* test_config_store.c - host tests of the pm_config NVS blob codec (pm_config_store.c). */
#include <string.h>

#include "pm_config_store.h"
#include "test.h"

void test_config_store(void);

/* ------------------------------------------------------------------------------------------------ helpers */

/** Hand-built blob: records are appended with rec_*(), finished with build(). */
typedef struct {
    uint8_t buf[1024];
    size_t pos;
} blob_t;

static void blob_init(blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->pos = PM_CONFIG_STORE_HDR_LEN;
}

static void rec(blob_t *b, uint16_t tag, const void *val, uint16_t len)
{
    pm_put_le16(&b->buf[b->pos], tag);
    pm_put_le16(&b->buf[b->pos + 2], len);
    memcpy(&b->buf[b->pos + 4], val, len);
    b->pos += 4u + len;
}

static void rec_u8(blob_t *b, uint16_t tag, uint8_t v) { rec(b, tag, &v, 1); }

static void rec_u16(blob_t *b, uint16_t tag, uint16_t v)
{
    uint8_t x[2];
    pm_put_le16(x, v);
    rec(b, tag, x, 2);
}

static void rec_u32(blob_t *b, uint16_t tag, uint32_t v)
{
    uint8_t x[4];
    pm_put_le32(x, v);
    rec(b, tag, x, 4);
}

static size_t build(blob_t *b, uint32_t magic, uint16_t schema)
{
    pm_put_le32(&b->buf[0], magic);
    pm_put_le16(&b->buf[4], schema);
    pm_put_le16(&b->buf[6], (uint16_t)(b->pos - PM_CONFIG_STORE_HDR_LEN));
    uint32_t crc = pm_crc32(0, &b->buf[4], 4);
    crc = pm_crc32(crc, &b->buf[PM_CONFIG_STORE_HDR_LEN], b->pos - PM_CONFIG_STORE_HDR_LEN);
    pm_put_le32(&b->buf[8], crc);
    return b->pos;
}

static bool is_defaults(const pm_settings_t *s)
{
    static pm_settings_t d;
    pm_settings_defaults(&d);
    return pm_settings_diff(&d, s) == 0 && s->version == d.version;
}

/** Every field different from its default, names at maximum length, scenes of different lengths. */
static void make_custom(pm_settings_t *s)
{
    pm_settings_defaults(s);
    memset(s->short_name, 'S', PM_SHORT_NAME_LEN - 1);
    s->short_name[0] = 'x';
    memset(s->long_name, 'L', PM_LONG_NAME_LEN - 1);
    s->long_name[PM_LONG_NAME_LEN - 2] = 'z';
    s->ip.dhcp = false;
    s->ip.static_ip = 0x0A00002Au;   /* 10.0.0.42 */
    s->ip.static_mask = 0xFFFFFF00u;
    s->ip.gateway = 0x0A000001u;
    s->rdm_bg_interval_s = 600;
    s->display_level = 7;
    s->display_dim_timeout_s = 3600;
    s->address_format = 1;
    s->panel_lock = true;
    s->hw_revision_override = 2;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        pm_port_settings_t *p = &s->port[i];
        p->enabled = false;
        p->direction = PM_PORT_DIR_INPUT;
        p->address = (pm_port_address_t)(0x7F00u + i);
        p->merge = PM_MERGE_LTP;
        p->rdm_enabled = false;
        p->rdm_bg_discovery = false;
        p->output_style = PM_OUTPUT_DELTA;
        p->failsafe = (pm_failsafe_t)(PM_FAILSAFE_ZERO + (i % 3u));
        p->dmx_break_us = (uint16_t)(352 - i);
        p->dmx_mab_us = (uint16_t)(88 - i);
        p->dmx_rate_hz = (uint8_t)(1 + i);
    }
    static const uint16_t lens[PM_NUM_PORTS] = {0, 1, 512, 7};
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        s->port[i].scene_len = lens[i];
        for (unsigned k = 0; k < lens[i]; k++) s->port[i].scene[k] = (uint8_t)(k * 7u + i + 1u);
    }
}

/* ------------------------------------------------------------------------------------------------ tests */

static void test_round_trip(void)
{
    g_context = "config round trip";
    static pm_settings_t a, b;
    static uint8_t buf[PM_CONFIG_STORE_MAIN_MAX];
    make_custom(&a);
    CHECK(!pm_settings_sanitize(&a)); /* the custom values are all valid */

    size_t n = pm_config_store_encode_main(&a, buf, sizeof(buf));
    CHECK_EQ(n, PM_CONFIG_STORE_MAIN_MAX); /* max-length names -> exactly the bound */
    uint16_t schema = 0xFFFF;
    memset(&b, 0xA5, sizeof(b));
    CHECK_EQ(pm_config_store_decode_main(buf, n, &b, &schema), PM_OK);
    CHECK_EQ(schema, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(b.version, PM_SETTINGS_VERSION);
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) CHECK_EQ(b.port[i].scene_len, 0); /* main has no scenes */

    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        static uint8_t sb[PM_CONFIG_STORE_SCENE_MAX];
        size_t sn = pm_config_store_encode_scene(&a.port[i], sb, sizeof(sb));
        CHECK_EQ(sn, PM_CONFIG_STORE_HDR_LEN + (a.port[i].scene_len ? 4u + a.port[i].scene_len : 0u));
        CHECK_EQ(pm_config_store_decode_scene(sb, sn, &b.port[i]), PM_OK);
    }
    CHECK_EQ(pm_settings_diff(&a, &b), 0);
    CHECK(strcmp(a.short_name, b.short_name) == 0);
    CHECK_EQ(strlen(b.short_name), PM_SHORT_NAME_LEN - 1);
    CHECK_EQ(strlen(b.long_name), PM_LONG_NAME_LEN - 1);
    CHECK_EQ(b.ip.static_ip, 0x0A00002Au);
    CHECK_EQ(b.port[2].scene_len, 512);
    CHECK_EQ(b.port[2].scene[511], (uint8_t)(511u * 7u + 3u));
    CHECK(memcmp(&a.port, &b.port, sizeof(a.port)) == 0);

    /* defaults also round-trip, and encoding is deterministic */
    static uint8_t buf2[PM_CONFIG_STORE_MAIN_MAX];
    pm_settings_defaults(&a);
    n = pm_config_store_encode_main(&a, buf, sizeof(buf));
    CHECK(n > PM_CONFIG_STORE_HDR_LEN && n < PM_CONFIG_STORE_MAIN_MAX);
    CHECK_EQ(pm_config_store_encode_main(&a, buf2, sizeof(buf2)), n);
    CHECK(memcmp(buf, buf2, n) == 0);
    CHECK_EQ(pm_config_store_decode_main(buf, n, &b, NULL), PM_OK);
    CHECK(is_defaults(&b));
}

static void test_size_bounds(void)
{
    g_context = "config size bounds";
    static pm_settings_t s;
    static uint8_t buf[PM_CONFIG_STORE_MAIN_MAX + PM_CONFIG_STORE_SCENE_MAX];
    CHECK_EQ(PM_CONFIG_STORE_MAIN_MAX, 393);
    CHECK_EQ(PM_CONFIG_STORE_SCENE_MAX, 528);
    make_custom(&s);
    CHECK_EQ(pm_config_store_encode_main(&s, buf, PM_CONFIG_STORE_MAIN_MAX - 1), 0); /* too small */
    CHECK_EQ(pm_config_store_encode_main(&s, buf, 4), 0);
    CHECK_EQ(pm_config_store_encode_main(&s, NULL, 1000), 0);
    CHECK_EQ(pm_config_store_encode_scene(&s.port[2], buf, PM_CONFIG_STORE_SCENE_MAX), PM_CONFIG_STORE_SCENE_MAX);
    CHECK_EQ(pm_config_store_encode_scene(&s.port[2], buf, PM_CONFIG_STORE_SCENE_MAX - 1), 0);
    CHECK_EQ(pm_config_store_encode_scene(&s.port[0], buf, PM_CONFIG_STORE_HDR_LEN), PM_CONFIG_STORE_HDR_LEN);
    s.port[3].scene_len = 600; /* invalid length: clamped to 512 on encode */
    CHECK_EQ(pm_config_store_encode_scene(&s.port[3], buf, sizeof(buf)), PM_CONFIG_STORE_SCENE_MAX);
}

static void test_unknown_and_missing(void)
{
    g_context = "config unknown/missing tags";
    static pm_settings_t s;
    static blob_t b;
    blob_init(&b);
    rec(&b, 0x7777, "hello", 5);                                    /* unknown global tag */
    rec(&b, PM_CFG_TAG_SHORT_NAME, "Stage Left", 10);
    rec_u8(&b, 0x10FF, 9);                                          /* unknown port field */
    rec_u16(&b, PM_CFG_TAG_PORT(7, PM_CFG_PTAG_ADDRESS), 0x123);    /* port index out of range */
    rec_u16(&b, PM_CFG_TAG_PORT(1, PM_CFG_PTAG_ADDRESS), 0x0234);
    rec(&b, 0x2001, NULL, 0);                                       /* unknown, empty value */
    rec_u8(&b, PM_CFG_TAG_IP_DHCP, 0);
    size_t n = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_main(b.buf, n, &s, NULL), PM_OK);
    CHECK(strcmp(s.short_name, "Stage Left") == 0);
    CHECK_EQ(s.port[1].address, 0x0234);
    CHECK_EQ(s.ip.dhcp, false);
    /* everything not in the blob keeps its default */
    CHECK(strcmp(s.long_name, "Pixelmapper Node") == 0);
    CHECK_EQ(s.port[0].address, 1);
    CHECK_EQ(s.port[3].address, 4);
    CHECK_EQ(s.ip.static_mask, 0xFF000000u);
    CHECK_EQ(s.display_level, 255);
    CHECK_EQ(s.hw_revision_override, 0xFF);
    CHECK_EQ(s.port[1].dmx_rate_hz, 40);

    /* empty payload = all defaults */
    blob_init(&b);
    n = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_main(b.buf, n, &s, NULL), PM_OK);
    CHECK(is_defaults(&s));

    /* unknown record in a scene blob is skipped */
    static pm_port_settings_t p;
    blob_init(&b);
    rec(&b, 0x0099, "xx", 2);
    rec(&b, PM_CFG_TAG_SCENE_DATA, "\x01\x02\x03", 3);
    n = build(&b, PM_CONFIG_STORE_MAGIC_SCENE, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_scene(b.buf, n, &p), PM_OK);
    CHECK_EQ(p.scene_len, 3);
    CHECK_EQ(p.scene[2], 3);
    CHECK_EQ(p.scene[3], 0);
}

static void test_corrupt(void)
{
    g_context = "config corrupt";
    static pm_settings_t a, s;
    static uint8_t buf[PM_CONFIG_STORE_MAIN_MAX];
    make_custom(&a);
    size_t n = pm_config_store_encode_main(&a, buf, sizeof(buf));

    CHECK_EQ(pm_config_store_decode_main(buf, n - 1, &s, NULL), PM_ERR_SHORT); /* truncated */
    CHECK(is_defaults(&s));                                                   /* fallback in `out` */
    CHECK_EQ(pm_config_store_decode_main(buf, 11, &s, NULL), PM_ERR_SHORT);
    CHECK_EQ(pm_config_store_decode_main(buf, 0, &s, NULL), PM_ERR_SHORT);
    CHECK_EQ(pm_config_store_decode_main(NULL, n, &s, NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_config_store_decode_main(buf, n, NULL, NULL), PM_ERR_INVALID_ARG);

    buf[40] ^= 0x01; /* bit flip in the payload */
    CHECK_EQ(pm_config_store_decode_main(buf, n, &s, NULL), PM_ERR_BAD_CHECKSUM);
    CHECK(is_defaults(&s));
    buf[40] ^= 0x01;
    buf[4] ^= 0x01; /* schema field is covered by the CRC too */
    CHECK_EQ(pm_config_store_decode_main(buf, n, &s, NULL), PM_ERR_BAD_CHECKSUM);
    buf[4] ^= 0x01;
    buf[0] = 'X';
    CHECK_EQ(pm_config_store_decode_main(buf, n, &s, NULL), PM_ERR_BAD_ID);
    buf[0] = 'P';
    CHECK_EQ(pm_config_store_decode_main(buf, n, &s, NULL), PM_OK);
    /* a main blob is not a scene blob and vice versa */
    static pm_port_settings_t p;
    p.scene_len = 5;
    CHECK_EQ(pm_config_store_decode_scene(buf, n, &p), PM_ERR_BAD_ID);
    CHECK_EQ(p.scene_len, 0);

    /* record overruns the payload, CRC valid */
    static blob_t b;
    blob_init(&b);
    rec_u8(&b, PM_CFG_TAG_PANEL_LOCK, 1);
    pm_put_le16(&b.buf[b.pos], PM_CFG_TAG_DISPLAY_LEVEL);
    pm_put_le16(&b.buf[b.pos + 2], 10); /* claims 10 bytes, only 2 follow */
    b.pos += 6;
    size_t bn = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_main(b.buf, bn, &s, NULL), PM_ERR_BAD_FORMAT);
    CHECK_EQ(s.panel_lock, false); /* nothing applied from a broken blob */

    /* dangling half record header */
    blob_init(&b);
    rec_u8(&b, PM_CFG_TAG_PANEL_LOCK, 1);
    b.pos += 3;
    bn = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_main(b.buf, bn, &s, NULL), PM_ERR_BAD_FORMAT);

    /* trailing bytes after the declared payload are ignored (caller passes a larger buffer) */
    blob_init(&b);
    rec_u8(&b, PM_CFG_TAG_PANEL_LOCK, 1);
    bn = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_main(b.buf, bn + 20, &s, NULL), PM_OK);
    CHECK_EQ(s.panel_lock, true);
}

static void test_sanitize(void)
{
    g_context = "config sanitize";
    static pm_settings_t s;
    static blob_t b;
    static const char long_short[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123"; /* 30 chars > 17 */
    blob_init(&b);
    rec(&b, PM_CFG_TAG_SHORT_NAME, long_short, 30);
    rec(&b, PM_CFG_TAG_LONG_NAME, "ab\0cd", 5);                      /* embedded NUL ends the string */
    rec_u16(&b, PM_CFG_TAG_PORT(0, PM_CFG_PTAG_DMX_BREAK), 1000);    /* > 352 */
    rec_u8(&b, PM_CFG_TAG_PORT(0, PM_CFG_PTAG_DMX_RATE), 0);         /* < 1 */
    rec_u16(&b, PM_CFG_TAG_PORT(0, PM_CFG_PTAG_DMX_MAB), 2);         /* < 12 */
    rec_u8(&b, PM_CFG_TAG_PORT(1, PM_CFG_PTAG_DIRECTION), 9);
    rec_u8(&b, PM_CFG_TAG_PORT(1, PM_CFG_PTAG_FAILSAFE), 200);
    rec_u8(&b, PM_CFG_TAG_PORT(1, PM_CFG_PTAG_MERGE), 3);
    rec_u16(&b, PM_CFG_TAG_PORT(2, PM_CFG_PTAG_ADDRESS), 0x9234);    /* > 15 bit */
    rec_u32(&b, PM_CFG_TAG_PORT(3, PM_CFG_PTAG_DMX_BREAK), 70000);   /* does not fit u16: ignored */
    rec_u16(&b, PM_CFG_TAG_PORT(3, PM_CFG_PTAG_DMX_RATE), 300);      /* does not fit u8: ignored */
    rec_u16(&b, PM_CFG_TAG_PORT(3, PM_CFG_PTAG_DMX_MAB), 20);
    rec(&b, PM_CFG_TAG_PORT(3, PM_CFG_PTAG_ADDRESS), "\x01\x02\x03\x04\x05", 5); /* bad width: ignored */
    rec_u16(&b, PM_CFG_TAG_RDM_BG_INTERVAL, 0);
    rec_u16(&b, PM_CFG_TAG_DISPLAY_DIM_TIMEOUT, 9999);
    rec_u8(&b, PM_CFG_TAG_ADDRESS_FORMAT, 77);
    rec_u8(&b, PM_CFG_TAG_PANEL_LOCK, 5);                             /* any non-zero = true */
    rec_u16(&b, PM_CFG_TAG_DISPLAY_LEVEL, 0x0080);                    /* wider than stored but fits */
    size_t n = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_main(b.buf, n, &s, NULL), PM_OK);
    CHECK_EQ(strlen(s.short_name), PM_SHORT_NAME_LEN - 1);
    CHECK(strncmp(s.short_name, long_short, PM_SHORT_NAME_LEN - 1) == 0);
    CHECK(strcmp(s.long_name, "ab") == 0);
    CHECK_EQ(s.port[0].dmx_break_us, 352);
    CHECK_EQ(s.port[0].dmx_rate_hz, 1);
    CHECK_EQ(s.port[0].dmx_mab_us, 12);
    CHECK_EQ(s.port[1].direction, PM_PORT_DIR_OUTPUT);
    CHECK_EQ(s.port[1].failsafe, PM_FAILSAFE_HOLD);
    CHECK_EQ(s.port[1].merge, PM_MERGE_HTP);
    CHECK(s.port[2].address <= PM_PA_MAX);
    CHECK_EQ(s.port[3].dmx_break_us, 176);
    CHECK_EQ(s.port[3].dmx_rate_hz, 40);
    CHECK_EQ(s.port[3].dmx_mab_us, 20);
    CHECK_EQ(s.port[3].address, 4);
    CHECK_EQ(s.rdm_bg_interval_s, 1);
    CHECK_EQ(s.display_dim_timeout_s, 3600);
    CHECK_EQ(s.address_format, 0);
    CHECK_EQ(s.panel_lock, true);
    CHECK_EQ(s.display_level, 0x80);
    CHECK(!pm_settings_sanitize(&s)); /* result is already sanitized */

    /* oversized scene record is cut to 512 */
    static blob_t sb;
    static uint8_t big[600];
    static pm_port_settings_t p;
    memset(big, 0x55, sizeof(big));
    blob_init(&sb);
    rec(&sb, PM_CFG_TAG_SCENE_DATA, big, sizeof(big));
    n = build(&sb, PM_CONFIG_STORE_MAGIC_SCENE, PM_CONFIG_STORE_SCHEMA);
    CHECK_EQ(pm_config_store_decode_scene(sb.buf, n, &p), PM_OK);
    CHECK_EQ(p.scene_len, 512);
    CHECK_EQ(p.scene[511], 0x55);
}

static void test_migration(void)
{
    g_context = "config migration v0";
    static pm_settings_t s;
    static blob_t b;
    uint16_t schema = 0xFFFF;

    /* v0 (pre-release): DISPLAY_LEVEL in percent, node-wide failsafe tag 0x0030, no per-port failsafe */
    blob_init(&b);
    rec(&b, PM_CFG_TAG_SHORT_NAME, "Old Node", 8);
    rec_u8(&b, PM_CFG_TAG_DISPLAY_LEVEL, 50);
    rec_u8(&b, PM_CFG_TAG_V0_FAILSAFE, PM_FAILSAFE_FULL);
    rec_u16(&b, PM_CFG_TAG_PORT(0, PM_CFG_PTAG_ADDRESS), 0x0010);
    size_t n = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, 0);
    CHECK_EQ(pm_config_store_decode_main(b.buf, n, &s, &schema), PM_OK);
    CHECK_EQ(schema, 0);
    CHECK_EQ(s.version, PM_SETTINGS_VERSION);
    CHECK(strcmp(s.short_name, "Old Node") == 0);
    CHECK_EQ(s.display_level, 128); /* 50 % -> 128/255 */
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) CHECK_EQ(s.port[i].failsafe, PM_FAILSAFE_FULL);
    CHECK_EQ(s.port[0].address, 0x0010);
    CHECK_EQ(s.port[1].address, 2);
    CHECK_EQ(s.display_dim_timeout_s, 60); /* missing -> default */

    /* v0 with an out-of-range percentage and failsafe value: clamped / sanitized */
    blob_init(&b);
    rec_u8(&b, PM_CFG_TAG_DISPLAY_LEVEL, 180);
    rec_u8(&b, PM_CFG_TAG_V0_FAILSAFE, 9);
    n = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, 0);
    CHECK_EQ(pm_config_store_decode_main(b.buf, n, &s, NULL), PM_OK);
    CHECK_EQ(s.display_level, 255);
    CHECK_EQ(s.port[2].failsafe, PM_FAILSAFE_HOLD);

    /* the same records as v1: no step runs, the removed v0 tag is ignored */
    g_context = "config migration v1";
    blob_init(&b);
    rec_u8(&b, PM_CFG_TAG_DISPLAY_LEVEL, 50);
    rec_u8(&b, PM_CFG_TAG_V0_FAILSAFE, PM_FAILSAFE_FULL);
    n = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, 1);
    CHECK_EQ(pm_config_store_decode_main(b.buf, n, &s, &schema), PM_OK);
    CHECK_EQ(schema, 1);
    CHECK_EQ(s.display_level, 50);
    CHECK_EQ(s.port[0].failsafe, PM_FAILSAFE_HOLD);

    /* newer schema (firmware downgrade): best effort with the known tags */
    g_context = "config migration newer";
    blob_init(&b);
    rec_u8(&b, PM_CFG_TAG_DISPLAY_LEVEL, 50);
    rec(&b, 0x0040, "future", 6);
    n = build(&b, PM_CONFIG_STORE_MAGIC_MAIN, 9);
    CHECK_EQ(pm_config_store_decode_main(b.buf, n, &s, &schema), PM_OK);
    CHECK_EQ(schema, 9);
    CHECK_EQ(s.display_level, 50);
}

static void test_find(void)
{
    g_context = "config find";
    static blob_t b;
    blob_init(&b);
    rec_u8(&b, 0x0005, 1);
    rec_u32(&b, 0x0006, 0xA1B2C3D4u);
    const uint8_t *payload = &b.buf[PM_CONFIG_STORE_HDR_LEN];
    size_t len = b.pos - PM_CONFIG_STORE_HDR_LEN;
    const uint8_t *val;
    uint16_t vlen;
    uint32_t v = 0;
    CHECK(pm_config_store_find(payload, len, 0x0006, &val, &vlen));
    CHECK_EQ(vlen, 4);
    CHECK(pm_config_store_get_uint(val, vlen, &v));
    CHECK_EQ(v, 0xA1B2C3D4u);
    CHECK(!pm_config_store_find(payload, len, 0x0007, &val, &vlen));
    CHECK(!pm_config_store_find(payload, len - 1, 0x0006, &val, &vlen)); /* truncated record */
    CHECK(!pm_config_store_get_uint(val, 0, &v));
    CHECK(!pm_config_store_get_uint(val, 5, &v));
}

void test_config_store(void)
{
    test_round_trip();
    test_size_bounds();
    test_unknown_and_missing();
    test_corrupt();
    test_sanitize();
    test_migration();
    test_find();
    g_context = NULL;
}
