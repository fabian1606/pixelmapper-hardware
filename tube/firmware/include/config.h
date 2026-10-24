/*
 * tube_fw - hardware & feature configuration (ATmega8, LED tube, DMX512/RDM)
 *
 * EVERY board-specific detail lives in this file. Items that still have to be
 * verified on the real PCB are marked "TODO(pinout)". The defaults below only
 * make the project compile; they are NOT the real wiring.
 */
#pragma once

#include <avr/io.h>

/* ======================================================================
 * Clock
 * ==================================================================== */
/* Resolved: CQ16000 crystal, 16 MHz. Set in platformio.ini
 * (board_build.f_cpu) - that becomes -DF_CPU. This is only the fallback. */
#ifndef F_CPU
#define F_CPU 16000000UL
#endif

/* ======================================================================
 * RS-485 transceiver (USART: PD0 = RXD, PD1 = TXD)
 * ==================================================================== */
/* Resolved: DE on PD2, DE and /RE tied together (MAX485-type),
 * high = transmit.
 * For a receive-only build (RDM disabled) remove the four RS485_DE_* lines
 * and define RS485_DE_PIN_NONE instead. */
#define RS485_DE_PORT        PORTD
#define RS485_DE_DDR         DDRD
#define RS485_DE_BIT         2
#define RS485_DE_ACTIVE_HIGH 1      /* 1: DE high = transmit */
/* #define RS485_DE_PIN_NONE */

#ifdef RS485_DE_PIN_NONE
#define RDM_ENABLED 0
#else
#define RDM_ENABLED 1
#endif

/* ======================================================================
 * LED outputs
 * ==================================================================== */
/* Number of RGB segments of the tube (personality 1/4 use 3*N channels). */
#define LED_SEGMENTS 4                          /* resolved: 4 RGB segments */
#define LED_CHANNELS (3 * LED_SEGMENTS)

/* Output backend. Only the direct-GPIO BCM backend exists so far; a
 * shift-register / LED-driver backend can be added in src/output_*.c
 * implementing the API in src/output.h. */
#define OUTPUT_BACKEND_GPIO_BCM 1
#define OUTPUT_BACKEND          OUTPUT_BACKEND_GPIO_BCM

/* Channel table: one X(port letter B/C/D, bit 0..7, LED_ACTIVE_HIGH |
 * LED_ACTIVE_LOW) per channel, order = segment 1 R,G,B, segment 2 R,G,B ...
 * The app builds its channel map from it (LED_CHANNEL_MAP), the bootloader
 * its identify pin masks (LED_MASK_B/C/D) - one source for both.
 * Pins NOT available: PB6/PB7 (crystal), PC6 (reset), PD0/PD1 (USART),
 * the RS-485 DE pin. PB3..PB5 are ISP pins (usable, but LEDs flicker while
 * programming). */
#define LED_PORT_B 0
#define LED_PORT_C 1
#define LED_PORT_D 2
#define LED_ACTIVE_HIGH 0
#define LED_ACTIVE_LOW  1

/* Resolved (pin-finder test): all 12 gates active-high.
 * Segment numbering as reported by the user; which physical end of the tube
 * is segment 1 is still unknown.
 * Unused: PB2, PD3 (left as inputs), PB3..PB5 (ISP, left as inputs). */
#define LED_CHANNELS_X(X)                                                          \
    X(D, 6, LED_ACTIVE_HIGH) X(D, 7, LED_ACTIVE_HIGH) X(D, 5, LED_ACTIVE_HIGH) /* seg 1 R G B */ \
    X(B, 1, LED_ACTIVE_HIGH) X(D, 4, LED_ACTIVE_HIGH) X(B, 0, LED_ACTIVE_HIGH) /* seg 2 R G B */ \
    X(C, 4, LED_ACTIVE_HIGH) X(C, 5, LED_ACTIVE_HIGH) X(C, 3, LED_ACTIVE_HIGH) /* seg 3 R G B */ \
    X(C, 1, LED_ACTIVE_HIGH) X(C, 2, LED_ACTIVE_HIGH) X(C, 0, LED_ACTIVE_HIGH) /* seg 4 R G B */

#define LED_CH_(port, bit, pol) { LED_PORT_##port, (bit), (pol) },
#define LED_CHANNEL_MAP { LED_CHANNELS_X(LED_CH_) }
#define LED_MB_(port, bit, pol) | (LED_PORT_##port == LED_PORT_B ? 1 << (bit) : 0)
#define LED_MC_(port, bit, pol) | (LED_PORT_##port == LED_PORT_C ? 1 << (bit) : 0)
#define LED_MD_(port, bit, pol) | (LED_PORT_##port == LED_PORT_D ? 1 << (bit) : 0)
#define LED_POL_(port, bit, pol) | (pol)
#define LED_MASK_B         (0 LED_CHANNELS_X(LED_MB_))
#define LED_MASK_C         (0 LED_CHANNELS_X(LED_MC_))
#define LED_MASK_D         (0 LED_CHANNELS_X(LED_MD_))
#define LED_ANY_ACTIVE_LOW (0 LED_CHANNELS_X(LED_POL_))

/* ======================================================================
 * PWM (Binary Code Modulation on Timer1 compare A)
 * ==================================================================== */
/* LSB time in microseconds. PWM period = 255 * BCM_BASE_US
 * -> 8 us = 2.04 ms = 490 Hz (independent of F_CPU). */
#define BCM_BASE_US   8
#define GAMMA_ENABLE  1         /* 1: gamma 2.2 table (256 B flash) */

/* ======================================================================
 * DMX512
 * ==================================================================== */
#define DMX_LOSS_HOLD     0     /* keep last values */
#define DMX_LOSS_BLACKOUT 1     /* all off */
#define DMX_LOSS_WHITE    2     /* fallback colour white */

#define DMX_LOSS_TIMEOUT_MS 1000
#define DMX_LOSS_MODE       DMX_LOSS_HOLD
#define DMX_LOSS_WHITE_LEVEL 128   /* level for DMX_LOSS_WHITE (pre-gamma) */

/* ======================================================================
 * RDM (ANSI E1.20)
 * ==================================================================== */
#define RDM_MANUFACTURER_ID   0x7FF0     /* ESTA prototype range; also UID + trailer */
#define RDM_DEVICE_MODEL_ID   0x0001     /* app, bootloader and trailer */
#define RDM_PRODUCT_CATEGORY  0x0101     /* PRODUCT_CATEGORY_FIXTURE_FIXED */
/* Firmware version: the only place. DEVICE_INFO, SOFTWARE_VERSION_LABEL and
 * the app trailer (boot_shared.h, read by uploaders) are all derived from it.
 * FW_VERSION_ID = 0x00MMmmpp. */
#define FW_VERSION_MAJOR      0
#define FW_VERSION_MINOR      5
#define FW_VERSION_PATCH      0
#define FW_VERSION_ID         (((uint32_t)FW_VERSION_MAJOR << 16) | (FW_VERSION_MINOR << 8) | FW_VERSION_PATCH)
#define FW_STR_(x)            #x
#define FW_STR(x)             FW_STR_(x)
#define FW_VERSION_LABEL      "tube_fw " FW_STR(FW_VERSION_MAJOR) "." FW_STR(FW_VERSION_MINOR) "." FW_STR(FW_VERSION_PATCH)
#define MODEL_DESCRIPTION     "XBAR500 LED tube (custom fw)"
#define MANUFACTURER_LABEL    "Prototype"
#define DEVICE_LABEL_MAX      32         /* stored in EEPROM, full E1.20 length */
/* manufacturer specific PIDs (PID_ENTER_BOOTLOADER ...): rdm_boot.h */

#define RDM_TURNAROUND_US     180        /* wait after request (min 176 us) */
#define RDM_BREAK_US          200        /* response break 176..352 us */
#define RDM_MAB_US            20         /* response MAB 12..88 us */

/* Identify: blink white */
#define IDENTIFY_LEVEL        255

/* ======================================================================
 * Self-test, watchdog, device ID
 * ==================================================================== */
#define SELFTEST_ENABLE   1
#define SELFTEST_STEP_MS  150       /* per colour per segment */
#define SELFTEST_LEVEL    255

#define WDT_TIMEOUT       WDTO_500MS  /* fuses have WDTON set: WDT always on */

/* ADC channel sampled for the random device ID (floating/noisy input is
 * best). ADC6/ADC7 exist on TQFP only. TODO(pinout): pick an unused one. */
#define ADC_NOISE_CHANNEL 7

/* EEPROM layout. Device ID is stored big-endian at address 0 so it can be
 * written with avrdude "-U eeprom:w:0xAA,0xBB,0xCC,0xDD:m". */
#define EE_ADDR_DEVICE_ID 0x000
#define EE_ADDR_SETTINGS  0x010
#define SETTINGS_MAGIC    0x5A
#define SETTINGS_VERSION  2       /* 2: label 32 chars (1: 16) */
#define DEFAULT_START_ADDRESS 1
#define DEFAULT_PERSONALITY   1

/* ======================================================================
 * Sanity checks
 * ==================================================================== */
#define DMX_BAUD 250000UL
#define DMX_UBRR ((F_CPU + 8UL * DMX_BAUD) / (16UL * DMX_BAUD) - 1)
#define DMX_BAUD_REAL (F_CPU / (16UL * (DMX_UBRR + 1)))
#if (DMX_BAUD_REAL * 1000UL / DMX_BAUD) < 980 || (DMX_BAUD_REAL * 1000UL / DMX_BAUD) > 1020
#error "F_CPU cannot produce 250000 baud within 2% (U2X off) - use 8/16 MHz etc."
#endif
#if DMX_UBRR > 4095
#error "UBRR out of range"
#endif
#if LED_SEGMENTS < 1 || LED_SEGMENTS > 8
#error "LED_SEGMENTS out of range"
#endif
