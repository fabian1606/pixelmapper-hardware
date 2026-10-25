/* pm_protocol: UID, checksums, settings, image description block, generated PID header. */
#include <stddef.h>
#include <string.h>

#include "pm_fw.h"
#include "pm_pids.h"
#include "pm_settings.h"
#include "pm_types.h"
#include "test.h"

static void test_uid(void)
{
    pm_uid_t u;
    char s[PM_UID_STR_LEN];
    CHECK_EQ(pm_uid_parse("7ff0:80123456", &u), PM_OK);
    CHECK(strcmp(pm_uid_format(&u, s), "7FF0:80123456") == 0);
    CHECK_EQ(pm_uid_manufacturer(&u), 0x7FF0);
    CHECK_EQ(pm_uid_device(&u), 0x80123456);
    CHECK_EQ(pm_uid_parse("7FF0-80123456", &u), PM_ERR_INVALID_ARG);
    pm_uid_t bc = pm_uid_make(0xFFFF, 0xFFFFFFFF), vc = pm_uid_make(0x7FF0, 0xFFFFFFFF),
             other = pm_uid_make(0x4144, 0xFFFFFFFF), self = pm_uid_make(0x7FF0, 0x80000001);
    CHECK(pm_uid_addresses(&bc, &self));
    CHECK(pm_uid_addresses(&vc, &self));
    CHECK(!pm_uid_addresses(&other, &self));
    const uint8_t mac[6] = {0x02, 0x00, 0x5A, 0x12, 0x34, 0x56};
    pm_uid_t n = pm_uid_node_from_mac(PM_ESTA_MANUFACTURER, mac);
    CHECK_EQ(pm_uid_device(&n), 0x8A123456); /* 0x80000000 | lower 28 bits of the MAC */
}

static void test_crc(void)
{
    CHECK_EQ(pm_crc32(0, "123456789", 9), 0xCBF43926u);     /* CRC-32/ISO-HDLC check value */
    CHECK_EQ(pm_crc16_ccitt(0xFFFF, "123456789", 9), 0x29B1); /* CRC-16/CCITT-FALSE check value */
    uint32_t c = pm_crc32(0, "1234", 4);
    CHECK_EQ(pm_crc32(c, "56789", 5), 0xCBF43926u);          /* chainable */
}

static void test_settings(void)
{
    static pm_settings_t a, b;
    pm_settings_defaults(&a);
    CHECK(!pm_settings_sanitize(&a)); /* defaults are valid */
    CHECK_EQ(a.port[0].address, 1);
    CHECK_EQ(a.port[3].address, 4);
    b = a;
    CHECK_EQ(pm_settings_diff(&a, &b), 0);
    b.port[2].merge = PM_MERGE_LTP;
    CHECK_EQ(pm_settings_diff(&a, &b), PM_CHG_PORT(PM_CHG_P_MERGE, 2));
    b.port[1].dmx_mab_us = 5;
    CHECK(pm_settings_sanitize(&b));
    CHECK_EQ(b.port[1].dmx_mab_us, 12);

    /* padding bytes after `bool dhcp` in pm_ip_settings_t must not count as a change */
    pm_settings_defaults(&b);
    memset((uint8_t *)&b.ip + sizeof(b.ip.dhcp), 0xA5, offsetof(pm_ip_settings_t, static_ip) - sizeof(b.ip.dhcp));
    CHECK_EQ(pm_settings_diff(&a, &b), 0);
    b.ip.gateway = 0x0A000001u;
    CHECK_EQ(pm_settings_diff(&a, &b), PM_CHG_IP);

    /* MBS (PORTn_DMX_MBS) belongs to the timing group */
    pm_settings_defaults(&b);
    CHECK_EQ(b.port[0].dmx_mbs_us, 0);
    b.port[3].dmx_mbs_us = 250;
    CHECK(pm_settings_sanitize(&b));
    CHECK_EQ(b.port[3].dmx_mbs_us, 100);
    CHECK_EQ(pm_settings_diff(&a, &b), PM_CHG_PORT(PM_CHG_P_TIMING, 3));
}

static void test_image_desc(void)
{
    pm_image_desc_t d = {
        .magic = PM_NODE_IMAGE_MAGIC, .format = PM_NODE_IMAGE_FORMAT, .size = PM_IMAGE_DESC_SIZE,
        .esta_manufacturer = PM_ESTA_MANUFACTURER, .device_model_id = PM_MODEL_NODE,
        .fw_version_id = PM_FW_VERSION_ID(0, 1, 0), .hw_rev_mask = 1, .image_length = 300000,
        .image_crc32 = 0x12345678, .version_label = "0.1.0",
    };
    uint8_t raw[PM_IMAGE_DESC_SIZE];
    pm_image_desc_encode(&d, true, raw);
    CHECK(raw[0] == 'P' && raw[1] == 'M' && raw[2] == 'I' && raw[3] == 'M');
    pm_image_desc_t back;
    CHECK_EQ(pm_image_desc_decode(raw, sizeof(raw), &back), PM_OK);
    CHECK_EQ(pm_image_desc_check(&back, PM_ESTA_MANUFACTURER, 0, 4u << 20), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_image_desc_check(&back, PM_ESTA_MANUFACTURER, 1, 4u << 20), PM_FW_ERROR_DESC_HW_REV);
    CHECK_EQ(pm_image_desc_check(&back, 0x1234, 0, 4u << 20), PM_FW_ERROR_DESC_MANUFACTURER);
    CHECK_EQ(pm_image_desc_check(&back, PM_ESTA_MANUFACTURER, 0, 1000), PM_FW_ERROR_TOO_LARGE);
    back.device_model_id = PM_MODEL_TUBE; /* desc_crc32 no longer matches -> magic/format error first */
    CHECK_EQ(pm_image_desc_check(&back, PM_ESTA_MANUFACTURER, 0, 4u << 20), PM_FW_ERROR_DESC_MAGIC);
    pm_image_desc_t in_image = d;
    in_image.image_length = 0;
    in_image.image_crc32 = 0;
    in_image.desc_crc32 = 0;
    CHECK(pm_image_desc_matches(&in_image, &d));
}

static void test_pids(void)
{
    CHECK_EQ(PM_PID_BOOT_INFO, 0x8010);
    CHECK_EQ(PM_PID_PORT_DMX_BREAK(3), PM_PID_PORT3_DMX_BREAK);
    CHECK_EQ(PM_PDL_BOOT_INFO_GET_RESPONSE, 19);
    CHECK_EQ(PM_PDL_FW_WRITE_SET_REQUEST, 4 + PM_NODE_FW_MAX_BLOCK);
    unsigned n = 0, bad = 0;
#define COUNT(pid, pdl, dt, cc, unit, prefix, mn, mx, def, desc) \
    n++;                                                         \
    if (sizeof(desc) - 1 > 32 || (pid) < 0x8000u) bad++;
    PM_NODE_PID_DESC_LIST(COUNT)
#undef COUNT
    CHECK(n > 20);
    CHECK_EQ(bad, 0);
}

void test_protocol(void)
{
    test_uid();
    test_crc();
    test_settings();
    test_image_desc();
    test_pids();
}
