/*
 * pm_ui.c - front panel, ESP-IDF glue: UI task (core 0, lowest application priority), event queue, encoder (PCNT,
 * x4 quadrature) and buttons (GPIO, debounced), ST7735S backend, platform functions for ui_core.
 *
 * All UI logic lives in ui_core.c (pure C, also rendered on the host). This file only moves data:
 *   pm_ui_notify() (any task)  -> FreeRTOS queue -> UI task -> ui_core_event()
 *   PCNT + GPIO every 5 ms     -> ui_core_input()
 *   every 20 ms                -> ui_core_tick() -> ui_gfx -> lcd_st7735_blit() (DMA, dirty tiles only)
 */
#include "pm_ui.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lcd_st7735.h"
#include "pm_config.h"
#include "ui_core.h"
#include "ui_font.h"
#include "ui_gfx.h"

static const char *TAG = "pm_ui";

#define UI_TASK_PRIO 1           /* lowest application priority (idle = 0) */
#define UI_TASK_STACK 6144
#define UI_QUEUE_LEN 16
#define UI_POLL_MS 5
#define UI_TICK_MS 20
#define SPI_HZ (20 * 1000 * 1000) /* ST7735S write cycle >= 66 ns; raise after measuring on the real module */
#define ENC_COUNTS_PER_DETENT 4    /* x4 decoding of a typical EC11 (one full quadrature cycle per detent) */
#define ENC_LIMIT 10000
#define DEBOUNCE_SAMPLES 3         /* 3 x 5 ms */

/** Queue item: the event plus a copy of its data (RDM responses). */
typedef struct {
    pm_ui_event_t ev;
    uint8_t data[PM_RDM_MAX_MSG_LEN];
} ui_msg_t;

static struct {
    const pm_board_t *board;
    pm_ui_model_t model;
    QueueHandle_t q;
    TaskHandle_t task;
    uint16_t *strip;
    bool lcd_ok;
    pcnt_unit_handle_t pcnt;
    int enc_last;
    int enc_rest;
    uint8_t push_cnt, back_cnt;
    bool push, back;
    uint32_t settings_flags; /* SETTINGS_CHANGED never gets lost in a full queue: bit 0 local, bit 1 remote */
    portMUX_TYPE info_lock;
    ui_node_info_t info;
    bool info_dirty;
} S = {.info_lock = portMUX_INITIALIZER_UNLOCKED};

/* ------------------------------------------------------------------------------------------------ platform */

static uint32_t p_now(void *ctx)
{
    (void)ctx;
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void p_settings_get(void *ctx, pm_settings_t *out)
{
    (void)ctx;
    pm_config_get(out);
}

static void p_settings_set(void *ctx, const pm_settings_t *s)
{
    (void)ctx;
    esp_err_t err = pm_config_update(s, PM_SRC_LOCAL_UI);
    if (err != ESP_OK) ESP_LOGW(TAG, "settings update failed: %s", esp_err_to_name(err));
}

static void p_backlight(void *ctx, uint8_t level, bool panel_on)
{
    (void)ctx;
    if (!S.lcd_ok) return;
    if (panel_on) {
        lcd_st7735_power(true);
        lcd_st7735_backlight(level);
    } else {
        lcd_st7735_backlight(0);
        lcd_st7735_power(false);
    }
}

static void p_restart(void *ctx)
{
    (void)ctx;
    pm_config_flush();
    esp_restart(); /* ui_core calls this ~300 ms after the "Restarting" frame */
}

/* Settings copies are 2.3 KB each and needed only for a moment: taken from the heap (PSRAM if present) instead of
 * keeping three static copies in internal RAM. */
static pm_settings_t *settings_alloc(unsigned n)
{
    pm_settings_t *p = heap_caps_malloc(n * sizeof(pm_settings_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = malloc(n * sizeof(pm_settings_t));
    if (!p) ESP_LOGE(TAG, "no memory for a settings copy");
    return p;
}

static void p_factory(void *ctx, bool keep_network)
{
    (void)ctx;
    pm_settings_t *buf = settings_alloc(2);
    if (!buf) return;
    pm_settings_t *cur = &buf[0], *def = &buf[1];
    pm_config_get(cur);
    pm_settings_defaults(def);
    def->hw_revision_override = cur->hw_revision_override;
    if (keep_network) def->ip = cur->ip;
    pm_config_update(def, PM_SRC_LOCAL_UI);
    free(buf);
}

static void p_record_scene(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    pm_settings_t *s = settings_alloc(1);
    if (!s) return;
    pm_config_get(s);
    s->port[port].scene_len = pm_port_output_read(port, s->port[port].scene, sizeof(s->port[port].scene));
    pm_config_update(s, PM_SRC_LOCAL_UI);
    free(s);
}

static void p_tod_flush(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    pm_port_tod_control(port, 0x01 /* ArtTodControl AtcFlush */);
}

static const ui_platform_t s_plat = {
    .now_ms = p_now,
    .settings_get = p_settings_get,
    .settings_set = p_settings_set,
    .backlight = p_backlight,
    .restart = p_restart,
    .factory_reset = p_factory,
    .record_scene = p_record_scene,
    .tod_flush = p_tod_flush,
};

/* ------------------------------------------------------------------------------------------------ display */

static void blit(void *ctx, int x, int y, int w, int h, const uint16_t *px)
{
    (void)ctx;
    if (S.lcd_ok) lcd_st7735_blit(x, y, w, h, px);
}

static void boot_screen(const char *line2, uint16_t color)
{
    ui_gfx_begin();
    ui_text_c(0, UI_W, 44, UI_FONT_N, 1, UI_C_WHITE, "Pixelmapper Node");
    ui_text_c(0, UI_W, 66, UI_FONT_S, 1, color, line2);
    ui_gfx_render(false);
}

/* ------------------------------------------------------------------------------------------------ input */

static bool pin_ok(int pin) { return pin >= 0; }

static bool read_btn(int pin) { return pin_ok(pin) && gpio_get_level(pin) == 0; /* active low */ }

static esp_err_t input_init(const pm_board_input_pins_t *in)
{
    uint64_t mask = 0;
    if (pin_ok(in->enc_push)) mask |= 1ull << in->enc_push;
    if (pin_ok(in->back)) mask |= 1ull << in->back;
    if (mask) {
        gpio_config_t io = {.pin_bit_mask = mask, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE};
        ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "buttons");
    }
    if (!pin_ok(in->enc_a) || !pin_ok(in->enc_b)) return ESP_OK;
    pcnt_unit_config_t ucfg = {.low_limit = -ENC_LIMIT, .high_limit = ENC_LIMIT, .flags.accum_count = 1};
    ESP_RETURN_ON_ERROR(pcnt_new_unit(&ucfg, &S.pcnt), TAG, "pcnt unit");
    pcnt_glitch_filter_config_t filt = {.max_glitch_ns = 1000};
    ESP_RETURN_ON_ERROR(pcnt_unit_set_glitch_filter(S.pcnt, &filt), TAG, "pcnt filter");
    pcnt_chan_config_t ca = {.edge_gpio_num = in->enc_a, .level_gpio_num = in->enc_b};
    pcnt_chan_config_t cb = {.edge_gpio_num = in->enc_b, .level_gpio_num = in->enc_a};
    pcnt_channel_handle_t cha, chb;
    ESP_RETURN_ON_ERROR(pcnt_new_channel(S.pcnt, &ca, &cha), TAG, "pcnt a");
    ESP_RETURN_ON_ERROR(pcnt_new_channel(S.pcnt, &cb, &chb), TAG, "pcnt b");
    /* x4 quadrature decoding (ESP-IDF rotary encoder example) */
    pcnt_channel_set_edge_action(cha, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
    pcnt_channel_set_level_action(cha, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    pcnt_channel_set_edge_action(chb, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
    pcnt_channel_set_level_action(chb, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
    gpio_pullup_en(in->enc_a);
    gpio_pullup_en(in->enc_b);
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(S.pcnt, ENC_LIMIT), TAG, "pcnt wp");
    ESP_RETURN_ON_ERROR(pcnt_unit_add_watch_point(S.pcnt, -ENC_LIMIT), TAG, "pcnt wp");
    ESP_RETURN_ON_ERROR(pcnt_unit_enable(S.pcnt), TAG, "pcnt enable");
    ESP_RETURN_ON_ERROR(pcnt_unit_clear_count(S.pcnt), TAG, "pcnt clear");
    return pcnt_unit_start(S.pcnt);
}

/** Debounced level: a change is taken after DEBOUNCE_SAMPLES equal samples. */
static bool debounce(bool raw, bool *state, uint8_t *cnt)
{
    if (raw == *state) {
        *cnt = 0;
    } else if (++*cnt >= DEBOUNCE_SAMPLES) {
        *state = raw;
        *cnt = 0;
    }
    return *state;
}

static void input_poll(void)
{
    int det = 0;
    if (S.pcnt) {
        int c = 0;
        if (pcnt_unit_get_count(S.pcnt, &c) == ESP_OK) {
            S.enc_rest += c - S.enc_last;
            S.enc_last = c;
            det = S.enc_rest / ENC_COUNTS_PER_DETENT;
            S.enc_rest -= det * ENC_COUNTS_PER_DETENT;
        }
    }
    const pm_board_input_pins_t *in = &S.board->input;
    bool push = debounce(read_btn(in->enc_push), &S.push, &S.push_cnt);
    bool back = debounce(read_btn(in->back), &S.back, &S.back_cnt);
    ui_core_input(det, push, back);
}

/* ------------------------------------------------------------------------------------------------ task */

static void ui_task(void *arg)
{
    (void)arg;
    static ui_msg_t msg;
    TickType_t wake = xTaskGetTickCount();
    uint32_t n = 0;
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(UI_POLL_MS));
        input_poll();
        if (++n % (UI_TICK_MS / UI_POLL_MS)) continue;
        if (S.info_dirty) {
            static ui_node_info_t info;
            taskENTER_CRITICAL(&S.info_lock);
            info = S.info;
            S.info_dirty = false;
            taskEXIT_CRITICAL(&S.info_lock);
            ui_core_set_info(&info);
        }
        uint32_t sf = __atomic_exchange_n(&S.settings_flags, 0u, __ATOMIC_ACQ_REL);
        if (sf) {
            pm_ui_event_t ev = {.type = PM_UI_EV_SETTINGS_CHANGED,
                                .value = (sf & 2u) ? PM_SRC_ARTNET /* any remote source */ : PM_SRC_LOCAL_UI};
            ui_core_event(&ev);
        }
        while (xQueueReceive(S.q, &msg, 0) == pdTRUE) {
            if (msg.ev.len) msg.ev.data = msg.data; /* the copy in the queue item */
            ui_core_event(&msg.ev);
        }
        ui_core_tick();
    }
}

/* ------------------------------------------------------------------------------------------------ API */

esp_err_t pm_ui_init(const pm_board_t *board, const pm_ui_model_t *model)
{
    S.board = board;
    if (model) S.model = *model;
    /* queue storage (~4.5 KB) in PSRAM if present: only tasks post to it, never an ISR */
    S.q = xQueueCreateWithCaps(UI_QUEUE_LEN, sizeof(ui_msg_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!S.q) S.q = xQueueCreate(UI_QUEUE_LEN, sizeof(ui_msg_t));
    if (!S.q) return ESP_ERR_NO_MEM;
    /* cache-line aligned so spi_master can DMA from it without a bounce buffer */
    S.strip = heap_caps_aligned_alloc(64, sizeof(uint16_t) * UI_W * UI_STRIP_H, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!S.strip) return ESP_ERR_NO_MEM;

    const pm_board_display_pins_t *d = &board->display;
    if (pin_ok(d->sclk) && pin_ok(d->mosi) && pin_ok(d->dc)) {
        esp_err_t err = lcd_st7735_init(d, SPI_HZ, (int)(sizeof(uint16_t) * UI_W * UI_STRIP_H));
        S.lcd_ok = err == ESP_OK;
        if (!S.lcd_ok) ESP_LOGE(TAG, "display init failed: %s (running without display)", esp_err_to_name(err));
    }
    ui_gfx_init(S.strip, true, blit, NULL);
    if (input_init(&board->input) != ESP_OK) ESP_LOGE(TAG, "input init failed");
    if (S.lcd_ok) {
        boot_screen("starting" UI_G_ELLIPSIS, UI_C_GRAY);
        lcd_st7735_backlight(255);
    }

    static ui_node_info_t info;
    taskENTER_CRITICAL(&S.info_lock);
    info = S.info;
    S.info_dirty = false;
    taskEXIT_CRITICAL(&S.info_lock);
    ui_core_init(&s_plat, &S.model, &info);
    ESP_LOGI(TAG, "panel %s, strip buffer %u B", S.lcd_ok ? "ok" : "missing",
             (unsigned)(sizeof(uint16_t) * UI_W * UI_STRIP_H));
    return ESP_OK;
}

esp_err_t pm_ui_start(void)
{
    if (S.task) return ESP_OK;
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "pm_ui", UI_TASK_STACK, NULL, UI_TASK_PRIO, &S.task, 0);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void pm_ui_notify(const pm_ui_event_t *ev)
{
    if (!ev) return;
    if (ev->type == PM_UI_EV_SETTINGS_CHANGED) {
        __atomic_fetch_or(&S.settings_flags, ev->value == PM_SRC_LOCAL_UI ? 1u : 2u, __ATOMIC_ACQ_REL);
        return;
    }
    if (!S.q) return;
    ui_msg_t m; /* ~290 B on the caller's stack; the queue copies it */
    m.ev = *ev;
    m.ev.data = NULL;
    m.ev.len = 0;
    if (ev->data && ev->len) {
        m.ev.len = ev->len > sizeof(m.data) ? (uint16_t)sizeof(m.data) : ev->len;
        memcpy(m.data, ev->data, m.ev.len);
    }
    xQueueSend(S.q, &m, 0); /* never blocks; dropped when full */
}

bool pm_ui_factory_reset_requested(void) { return false; /* pm_config_reset_keys_held() in main */ }

void pm_ui_set_node_info(const pm_uid_t *uid, const uint8_t mac[6], const char *fw_version, uint8_t hw_revision)
{
    ui_node_info_t info = {.hw_revision = hw_revision};
    if (uid) info.uid = *uid;
    if (mac) memcpy(info.mac, mac, 6);
    if (fw_version) strncpy(info.fw_version, fw_version, sizeof(info.fw_version) - 1);
    taskENTER_CRITICAL(&S.info_lock);
    S.info = info;
    S.info_dirty = true;
    taskEXIT_CRITICAL(&S.info_lock);
}
