/*
 * tube_boot_rdm - RDM bootloader for the ATmega8 in the LED tube. Lives in the
 * 2 KB boot section 0x1800..0x1FFF (hfuse BOOTSZ=00, BOOTRST on) and runs
 * after every reset. 0x1800.. is the NRWW section: the CPU keeps running from
 * here while app pages (RWW) are erased/written, so page writes happen in the
 * background and RDM requests are still answered in time.
 *
 * Stays active when
 *   - the app asked for it (RAM magic + watchdog reset, see boot_shared.h),
 *   - the app is invalid (trailer in boot_shared.h: magic/format, manufacturer/
 *     model ID, length, CRC; blank flash),
 *   - a GET BOOT_INFO (0x8010) to our UID arrives within ~200 ms after
 *     reset (rescue path; GET to broadcast is invalid in E1.20 -> ignored),
 *     or any SET of a bootloader PID to the vendorcast 7FF0:FFFFFFFF (rescue
 *     without a known UID: every tube in its window stays).
 * Otherwise the app at 0x0000 is started. Once active it leaves on FINISH
 * (app valid; else last error 2/3/4 = CRC/length, foreign model, unknown
 * trailer format) or after ~60 s without a bootloader PID (0x8010..0x8013)
 * addressed to us, if the app is valid. The app is always started via a
 * watchdog reset with BOOT_MAGIC_APP (clean reset state, request magic gone).
 * During the rescue window all RDM requests are answered already; only the
 * bootloader PIDs make it stay.
 *
 * Minimal E1.20 responder with the app's UID (7FF0 + EEPROM device ID):
 * DISC_UNIQUE_BRANCH / DISC_MUTE / DISC_UN_MUTE (control field: Boot-Loader
 * flag while no valid app is installed), GET DEVICE_INFO (sw version
 * BOOT_RDM_SW_VERSION_ID = 0xB00000xx), SOFTWARE_VERSION_LABEL,
 * SUPPORTED_PARAMETERS (empty: the manufacturer PIDs in rdm_boot.h are not
 * exposed, so no PARAMETER_DESCRIPTION is needed - E1.20 10.4.1, Table A-3),
 * IDENTIFY_DEVICE (all LED channels on) and the manufacturer PIDs.
 * Unknown PID: NACK UNKNOWN_PID, wrong CC: UNSUPPORTED_COMMAND_CLASS, wrong
 * PDL: FORMAT_ERROR. Requests are received up to PD 228 (8-bit RX index).
 * Non-RDM start codes (DMX from the node) are ignored. Broadcasts are never
 * answered; broadcast SET is executed, broadcast GET ignored, manufacturer
 * PIDs to FFFF:FFFFFFFF ignored (E1.20 6.2.10.2). No ACK_TIMER: page write
 * busy -> NACK BUFFER_FULL, APP_CRC is synchronous in chunks of <= 256 B
 * (reply < 2 ms) - see README "E1.20-Konformität".
 *
 * Polled USART, no interrupts, no C runtime (-nostartfiles): boot_entry()
 * sets the stack below the RAM magic, avr-libc's __do_clear_bss clears .bss;
 * hot flags live in r2..r8 (see below). There must be no .data
 * (initialised variables) - extra_script.py fails the build otherwise.
 */
#include "config.h"
#include "boot_shared.h"
#include "rdm_boot.h"
#include <avr/boot.h>
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/wdt.h>
#include <string.h>
#include <util/delay.h>

#ifndef BOOT_LAYOUT_2K
#error "tube_boot_rdm needs the 2 KB layout (-DBOOT_LAYOUT_2K)"
#endif
#if BOOT_RDM_PAGE_SIZE != SPM_PAGESIZE
#error "rdm_boot.h page size does not match the chip"
#endif
#ifdef RS485_DE_PIN_NONE
#error "bootloader needs the RS-485 DE pin"
#endif
/* identify drives the LED pins directly (config.h channel table) */
_Static_assert(!LED_ANY_ACTIVE_LOW, "bootloader identify assumes active-high LED channels");
_Static_assert(!(LED_MASK_D & ((1 << PD0) | (1 << PD1) | (1 << RS485_DE_BIT))), "LED pin on RXD/TXD/DE");

#define STR_(x)  #x
#define STR(x)   STR_(x)
#define SW_LABEL "tube_boot_rdm " STR(BOOT_RDM_VERSION)
#if (BOOT_RDM_SW_VERSION_ID & 0xFF) != BOOT_RDM_VERSION
#error "rdm_boot.h: BOOT_RDM_SW_VERSION_ID and BOOT_RDM_VERSION disagree"
#endif

/* Size matters (2 KB limit): gcc's inlining of the small helpers below swings
 * the image by +-150 B; these NOINLINEs are the smallest combination found
 * with avr-gcc 7.3 -Os. Re-check the size after changes (v2: 2040, v3: 1948
 * of 2048 B; a table-driven respond() and register muted/identify were tried:
 * larger). */
#define NOINLINE __attribute__((noinline))

/* command classes */
#define CC_DISC   0x10
#define CC_GET    0x20
#define CC_SET    0x30
/* response types (no ACK_TIMER: busy -> NACK BUFFER_FULL, so no queued
 * messages, message count always 0, no QUEUED_MESSAGE / STATUS_MESSAGES) */
#define RT_ACK       0x00
#define RT_NACK      0x02
/* NACK reasons */
#define NR_UNKNOWN_PID             0x0000
#define NR_FORMAT_ERROR            0x0001
#define NR_UNSUPPORTED_CC          0x0005
#define NR_DATA_OUT_OF_RANGE       0x0006
#define NR_BUFFER_FULL             0x0007
#define NR_SUB_DEVICE_OUT_OF_RANGE 0x0009
/* PIDs */
#define PID_DISC_UNIQUE_BRANCH     0x0001
#define PID_DISC_MUTE              0x0002
#define PID_DISC_UN_MUTE           0x0003
#define PID_SUPPORTED_PARAMETERS   0x0050
#define PID_DEVICE_INFO            0x0060
#define PID_SOFTWARE_VERSION_LABEL 0x00C0
#define PID_IDENTIFY_DEVICE        0x1000
/* DISC_MUTE / DISC_UN_MUTE control field */
#define CF_BOOTLOADER              0x04       /* low byte, bit 2 (E1.20 7.6.1) */

/* packet offsets (start code 0xCC at 0) */
#define O_SUBSC  1
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

/* Largest request PD received: the 8-bit RX index must stay below the
 * RX_BREAK / RX_OFF markers (message length 252 -> idx <= 254 at the end).
 * E1.20 allows 231; PD 229..231 is not received (no request of this loader
 * or a standard PID needs it). */
#define RX_MAX_PD 228
#define WRITE_PAGE_PDL (2 + SPM_PAGESIZE)     /* address + page */
#define PK_SIZE   (O_PD + RX_MAX_PD + 2)      /* also holds every response */

#define RX_OFF    0xFF                        /* wait for break */
#define RX_BREAK  0xFE                        /* break seen, next = start code */

/* Timer1 at F_CPU/1024 = 15625 Hz, overflow every 4.19 s */
#define T1_PRESCALE   ((1 << CS12) | (1 << CS10))
#define WINDOW_TICKS  (uint16_t)(F_CPU / 1024 * 200 / 1000)   /* rescue window */
#define IDLE_OVF      14                                       /* ~59 s */

#define CRC_MAX_LEN   BOOT_RDM_CRC_MAX_LEN    /* APP_CRC chunk: ~1.05 ms, E1.20 reply < 2 ms */

enum { FL_IDLE, FL_FILL, FL_VERIFY };   /* background page write */

#if RS485_DE_ACTIVE_HIGH
#define DE_TX()  (RS485_DE_PORT |= (1 << RS485_DE_BIT))
#define DE_RX()  (RS485_DE_PORT &= ~(1 << RS485_DE_BIT))
#else
#define DE_TX()  (RS485_DE_PORT &= ~(1 << RS485_DE_BIT))
#define DE_RX()  (RS485_DE_PORT |= (1 << RS485_DE_BIT))
#endif

static uint8_t pk[PK_SIZE];                   /* request in, response out */
#define pd  (pk + O_PD)
/* Hot 8-bit state lives in call-saved registers that nothing else uses (only
 * C file; avr-libc's asm routines do not touch r2..r8): 2 B per access instead
 * of 4 B lds/sts - needed to fit 2 KB. Not cleared by boot_entry -> boot_main
 * initialises every one of them. */
register uint8_t idx asm("r2");               /* RX position or RX_OFF / RX_BREAK */
register uint8_t fl asm("r3");                /* FL_* background page write */
register uint8_t stay asm("r4");              /* 0: rescue window only */
register uint8_t rt asm("r5");                /* response type */
register uint8_t last_err asm("r6");          /* BOOT_ERR_* */
register uint8_t ovf asm("r7");               /* Timer1 overflows left until timeout */
register uint8_t finish asm("r8");            /* FINISH acked -> check app after the reply */

static uint8_t muted;
static uint8_t identify;                      /* IDENTIFY_DEVICE: LEDs blink white */
static uint8_t uid[8];                        /* UID + its DUB checksum (send_dub_response) */

static uint16_t fl_addr;
static uint8_t page[SPM_PAGESIZE];
/* installed app as of the last app_check() (boot, FINISH, idle timeout):
 * [0] trailer valid, [1..4] its FW_VERSION_ID big-endian - BOOT_INFO bytes
 * 14..18. Cached because the check takes ~10 ms (> E1.20 2 ms) and the RWW
 * flash is unreadable during a page write; WRITE_PAGE clears [0]. */
static uint8_t app_st[5];

static const char s_sw[sizeof(SW_LABEL) - 1] PROGMEM = SW_LABEL;   /* no NUL */

void boot_main(void) __attribute__((OS_main, noreturn, used));

/* wdt_enable() without the SREG save / cli (interrupts are never enabled
 * here): timed sequence, the second write within 4 cycles */
#define WDT_SET(to) asm volatile("out %0, %1\n\tout %0, %2" : :                \
                                 "I"(_SFR_IO_ADDR(WDTCR)), "r"((uint8_t)((1 << WDCE) | (1 << WDE))), \
                                 "r"((uint8_t)((1 << WDE) | (to))))

/* First word of the boot section (BOOTRST jumps here): zero reg, stack below
 * the RAM magic, then avr-libc's __do_clear_bss (.init4, linked because there
 * is .bss), which falls through to boot_init9 below -> boot_main. */
void __attribute__((naked, used, section(".vectors"))) boot_entry(void)
{
    asm volatile("clr __zero_reg__");
    SP = BOOT_STACK_TOP;
    asm volatile("rjmp __do_clear_bss");
}

void __attribute__((naked, used, section(".init9"))) boot_init9(void)
{
    asm volatile("rjmp boot_main");
}

/* Leaves through a watchdog reset with BOOT_MAGIC_APP: the next boot_main()
 * jumps to the app before touching any peripheral, so the app always starts
 * from a clean reset state (and the RDM bootloader request magic is gone). */
static void NOINLINE __attribute__((noreturn)) start_app(void)
{
    BOOT_MAGIC = BOOT_MAGIC_APP;
    WDT_SET(WDTO_15MS);
    for (;;)
        ;
}

/* stay active, (re)start the ~60 s idle timeout */
static void NOINLINE stay_idle(void)
{
    stay = 1;
    TCNT1 = 0;
    TIFR = (1 << TOV1);
    ovf = IDLE_OVF;
}

static uint8_t busy(void)
{
    return fl != FL_IDLE;
}

/* IDENTIFY_DEVICE (E1.20 10.11.1: the responder shall physically identify
 * itself): all LED channels blink white, otherwise off. Called from the main
 * loop - as a function: inline there it costs ~150 B more (register pressure). */
static void NOINLINE leds(void)
{
    uint16_t t = TCNT1;                       /* 15625 Hz: bit 12 -> ~1.9 Hz blink */
    uint8_t m = -(identify & (uint8_t)(t >> 12));   /* 0x00 / 0xFF */
    PORTB = m & LED_MASK_B;                   /* no other outputs on port B / C */
    PORTC = m & LED_MASK_C;
    PORTD = (PORTD & ~LED_MASK_D) | (m & LED_MASK_D);
}

/* --------------------------------------------------------------- background jobs */

/* one SPM instruction (the avr-libc boot_* macros, but not inlined 4 times);
 * data (r1:r0) only matters for a page buffer fill */
#define SPM_FILL  (1 << SPMEN)
#define SPM_ERASE ((1 << PGERS) | (1 << SPMEN))
#define SPM_WRITE ((1 << PGWRT) | (1 << SPMEN))
#define SPM_RWW   ((1 << RWWSRE) | (1 << SPMEN))
static void NOINLINE spm(uint8_t op, uint16_t addr, uint16_t data)
{
    asm volatile("movw r0, %3\n\t"
                 "out %0, %1\n\t"
                 "spm\n\t"
                 "clr __zero_reg__"
                 : : "I"(_SFR_IO_ADDR(SPMCR)), "r"(op), "z"(addr), "r"(data) : "r0");
}

/* Background page write: WRITE_PAGE starts the erase, then fill + write ->
 * verify; each step only starts when the previous SPM operation is done.
 * The CPU runs on (NRWW) meanwhile. */
static void flash_step(void)
{
    if (fl == FL_IDLE || boot_spm_busy()) return;
    if (fl == FL_FILL) {                      /* erase (started by WRITE_PAGE) done */
        for (uint8_t i = 0; i < SPM_PAGESIZE; i += 2)
            spm(SPM_FILL, fl_addr + i, page[i] | ((uint16_t)page[i + 1] << 8));
        spm(SPM_WRITE, fl_addr, 0);
        fl = FL_VERIFY;
    } else {
        spm(SPM_RWW, 0, 0);
        for (uint8_t i = 0; i < SPM_PAGESIZE; i++)
            if (pgm_read_byte(fl_addr + i) != page[i]) last_err = BOOT_ERR_VERIFY;
        fl = FL_IDLE;
    }
}

/* CRC16-CCITT (0x1021, MSB first, = _crc_xmodem_update) of flash
 * a .. a+n-1 continuing from c (start: 0xFFFF); flash must not be busy.
 * Bitwise in asm: 36 B instead of 76 B, ~65 cycles/byte -> APP_CRC chunk of
 * 256 B ~1.05 ms (E1.20 reply < 2 ms), whole app ~25 ms (< WDT 1 s). */
static uint16_t NOINLINE crc_flash(uint16_t c, uint16_t a, uint16_t n)
{
    uint8_t bits;
    asm volatile("rjmp 3f\n"
                 "1:\tlpm __tmp_reg__, Z+\n\t"
                 "eor %B0, __tmp_reg__\n\t"
                 "ldi %3, 8\n"
                 "2:\tlsl %A0\n\t"
                 "rol %B0\n\t"
                 "brcc 4f\n\t"
                 "eor %A0, %A4\n\t"
                 "eor %B0, %B4\n"
                 "4:\tdec %3\n\t"
                 "brne 2b\n"
                 "3:\tsubi %A2, 1\n\t"
                 "sbci %B2, 0\n\t"
                 "brcc 1b"
                 : "+r"(c), "+z"(a), "+d"(n), "=&d"(bits)
                 : "r"((uint16_t)0x1021));
    return c;
}

/* next flash byte, sequential (lpm Z+): much smaller than pgm_read_byte()
 * with a new address every time */
#define LPM_INC(z) ({ uint8_t b_; asm volatile("lpm %0, Z+" : "=r"(b_), "+z"(z)); b_; })

/* Trailer check (boot_shared.h): BOOT_ERR_NONE = app may start, else the
 * reason (blank flash: magic 0xFFFF -> BOOT_ERR_FORMAT). Flash must not be
 * busy. Snapshots validity + FW_VERSION_ID into app_st[] for BOOT_INFO. */
static uint8_t app_check(void)
{
    uint16_t z = TRAILER_START;
    uint8_t e = BOOT_ERR_FORMAT;
    app_st[0] = 0;
    uint8_t x = LPM_INC(z) ^ (TRAILER_MAGIC & 0xFF);
    x |= LPM_INC(z) ^ (TRAILER_MAGIC >> 8);
    x |= LPM_INC(z) ^ TRAILER_FORMAT;
    if (!x) {
        e = BOOT_ERR_MODEL;
        LPM_INC(z);                           /* reserved, not checked */
        x = LPM_INC(z) ^ (RDM_MANUFACTURER_ID & 0xFF);
        x |= LPM_INC(z) ^ (RDM_MANUFACTURER_ID >> 8);
        x |= LPM_INC(z) ^ (RDM_DEVICE_MODEL_ID & 0xFF);
        x |= LPM_INC(z) ^ (RDM_DEVICE_MODEL_ID >> 8);
        if (!x) {
            uint16_t len = pgm_read_word(APP_TRAILER);
            e = BOOT_ERR_APP_INVALID;
            if (len && len <= APP_MAX_SIZE &&
                crc_flash(crc_flash(0xFFFF, 0, len), TRAILER_START, TRAILER_META_LEN) == pgm_read_word(APP_TRAILER + 2)) {
                e = BOOT_ERR_NONE;
                app_st[0] = 1;
            }
        }
    }
    z = TRAILER_START + 8;
    uint8_t *d = app_st + 5;
    do *--d = LPM_INC(z); while (d != app_st + 1);   /* FW_VERSION_ID, LE -> BE */
    return e;
}

/* --------------------------------------------------------------- line level */

static uint8_t NOINLINE put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xFF;
    return 2;
}

static uint16_t NOINLINE get16(const uint8_t *p)
{
    return ((uint16_t)p[0] << 8) | p[1];
}


/* Polls the USART; 1 when a complete RDM packet (length per its own header) is
 * in pk[]. Anything not starting with break + 0xCC is skipped. */
static uint8_t rx_poll(void)
{
    uint8_t st = UCSRA;
    if (!(st & (1 << RXC))) return 0;
    uint8_t d = UDR;
    if (st & (1 << FE)) {                     /* break = 0x00 + framing error */
        idx = d ? RX_OFF : RX_BREAK;
        return 0;
    }
    if (idx == RX_OFF) return 0;
    if (idx == RX_BREAK) {
        if (d != 0xCC) {                      /* DMX (0x00) or other start code */
            idx = RX_OFF;
            return 0;
        }
        idx = 0;
    }
    pk[idx++] = d;
    if (idx == O_LEN + 1) {
        if (d < O_PD || d > O_PD + RX_MAX_PD) idx = RX_OFF;
    } else if (idx > O_LEN + 1 && idx == pk[O_LEN] + 2) {   /* + checksum */
        idx = RX_OFF;
        return 1;
    }
    return 0;
}

static void tx(const uint8_t *p, uint8_t n, uint8_t with_break)
{
    _delay_us(RDM_TURNAROUND_US);
    UCSRB = 0;                                /* RX off (no echo), PD1 = GPIO high */
    DE_TX();
    if (with_break) {
        PORTD &= ~(1 << PD1);
        _delay_us(RDM_BREAK_US);
        PORTD |= (1 << PD1);
        _delay_us(RDM_MAB_US);
    }
    UCSRB = (1 << TXEN);
    UCSRA = (1 << TXC);                       /* clear TXC */
    while (n--) {
        while (!(UCSRA & (1 << UDRE)))
            ;
        UDR = *p++;
    }
    while (!(UCSRA & (1 << TXC)))
        ;
    UCSRB = 0;
    DE_RX();
    idx = RX_OFF;
    UCSRB = (1 << RXEN);
}

/* 7 x FE, AA, then UID and checksum, every byte b as (b | AA, b | 55);
 * the checksum is the sum of those 12 bytes = sum(UID) + 6 * FF */
static void send_dub_response(void)
{
    uint8_t *r = pk;                          /* request is no longer needed */
    uint16_t sum = 6 * 0xFF;
    for (uint8_t i = 0; i < 6; i++) sum += uid[i];
    put16(uid + 6, sum);
    for (uint8_t i = 0; i < 7; i++) {
        asm("" : "+r"(i));                    /* no unrolling */
        *r++ = 0xFE;
    }
    *r++ = 0xAA;
    for (uint8_t i = 0; i < 8; i++) {
        *r++ = uid[i] | 0xAA;
        *r++ = uid[i] | 0x55;
    }
    tx(pk, 24, 0);                            /* no break */
}

/* --------------------------------------------------------------- RDM */

/* GET DEVICE_INFO answer - constant in the bootloader */
static const uint8_t dev_info[19] PROGMEM = {
    0x01, 0x00,                                           /* RDM protocol 1.0 */
    RDM_DEVICE_MODEL_ID >> 8, RDM_DEVICE_MODEL_ID & 0xFF,
    RDM_PRODUCT_CATEGORY >> 8, RDM_PRODUCT_CATEGORY & 0xFF,
    (uint8_t)(BOOT_RDM_SW_VERSION_ID >> 24), (uint8_t)(BOOT_RDM_SW_VERSION_ID >> 16),
    (uint8_t)(BOOT_RDM_SW_VERSION_ID >> 8), (uint8_t)BOOT_RDM_SW_VERSION_ID,
    0x00, 0x00,                                           /* footprint 0 */
    0x00, 0x00,                                           /* personality 0 of 0 */
    0xFF, 0xFF,                                           /* no DMX start address */
    0x00, 0x00,                                           /* sub-devices */
    0x00,                                                 /* sensors */
};

/* GET BOOT_INFO answer bytes 0..13 (4/5 = busy/last error filled in) */
static const uint8_t boot_info_c[14] PROGMEM = {
    BOOT_RDM_VERSION, SPM_PAGESIZE, BOOT_START >> 8, BOOT_START & 0xFF, 0, 0,
    RDM_MANUFACTURER_ID >> 8, RDM_MANUFACTURER_ID & 0xFF,
    RDM_DEVICE_MODEL_ID >> 8, RDM_DEVICE_MODEL_ID & 0xFF,
    (uint8_t)(BOOT_RDM_SW_VERSION_ID >> 24), (uint8_t)(BOOT_RDM_SW_VERSION_ID >> 16),
    (uint8_t)(BOOT_RDM_SW_VERSION_ID >> 8), (uint8_t)BOOT_RDM_SW_VERSION_ID,
};
#define BOOT_INFO_PDL (sizeof(boot_info_c) + sizeof(app_st))

/* supported PID + command class pairs and their request PDL; index = handler.
 * GET SUPPORTED_PARAMETERS answers an empty list: the required PIDs of Table
 * A-3 "shall not be reported" and manufacturer PIDs "may or may not be
 * included" (E1.20 10.4.1). Not exposing the bootloader PIDs makes
 * PARAMETER_DESCRIPTION unnecessary (Table A-3); uploaders use rdm_boot.h. */
typedef struct {
    uint16_t pid;
    uint8_t cc;
    uint8_t pdl;
} pid_def_t;
enum { P_SUPPORTED, P_DEVINFO, P_SWLABEL, P_ID_GET, P_ID_SET,
       P_INFO, P_WRITE, P_CRC, P_FINISH, P_COUNT };
static const pid_def_t pids[P_COUNT] PROGMEM = {
    { PID_SUPPORTED_PARAMETERS,   CC_GET, 0 },
    { PID_DEVICE_INFO,            CC_GET, 0 },
    { PID_SOFTWARE_VERSION_LABEL, CC_GET, 0 },
    { PID_IDENTIFY_DEVICE,        CC_GET, 0 },
    { PID_IDENTIFY_DEVICE,        CC_SET, 1 },
    { PID_BOOT_INFO,              CC_GET, 0 },
    { PID_BOOT_WRITE_PAGE,        CC_SET, WRITE_PAGE_PDL },
    { PID_BOOT_APP_CRC,           CC_GET, 4 },
    { PID_BOOT_FINISH,            CC_SET, 0 },
};

static uint16_t NOINLINE checksum(const uint8_t *p, uint8_t len)
{
    uint16_t sum = 0;
    while (len--) sum += *p++;
    return sum;
}

/* constant answer from flash -> pd, returns its PDL */
static uint8_t NOINLINE answer_P(const void *p, uint8_t n)
{
    memcpy_P(pd, p, n);
    return n;
}

static uint8_t nack(uint16_t reason)
{
    rt = RT_NACK;
    return put16(pd, reason);
}

/* GET/SET; returns response PDL, rt is set. Busy (page write running)
 * -> NACK BUFFER_FULL: the request is dropped, the controller repeats it. */
static uint8_t respond(uint8_t cc, uint16_t pid, uint8_t pdl)
{
    uint8_t i = 0, known = 0;
    for (;; i++) {
        if (i == P_COUNT) return nack(known ? NR_UNSUPPORTED_CC : NR_UNKNOWN_PID);
        if (pgm_read_word(&pids[i].pid) == pid) {
            known = 1;
            if (pgm_read_byte(&pids[i].cc) == cc) break;
        }
    }
    if (pgm_read_byte(&pids[i].pdl) != pdl) return nack(NR_FORMAT_ERROR);

    switch (i) {
    case P_SUPPORTED:
        return 0;                             /* empty list, see pids[] */
    case P_DEVINFO:
        return answer_P(dev_info, sizeof(dev_info));
    case P_SWLABEL:
        return answer_P(s_sw, sizeof(s_sw));
    case P_ID_GET:
        pd[0] = identify;
        return 1;
    case P_ID_SET:
        if (pd[0] > 1) return nack(NR_DATA_OUT_OF_RANGE);
        identify = pd[0];
        return 0;
    case P_INFO:
        answer_P(boot_info_c, sizeof(boot_info_c));
        pd[4] = busy();
        pd[5] = last_err;
        last_err = BOOT_ERR_NONE;             /* read clears */
        memcpy(pd + sizeof(boot_info_c), app_st, sizeof(app_st));
        return BOOT_INFO_PDL;
    case P_WRITE: {
        uint16_t a = get16(pd);
        if (a >= BOOT_START || (a & (SPM_PAGESIZE - 1))) return nack(NR_DATA_OUT_OF_RANGE);
        if (busy()) return nack(NR_BUFFER_FULL);
        memcpy(page, pd + 2, SPM_PAGESIZE);
        fl_addr = a;
        app_st[0] = 0;                        /* installed app no longer known valid */
        spm(SPM_ERASE, a, 0);                 /* SPM idle (not busy); RWW erase runs on */
        fl = FL_FILL;
        return 0;
    }
    case P_CRC: {                             /* synchronous, chunk <= 256 B */
        uint16_t a = get16(pd), n = get16(pd + 2);
        if (n > CRC_MAX_LEN || a > BOOT_START - n) return nack(NR_DATA_OUT_OF_RANGE);
        if (busy()) return nack(NR_BUFFER_FULL);
        return put16(pd, crc_flash(0xFFFF, a, n));
    }
    default:                                  /* P_FINISH */
        if (busy()) return nack(NR_BUFFER_FULL);
        finish = 1;
        return 0;
    }
}

static void handle(void)
{
    uint8_t *p = pk;
    asm("" : "+b"(p));                       /* pk in Y/Z: ldd/std instead of lds/sts */
    uint8_t len = p[O_LEN];
    uint8_t pdl = p[O_PDL];
    if (p[O_SUBSC] != 0x01 || pdl + O_PD != len || checksum(p, len) != get16(p + len)) return;

    uint8_t bcast = 0;                        /* FFFF:FFFFFFFF or 7FF0:FFFFFFFF */
    if (memcmp(p + O_DEST, uid, 6)) {
        if ((p[5] & p[6] & p[7] & p[8]) != 0xFF) return;
        if ((p[3] & p[4]) == 0xFF) {
            if (p[O_PID] & 0x80) return;      /* manufacturer PID to all manufacturers: ignored (E1.20 6.2.10.2) */
        } else if (p[3] != uid[0] || p[4] != uid[1]) {
            return;
        }
        bcast = 1;
    }

    uint8_t cc = p[O_CC];
    uint16_t pid = get16(p + O_PID);
    rt = RT_ACK;

    if (cc == CC_DISC) {
        if (pid == PID_DISC_UNIQUE_BRANCH) {
            if (!muted && pdl == 12 && memcmp(p + O_PD, uid, 6) <= 0 && memcmp(uid, p + O_PD + 6, 6) <= 0)
                send_dub_response();
            return;
        }
        if ((pid != PID_DISC_MUTE && pid != PID_DISC_UN_MUTE) || pdl != 0) return;
        muted = (pid == PID_DISC_MUTE);
        p[O_PD] = 0;                                  /* control field: Boot-Loader flag while the */
        p[O_PD + 1] = app_st[0] ? 0 : CF_BOOTLOADER;  /* device cannot run without an upload (7.6.1) */
        pdl = 2;
    } else if (cc == CC_GET || cc == CC_SET) {
        if (bcast && cc == CC_GET) return;    /* broadcast GET: invalid (E1.20), ignored */
        if (pid >= PID_BOOT_INFO && pid <= PID_BOOT_FINISH) stay_idle();   /* bootloader traffic */
        uint8_t sh = p[O_SUBDEV], sl = p[O_SUBDEV + 1];   /* root only; 0xFFFF (all) only for SET */
        pdl = ((sh | sl) && !(cc == CC_SET && (sh & sl) == 0xFF)) ? nack(NR_SUB_DEVICE_OUT_OF_RANGE) : respond(cc, pid, pdl);
    } else {
        return;
    }

    if (!bcast) {                             /* broadcast: executed, never answered */
        /* build the response in place (TN, sub-device, PID stay) */
        for (uint8_t i = 0; i < 6; i++) {    /* dest = request source, source = us */
            p[O_DEST + i] = p[O_SRC + i];
            p[O_SRC + i] = uid[i];
        }
        p[O_RT] = rt;
        p[O_MSGCNT] = 0;                      /* never queued messages */
        p[O_CC] = cc + 1;
        p[O_PDL] = pdl;
        len = O_PD + pdl;
        p[O_LEN] = len;
        put16(p + len, checksum(p, len));
        tx(p, len + 2, 1);
    }

    if (finish) {                             /* ACK is out (or broadcast) */
        finish = 0;
        last_err = app_check();
        if (!last_err) start_app();
    }
}

/* --------------------------------------------------------------- main */

void boot_main(void)
{
    uint8_t mcucsr = MCUCSR;
    uint16_t magic = BOOT_MAGIC;
    MCUCSR = 0;
    BOOT_MAGIC = 0;
    stay = 0;
    if (mcucsr & (1 << WDRF)) {
        if (magic == BOOT_MAGIC_APP) ((void (*)(void))0)();   /* from start_app() */
        stay = (magic == BOOT_MAGIC_VALUE);                    /* app: PID 0x8000 */
    }
    wdt_reset();
    WDT_SET(WDTO_1S);                         /* WDTON: reset default is ~16 ms */
    idx = RX_OFF;
    fl = FL_IDLE;
    rt = RT_ACK;
    last_err = BOOT_ERR_NONE;
    finish = 0;

    put16(uid, RDM_MANUFACTURER_ID);
    EEARH = 0;                                /* undefined after reset */
#if EE_ADDR_DEVICE_ID > 252
#error "EE_ADDR_DEVICE_ID: EEARH"
#endif
    for (uint8_t i = 0; i < 4; i++) {         /* same EEPROM ID as the app (blank = FFFFFFFF) */
        EEARL = EE_ADDR_DEVICE_ID + i;
        EECR |= (1 << EERE);
        uid[2 + i] = EEDR;
    }

    DE_RX();
    RS485_DE_DDR |= (1 << RS485_DE_BIT);
    PORTD |= (1 << PD1);                      /* TXD idle high (break via GPIO) */
    DDRD |= (1 << PD1) | LED_MASK_D;          /* LED channels: outputs, off (identify) */
    DDRC = LED_MASK_C;
    DDRB = LED_MASK_B;
    UBRRH = (uint8_t)(DMX_UBRR >> 8);
    UBRRL = (uint8_t)DMX_UBRR;
    UCSRC = (1 << URSEL) | (1 << USBS) | (1 << UCSZ1) | (1 << UCSZ0);   /* 8N2 */
    UCSRB = (1 << RXEN);

    if (app_check() || stay) {                /* invalid app or requested */
        stay_idle();
    } else {
        TCNT1 = (uint16_t)(0 - WINDOW_TICKS);
        TIFR = (1 << TOV1);
        ovf = 1;
    }
    TCCR1B = T1_PRESCALE;

    for (;;) {
        wdt_reset();
        if (TIFR & (1 << TOV1)) {
            TIFR = (1 << TOV1);
            if (!--ovf) {
                if (!stay || (!busy() && !app_check())) start_app();
                ovf = IDLE_OVF;
            }
        }
        flash_step();
        leds();
        if (rx_poll()) {
            handle();
        }
    }
}
