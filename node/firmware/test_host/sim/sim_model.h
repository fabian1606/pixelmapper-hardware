/*
 * sim_model.h - full device models for the simulated RDM bus (sim_bus.h), used by the virtual node.
 *
 * Tube: the app tube_fw (DEVICE_INFO, labels, DMX_START_ADDRESS, IDENTIFY, ENTER_BOOTLOADER) and the RDM bootloader
 * tube_boot_rdm v3 (BOOT_INFO, BOOT_WRITE_PAGE, BOOT_APP_CRC, BOOT_FINISH) with the same UID, as in
 * crates/pixelmapper-net/src/sim/tube.rs and tube/tools/flash_rdm.py (FakeTube): 0x1800-byte app flash with the
 * 16-byte trailer at 0x17F0 (protocol/image-format.md §1), trailer check on FINISH/reset, rescue window after power-on
 * (sim_bus), 60 s bootloader idle timeout, silent phases after resets. Time = the bus clock (sim_bus_t.now_ms).
 *
 * Fixture: a generic RDM fixture of another manufacturer (DEVICE_INFO, labels, DMX_START_ADDRESS, IDENTIFY).
 */
#pragma once

#include "sim_bus.h"

#define SIM_TUBE_APP_END 0x1800u           /* BOOT_INFO app_end, start of the bootloader */
#define SIM_TUBE_TRAILER 0x17F0u
#define SIM_TUBE_ENTER_BOOT_MS 40u         /* silence after ENTER_BOOTLOADER (EEPROM flush, reset, CRC check) */
#define SIM_TUBE_APP_START_MS 1800u        /* silence after the app starts (self-test) */
#define SIM_TUBE_BOOT_TIMEOUT_MS 60000u    /* staying bootloader starts a valid app after this long without a boot PID */

typedef struct sim_model {
    bool tube;                      /* tube (app + bootloader) or generic fixture */
    uint16_t model_id;              /* DEVICE_MODEL_ID */
    uint16_t footprint;             /* fixture: DMX footprint */
    uint32_t fixture_version;       /* fixture: software version id */
    /* tube */
    uint8_t flash[SIM_TUBE_APP_END];
    uint32_t running_version;       /* version of the running app (DEVICE_INFO) */
    bool cached_valid;              /* BOOT_INFO app_valid (last trailer check, WRITE_PAGE clears it) */
    uint32_t cached_version;        /* BOOT_INFO app_fw_version */
    uint8_t last_error;             /* BOOT_INFO last_error (reading clears it) */
    uint8_t busy;                   /* requests a page write keeps the bootloader busy */
    uint8_t busy_requests;          /* setting: busy requests after each page write (0 = never busy) */
    uint32_t idle_deadline_ms;      /* staying bootloader: start a valid app then */
    uint32_t silent_until_ms;       /* no answers until then (0 = not silent) */
    uint32_t app_start_ms;          /* silence after an app start (default SIM_TUBE_APP_START_MS) */
    /* settings (EEPROM) and state */
    uint16_t start_address;
    uint8_t label_len;
    char label[32];
    bool identify;
    /* counters */
    uint32_t pages_written;
    uint32_t finishes;
    uint32_t enters;
} sim_model_t;

/** Tube with app image `img` (SIM_TUBE_APP_END bytes, NULL = blank flash: stays in the bootloader). Runs the app if
 *  the trailer is valid for its manufacturer/MODEL_TUBE. Call after sim_add(); sets dev->tube/app_valid/mode. */
void sim_model_tube(sim_bus_t *b, int idx, sim_model_t *m, const uint8_t *img);

/** Generic fixture (DEVICE_INFO model `model_id`, footprint, version). */
void sim_model_fixture(sim_bus_t *b, int idx, sim_model_t *m, uint16_t model_id, uint16_t footprint);

/** Build a valid tube app image into `img` (SIM_TUBE_APP_END bytes): `app_len` pattern bytes + trailer. */
void sim_model_build_tube_image(uint8_t *img, uint16_t app_len, uint16_t manufacturer, uint16_t model,
                                uint32_t version);

/** Trailer check as the bootloader does it on FINISH: PM_BOOT_ERR_* (0 = valid). */
uint8_t sim_model_trailer_check(const uint8_t *flash, uint16_t manufacturer, uint16_t model);

/* ---- used by sim_bus.c ---- */

/** Lazy timers of a model device (after power-on/presence handling). False while it cannot answer. */
bool sim_model_update(sim_bus_t *b, sim_dev_t *d);
/** Power-on (end of the off time): reset state, window/app decided by sim_bus. */
void sim_model_power_on(sim_bus_t *b, sim_dev_t *d);
/** End of the rescue window without a catch: app if valid (silent self-test), else staying bootloader. */
void sim_model_window_end(sim_bus_t *b, sim_dev_t *d);
/** GET/SET answer of a model device (wire bytes into `out`, 0 = silent). `bcast`: executed, never answered. */
int sim_model_respond(sim_bus_t *b, sim_dev_t *d, const pm_rdm_msg_t *m, bool bcast, uint8_t *out);
