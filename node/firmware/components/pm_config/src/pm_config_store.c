/* pm_config_store.c - NVS blob codec of pm_settings_t with schema migration (pure C11), see pm_config_store.h. */
#include "pm_config_store.h"

#include <string.h>

/* ------------------------------------------------------------------------------------------------ writer */

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    bool ok;
} writer_t;

static void put_rec(writer_t *w, uint16_t tag, const void *val, uint16_t len)
{
    if (!w->ok || w->cap - w->pos < (size_t)PM_CONFIG_STORE_REC_HDR_LEN + len) {
        w->ok = false;
        return;
    }
    pm_put_le16(&w->buf[w->pos], tag);
    pm_put_le16(&w->buf[w->pos + 2], len);
    if (len > 0) memcpy(&w->buf[w->pos + 4], val, len);
    w->pos += PM_CONFIG_STORE_REC_HDR_LEN + len;
}

static void put_u8(writer_t *w, uint16_t tag, uint32_t v)
{
    uint8_t b = (uint8_t)v;
    put_rec(w, tag, &b, 1);
}

static void put_u16(writer_t *w, uint16_t tag, uint16_t v)
{
    uint8_t b[2];
    pm_put_le16(b, v);
    put_rec(w, tag, b, 2);
}

static void put_u32(writer_t *w, uint16_t tag, uint32_t v)
{
    uint8_t b[4];
    pm_put_le32(b, v);
    put_rec(w, tag, b, 4);
}

static void put_str(writer_t *w, uint16_t tag, const char *str, size_t cap)
{
    uint16_t n = 0;
    while (n + 1u < cap && str[n] != '\0') n++;
    put_rec(w, tag, str, n);
}

/** Reserve the header; returns false when even that does not fit. */
static bool begin(writer_t *w, uint8_t *buf, size_t cap)
{
    w->buf = buf;
    w->cap = cap;
    w->pos = PM_CONFIG_STORE_HDR_LEN;
    w->ok = buf != NULL && cap >= PM_CONFIG_STORE_HDR_LEN;
    return w->ok;
}

static size_t finish(writer_t *w, uint32_t magic)
{
    if (!w->ok || w->pos - PM_CONFIG_STORE_HDR_LEN > 0xFFFFu) return 0;
    pm_put_le32(&w->buf[0], magic);
    pm_put_le16(&w->buf[4], PM_CONFIG_STORE_SCHEMA);
    pm_put_le16(&w->buf[6], (uint16_t)(w->pos - PM_CONFIG_STORE_HDR_LEN));
    uint32_t crc = pm_crc32(0, &w->buf[4], 4);
    crc = pm_crc32(crc, &w->buf[PM_CONFIG_STORE_HDR_LEN], w->pos - PM_CONFIG_STORE_HDR_LEN);
    pm_put_le32(&w->buf[8], crc);
    return w->pos;
}

size_t pm_config_store_encode_main(const pm_settings_t *s, uint8_t *buf, size_t cap)
{
    writer_t w;
    if (s == NULL || !begin(&w, buf, cap)) return 0;
    put_str(&w, PM_CFG_TAG_SHORT_NAME, s->short_name, sizeof(s->short_name));
    put_str(&w, PM_CFG_TAG_LONG_NAME, s->long_name, sizeof(s->long_name));
    put_u8(&w, PM_CFG_TAG_IP_DHCP, s->ip.dhcp ? 1u : 0u);
    put_u32(&w, PM_CFG_TAG_IP_ADDR, s->ip.static_ip);
    put_u32(&w, PM_CFG_TAG_IP_MASK, s->ip.static_mask);
    put_u32(&w, PM_CFG_TAG_IP_GATEWAY, s->ip.gateway);
    put_u16(&w, PM_CFG_TAG_RDM_BG_INTERVAL, s->rdm_bg_interval_s);
    put_u8(&w, PM_CFG_TAG_DISPLAY_LEVEL, s->display_level);
    put_u16(&w, PM_CFG_TAG_DISPLAY_DIM_TIMEOUT, s->display_dim_timeout_s);
    put_u8(&w, PM_CFG_TAG_ADDRESS_FORMAT, s->address_format);
    put_u8(&w, PM_CFG_TAG_PANEL_LOCK, s->panel_lock ? 1u : 0u);
    put_u8(&w, PM_CFG_TAG_HW_REV_OVERRIDE, s->hw_revision_override);
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        const pm_port_settings_t *p = &s->port[i];
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_ENABLED), p->enabled ? 1u : 0u);
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_DIRECTION), (uint32_t)p->direction);
        put_u16(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_ADDRESS), p->address);
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_MERGE), (uint32_t)p->merge);
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_RDM_ENABLED), p->rdm_enabled ? 1u : 0u);
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_RDM_BG_DISCOVERY), p->rdm_bg_discovery ? 1u : 0u);
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_OUTPUT_STYLE), (uint32_t)p->output_style);
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_FAILSAFE), (uint32_t)p->failsafe);
        put_u16(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_DMX_BREAK), p->dmx_break_us);
        put_u16(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_DMX_MAB), p->dmx_mab_us);
        put_u8(&w, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_DMX_RATE), p->dmx_rate_hz);
    }
    return finish(&w, PM_CONFIG_STORE_MAGIC_MAIN);
}

size_t pm_config_store_encode_scene(const pm_port_settings_t *p, uint8_t *buf, size_t cap)
{
    writer_t w;
    if (p == NULL || !begin(&w, buf, cap)) return 0;
    uint16_t n = p->scene_len > sizeof(p->scene) ? (uint16_t)sizeof(p->scene) : p->scene_len;
    if (n > 0) put_rec(&w, PM_CFG_TAG_SCENE_DATA, p->scene, n);
    return finish(&w, PM_CONFIG_STORE_MAGIC_SCENE);
}

/* ------------------------------------------------------------------------------------------------ reader */

bool pm_config_store_get_uint(const uint8_t *val, uint16_t val_len, uint32_t *out)
{
    if (val_len < 1 || val_len > 4) return false;
    uint32_t v = 0;
    for (unsigned i = val_len; i-- > 0;) v = (v << 8) | val[i];
    *out = v;
    return true;
}

/** Walk all records of `payload`; calls `fn` for each. Returns false on a record that overruns the payload. */
typedef void (*rec_fn)(void *ctx, uint16_t tag, const uint8_t *val, uint16_t len);

static bool walk(const uint8_t *payload, size_t len, rec_fn fn, void *ctx)
{
    size_t pos = 0;
    while (pos < len) {
        if (len - pos < PM_CONFIG_STORE_REC_HDR_LEN) return false;
        uint16_t tag = pm_get_le16(&payload[pos]);
        uint16_t vlen = pm_get_le16(&payload[pos + 2]);
        pos += PM_CONFIG_STORE_REC_HDR_LEN;
        if (len - pos < vlen) return false;
        if (fn != NULL) fn(ctx, tag, &payload[pos], vlen);
        pos += vlen;
    }
    return true;
}

bool pm_config_store_find(const uint8_t *payload, size_t len, uint16_t tag, const uint8_t **val, uint16_t *val_len)
{
    size_t pos = 0;
    while (pos < len && len - pos >= PM_CONFIG_STORE_REC_HDR_LEN) {
        uint16_t t = pm_get_le16(&payload[pos]);
        uint16_t vlen = pm_get_le16(&payload[pos + 2]);
        pos += PM_CONFIG_STORE_REC_HDR_LEN;
        if (len - pos < vlen) return false;
        if (t == tag) {
            *val = &payload[pos];
            *val_len = vlen;
            return true;
        }
        pos += vlen;
    }
    return false;
}

/** Check header + CRC. On success *payload / *payload_len describe the records. */
static pm_err_t check_header(const uint8_t *buf, size_t len, uint32_t magic, const uint8_t **payload,
                             size_t *payload_len, uint16_t *schema)
{
    if (buf == NULL) return PM_ERR_INVALID_ARG;
    if (len < PM_CONFIG_STORE_HDR_LEN) return PM_ERR_SHORT;
    if (pm_get_le32(buf) != magic) return PM_ERR_BAD_ID;
    size_t n = pm_get_le16(&buf[6]);
    if (n > len - PM_CONFIG_STORE_HDR_LEN) return PM_ERR_SHORT;
    uint32_t crc = pm_crc32(0, &buf[4], 4);
    crc = pm_crc32(crc, &buf[PM_CONFIG_STORE_HDR_LEN], n);
    if (crc != pm_get_le32(&buf[8])) return PM_ERR_BAD_CHECKSUM;
    if (!walk(&buf[PM_CONFIG_STORE_HDR_LEN], n, NULL, NULL)) return PM_ERR_BAD_FORMAT;
    *payload = &buf[PM_CONFIG_STORE_HDR_LEN];
    *payload_len = n;
    *schema = pm_get_le16(&buf[4]);
    return PM_OK;
}

static void get_str(char *dst, size_t cap, const uint8_t *val, uint16_t len)
{
    size_t n = len < cap - 1 ? len : cap - 1;
    memset(dst, 0, cap);
    for (size_t i = 0; i < n && val[i] != 0; i++) dst[i] = (char)val[i];
}

/* Integer setters: a value that does not fit the field keeps the default. */
#define SET_UINT(field, maxv)                                     \
    do {                                                          \
        uint32_t v_;                                              \
        if (pm_config_store_get_uint(val, len, &v_) && v_ <= (maxv)) (field) = (v_); \
    } while (0)
#define SET_U32(field)                                            \
    do {                                                          \
        uint32_t v_;                                              \
        if (pm_config_store_get_uint(val, len, &v_)) (field) = v_; \
    } while (0)
#define SET_BOOL(field)                                           \
    do {                                                          \
        uint32_t v_;                                              \
        if (pm_config_store_get_uint(val, len, &v_)) (field) = (v_ != 0); \
    } while (0)

static void apply_main(void *ctx, uint16_t tag, const uint8_t *val, uint16_t len)
{
    pm_settings_t *s = ctx;
    if ((tag & 0xF000u) == 0x1000u) {
        unsigned port = (tag >> 8) & 0x0Fu;
        if (port >= PM_NUM_PORTS) return;
        pm_port_settings_t *p = &s->port[port];
        switch (tag & 0xFFu) {
        case PM_CFG_PTAG_ENABLED: SET_BOOL(p->enabled); break;
        case PM_CFG_PTAG_DIRECTION: SET_UINT(p->direction, 0xFFu); break;
        case PM_CFG_PTAG_ADDRESS: SET_UINT(p->address, 0xFFFFu); break;
        case PM_CFG_PTAG_MERGE: SET_UINT(p->merge, 0xFFu); break;
        case PM_CFG_PTAG_RDM_ENABLED: SET_BOOL(p->rdm_enabled); break;
        case PM_CFG_PTAG_RDM_BG_DISCOVERY: SET_BOOL(p->rdm_bg_discovery); break;
        case PM_CFG_PTAG_OUTPUT_STYLE: SET_UINT(p->output_style, 0xFFu); break;
        case PM_CFG_PTAG_FAILSAFE: SET_UINT(p->failsafe, 0xFFu); break;
        case PM_CFG_PTAG_DMX_BREAK: SET_UINT(p->dmx_break_us, 0xFFFFu); break;
        case PM_CFG_PTAG_DMX_MAB: SET_UINT(p->dmx_mab_us, 0xFFFFu); break;
        case PM_CFG_PTAG_DMX_RATE: SET_UINT(p->dmx_rate_hz, 0xFFu); break;
        default: break; /* unknown port field: skip */
        }
        return;
    }
    switch (tag) {
    case PM_CFG_TAG_SHORT_NAME: get_str(s->short_name, sizeof(s->short_name), val, len); break;
    case PM_CFG_TAG_LONG_NAME: get_str(s->long_name, sizeof(s->long_name), val, len); break;
    case PM_CFG_TAG_IP_DHCP: SET_BOOL(s->ip.dhcp); break;
    case PM_CFG_TAG_IP_ADDR: SET_U32(s->ip.static_ip); break;
    case PM_CFG_TAG_IP_MASK: SET_U32(s->ip.static_mask); break;
    case PM_CFG_TAG_IP_GATEWAY: SET_U32(s->ip.gateway); break;
    case PM_CFG_TAG_RDM_BG_INTERVAL: SET_UINT(s->rdm_bg_interval_s, 0xFFFFu); break;
    case PM_CFG_TAG_DISPLAY_LEVEL: SET_UINT(s->display_level, 0xFFu); break;
    case PM_CFG_TAG_DISPLAY_DIM_TIMEOUT: SET_UINT(s->display_dim_timeout_s, 0xFFFFu); break;
    case PM_CFG_TAG_ADDRESS_FORMAT: SET_UINT(s->address_format, 0xFFu); break;
    case PM_CFG_TAG_PANEL_LOCK: SET_BOOL(s->panel_lock); break;
    case PM_CFG_TAG_HW_REV_OVERRIDE: SET_UINT(s->hw_revision_override, 0xFFu); break;
    default: break; /* unknown tag (newer firmware or removed field): skip */
    }
}

/* ------------------------------------------------------------------------------------------------ migration */

/** One schema step vN -> vN+1: `s` is decoded with the current tag table, `payload` is the raw record list. */
typedef void (*migrate_fn)(pm_settings_t *s, const uint8_t *payload, size_t len);

/** v0 -> v1: DISPLAY_LEVEL was a percentage; the failsafe mode was node-wide (tag 0x0030). */
static void migrate_v0_to_v1(pm_settings_t *s, const uint8_t *payload, size_t len)
{
    const uint8_t *val;
    uint16_t vlen;
    uint32_t v;
    if (pm_config_store_find(payload, len, PM_CFG_TAG_DISPLAY_LEVEL, &val, &vlen) &&
        pm_config_store_get_uint(val, vlen, &v)) {
        if (v > 100u) v = 100u;
        s->display_level = (uint8_t)((v * 255u + 50u) / 100u);
    }
    if (pm_config_store_find(payload, len, PM_CFG_TAG_V0_FAILSAFE, &val, &vlen) &&
        pm_config_store_get_uint(val, vlen, &v) && v <= 0xFFu) {
        for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
            const uint8_t *pv;
            uint16_t pl;
            /* a per-port value (if a v0 blob had one) wins over the node-wide one */
            if (!pm_config_store_find(payload, len, PM_CFG_TAG_PORT(i, PM_CFG_PTAG_FAILSAFE), &pv, &pl)) {
                s->port[i].failsafe = (pm_failsafe_t)v;
            }
        }
    }
}

/** Index = source schema version. Must have exactly PM_CONFIG_STORE_SCHEMA entries. */
static const migrate_fn s_main_steps[PM_CONFIG_STORE_SCHEMA] = {
    migrate_v0_to_v1,
};

pm_err_t pm_config_store_decode_main(const uint8_t *buf, size_t len, pm_settings_t *out, uint16_t *schema_out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_settings_defaults(out);
    const uint8_t *payload;
    size_t n;
    uint16_t schema;
    pm_err_t err = check_header(buf, len, PM_CONFIG_STORE_MAGIC_MAIN, &payload, &n, &schema);
    if (err != PM_OK) return err;
    walk(payload, n, apply_main, out);
    for (unsigned v = schema; v < PM_CONFIG_STORE_SCHEMA; v++) s_main_steps[v](out, payload, n);
    out->version = PM_SETTINGS_VERSION;
    pm_settings_sanitize(out);
    if (schema_out != NULL) *schema_out = schema;
    return PM_OK;
}

static void apply_scene(void *ctx, uint16_t tag, const uint8_t *val, uint16_t len)
{
    pm_port_settings_t *p = ctx;
    if (tag != PM_CFG_TAG_SCENE_DATA) return;
    uint16_t n = len > sizeof(p->scene) ? (uint16_t)sizeof(p->scene) : len;
    memset(p->scene, 0, sizeof(p->scene));
    memcpy(p->scene, val, n);
    p->scene_len = n;
}

pm_err_t pm_config_store_decode_scene(const uint8_t *buf, size_t len, pm_port_settings_t *p)
{
    if (p == NULL) return PM_ERR_INVALID_ARG;
    p->scene_len = 0;
    memset(p->scene, 0, sizeof(p->scene));
    const uint8_t *payload;
    size_t n;
    uint16_t schema;
    pm_err_t err = check_header(buf, len, PM_CONFIG_STORE_MAGIC_SCENE, &payload, &n, &schema);
    if (err != PM_OK) return err;
    walk(payload, n, apply_scene, p);
    return PM_OK;
}
