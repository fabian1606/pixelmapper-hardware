#include "config.h"
#include "rdm.h"

#if RDM_ENABLED

#include "dmx.h"
#include "personality.h"
#include "settings.h"
#include "boot_shared.h"
#include "rdm_boot.h"
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <string.h>
#include <util/delay.h>

/* command classes */
#define CC_DISC   0x10
#define CC_GET    0x20
#define CC_SET    0x30
/* response types (no ACK_TIMER: every request is answered at once, message
 * count is always 0 -> no QUEUED_MESSAGE / STATUS_MESSAGES needed) */
#define RT_ACK    0x00
#define RT_NACK   0x02
/* NACK reasons */
#define NR_UNKNOWN_PID             0x0000
#define NR_FORMAT_ERROR            0x0001
#define NR_WRITE_PROTECT           0x0004
#define NR_UNSUPPORTED_CC          0x0005
#define NR_DATA_OUT_OF_RANGE       0x0006
#define NR_SUB_DEVICE_OUT_OF_RANGE 0x0009
/* PIDs */
#define PID_DISC_UNIQUE_BRANCH     0x0001
#define PID_DISC_MUTE              0x0002
#define PID_DISC_UN_MUTE           0x0003
#define PID_SUPPORTED_PARAMETERS   0x0050
#define PID_PARAMETER_DESCRIPTION  0x0051
#define PID_DEVICE_INFO            0x0060
#define PID_DEVICE_MODEL_DESC      0x0080
#define PID_MANUFACTURER_LABEL     0x0081
#define PID_DEVICE_LABEL           0x0082
#define PID_SOFTWARE_VERSION_LABEL 0x00C0
#define PID_DMX_PERSONALITY        0x00E0
#define PID_DMX_PERSONALITY_DESC   0x00E1
#define PID_SLOT_INFO              0x0120
#define PID_SLOT_DESCRIPTION       0x0121
#define PID_DEFAULT_SLOT_VALUE     0x0122
#define PID_DMX_START_ADDRESS      0x00F0
#define PID_IDENTIFY_DEVICE        0x1000

/* packet offsets */
#define O_LEN    2
#define O_DEST   3
#define O_SRC    9
#define O_RT     16
#define O_MSGCNT 17
#define O_SUBDEV 18
#define O_CC     20
#define O_PID    21
#define O_PDL    23
#define O_PD     24

uint8_t rdm_identify;
static uint8_t uid[6];
static uint8_t muted;
static uint8_t rt;
static uint8_t boot_req;                 /* SET ENTER_BOOTLOADER accepted */
#define pk  rdm_buf
#define pd  (rdm_buf + O_PD)

/* optional + manufacturer PIDs. The "Required" ones of E1.20 Table A-3 - DISC_*,
 * SUPPORTED_PARAMETERS, PARAMETER_DESCRIPTION, DEVICE_INFO, SOFTWARE_VERSION_LABEL,
 * DMX_START_ADDRESS, IDENTIFY_DEVICE - "shall not be reported" (10.4.1) */
static const uint16_t supported[] PROGMEM = {
    PID_DEVICE_MODEL_DESC, PID_MANUFACTURER_LABEL, PID_DEVICE_LABEL,
    PID_DMX_PERSONALITY, PID_DMX_PERSONALITY_DESC,
    PID_SLOT_INFO, PID_SLOT_DESCRIPTION, PID_DEFAULT_SLOT_VALUE,
    PID_ENTER_BOOTLOADER,
};

/* PARAMETER_DESCRIPTION of PID_ENTER_BOOTLOADER (after the echoed PID):
 * PDL size 4, DS_ASCII, CC_SET, type 0, unit/prefix 0, min/max/default 0 */
static const uint8_t pdesc_boot[18] PROGMEM = { 4, 0x02, 0x02 };
static const char s_pdesc_boot[] PROGMEM = "Enter bootloader (SET \"BOOT\")";
static const char s_model[] PROGMEM = MODEL_DESCRIPTION;
static const char s_manuf[] PROGMEM = MANUFACTURER_LABEL;
static const char s_sw[] PROGMEM = FW_VERSION_LABEL;

void rdm_init(uint32_t id)
{
    uid[0] = RDM_MANUFACTURER_ID >> 8;
    uid[1] = RDM_MANUFACTURER_ID & 0xFF;
    for (uint8_t i = 0; i < 4; i++) uid[2 + i] = id >> (24 - 8 * i);
}

static uint8_t nack(uint16_t reason)
{
    rt = RT_NACK;
    pd[0] = reason >> 8;
    pd[1] = reason & 0xFF;
    return 2;
}

static uint8_t put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
    return 2;
}

static uint16_t get16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}

static uint8_t put_str_P(uint8_t *p, PGM_P s)
{
    uint8_t n = strlen_P(s);
    if (n > 32) n = 32;
    memcpy_P(p, s, n);
    return n;
}

static void apply_dmx_config(void)
{
    dmx_configure(g_set.start_address, pers_footprint(g_set.personality));
}

/* request PDL check for a known PID */
#define PDL(n) do { if (pdl != (n)) return nack(NR_FORMAT_ERROR); } while (0)

/* returns response PDL; rt is set to ACK or NACK */
static uint8_t handle_get(uint16_t pid, uint8_t pdl)
{
    uint8_t fp = pers_footprint(g_set.personality);
    switch (pid) {
    case PID_SUPPORTED_PARAMETERS: {
        PDL(0);
        uint8_t n = 0;
        for (uint8_t i = 0; i < sizeof(supported) / 2; i++)
            n += put16(pd + n, pgm_read_word(&supported[i]));
        return n;
    }
    case PID_PARAMETER_DESCRIPTION:              /* pd[0..1] = PID, echoed */
        PDL(2);
        if (get16(pd) != PID_ENTER_BOOTLOADER) return nack(NR_DATA_OUT_OF_RANGE);
        memcpy_P(pd + 2, pdesc_boot, sizeof(pdesc_boot));
        return 2 + sizeof(pdesc_boot) + put_str_P(pd + 2 + sizeof(pdesc_boot), s_pdesc_boot);
    case PID_DEVICE_INFO:
        PDL(0);
        put16(pd + 0, 0x0100);                       /* RDM protocol 1.0 */
        put16(pd + 2, RDM_DEVICE_MODEL_ID);
        put16(pd + 4, RDM_PRODUCT_CATEGORY);
        put16(pd + 6, FW_VERSION_ID >> 16);
        put16(pd + 8, FW_VERSION_ID & 0xFFFF);
        put16(pd + 10, fp);
        pd[12] = g_set.personality;
        pd[13] = PERS_COUNT;
        put16(pd + 14, fp ? g_set.start_address : 0xFFFF);
        put16(pd + 16, 0);                           /* sub-devices */
        pd[18] = 0;                                  /* sensors */
        return 19;
    case PID_SOFTWARE_VERSION_LABEL: PDL(0); return put_str_P(pd, s_sw);
    case PID_DEVICE_MODEL_DESC:      PDL(0); return put_str_P(pd, s_model);
    case PID_MANUFACTURER_LABEL:     PDL(0); return put_str_P(pd, s_manuf);
    case PID_DEVICE_LABEL:
        PDL(0);
        memcpy(pd, g_set.label, g_set.label_len);
        return g_set.label_len;
    case PID_DMX_START_ADDRESS:      PDL(0); return put16(pd, g_set.start_address);
    case PID_DMX_PERSONALITY:
        PDL(0);
        pd[0] = g_set.personality;
        pd[1] = PERS_COUNT;
        return 2;
    case PID_DMX_PERSONALITY_DESC: {
        PDL(1);
        uint8_t p = pd[0];
        if (!pers_footprint(p)) return nack(NR_DATA_OUT_OF_RANGE);
        put16(pd + 1, pers_footprint(p));
        return 3 + put_str_P(pd + 3, pers_description(p));
    }
    case PID_SLOT_INFO: {
        PDL(0);
        uint8_t n = 0;
        for (uint8_t s = 0; s < fp; s++) {
            n += put16(pd + n, s);
            pd[n++] = 0x00;                          /* ST_PRIMARY */
            n += put16(pd + n, pers_slot_label(g_set.personality, s));
        }
        return n;
    }
    case PID_SLOT_DESCRIPTION: {
        PDL(2);
        uint16_t s = get16(pd);
        if (s >= fp) return nack(NR_DATA_OUT_OF_RANGE);
        return 2 + pers_slot_desc(g_set.personality, s, (char *)pd + 2);
    }
    case PID_DEFAULT_SLOT_VALUE: {
        PDL(0);
        uint8_t n = 0;
        for (uint8_t s = 0; s < fp; s++) {
            n += put16(pd + n, s);
            pd[n++] = pers_slot_default(g_set.personality, s);
        }
        return n;
    }
    case PID_IDENTIFY_DEVICE:
        PDL(0);
        pd[0] = rdm_identify;
        return 1;
    case PID_ENTER_BOOTLOADER:                 /* SET only */
        return nack(NR_UNSUPPORTED_CC);
    default:
        return nack(NR_UNKNOWN_PID);
    }
}

static uint8_t handle_set(uint16_t pid, uint8_t pdl)
{
    switch (pid) {
    case PID_DMX_START_ADDRESS: {
        if (pdl != 2) return nack(NR_FORMAT_ERROR);
        uint16_t a = ((uint16_t)pd[0] << 8) | pd[1];
        if (a < 1 || a + pers_footprint(g_set.personality) - 1 > 512)
            return nack(NR_DATA_OUT_OF_RANGE);
        g_set.start_address = a;
        break;
    }
    case PID_DMX_PERSONALITY: {
        if (pdl != 1) return nack(NR_FORMAT_ERROR);
        uint8_t fp = pers_footprint(pd[0]);
        if (!fp || g_set.start_address + fp - 1 > 512) return nack(NR_DATA_OUT_OF_RANGE);
        g_set.personality = pd[0];
        break;
    }
    case PID_DEVICE_LABEL: {
        if (pdl > 32) return nack(NR_FORMAT_ERROR);
        uint8_t n = 0;                         /* a NUL ends the text (E1.20 10.1) */
        while (n < pdl && n < DEVICE_LABEL_MAX && pd[n]) n++;
        memcpy(g_set.label, pd, n);
        g_set.label_len = n;
        break;
    }
    case PID_IDENTIFY_DEVICE:
        if (pdl != 1) return nack(NR_FORMAT_ERROR);
        if (pd[0] > 1) return nack(NR_DATA_OUT_OF_RANGE);
        rdm_identify = pd[0];
        return 0;
    case PID_ENTER_BOOTLOADER:                 /* payload "BOOT" */
        if (pdl != 4) return nack(NR_FORMAT_ERROR);
        if (pd[0] != 'B' || pd[1] != 'O' || pd[2] != 'O' || pd[3] != 'T')
            return nack(NR_DATA_OUT_OF_RANGE);
        boot_req = 1;
        return 0;
    case PID_SUPPORTED_PARAMETERS: case PID_PARAMETER_DESCRIPTION: case PID_DEVICE_INFO:
    case PID_SOFTWARE_VERSION_LABEL: case PID_DEVICE_MODEL_DESC:
    case PID_MANUFACTURER_LABEL: case PID_DMX_PERSONALITY_DESC:
    case PID_SLOT_INFO: case PID_SLOT_DESCRIPTION: case PID_DEFAULT_SLOT_VALUE:
        return nack(NR_UNSUPPORTED_CC);
    default:
        return nack(NR_UNKNOWN_PID);
    }
    settings_changed();
    apply_dmx_config();
    return 0;
}

static void send_dub_response(void)
{
    uint8_t r[24];
    uint16_t sum = 0;
    for (uint8_t i = 0; i < 7; i++) r[i] = 0xFE;
    r[7] = 0xAA;
    for (uint8_t i = 0; i < 6; i++) {
        r[8 + 2 * i] = uid[i] | 0xAA;
        r[9 + 2 * i] = uid[i] | 0x55;
        sum += r[8 + 2 * i] + r[9 + 2 * i];
    }
    r[20] = (sum >> 8) | 0xAA;
    r[21] = (sum >> 8) | 0x55;
    r[22] = (sum & 0xFF) | 0xAA;
    r[23] = (sum & 0xFF) | 0x55;
    _delay_us(RDM_TURNAROUND_US);
    rs485_tx(r, sizeof(r), 0);
}

void rdm_poll(void)
{
    if (!rdm_ready) return;

    uint8_t len = pk[O_LEN];
    uint8_t pdl = pk[O_PDL];
    uint16_t sum = 0;
    for (uint8_t i = 0; i < len; i++) sum += pk[i];
    if (pk[1] != 0x01 || pdl + 24 != len || sum != (((uint16_t)pk[len] << 8) | pk[len + 1]))
        goto done;

    uint8_t bcast;
    if (!memcmp(pk + O_DEST, uid, 6))
        bcast = 0;
    else if (pk[5] == 0xFF && pk[6] == 0xFF && pk[7] == 0xFF && pk[8] == 0xFF &&
             ((pk[3] == 0xFF && pk[4] == 0xFF) || (pk[3] == uid[0] && pk[4] == uid[1])))
        bcast = 1;
    else
        goto done;

    uint8_t cc = pk[O_CC];
    uint16_t pid = ((uint16_t)pk[O_PID] << 8) | pk[O_PID + 1];
    uint16_t subdev = ((uint16_t)pk[O_SUBDEV] << 8) | pk[O_SUBDEV + 1];
    rt = RT_ACK;

    if (cc == CC_DISC) {
        if (pid == PID_DISC_UNIQUE_BRANCH) {
            if (!muted && pdl == 12 && memcmp(pd, uid, 6) <= 0 && memcmp(uid, pd + 6, 6) <= 0)
                send_dub_response();
            goto done;
        }
        if ((pid != PID_DISC_MUTE && pid != PID_DISC_UN_MUTE) || pdl != 0) goto done;
        muted = (pid == PID_DISC_MUTE);
        pd[0] = pd[1] = 0;                        /* control field */
        pdl = 2;
    } else if (cc == CC_GET || cc == CC_SET) {
        if (subdev != 0 && !(cc == CC_SET && subdev == 0xFFFF))
            pdl = nack(NR_SUB_DEVICE_OUT_OF_RANGE);
        else
            pdl = (cc == CC_GET) ? handle_get(pid, pdl) : handle_set(pid, pdl);
    } else {
        goto done;
    }
    if (bcast) {                                  /* never answer broadcasts */
        boot_req = 0;                             /* and never enter the bootloader */
        goto done;
    }

    /* build the response in place */
    memcpy(pk + O_DEST, pk + O_SRC, 6);
    memcpy(pk + O_SRC, uid, 6);
    pk[O_RT] = rt;
    pk[O_MSGCNT] = 0;
    pk[O_CC] = cc + 1;
    pk[O_PDL] = pdl;
    len = 24 + pdl;
    pk[O_LEN] = len;
    sum = 0;
    for (uint8_t i = 0; i < len; i++) sum += pk[i];
    pk[len] = sum >> 8;
    pk[len + 1] = sum & 0xFF;
    _delay_us(RDM_TURNAROUND_US);
    rs485_tx(pk, len + 2, 1);

    if (boot_req) {
        /* ACK is out: finish pending EEPROM writes (a reset would cut them
         * off -> CRC error -> defaults), ask the bootloader to stay, then
         * watchdog reset */
        settings_flush();
        cli();
        BOOT_MAGIC = BOOT_MAGIC_VALUE;
        wdt_enable(WDTO_15MS);
        for (;;)
            ;
    }

done:
    rdm_ready = 0;
}

#endif
