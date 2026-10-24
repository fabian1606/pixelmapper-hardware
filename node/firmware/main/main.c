/*
 * main.c - wiring of the Pixelmapper Node.
 *
 * Core assignment (docs/plan.md "Firmware-Architektur"):
 *   core 1: pm_port engine only (DMX/RDM line timing, discovery, ArtSync commit)
 *   core 0: pm_net network task (pm_artnet + RDM responder, single-threaded), pm_config, pm_ota, pm_ui (lowest)
 *
 * Data flow:
 *   UDP -> pm_net task -> pm_artnet_handle_packet -> ops (this file) -> pm_port / pm_config / pm_ota / responder
 *   pm_port events (core 1 queue) -> port_event_task -> pm_net_post -> pm_artnet_* (TOD, RDM response, input)
 *   pm_config listeners -> pm_net_post -> pm_artnet_settings_changed / pm_rdm_responder_settings_changed,
 *                       -> pm_port_configure, pm_net_apply_ip, pm_ui_notify
 * Phase 0: skeleton - components are stubs, the wiring below shows the intended calls.
 */
#include <string.h>

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

static pm_uid_t s_uid;
static pm_artnet_node_t *s_artnet;
static pm_rdm_responder_t *s_responder;
static _Alignas(max_align_t) uint8_t s_artnet_mem[24 * 1024];
static _Alignas(max_align_t) uint8_t s_responder_mem[4 * 1024];

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
    static pm_settings_t s;
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

/** ArtRdm routing: to the node UID (or a broadcast that includes it) -> responder, else -> DMX line. */
static pm_err_t an_rdm_request(void *ctx, const pm_artnet_rdm_request_t *req)
{
    (void)ctx;
    uint8_t wire[PM_RDM_MAX_MSG_LEN];
    if (req->rdm_len + 1u > sizeof(wire)) return PM_ERR_INVALID_ARG;
    wire[0] = PM_RDM_START_CODE;
    memcpy(wire + 1, req->rdm, req->rdm_len);
    pm_rdm_msg_t m;
    if (pm_rdm_decode(wire, req->rdm_len + 1u, &m) != PM_OK) return PM_ERR_BAD_FORMAT;
    if (pm_uid_addresses(&m.dest, &s_uid)) {
        uint8_t resp[PM_RDM_MAX_MSG_LEN];
        int n = pm_rdm_responder_handle(s_responder, wire, req->rdm_len + 1u, resp, sizeof(resp));
        if (n > 1) pm_artnet_rdm_response(s_artnet, req->token, resp + 1, (size_t)n - 1u);
        if (pm_uid_equal(&m.dest, &s_uid)) return PM_OK; /* only for us */
    }
    return pm_port_rdm_submit(req->port, req->token, wire, (uint16_t)(req->rdm_len + 1u)) == ESP_OK ? PM_OK
                                                                                                    : PM_ERR_FULL;
}

static uint8_t an_rdm_fifo_avail(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    return pm_port_rdm_queue_free(port);
}

static void an_indicator(void *ctx, uint8_t ac_led_command)
{
    (void)ctx;
    pm_ui_event_t ev = {.type = PM_UI_EV_LED_MODE, .value = ac_led_command};
    pm_ui_notify(&ev);
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

static void rr_net_apply(void *ctx) { an_ip_apply(ctx); }
static void rr_net_renew(void *ctx)
{
    (void)ctx;
    pm_net_renew_dhcp();
}

static void rr_identify(void *ctx, bool on)
{
    (void)ctx;
    pm_ui_event_t ev = {.type = PM_UI_EV_IDENTIFY, .value = on};
    pm_ui_notify(&ev);
}

static void rr_reset(void *ctx, uint8_t kind)
{
    (void)ctx; (void)kind;
    pm_config_flush();
    esp_restart();
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

/* ------------------------------------------------------------------------------------------------ glue tasks */

static void net_rx(void *ctx, uint32_t src_ip, uint16_t src_port, const uint8_t *data, size_t len)
{
    (void)ctx;
    pm_artnet_handle_packet(s_artnet, src_ip, src_port, data, len);
}

static void net_tick(void *ctx)
{
    (void)ctx;
    pm_artnet_tick(s_artnet);
}

/** Settings listener (any task): reconfigure ports/IP here, forward to the network task for Art-Net/RDM. */
static void on_settings(void *ctx, const pm_settings_t *s, pm_change_mask_t mask, pm_change_source_t src)
{
    (void)ctx;
    for (pm_port_idx_t i = 0; i < PM_NUM_PORTS; i++) {
        if (mask & PM_CHG_PORT(0x7F, i)) {
            pm_port_cfg_t c = port_cfg_from(s, i);
            pm_port_configure(i, &c);
        }
    }
    if (mask & PM_CHG_IP && src != PM_SRC_IPPROG) pm_net_apply_ip(&s->ip, PM_ARTNET_OEM_NODE);
    /* Phase 1: pm_net_post() a work item that calls pm_artnet_settings_changed() and
     * pm_rdm_responder_settings_changed() in the network task (they are not thread-safe). */
    pm_ui_event_t ev = {.type = PM_UI_EV_SETTINGS_CHANGED, .value = src};
    pm_ui_notify(&ev);
}

/** Drains pm_port events (core 1 -> core 0). Phase 1: forward via pm_net_post to pm_artnet / responder / UI. */
static void port_event_task(void *arg)
{
    (void)arg;
    static pm_port_event_t ev;
    for (;;) {
        if (pm_port_event_receive(&ev, 1000)) {
            ESP_LOGD(TAG, "port %u event %d", ev.port, ev.type);
        }
    }
}

/** Rollback protection: mark the image valid after 30 s of normal operation (image-format.md §2.3 step 8). */
static void mark_valid_cb(void *arg)
{
    (void)arg;
    if (pm_ota_pending_verify()) {
        ESP_LOGI(TAG, "marking firmware valid");
        pm_ota_mark_valid();
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Pixelmapper Node %d.%d.%d", PM_FW_VERSION_MAJOR, PM_FW_VERSION_MINOR, PM_FW_VERSION_PATCH);

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(pm_config_init(false));
    static pm_settings_t settings;
    pm_config_get(&settings);
    if (pm_board_init(settings.hw_revision_override) != PM_OK) {
        ESP_LOGE(TAG, "unknown hardware revision, using rev 0 pins");
    }
    const pm_board_t *board = pm_board_get();

    static const pm_ui_model_t ui_model = {0}; /* Phase 1: fill with live-data callbacks */
    ESP_ERROR_CHECK(pm_ui_init(board, &ui_model));
    if (pm_ui_factory_reset_requested()) {
        pm_config_factory_reset(PM_SRC_LOCAL_UI);
        pm_config_get(&settings);
    }

    ESP_ERROR_CHECK(pm_net_init(board));
    uint8_t mac[6];
    pm_net_get_mac(mac);
    s_uid = pm_uid_node_from_mac(PM_ESTA_MANUFACTURER, mac);
    char uid_str[PM_UID_STR_LEN];
    ESP_LOGI(TAG, "hw rev %u, UID %s", pm_board_revision(), pm_uid_format(&s_uid, uid_str));

    ESP_ERROR_CHECK(pm_ota_init(pm_board_revision()));

    ESP_ERROR_CHECK(pm_port_init(board, &s_uid));
    for (pm_port_idx_t i = 0; i < board->num_ports; i++) {
        pm_port_cfg_t c = port_cfg_from(&settings, i);
        ESP_ERROR_CHECK(pm_port_configure(i, &c));
    }
    ESP_ERROR_CHECK(pm_port_start());

    const pm_artnet_identity_t an_id = {
        .esta_man = PM_ESTA_MANUFACTURER,
        .oem = PM_ARTNET_OEM_NODE,
        .vers_info = PM_FW_VERSINFO(FW_VERSION_ID),
        .uid = s_uid,
        .num_ports = board->num_ports,
    };
    const pm_artnet_ops_t an_ops = {
        .now_ms = now_ms, .random = rnd, .send_udp = an_send_udp, .net_info = an_net_info,
        .settings_apply = an_settings_apply, .ip_apply = an_ip_apply,
        .port_output = an_port_output, .port_commit = an_port_commit, .port_status = an_port_status,
        .port_clear = an_port_clear, .tod_get = an_tod_get, .tod_control = an_tod_control,
        .rdm_request = an_rdm_request, .rdm_fifo_avail = an_rdm_fifo_avail, .indicator = an_indicator,
        .fw_sink = pm_ota_sink(),
    };
    configASSERT(pm_artnet_node_size() <= sizeof(s_artnet_mem));
    s_artnet = pm_artnet_node_init(s_artnet_mem, &an_id, &an_ops, &settings);

    const pm_rdm_responder_identity_t rr_id = {
        .uid = s_uid,
        .model_id = PM_MODEL_NODE,
        .sw_version_id = FW_VERSION_ID,
        .sw_version_label = pm_image_desc.version_label,
        .model_description = "Pixelmapper Node",
        .manufacturer_label = "Prototype",
        .hw_revision = pm_board_revision(),
    };
    const pm_rdm_responder_ops_t rr_ops = {
        .now_ms = now_ms, .settings_get = rr_settings_get, .settings_apply = rr_settings_apply,
        .net_info = rr_net_info, .net_apply = rr_net_apply, .net_renew_dhcp = rr_net_renew,
        .identify = rr_identify, .reset = rr_reset, .factory_defaults = rr_factory,
        .boot_catch_start = rr_catch_start, .boot_catch_abort = rr_catch_abort,
        .boot_catch_status = rr_catch_status, .fw_sink = pm_ota_sink(),
    };
    configASSERT(pm_rdm_responder_size() <= sizeof(s_responder_mem));
    s_responder = pm_rdm_responder_init(s_responder_mem, &rr_id, &rr_ops);

    ESP_ERROR_CHECK(pm_config_subscribe(on_settings, NULL));
    ESP_ERROR_CHECK(pm_net_apply_ip(&settings.ip, PM_ARTNET_OEM_NODE));
    ESP_ERROR_CHECK(pm_net_start(net_rx, net_tick, NULL, 10));
    xTaskCreatePinnedToCore(port_event_task, "pm_portev", 4096, NULL, 5, NULL, 0);
    ESP_ERROR_CHECK(pm_ui_start());

    static esp_timer_handle_t valid_timer;
    const esp_timer_create_args_t targs = {.callback = mark_valid_cb, .name = "pm_valid"};
    ESP_ERROR_CHECK(esp_timer_create(&targs, &valid_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(valid_timer, 30ull * 1000 * 1000));
}
