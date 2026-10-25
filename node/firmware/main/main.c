/*
 * main.c - ESP-IDF adapter of the Pixelmapper Node: boots the components and implements pm_node_ops_t on top of
 * pm_config / pm_port / pm_net / pm_ota / pm_ui. The platform-independent wiring (RDM routing, event dispatch,
 * settings fan-out, identify mirroring, tube-update display, live port state) lives in components/pm_node.
 *
 * Core assignment (docs/plan.md "Firmware-Architektur", CLAUDE.md):
 *   core 1: pm_port engine only (DMX/RDM line timing, discovery, ArtSync commit) - started by pm_port_start()
 *   core 0: pm_net network task (pm_node = pm_artnet + RDM responder, single-threaded), pm_config writer task,
 *           pm_ota (runs in the network task), pm_ui task (lowest, 6 KB stack)
 *
 * Threading rule: pm_node (and with it pm_artnet and the responder) is NOT thread-safe. Everything that touches it
 * runs in the network task:
 *   - UDP rx (pm_net -> net_rx -> pm_node_handle_packet, with the destination address from IP_PKTINFO)
 *   - the tick (10 ms, 1 ms while RDM requests are in flight on a line): pm_port events (non-blocking drain) ->
 *     pm_node_port_event, pm_node_tick (pm_artnet_tick, pm_rdm_responder_poll, live snapshot), pm_ota_poll
 *   - settings changes: the pm_config listener runs in the WRITER's task (UI task, network task, ...) inside the
 *     config lock. It only ORs the mask into s_chg_pending[src] and posts work_settings_changed once (coalesced,
 *     no allocation per change); the network task then calls pm_node_settings_changed per source, which does
 *     pm_port_configure, pm_net_apply_ip (not for ArtIpProg/RDM sources), the responder's queued messages,
 *     pm_artnet_settings_changed and the UI notification.
 *   - front panel: RDM requests (fixed slot pool, pm_net_post) and identify (pm_net_post) -> pm_node
 *   - OTA progress: the sink runs in the network task, its listener -> pm_node_fw_progress (+ config flush)
 * Data for other tasks leaves the network task as copies: pm_ui_notify (copies), the live port snapshot (s_live,
 * spinlock), pm_net_get_status / pm_port_get_stats / pm_port_tod_get (thread-safe component APIs).
 *
 * Writes from the network side use pm_config_modify (read-modify-write under the config lock, only the fields of
 * the change mask), so a panel change that is stored but not yet distributed is never overwritten.
 */
#include <stdalign.h>
#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pm_board.h"
#include "pm_config.h"
#include "pm_net.h"
#include "pm_node.h"
#include "pm_ota.h"
#include "pm_pids.h"
#include "pm_port.h"
#include "pm_ui.h"

static const char *TAG = "pm_main";

#define FW_VERSION_ID PM_FW_VERSION_ID(PM_FW_VERSION_MAJOR, PM_FW_VERSION_MINOR, PM_FW_VERSION_PATCH)
#define NET_TICK_MS 10u
#define NET_TICK_FAST_MS 1u      /* while pm_node_busy(): RDM responses come back within a few ms */
#define PORT_EVENTS_PER_TICK 16
#define OTA_POLL_MS 1000u
#define MARK_VALID_AFTER_US (30ull * 1000 * 1000)
#define RESET_DELAY_US (500ull * 1000)
#define UI_RDM_SLOTS 4           /* panel RDM requests in flight to the network task */

_Static_assert((PM_UI_RDM_TOKEN_TAG & PM_NODE_UI_TOKEN_FLAG) == 0 && (PM_UI_RDM_TOKEN_MASK & 0x80000000u) != 0,
               "panel RDM tokens must keep bit 31 clear: pm_node tags them with it on the line");

static pm_uid_t s_uid;
static pm_node_t *s_node;
static bool s_net_ok;
static bool s_port_ok;

/* ------------------------------------------------------------------------------------------------ helpers */

static uint32_t now_ms(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t rnd(void *ctx)
{
    (void)ctx;
    return esp_random();
}

static pm_port_cfg_t to_port_cfg(const pm_node_port_cfg_t *n)
{
    pm_port_cfg_t c = {
        .mode = n->mode == PM_NODE_PORT_OUTPUT ? PM_PORT_MODE_OUTPUT
                                               : (n->mode == PM_NODE_PORT_INPUT ? PM_PORT_MODE_INPUT : PM_PORT_MODE_OFF),
        .rdm_enabled = n->rdm_enabled,
        .rdm_bg_discovery = n->rdm_bg_discovery,
        .rdm_bg_interval_ms = n->rdm_bg_interval_ms,
        .break_us = n->break_us,
        .mab_us = n->mab_us,
        .rate_hz = n->rate_hz,
        .continuous = n->continuous,
        .mbs_us = n->mbs_us,
    };
    return c;
}

static pm_port_cfg_t port_cfg_from(const pm_settings_t *s, pm_port_idx_t i)
{
    pm_node_port_cfg_t n;
    pm_node_port_cfg_from(s, i, &n);
    return to_port_cfg(&n);
}

static pm_err_t from_esp(esp_err_t e)
{
    switch (e) {
    case ESP_OK: return PM_OK;
    case ESP_ERR_NO_MEM: return PM_ERR_FULL;
    case ESP_ERR_INVALID_ARG: return PM_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_STATE: return PM_ERR_STATE;
    default: return PM_ERR_IO;
    }
}

/** Restart after a delay (RESET_DEVICE: the RDM response must leave first). Config is flushed by the shutdown
 *  handler. */
static void restart_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void restart_later(void)
{
    static esp_timer_handle_t t;
    if (t == NULL) {
        const esp_timer_create_args_t a = {.callback = restart_cb, .name = "pm_reset"};
        if (esp_timer_create(&a, &t) != ESP_OK) esp_restart();
    }
    esp_timer_stop(t);
    esp_timer_start_once(t, RESET_DELAY_US);
}

/* ------------------------------------------------------------------------------------------------ pm_node ops */

static void op_config_get(void *ctx, pm_settings_t *out)
{
    (void)ctx;
    pm_config_get(out);
}

static pm_err_t op_config_modify(void *ctx, pm_node_modify_fn fn, void *fctx, pm_change_source_t src)
{
    (void)ctx;
    return from_esp(pm_config_modify(fn, fctx, src));
}

static pm_err_t op_factory(void *ctx, pm_change_source_t src)
{
    (void)ctx;
    return from_esp(pm_config_factory_reset(src));
}

static pm_err_t op_send_udp(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    (void)ctx;
    return pm_net_send_udp(ip, port, data, len) == ESP_OK ? PM_OK : PM_ERR_IO;
}

static void op_net_info(void *ctx, pm_artnet_net_info_t *out)
{
    (void)ctx;
    pm_net_status_t st;
    pm_net_get_status(&st);
    out->ip = st.ip;
    out->mask = st.mask;
    out->gateway = st.gateway;
    memcpy(out->mac, st.mac, 6);
    out->dhcp_active = st.mode == PM_NET_ADDR_DHCP;
    out->link_up = st.link_up;
}

static void op_net_apply_ip(void *ctx, const pm_ip_settings_t *ip)
{
    (void)ctx;
    pm_net_apply_ip(ip, PM_ARTNET_OEM_NODE);
}

static void op_net_renew(void *ctx)
{
    (void)ctx;
    pm_net_renew_dhcp();
}

static pm_err_t op_port_configure(void *ctx, pm_port_idx_t port, const pm_node_port_cfg_t *cfg)
{
    (void)ctx;
    pm_port_cfg_t c = to_port_cfg(cfg);
    esp_err_t e = pm_port_configure(port, &c);
    if (e != ESP_OK) ESP_LOGW(TAG, "port %u configure: %s", (unsigned)port + 1, esp_err_to_name(e));
    return from_esp(e);
}

static void op_port_output(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    (void)ctx;
    pm_port_output_write(port, data, len, defer);
}

static void op_port_commit(void *ctx)
{
    (void)ctx;
    pm_port_commit();
}

static void op_port_clear(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    pm_port_output_clear(port);
}

static void op_port_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out)
{
    (void)ctx;
    pm_port_stats_t st;
    pm_port_get_stats(port, &st);
    out->output_active = st.frames_out > 0;
    out->input_signal = st.input_signal;
    out->rdm_discovery_running = st.discovery_running;
    out->tod_count = st.tod_count;
}

static uint16_t op_input_read(void *ctx, pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    (void)ctx;
    return pm_port_input_read(port, out, cap);
}

static pm_err_t op_rdm_submit(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *wire, uint16_t len)
{
    (void)ctx;
    return from_esp(pm_port_rdm_submit(port, token, wire, len));
}

static uint8_t op_rdm_queue_free(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    return pm_port_rdm_queue_free(port);
}

static uint8_t op_rdm_queue_len(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    (void)port;
    return (uint8_t)PM_RDM_QUEUE_LEN; /* pm_port: one pm_rdm_ctrl queue per port */
}

static uint16_t op_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)ctx;
    return pm_port_tod_get(port, out, cap);
}

static void op_tod_control(void *ctx, pm_port_idx_t port, uint8_t command)
{
    (void)ctx;
    pm_port_tod_control(port, command);
}

static pm_err_t op_catch_start(void *ctx, pm_port_idx_t port, const pm_uid_t *target)
{
    (void)ctx;
    return pm_port_boot_catch_start(port, target) == ESP_OK ? PM_OK : PM_ERR_STATE;
}

static void op_catch_abort(void *ctx)
{
    (void)ctx;
    pm_port_boot_catch_abort();
}

static void op_catch_status(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    (void)ctx;
    pm_port_boot_catch_status(port, out);
}

static void op_reset(void *ctx, uint8_t kind)
{
    (void)ctx;
    ESP_LOGW(TAG, "RESET_DEVICE 0x%02x", kind);
    restart_later();
}

static void op_ui_event(void *ctx, const pm_node_ui_event_t *ev)
{
    (void)ctx;
    static const pm_ui_event_type_t MAP[] = {
        [PM_NODE_UI_SETTINGS_CHANGED] = PM_UI_EV_SETTINGS_CHANGED,
        [PM_NODE_UI_LED_MODE] = PM_UI_EV_LED_MODE,
        [PM_NODE_UI_IDENTIFY] = PM_UI_EV_IDENTIFY,
        [PM_NODE_UI_TOD_CHANGED] = PM_UI_EV_TOD_CHANGED,
        [PM_NODE_UI_RDM_RESPONSE] = PM_UI_EV_RDM_RESPONSE,
        [PM_NODE_UI_FW_PROGRESS] = PM_UI_EV_FW_PROGRESS,
        [PM_NODE_UI_BOOT_CATCH] = PM_UI_EV_BOOT_CATCH,
        [PM_NODE_UI_ERROR] = PM_UI_EV_ERROR,
        [PM_NODE_UI_TUBE_UPDATE] = PM_UI_EV_TUBE_UPDATE,
    };
    if ((unsigned)ev->type >= sizeof(MAP) / sizeof(MAP[0])) return;
    const pm_ui_event_t u = {.type = MAP[ev->type], .port = ev->port, .value = ev->value, .token = ev->token,
                             .len = ev->len, .data = ev->data};
    pm_ui_notify(&u);
}

/* Live port state for the UI: written by the network task every 100 ms, read by the UI task. */
static portMUX_TYPE s_live_lock = portMUX_INITIALIZER_UNLOCKED;
static pm_ui_port_live_t s_live[PM_NUM_PORTS];

static void op_port_live(void *ctx, const pm_artnet_port_live_t *live)
{
    (void)ctx;
    pm_ui_port_live_t v[PM_NUM_PORTS];
    for (unsigned p = 0; p < PM_NUM_PORTS; p++) {
        v[p] = (pm_ui_port_live_t){.rx_active = live[p].rx_active, .rx_fps = live[p].rx_fps,
                                   .merge_sources = live[p].merge_sources, .data_loss = live[p].data_loss,
                                   .merging = live[p].merging, .synced = live[p].synced};
    }
    portENTER_CRITICAL(&s_live_lock);
    memcpy(s_live, v, sizeof(s_live));
    portEXIT_CRITICAL(&s_live_lock);
}

/* ------------------------------------------------------------------------------------------------ settings changes */

/* Pending change masks per source, coalesced until the network task runs (no allocation per change). */
static portMUX_TYPE s_chg_lock = portMUX_INITIALIZER_UNLOCKED;
static pm_change_mask_t s_chg_pending[PM_SRC_SYSTEM + 1];
static bool s_chg_posted;

/** Network task: hand the pending changes to pm_node, per source. */
static void work_settings_changed(void *arg)
{
    (void)arg;
    pm_change_mask_t masks[PM_SRC_SYSTEM + 1];
    portENTER_CRITICAL(&s_chg_lock);
    memcpy(masks, s_chg_pending, sizeof(masks));
    memset(s_chg_pending, 0, sizeof(s_chg_pending));
    s_chg_posted = false;
    portEXIT_CRITICAL(&s_chg_lock);
    for (int src = 0; src <= PM_SRC_SYSTEM; src++) {
        if (masks[src] != 0) pm_node_settings_changed(s_node, masks[src], (pm_change_source_t)src);
    }
}

/** pm_config listener: runs in the writer's task inside the config lock - only copy and post. */
static void on_settings(void *ctx, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src)
{
    (void)ctx;
    (void)s;
    bool post = false;
    portENTER_CRITICAL(&s_chg_lock);
    s_chg_pending[src <= PM_SRC_SYSTEM ? src : PM_SRC_SYSTEM] |= mask;
    if (!s_chg_posted) s_chg_posted = post = true;
    portEXIT_CRITICAL(&s_chg_lock);
    if (post && pm_net_post(work_settings_changed, NULL) != ESP_OK) {
        portENTER_CRITICAL(&s_chg_lock);
        s_chg_posted = false; /* queue full: the next change posts again */
        portEXIT_CRITICAL(&s_chg_lock);
        ESP_LOGW(TAG, "network queue full, settings change notification delayed");
    }
}

/* ------------------------------------------------------------------------------------------------ OTA progress */

/** Network task (the sink runs there). */
static void on_ota(void *ctx, const pm_fw_status_t *st)
{
    (void)ctx;
    if (st->state == PM_FW_STATE_FAILED) ESP_LOGW(TAG, "firmware update failed: error %u", (unsigned)st->last_error);
    if (st->state == PM_FW_STATE_READY) pm_config_flush();
    pm_node_fw_progress(s_node, st);
}

/* ------------------------------------------------------------------------------------------------ network task glue */

static void net_rx(void *ctx, uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, const uint8_t *data, size_t len)
{
    (void)ctx;
    pm_node_handle_packet(s_node, src_ip, src_port, dst_ip, data, len);
}

static void handle_port_event(const pm_port_event_t *ev)
{
    static const pm_node_port_ev_type_t MAP[] = {
        [PM_PORT_EV_RDM_RESPONSE] = PM_NODE_PEV_RDM_RESPONSE,
        [PM_PORT_EV_TOD_CHANGED] = PM_NODE_PEV_TOD_CHANGED,
        [PM_PORT_EV_INPUT_FRAME] = PM_NODE_PEV_INPUT_FRAME,
        [PM_PORT_EV_INPUT_SIGNAL] = PM_NODE_PEV_INPUT_SIGNAL,
        [PM_PORT_EV_BOOT_CATCH] = PM_NODE_PEV_BOOT_CATCH,
        [PM_PORT_EV_ERROR] = PM_NODE_PEV_ERROR,
    };
    if ((unsigned)ev->type >= sizeof(MAP) / sizeof(MAP[0])) return;
    if (ev->type == PM_PORT_EV_ERROR) {
        ESP_LOGW(TAG, "port %u: driver error %s", (unsigned)ev->port + 1, esp_err_to_name((esp_err_t)ev->value));
    }
    const pm_node_port_event_t n = {.type = MAP[ev->type], .port = ev->port, .token = ev->token,
                                    .value = ev->value, .len = ev->len, .data = ev->data};
    pm_node_port_event(s_node, &n);
}

static void net_tick(void *ctx)
{
    (void)ctx;
    static pm_port_event_t ev; /* network task only */
    for (int i = 0; i < PORT_EVENTS_PER_TICK && pm_port_event_receive(&ev, 0); i++) handle_port_event(&ev);
    pm_node_tick(s_node);

    static uint32_t next_ota_poll;
    uint32_t now = now_ms(NULL);
    if ((int32_t)(now - next_ota_poll) >= 0) {
        next_ota_poll = now + OTA_POLL_MS;
        pm_ota_poll(); /* 30 s session timeout */
    }
    pm_net_set_tick_period(pm_node_busy(s_node) ? NET_TICK_FAST_MS : NET_TICK_MS);
}

/** Network task: link/address changed -> unsolicited ArtPollReply from the new address. */
static void on_net_change(void *ctx, const pm_net_status_t *st)
{
    (void)ctx;
    (void)st;
    pm_node_net_changed(s_node);
}

/* ------------------------------------------------------------------------------------------------ UI model */

static void ui_net_info(void *ctx, pm_ui_net_info_t *out)
{
    (void)ctx;
    pm_net_status_t st;
    pm_net_get_status(&st);
    out->ip = st.ip;
    out->mask = st.mask;
    out->gateway = st.gateway;
    out->mode = (uint8_t)st.mode;
    out->link_up = st.link_up;
}

static void ui_port_stats(void *ctx, pm_port_idx_t port, pm_port_stats_t *out)
{
    (void)ctx;
    pm_port_get_stats(port, out);
}

static void ui_port_live(void *ctx, pm_port_idx_t port, pm_ui_port_live_t *out)
{
    (void)ctx;
    if (port >= PM_NUM_PORTS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    portENTER_CRITICAL(&s_live_lock);
    *out = s_live[port];
    portEXIT_CRITICAL(&s_live_lock);
}

static uint16_t ui_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)ctx;
    return pm_port_tod_get(port, out, cap);
}

/* Panel RDM requests: a fixed pool of slots handed to the network task (no malloc per request). */
typedef struct {
    bool busy;
    pm_port_idx_t port;
    uint32_t token;
    uint16_t len;
    uint8_t data[PM_RDM_MAX_MSG_LEN];
} ui_rdm_slot_t;

static portMUX_TYPE s_ui_lock = portMUX_INITIALIZER_UNLOCKED;
static ui_rdm_slot_t s_ui_rdm[UI_RDM_SLOTS];

static void slot_free(ui_rdm_slot_t *r)
{
    portENTER_CRITICAL(&s_ui_lock);
    r->busy = false;
    portEXIT_CRITICAL(&s_ui_lock);
}

/** Network task: panel RDM request -> pm_node (responder or line; SETs to devices -> LOCAL_RDM_SET). */
static void work_ui_rdm(void *arg)
{
    ui_rdm_slot_t *r = arg;
    pm_err_t e = pm_node_ui_rdm_request(s_node, r->port, r->token, r->data, r->len);
    if (e != PM_OK) ESP_LOGD(TAG, "panel RDM request on port %u: %s", (unsigned)r->port + 1, pm_err_name(e));
    slot_free(r);
}

static esp_err_t ui_rdm_request(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    if (data == NULL || len < 2 || len > PM_RDM_MAX_MSG_LEN || port >= PM_NUM_PORTS ||
        (token & PM_NODE_UI_TOKEN_FLAG)) {
        return ESP_ERR_INVALID_ARG;
    }
    ui_rdm_slot_t *r = NULL;
    portENTER_CRITICAL(&s_ui_lock);
    for (unsigned i = 0; i < UI_RDM_SLOTS && r == NULL; i++) {
        if (!s_ui_rdm[i].busy) {
            r = &s_ui_rdm[i];
            r->busy = true;
        }
    }
    portEXIT_CRITICAL(&s_ui_lock);
    if (r == NULL) return ESP_ERR_NO_MEM;
    r->port = port;
    r->token = token;
    r->len = len;
    memcpy(r->data, data, len);
    esp_err_t e = pm_net_post(work_ui_rdm, r);
    if (e != ESP_OK) slot_free(r);
    return e;
}

static void work_ui_identify(void *arg) { pm_node_ui_identify(s_node, arg != NULL); }

static void ui_identify(void *ctx, bool on)
{
    (void)ctx;
    if (pm_net_post(work_ui_identify, on ? (void *)1 : NULL) != ESP_OK) {
        ESP_LOGW(TAG, "network queue full, panel identify dropped");
    }
}

/* ------------------------------------------------------------------------------------------------ rollback */

/** Rollback protection: mark the image valid after 30 s of normal operation (image-format.md §2.3 step 8). */
static void mark_valid_cb(void *arg)
{
    (void)arg;
    if (!pm_ota_pending_verify()) return;
    if (s_net_ok && s_port_ok) {
        ESP_LOGI(TAG, "30 s stable: marking firmware valid");
        pm_ota_mark_valid();
    } else {
        ESP_LOGE(TAG, "new firmware not healthy (net %d, ports %d): not marked valid, next reboot rolls back",
                 s_net_ok, s_port_ok);
    }
}

static void shutdown_flush(void) { pm_config_flush(); }

/* ------------------------------------------------------------------------------------------------ app_main */

void app_main(void)
{
    ESP_LOGI(TAG, "Pixelmapper Node %d.%d.%d", PM_FW_VERSION_MAJOR, PM_FW_VERSION_MINOR, PM_FW_VERSION_PATCH);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    /* settings -> board (hw_revision_override) -> power-on factory reset keys (pins from the board table) */
    ESP_ERROR_CHECK(pm_config_init(false));
    static pm_settings_t settings;
    pm_config_get(&settings);
    if (pm_board_init(settings.hw_revision_override) != PM_OK) {
        ESP_LOGE(TAG, "unknown hardware revision, using rev 0 pins");
    }
    const pm_board_t *board = pm_board_get();
    if (pm_config_reset_keys_held(board)) {
        ESP_LOGW(TAG, "encoder + BACK held at power-on: factory reset");
        pm_config_factory_reset(PM_SRC_SYSTEM);
        pm_config_flush();
    }
    esp_register_shutdown_handler(shutdown_flush);

    static const pm_ui_model_t ui_model = {
        .net_info = ui_net_info, .port_stats = ui_port_stats, .port_live = ui_port_live, .tod_get = ui_tod_get,
        .rdm_request = ui_rdm_request, .identify = ui_identify,
    };
    err = pm_ui_init(board, &ui_model);
    if (err != ESP_OK) ESP_LOGE(TAG, "pm_ui_init: %s (running without front panel)", esp_err_to_name(err));
    if (pm_ui_factory_reset_requested()) {
        pm_config_factory_reset(PM_SRC_LOCAL_UI);
        pm_config_flush();
    }
    pm_config_get(&settings);

    err = pm_net_init(board);
    s_net_ok = err == ESP_OK;
    if (!s_net_ok) ESP_LOGE(TAG, "Ethernet init failed: %s - DMX keeps running without network", esp_err_to_name(err));
    uint8_t mac[6];
    pm_net_get_mac(mac);
    s_uid = pm_uid_node_from_mac(PM_ESTA_MANUFACTURER, mac);
    char uid_str[PM_UID_STR_LEN];
    ESP_LOGI(TAG, "hw rev %u, UID %s", pm_board_revision(), pm_uid_format(&s_uid, uid_str));
    pm_ui_set_node_info(&s_uid, mac, pm_image_desc.version_label, pm_board_revision());

    ESP_ERROR_CHECK(pm_ota_init(pm_board_revision()));

    /* port engine: core 1 */
    err = pm_port_init(board, &s_uid);
    if (err == ESP_OK) {
        for (pm_port_idx_t i = 0; i < board->num_ports && err == ESP_OK; i++) {
            pm_port_cfg_t c = port_cfg_from(&settings, i);
            err = pm_port_configure(i, &c);
        }
    }
    if (err == ESP_OK) err = pm_port_start();
    s_port_ok = err == ESP_OK;
    if (!s_port_ok) ESP_LOGE(TAG, "port engine: %s", esp_err_to_name(err));

    /* pm_node = Art-Net node + RDM responder (network task objects, one internal-RAM block allocated once) */
    const pm_node_identity_t id = {
        .artnet = {
            .esta_man = PM_ESTA_MANUFACTURER,
            .oem = PM_ARTNET_OEM_NODE,
            .vers_info = PM_FW_VERSINFO(FW_VERSION_ID),
            .uid = s_uid,
            .num_ports = board->num_ports,
        },
        .responder = {
            .uid = s_uid,
            .model_id = PM_MODEL_NODE,
            .sw_version_id = FW_VERSION_ID,
            .sw_version_label = pm_image_desc.version_label,
            .model_description = "Pixelmapper Node",
            .manufacturer_label = "Prototype",
            .hw_revision = pm_board_revision(),
        },
    };
    static pm_node_ops_t ops = {
        .now_ms = now_ms, .random = rnd,
        .config_get = op_config_get, .config_modify = op_config_modify, .config_factory_reset = op_factory,
        .send_udp = op_send_udp, .net_info = op_net_info, .net_apply_ip = op_net_apply_ip,
        .net_renew_dhcp = op_net_renew,
        .port_configure = op_port_configure, .port_output = op_port_output, .port_commit = op_port_commit,
        .port_clear = op_port_clear, .port_status = op_port_status, .port_input_read = op_input_read,
        .port_rdm_submit = op_rdm_submit, .port_rdm_queue_free = op_rdm_queue_free,
        .port_rdm_queue_len = op_rdm_queue_len, .tod_get = op_tod_get, .tod_control = op_tod_control,
        .boot_catch_start = op_catch_start, .boot_catch_abort = op_catch_abort, .boot_catch_status = op_catch_status,
        .reset = op_reset, .ui_event = op_ui_event, .port_live = op_port_live,
    };
    ops.fw_sink = pm_ota_sink();
    size_t node_size = pm_node_size();
    void *mem = heap_caps_aligned_alloc(alignof(max_align_t), node_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_node = mem ? pm_node_init(mem, &id, &ops, &settings) : NULL;
    ESP_LOGI(TAG, "pm_node: %u bytes internal RAM (Art-Net %u, responder %u), free internal %u",
             (unsigned)node_size, (unsigned)pm_artnet_node_size(), (unsigned)pm_rdm_responder_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    if (s_node == NULL) {
        /* Nothing that dereferences the node may run: the network task is not started (DMX keeps running). */
        ESP_LOGE(TAG, "pm_node not available (%u bytes) - network task not started", (unsigned)node_size);
        s_net_ok = false;
    } else {
        ESP_ERROR_CHECK(pm_ota_set_listener(on_ota, NULL));
        ESP_ERROR_CHECK(pm_config_subscribe(on_settings, NULL));
        pm_net_set_change_cb(on_net_change, NULL);
        ESP_ERROR_CHECK(pm_net_apply_ip(&settings.ip, PM_ARTNET_OEM_NODE));
        err = pm_net_start(net_rx, net_tick, NULL, NET_TICK_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "pm_net_start: %s", esp_err_to_name(err));
            s_net_ok = false;
        }
    }

    err = pm_ui_start();
    if (err != ESP_OK) ESP_LOGE(TAG, "pm_ui_start: %s", esp_err_to_name(err));

    static esp_timer_handle_t valid_timer;
    const esp_timer_create_args_t targs = {.callback = mark_valid_cb, .name = "pm_valid"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &valid_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(valid_timer, MARK_VALID_AFTER_US));
}
