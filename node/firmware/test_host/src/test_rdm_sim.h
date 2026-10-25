/*
 * test_rdm_sim.h - simulated RS-485 RDM bus with virtual responders for the pm_rdm_ctrl host tests.
 *
 * Every responder sees each transaction. DUB answers of several responders are combined like a wired AND (each
 * responder picks its own preamble length, so answers may be misaligned), optionally reported as framing garbage.
 * A "tube" responder models tube_boot_rdm: after power-on it answers in a rescue window (26..225 ms); a GET
 * BOOT_INFO inside the window keeps it in the bootloader, otherwise the app starts (both un-muted after reset).
 */
#pragma once

#include "pm_rdm_ctrl.h"

#define SIM_MAX_DEVS 600

typedef enum {
    SIM_APP = 0,
    SIM_BOOT_WINDOW, /* tube: right after power-on */
    SIM_BOOT,        /* tube: caught / app invalid, bootloader active */
} sim_mode_t;

typedef struct {
    pm_uid_t uid;
    bool present;
    bool muted;
    uint16_t control;        /* DISC_MUTE control field (proxy/sub-device flags) */
    bool binding;            /* answer DISC_MUTE with a binding UID (PDL 8) */
    uint8_t preamble;        /* DUB preamble bytes 0..7 */
    /* quirks */
    bool never_ack_mute;     /* answers DUB, never answers DISC_MUTE */
    bool ignore_mute;        /* ACKs DISC_MUTE but keeps answering DUB */
    bool ignore_range;       /* answers every DUB, whatever the range */
    /* line timing of its responses (random per device, worst case E1.20 + inline proxy) */
    uint16_t resp_delay_us, break_us, mab_us;
    /* tube model */
    bool tube;
    bool app_valid;
    sim_mode_t mode;
    uint32_t power_on_ms;    /* off until then */
    /* counters */
    uint32_t requests;       /* GET/SET addressed to it */
    uint32_t boot_info_gets;
    uint8_t last_pid_hi, last_pid_lo;
} sim_dev_t;

typedef struct {
    sim_dev_t dev[SIM_MAX_DEVS];
    int n;
    uint32_t now_ms;
    uint64_t now_us;
    uint32_t rng;
    unsigned noise_permille;  /* chance that a single answer is corrupted */
    unsigned garbage_percent; /* chance that a DUB collision is reported as PM_RDM_RX_GARBAGE */
    uint8_t rx[600];
    /* statistics */
    uint32_t txns, dubs, collisions, broadcasts;
    uint32_t last_us;          /* line time of the last transaction as the bus saw it */
} sim_bus_t;

void sim_init(sim_bus_t *b, uint32_t seed);
uint32_t sim_rand(sim_bus_t *b);
/** Add a device; returns its index. */
int sim_add(sim_bus_t *b, pm_uid_t uid);
/** Add `n` devices with random distinct UIDs (manufacturer 0x0001..0x7FFE, some clustered). */
void sim_add_random(sim_bus_t *b, int n);
/** Tube: power off now, back on after off_ms (enters the rescue window). */
void sim_power_cycle(sim_bus_t *b, int idx, uint32_t off_ms);
/** Put one transaction on the bus and return what the controller's UART would see. */
void sim_transact(sim_bus_t *b, const pm_rdm_txn_t *t, pm_rdm_rx_t *rx);
int sim_find(const sim_bus_t *b, const pm_uid_t *uid);

/** Port engine model: DMX frames of `slots` slots, RDM in between with a 30 Hz budget (pm_rdm_ctrl_next_max as
 *  documented in pm_rdm_ctrl.h). Runs until now_ms >= until_ms or `stop(ctx)` returns true. */
typedef struct {
    uint32_t frames;
    uint32_t max_period_us;   /* longest break-to-break time */
    uint32_t rdm_txns;
    uint32_t est_violations;  /* transactions that took longer on the line than their est_us */
} sim_port_stats_t;

void sim_run(sim_bus_t *b, pm_rdm_ctrl_t *c, uint32_t until_ms, uint16_t slots, bool (*stop)(void *), void *ctx,
             sim_port_stats_t *st);
