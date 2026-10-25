/*
 * main.c - wiring of the Pixelmapper Node.
 *
 * Core assignment (docs/plan.md "Firmware-Architektur", CLAUDE.md):
 *   core 1: pm_port engine only (DMX/RDM line timing, discovery, ArtSync commit) - started by pm_port_start()
 *   core 0: pm_net network task (pm_artnet + RDM responder + pm_port event drain, single-threaded),
 *           pm_config writer task, pm_ota (runs in the network task), pm_ui task (lowest)
 *
 * Threading rule: pm_artnet and the RDM responder are NOT thread-safe. Everything that touches them runs in the
 * network task: UDP rx, the 10 ms tick, pm_port events (drained in the tick, non-blocking), settings changes
 * (listener -> pm_net_post), front-panel RDM requests (pm_net_post), OTA progress (sink runs there anyway).
 *
 * Data flow:
 *   UDP -> pm_net task -> pm_artnet_handle_packet -> ops (this file) -> pm_port / pm_config / pm_ota / responder
 *   pm_port events (core 1 queue) -> net_tick -> pm_artnet_* (TOD, RDM response, input) / responder / pm_ui
 *   pm_config listener (caller's task) -> pm_port_configure, pm_net_apply_ip, pm_ui_notify (all thread-safe)
 *                                      -> pm_net_post -> pm_artnet_settings_changed / pm_rdm_responder_settings_changed
 */
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "pm_artnet.h"
#include "pm_board.h"
#include "pm_config.h"
#include "pm_net.h"
#include "pm_ota.h"
#include "pm_pids.h"
#include "pm_port.h"
#include "pm_rdm_responder.h"
#include "pm_ui.h"

static const char *TAG = "pm_main";

#define FW_VERSION_ID PM_FW_VERSION_ID(PM_FW_VERSION_MAJOR, PM_FW_VERSION_MINOR, PM_FW_VERSION_PATCH)
#define NET_TICK_MS 10u
#define PORT_EVENTS_PER_TICK 16
#define OTA_POLL_MS 1000u
#define MARK_VALID_AFTER_US (30ull * 1000 * 1000)
#define RESET_DELAY_US (500ull * 1000)
/** RDM tokens of front-panel requests (responses go to pm_ui instead of pm_artnet). pm_artnet's tokens must keep
 *  this bit clear (interface request in the Phase-1 report). */
#define UI_TOKEN_FLAG 0x80000000u

static pm_uid_t s_uid;
static pm_artnet_node_t *s_artnet;
static pm_rdm_responder_t *s_responder;
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

static pm_port_cfg_t port_cfg_from(const pm_settings_t *s, pm_port_idx_t i)
{
    const pm_port_settings_t *p = &s->port[i];
    pm_port_cfg_t c = {
        .mode = !p->enabled ? PM_PORT_MODE_OFF
                            : (p->direction == PM_PORT_DIR_INPUT ? PM_PORT_MODE_INPUT : PM_PORT_MODE_OUTPUT),
        .rdm_enabled = p->rdm_enabled,
        .rdm_bg_discovery = p->rdm_bg_discovery,
        .rdm_bg_interval_ms = (uint32_t)s->rdm_bg_interval_s * 1000u,
        .break_us = p->dmx_break_us,
        .mab_us = p->dmx_mab_us,
        .rate_hz = p->dmx_rate_hz,
        .continuous = p->output_style == PM_OUTPUT_CONTINUOUS,
    };
    return c;
}

static void ui_event(pm_ui_event_type_t type, pm_port_idx_t port, int32_t value)
{
    pm_ui_event_t ev = {.type = type, .port = port, .value = value};
    pm_ui_notify(&ev);
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

/* ------------------------------------------------------------------------------------------------ RDM routing */

/**
 * Route one RDM request (wire bytes incl. start code) - network task only.
 * To the node UID (or a broadcast that includes it) -> responder; everything else -> DMX line of `port`.
 * `reply` receives the responder's answer (wire bytes) when there is one.
 */
static pm_err_t rdm_route(pm_port_idx_t port, uint32_t token, const uint8_t *wire, size_t len,
                          void (*reply)(uint32_t token, const uint8_t *resp, size_t resp_len))
{
    pm_rdm_msg_t m;
    if (pm_rdm_decode(wire, len, &m) != PM_OK) return PM_ERR_BAD_FORMAT;
    if (pm_uid_addresses(&m.dest, &s_uid)) {
        uint8_t resp[PM_RDM_MAX_MSG_LEN];
        int n = pm_rdm_responder_handle(s_responder, wire, len, resp, sizeof(resp));
        if (n > 1) reply(token, resp, (size_t)n);
        if (pm_uid_equal(&m.dest, &s_uid)) return PM_OK; /* only for us, never on the line */
    }
    esp_err_t e = pm_port_rdm_submit(port, token, wire, (uint16_t)len);
    if (e == ESP_ERR_NO_MEM) return PM_ERR_FULL;
    return e == ESP_OK ? PM_OK : PM_ERR_STATE;
}

static void reply_artnet(uint32_t token, const uint8_t *resp, size_t len)
{
    pm_artnet_rdm_response(s_artnet, token, resp + 1, len - 1u); /* ArtRdm carries no start code */
}

static void reply_ui(uint32_t token, const uint8_t *resp, size_t len)
{
    pm_ui_event_t ev = {.type = PM_UI_EV_RDM_RESPONSE, .token = token & ~UI_TOKEN_FLAG, .len = (uint16_t)len,
                        .data = resp};
    pm_ui_notify(&ev);
}

/* ------------------------------------------------------------------------------------------------ pm_artnet ops */

static pm_err_t an_send_udp(void *ctx, uint32_t ip, uint16_t port, const uint8_t *data, size_t len)
{
    (void)ctx;
    return pm_net_send_udp(ip, port, data, len) == ESP_OK ? PM_OK : PM_ERR_IO;
}

static void an_net_info(void *ctx, pm_artnet_net_info_t *out)
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

static pm_err_t an_settings_apply(void *ctx, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src)
{
    (void)ctx; (void)mask;
    return pm_config_update(s, src) == ESP_OK ? PM_OK : PM_ERR_IO;
}

static void an_ip_apply(void *ctx)
{
    (void)ctx;
    static pm_settings_t s; /* network task only */
    pm_config_get(&s);
    pm_net_apply_ip(&s.ip, PM_ARTNET_OEM_NODE);
}

static void an_port_output(void *ctx, pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    (void)ctx;
    pm_port_output_write(port, data, len, defer);
}

static void an_port_commit(void *ctx)
{
    (void)ctx;
    pm_port_commit();
}

static void an_port_status(void *ctx, pm_port_idx_t port, pm_artnet_port_status_t *out)
{
    (void)ctx;
    pm_port_stats_t st;
    pm_port_get_stats(port, &st);
    out->output_active = st.frames_out > 0;
    out->input_signal = st.input_signal;
    out->rdm_discovery_running = st.discovery_running;
    out->tod_count = st.tod_count;
}

static void an_port_clear(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    pm_port_output_clear(port);
}

static uint16_t an_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)ctx;
    return pm_port_tod_get(port, out, cap);
}

static void an_tod_control(void *ctx, pm_port_idx_t port, uint8_t command)
{
    (void)ctx;
    pm_port_tod_control(port, command);
}

static pm_err_t an_rdm_request(void *ctx, const pm_artnet_rdm_request_t *req)
{
    (void)ctx;
    uint8_t wire[PM_RDM_MAX_MSG_LEN];
    if (req->rdm_len + 1u > sizeof(wire)) return PM_ERR_INVALID_ARG;
    wire[0] = PM_RDM_START_CODE;
    memcpy(wire + 1, req->rdm, req->rdm_len);
    return rdm_route(req->port, req->token, wire, req->rdm_len + 1u, reply_artnet);
}

static uint8_t an_rdm_fifo_avail(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    return pm_port_rdm_queue_free(port);
}

static void an_indicator(void *ctx, uint8_t ac_led_command)
{
    (void)ctx;
    ui_event(PM_UI_EV_LED_MODE, 0, ac_led_command);
}

/* ------------------------------------------------------------------------------------------------ responder ops */

static void rr_settings_get(void *ctx, pm_settings_t *out)
{
    (void)ctx;
    pm_config_get(out);
}

static pm_err_t rr_settings_apply(void *ctx, const pm_settings_t *s, pm_change_mask_t mask)
{
    (void)ctx; (void)mask;
    return pm_config_update(s, PM_SRC_RDM) == ESP_OK ? PM_OK : PM_ERR_IO;
}

static void rr_net_info(void *ctx, pm_rdm_net_info_t *out)
{
    (void)ctx;
    pm_net_status_t st;
    pm_net_get_status(&st);
    out->ip = st.ip;
    out->mask = st.mask;
    out->gateway = st.gateway;
    out->dhcp_active = st.mode == PM_NET_ADDR_DHCP;
    memcpy(out->mac, st.mac, 6);
}

static void rr_net_apply(void *ctx) { an_ip_apply(ctx); } /* E1.37-2 INTERFACE_APPLY_CONFIGURATION */

static void rr_net_renew(void *ctx)
{
    (void)ctx;
    pm_net_renew_dhcp();
}

static void rr_identify(void *ctx, bool on)
{
    (void)ctx;
    ui_event(PM_UI_EV_IDENTIFY, 0, on);
}

static void rr_reset(void *ctx, uint8_t kind)
{
    (void)ctx;
    ESP_LOGW(TAG, "RESET_DEVICE 0x%02x", kind);
    restart_later();
}

static void rr_factory(void *ctx)
{
    (void)ctx;
    pm_config_factory_reset(PM_SRC_RDM);
}

static pm_err_t rr_catch_start(void *ctx, pm_port_idx_t port, const pm_uid_t *target)
{
    (void)ctx;
    return pm_port_boot_catch_start(port, target) == ESP_OK ? PM_OK : PM_ERR_STATE;
}

static void rr_catch_abort(void *ctx)
{
    (void)ctx;
    pm_port_boot_catch_abort();
}

static void rr_catch_status(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    (void)ctx;
    pm_port_boot_catch_status(port, out);
}

/* ------------------------------------------------------------------------------------------------ settings changes */

/* Pending change masks per source, coalesced until the network task runs (no allocation per change). */
static portMUX_TYPE s_chg_lock = portMUX_INITIALIZER_UNLOCKED;
static pm_change_mask_t s_chg_pending[PM_SRC_SYSTEM + 1];
static bool s_chg_posted;

/** Network task: forward pending settings changes to pm_artnet and the RDM responder. */
static void work_settings_changed(void *arg)
{
    (void)arg;
    pm_change_mask_t masks[PM_SRC_SYSTEM + 1];
    portENTER_CRITICAL(&s_chg_lock);
    memcpy(masks, s_chg_pending, sizeof(masks));
    memset(s_chg_pending, 0, sizeof(s_chg_pending));
    s_chg_posted = false;
    portEXIT_CRITICAL(&s_chg_lock);

    static pm_settings_t s; /* network task only */
    pm_config_get(&s);      /* last writer wins: the current state is what counts */
    pm_change_mask_t all = 0;
    for (int src = 0; src <= PM_SRC_SYSTEM; src++) {
        if (masks[src] == 0) continue;
        all |= masks[src];
        pm_rdm_responder_settings_changed(s_responder, masks[src], (pm_change_source_t)src);
    }
    if (all) pm_artnet_settings_changed(s_artnet, &s, all);
}

/** pm_config listener (any task, inside pm_config's lock): only thread-safe calls and posts. */
static void on_settings(void *ctx, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src)
{
    (void)ctx;
    for (pm_port_idx_t i = 0; i < PM_NUM_PORTS; i++) {
        if ((mask & PM_CHG_PORT(0x7F, i)) || (mask & PM_CHG_RDM_INTERVAL)) {
            pm_port_cfg_t c = port_cfg_from(s, i);
            pm_port_configure(i, &c);
        }
    }
    /* ArtIpProg applies after its reply (ops.ip_apply); E1.37-2 applies on INTERFACE_APPLY_CONFIGURATION. */
    if ((mask & PM_CHG_IP) && src != PM_SRC_IPPROG && src != PM_SRC_RDM) {
        pm_net_apply_ip(&s->ip, PM_ARTNET_OEM_NODE);
    }

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

    ui_event(PM_UI_EV_SETTINGS_CHANGED, 0, src);
}

/* ------------------------------------------------------------------------------------------------ OTA progress */

/** Network task (the sink runs there). */
static void on_ota(void *ctx, const pm_fw_status_t *st)
{
    (void)ctx;
    int32_t pct = -1;
    if (st->state == PM_FW_STATE_FAILED) {
        ESP_LOGW(TAG, "firmware update failed: error %u", (unsigned)st->last_error);
        pm_artnet_set_node_report(s_artnet, 0x000E /* RcFirmwareFail */, "Firmware update failed");
    } else if (st->total > 0) {
        pct = (int32_t)((uint64_t)st->received * 100u / st->total);
        if (st->state == PM_FW_STATE_READY) {
            pct = 100;
            pm_config_flush();
        }
    } else {
        return; /* idle */
    }
    ui_event(PM_UI_EV_FW_PROGRESS, 0, pct);
}

/* ------------------------------------------------------------------------------------------------ network task glue */

static void net_rx(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len)
{
    (void)ctx;
    pm_artnet_handle_packet(s_artnet, src_ip, src_port, data, len);
}

static void handle_port_event(const pm_port_event_t *ev)
{
    switch (ev->type) {
    case PM_PORT_EV_RDM_RESPONSE:
        if (ev->len < 2) break;
        if (ev->token & UI_TOKEN_FLAG) {
            reply_ui(ev->token, ev->data, ev->len);
        } else {
            pm_artnet_rdm_response(s_artnet, ev->token, ev->data + 1, ev->len - 1u);
        }
        break;
    case PM_PORT_EV_TOD_CHANGED:
        pm_artnet_tod_changed(s_artnet, ev->port);
        ui_event(PM_UI_EV_TOD_CHANGED, ev->port, 0);
        break;
    case PM_PORT_EV_INPUT_FRAME: {
        static uint8_t buf[512];
        uint16_t n = pm_port_input_read(ev->port, buf, sizeof(buf));
        if (n) pm_artnet_input_frame(s_artnet, ev->port, buf, n);
        break;
    }
    case PM_PORT_EV_INPUT_SIGNAL:
        pm_artnet_port_status_changed(s_artnet, ev->port);
        break;
    case PM_PORT_EV_BOOT_CATCH:
        pm_rdm_responder_queue_pid(s_responder, PM_PID_BOOT_CATCH_STATUS);
        if (ev->value == PM_BOOT_CATCH_STATE_CAUGHT) {
            pm_rdm_responder_status(s_responder, PM_STATUS_BOOT_CATCH_CAUGHT, PM_RDM_STATUS_ADVISORY,
                                    (int16_t)(ev->port + 1), 0);
        } else if (ev->value == PM_BOOT_CATCH_STATE_TIMEOUT) {
            pm_rdm_responder_status(s_responder, PM_STATUS_BOOT_CATCH_TIMEOUT, PM_RDM_STATUS_WARNING,
                                    (int16_t)(ev->port + 1), 0);
        }
        ui_event(PM_UI_EV_BOOT_CATCH, ev->port, ev->value);
        break;
    case PM_PORT_EV_ERROR:
        ESP_LOGW(TAG, "port %u: driver error %s", (unsigned)ev->port + 1, esp_err_to_name((esp_err_t)ev->value));
        ui_event(PM_UI_EV_ERROR, ev->port, ev->value);
        break;
    }
}

static void net_tick(void *ctx)
{
    (void)ctx;
    static pm_port_event_t ev; /* network task only */
    for (int i = 0; i < PORT_EVENTS_PER_TICK && pm_port_event_receive(&ev, 0); i++) handle_port_event(&ev);
    pm_artnet_tick(s_artnet);

    static uint32_t next_ota_poll;
    uint32_t now = now_ms(NULL);
    if ((int32_t)(now - next_ota_poll) >= 0) {
        next_ota_poll = now + OTA_POLL_MS;
        pm_ota_poll(); /* 30 s session timeout */
    }
}

/** Network task: link/address changed -> unsolicited ArtPollReply from the new address. */
static void on_net_change(void *ctx, const pm_net_status_t *st)
{
    (void)ctx; (void)st;
    pm_artnet_net_changed(s_artnet);
}

/* ------------------------------------------------------------------------------------------------ UI model */

static void ui_port_stats(void *ctx, pm_port_idx_t port, pm_port_stats_t *out)
{
    (void)ctx;
    pm_port_get_stats(port, out);
}

static uint16_t ui_tod_get(void *ctx, pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)ctx;
    return pm_port_tod_get(port, out, cap);
}

typedef struct {
    pm_port_idx_t port;
    uint32_t token;
    uint16_t len;
    uint8_t data[];
} ui_rdm_req_t;

/** Network task: front-panel RDM request -> responder or line; SETs to devices are reported (LOCAL_RDM_SET). */
static void work_ui_rdm(void *arg)
{
    ui_rdm_req_t *r = arg;
    pm_rdm_msg_t m;
    if (pm_rdm_decode(r->data, r->len, &m) == PM_OK) {
        pm_err_t e = rdm_route(r->port, r->token | UI_TOKEN_FLAG, r->data, r->len, reply_ui);
        if (e == PM_OK && m.cc == PM_RDM_CC_SET && !pm_uid_equal(&m.dest, &s_uid)) {
            pm_rdm_responder_local_set(s_responder, r->port, &m.dest, m.sub_device, m.pid);
        }
    }
    free(r);
}

static esp_err_t ui_rdm_request(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    if (data == NULL || len < 2 || len > PM_RDM_MAX_MSG_LEN || port >= PM_NUM_PORTS) return ESP_ERR_INVALID_ARG;
    ui_rdm_req_t *r = malloc(sizeof(*r) + len);
    if (r == NULL) return ESP_ERR_NO_MEM;
    r->port = port;
    r->token = token & ~UI_TOKEN_FLAG;
    r->len = len;
    memcpy(r->data, data, len);
    esp_err_t e = pm_net_post(work_ui_rdm, r);
    if (e != ESP_OK) free(r);
    return e;
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
        .port_stats = ui_port_stats, .tod_get = ui_tod_get, .rdm_request = ui_rdm_request,
        .net_info = NULL, .port_live = NULL, .identify = NULL, /* Phase 2: Paket A wires them (main rewrite) */
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

    ESP_ERROR_CHECK(pm_ota_init(pm_board_revision()));
    ESP_ERROR_CHECK(pm_ota_set_listener(on_ota, NULL));

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

    /* Art-Net node + RDM responder (network task objects, allocated once at init) */
    const pm_artnet_identity_t an_id = {
        .esta_man = PM_ESTA_MANUFACTURER,
        .oem = PM_ARTNET_OEM_NODE,
        .vers_info = PM_FW_VERSINFO(FW_VERSION_ID),
        .uid = s_uid,
        .num_ports = board->num_ports,
    };
    static pm_artnet_ops_t an_ops = {
        .now_ms = now_ms, .random = rnd, .send_udp = an_send_udp, .net_info = an_net_info,
        .settings_apply = an_settings_apply, .ip_apply = an_ip_apply,
        .port_output = an_port_output, .port_commit = an_port_commit, .port_status = an_port_status,
        .port_clear = an_port_clear, .tod_get = an_tod_get, .tod_control = an_tod_control,
        .rdm_request = an_rdm_request, .rdm_fifo_avail = an_rdm_fifo_avail, .indicator = an_indicator,
    };
    an_ops.fw_sink = pm_ota_sink();
    void *an_mem = heap_caps_malloc(pm_artnet_node_size(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_artnet = an_mem ? pm_artnet_node_init(an_mem, &an_id, &an_ops, &settings) : NULL;

    const pm_rdm_responder_identity_t rr_id = {
        .uid = s_uid,
        .model_id = PM_MODEL_NODE,
        .sw_version_id = FW_VERSION_ID,
        .sw_version_label = pm_image_desc.version_label,
        .model_description = "Pixelmapper Node",
        .manufacturer_label = "Prototype",
        .hw_revision = pm_board_revision(),
    };
    static pm_rdm_responder_ops_t rr_ops = {
        .now_ms = now_ms, .settings_get = rr_settings_get, .settings_apply = rr_settings_apply,
        .net_info = rr_net_info, .net_apply = rr_net_apply, .net_renew_dhcp = rr_net_renew,
        .identify = rr_identify, .reset = rr_reset, .factory_defaults = rr_factory,
        .boot_catch_start = rr_catch_start, .boot_catch_abort = rr_catch_abort,
        .boot_catch_status = rr_catch_status,
    };
    rr_ops.fw_sink = pm_ota_sink();
    void *rr_mem = heap_caps_malloc(pm_rdm_responder_size(), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_responder = rr_mem ? pm_rdm_responder_init(rr_mem, &rr_id, &rr_ops) : NULL;
    if (s_artnet == NULL || s_responder == NULL) {
        /* Phase 1: pm_artnet / pm_rdm may still be stubs that return NULL. Nothing that dereferences them may
         * run, so the network task is not started (DMX output keeps running on core 1). */
        ESP_LOGE(TAG, "Art-Net node (%p) or RDM responder (%p) not available - network task not started",
                 (void *)s_artnet, (void *)s_responder);
        s_net_ok = false;
    } else {
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
