/*
 * host.h - simulation API of the host renderer (fake model, virtual time, scripted input, snapshots).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pm_ui.h"

#define FAKE_DEVS 16

typedef struct {
    pm_uid_t uid;
    uint16_t start, footprint;
    uint8_t pers, pers_count;
    char label[33];
    bool identify;
    bool dead; /* never answers */
} fake_dev_t;

/** Everything the fake model reports. Scenes change it directly. */
typedef struct {
    pm_settings_t settings;
    pm_port_stats_t stats[PM_NUM_PORTS];  /* port engine: stats.fps = DMX frames sent (output) / received (input) */
    uint8_t rx_fps[PM_NUM_PORTS];         /* Art-Net: ArtDmx frames/s received for an output port (port_live) */
    bool loss[PM_NUM_PORTS], merging[PM_NUM_PORTS], synced[PM_NUM_PORTS];
    uint32_t ip, mask, gateway;
    uint8_t net_mode; /* pm_net_addr_mode_t */
    bool link;
    fake_dev_t dev[PM_NUM_PORTS][FAKE_DEVS];
    uint16_t n_dev[PM_NUM_PORTS];
    /* side effects seen by the platform */
    bool restarted, identify; /* identify: last model->identify() call (panel only) */
    int identify_calls;
    uint8_t backlight;
    bool panel_on;
} fake_t;

extern fake_t F;

/** Reset the simulation: default fake model (4 ports, devices on port 1), fresh UI, time 0. */
void sim_reset(void);
/** Like sim_reset, but the UI gets `model` instead of the fake model (e.g. all callbacks NULL). */
void sim_reset_model(const pm_ui_model_t *model);

/** Failed scene checks (the renderer exits non-zero, ctest pm_ui_render fails). */
extern int g_host_fail;
#define HCHECK(cond)                                                                \
    do {                                                                            \
        if (!(cond)) {                                                              \
            g_host_fail++;                                                          \
            printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
        }                                                                           \
    } while (0)
/** Advance virtual time (input polled every 5 ms, UI tick every 20 ms). */
void sim_run(uint32_t ms);
/** Virtual time in ms. */
uint32_t sim_now(void);
/** Run until a blink phase starts: (now % period) < 20. */
void sim_align(uint32_t period);
/** Encoder detents, `gap_ms` between detents (>80 ms = no acceleration). */
void key_rot(int detents, uint32_t gap_ms);
void key_push(void);
void key_back(void);
void key_back_long(void);
/** Hold push for ms (released afterwards unless `keep`). */
void key_hold_push(uint32_t ms, bool keep);
/** Hold BACK + push together for ms; released afterwards unless `keep`. */
void key_combo(uint32_t ms, bool keep);
void key_release_all(void);
/** Compact key script: "R3" turn 3, "R-2", "F5" turn fast, "P" push, "B" back, "L" back long, "W500" wait. */
void keys(const char *script);
/** Settings change from outside (applied to the fake + PM_UI_EV_SETTINGS_CHANGED with `src`). */
void remote_settings(const pm_settings_t *s, pm_change_source_t src);
void post_event(pm_ui_event_t ev);
/** Write out/<name>.png (4x scaled screen). */
void snap(const char *name);

typedef struct {
    const char *name;
    const char *doc;
    void (*run)(void);
} scene_t;

extern const scene_t g_scenes[];
extern const int g_num_scenes;
