/*
 * vn.h - virtual Pixelmapper Node for the host (macOS/Linux): the real pm_node / pm_artnet / pm_rdm code with
 * fake platform parts, reachable over real UDP sockets. See README section in vn_main.c.
 *
 * Parts:
 *   vn_main.c    command line, sockets (Art-Net unicast, emulated broadcast, control), peer address mapping,
 *                single-threaded event loop (like the network task), pm_node ops, reboot
 *   vn_port.c    fake pm_port: one pm_rdm_ctrl per port on a simulated RS-485 bus (sim_bus + sim_model) with
 *                tubes/fixtures, DMX output buffers, input frame generator, BOOT_CATCH
 *   vn_config.c  settings in RAM with deferred change notifications (like pm_config + main.c)
 *   vn_fw.c      firmware sink: pm_ota_session with a RAM flash backend (image-format.md §2.3)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "pm_node.h"
#include "sim_bus.h"
#include "sim_model.h"

/* ------------------------------------------------------------------------------------------------ time */

/** Monotonic milliseconds since start (+1000, never 0). */
uint32_t vn_now_ms(void);

/* ------------------------------------------------------------------------------------------------ config */

void vn_config_init(void);
void vn_config_get(pm_settings_t *out);
pm_err_t vn_config_modify(pm_node_modify_fn fn, void *fctx, pm_change_source_t src);
pm_err_t vn_config_factory_reset(pm_change_source_t src);
/** Deliver pending change masks to the node (per source), like main.c work_settings_changed. */
void vn_config_flush(pm_node_t *node);

/* ------------------------------------------------------------------------------------------------ port engine */

#define VN_MAX_MODELS 64     /* tubes/fixtures with a full model (more devices: plain sim devices) */

typedef struct {
    int input_mode;          /* 0 off, 1 ramp (changes every frame), 2 static (constant frame) */
    uint8_t input_value;
} vn_input_t;

void vn_port_init(const pm_uid_t *node_uid, uint16_t dmx_slots);
/** Add a device to port `port` (0-based). kind: 't' tube (with app image version `version`, 0 = blank flash),
 *  'f' fixture, 'p' plain (minimal sim device). Returns the bus index or -1. */
int vn_port_add(pm_port_idx_t port, char kind, pm_uid_t uid, uint32_t version);
bool vn_port_remove(pm_port_idx_t port, const pm_uid_t *uid);
bool vn_port_power_cycle(pm_port_idx_t port, const pm_uid_t *uid, uint32_t off_ms);
/** Add `n` plain devices with random UIDs. */
void vn_port_add_random(pm_port_idx_t port, int n);
sim_dev_t *vn_port_dev(pm_port_idx_t port, const pm_uid_t *uid);
void vn_port_set_input(pm_port_idx_t port, int mode, uint8_t value);
/** Current DMX output of a port (what the line sends). Returns the slot count. */
uint16_t vn_port_output(pm_port_idx_t port, uint8_t *out, uint16_t cap);
uint32_t vn_port_frames(pm_port_idx_t port);
/** Run the simulated lines up to now, then deliver queued events to `node`. */
void vn_port_run(pm_node_t *node);
/** Re-apply the line configuration after a node restart (forces a full discovery like a boot). */
void vn_port_reset(void);
bool vn_port_busy(void);

/* pm_node ops of the port engine */
pm_err_t vn_port_configure(void *ctx, pm_port_idx_t port, const pm_node_port_cfg_t *cfg);
void vn_port_output_write(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer);
void vn_port_commit(void *ctx);
void vn_port_clear(void *ctx, pm_port_idx_t port);
void vn_port_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out);
uint16_t vn_port_input_read(void *ctx, pm_port_idx_t port, uint8_t *out, uint16_t cap);
pm_err_t vn_port_rdm_submit(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *wire, uint16_t len);
uint8_t vn_port_rdm_queue_free(void *ctx, pm_port_idx_t port);
uint16_t vn_port_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap);
void vn_port_tod_control(void *ctx, pm_port_idx_t port, uint8_t command);
pm_err_t vn_port_catch_start(void *ctx, pm_port_idx_t port, const pm_uid_t *target);
void vn_port_catch_abort(void *ctx);
void vn_port_catch_status(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out);

/* ------------------------------------------------------------------------------------------------ firmware sink */

#define VN_FW_PARTITION (2u * 1024u * 1024u)

void vn_fw_init(uint8_t hw_rev);
const pm_fw_sink_t *vn_fw_sink(void);
/** 30-s session timeout (call about once per second). */
void vn_fw_poll(void);
/** Listener: progress to the node (set by vn_main). */
void vn_fw_set_listener(void (*fn)(const pm_fw_status_t *st));
/** True once after sink->reboot(); `*have_image` + `installed` = description of the newly installed image. */
bool vn_fw_take_reboot(pm_image_desc_t *installed, bool *have_image);
/** Make the next FW_BEGIN / FirmFirst fail in the backend (FLASH) - test hook. */
void vn_fw_fail_next_begin(bool on);
