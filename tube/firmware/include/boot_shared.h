/*
 * Definitions shared by tube_fw (application) and the bootloaders.
 * Change only together - all images have to agree.
 *
 * Two flash layouts:
 *   BOOT_LAYOUT_2K (tube_fw, tube_boot_rdm): BOOTSZ=00 -> 1024 words = 2 KB
 *       boot section at byte 0x1800 (= the whole NRWW section), hfuse 0x88
 *   default (legacy tube_boot, unchanged build): BOOTSZ=01 -> 512 words =
 *       1 KB at 0x1C00, hfuse 0x8A
 * The app code itself does not depend on the layout, only its size limit
 * (platformio.ini board_upload.maximum_size) does.
 */
#pragma once
#include <avr/io.h>

#ifdef BOOT_LAYOUT_2K
#define BOOT_START        0x1800
#else
#define BOOT_START        0x1C00
#endif

/* App trailer, last bytes of the application space (little-endian).
 *
 * Legacy 1 KB layout (old tube_boot, flash_dmx.py builds it): 4 bytes
 *   0x1BFC uint16 length, 0x1BFE uint16 CRC16 over flash 0 .. length-1
 *
 * BOOT_LAYOUT_2K (tube_fw + tube_boot_rdm) - STABLE CONTRACT, external
 * uploaders (Pixelmapper) read it from the .hex. 16 bytes:
 *   0x17F0 uint16 TRAILER_MAGIC 0x4D50 ("PM" in flash order)
 *   0x17F2 uint8  TRAILER_FORMAT 1
 *   0x17F3 uint8  reserved, 0xFF
 *   0x17F4 uint16 RDM manufacturer ID (RDM_MANUFACTURER_ID, config.h)
 *   0x17F6 uint16 RDM device model ID (RDM_DEVICE_MODEL_ID, config.h)
 *   0x17F8 uint32 FW_VERSION_ID (config.h, = DEVICE_INFO software version)
 *   0x17FC uint16 app length (even, <= APP_MAX_SIZE)
 *   0x17FE uint16 CRC16 over flash 0 .. length-1, then 0x17F0..0x17FB
 * 0x17F0..0x17FB come from the app build (section .trailer, tube_fw
 * src/main.c), length + CRC from its post-build step (tube_fw extra_script.py
 * -> tube_fw-<version>.hex). The uploader computes nothing.
 * CRC16-CCITT: poly 0x1021, init 0xFFFF, no reflection (_crc_xmodem_update). */
#ifdef BOOT_LAYOUT_2K
#define TRAILER_START     (BOOT_START - 16)            /* 0x17F0 */
#define TRAILER_MAGIC     0x4D50
#define TRAILER_FORMAT    1
#define TRAILER_META_LEN  12                           /* 0x17F0..0x17FB, in the CRC */
#define APP_TRAILER       (BOOT_START - 4)             /* length, CRC: 0x17FC / 0x17FE */
#define APP_MAX_SIZE      TRAILER_START                /* 6128 B */
#ifndef __ASSEMBLER__
#include <stdint.h>
typedef struct {
    uint16_t magic;
    uint8_t format;
    uint8_t reserved;
    uint16_t manufacturer;
    uint16_t model;
    uint32_t fw_version;
} app_trailer_meta_t;                                  /* 0x17F0..0x17FB */
_Static_assert(sizeof(app_trailer_meta_t) == TRAILER_META_LEN, "trailer layout");
#endif
#else
#define APP_TRAILER       (BOOT_START - 4)
#define APP_MAX_SIZE      APP_TRAILER                  /* legacy 7164 B */
#endif

/* "Stay in bootloader" request: the app writes BOOT_MAGIC_VALUE to the last
 * two bytes of SRAM and lets the watchdog reset the chip. Both images start
 * their stack below it (app: -Wl,--defsym=__stack=0x45D in platformio.ini). */
#define BOOT_MAGIC_ADDR   (RAMEND - 1)                 /* 0x045E..0x045F */
#define BOOT_MAGIC        (*(volatile uint16_t *)BOOT_MAGIC_ADDR)
#define BOOT_MAGIC_VALUE  0xB007
/* tube_boot_rdm only: written by the bootloader itself before its own
 * watchdog reset -> next boot jumps straight to the app (clean reset state) */
#define BOOT_MAGIC_APP    0xA990
#define BOOT_STACK_TOP    (RAMEND - 2)                 /* 0x045D */
