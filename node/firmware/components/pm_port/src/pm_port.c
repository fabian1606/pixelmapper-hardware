/*
 * pm_port.c - port engine on the ESP32-P4: platform layer around the pure line FSM (core/pm_port_line.c).
 *
 * Hardware use per port: one HP UART (1..4) at 250 kbit/s 8N2, UART in normal mode with
 *  - DE as a plain GPIO driven by the line FSM (not the UART RS-485 mode, see MEASURE.md "Design notes":
 *    the break is made by inverting TXD, and the RS-485 mode only asserts RTS while the FIFO sends),
 *  - break by TXD inversion, timed by one shared gptimer (1 MHz, alarm = earliest deadline of the 4 lines),
 *  - TX FIFO refill and RX drain in the UART ISR, bytes with framing errors discarded by the UART
 *    (err_wr_mask), break detection via BRK_DET.
 * All ISRs and the engine task run on core 1 and share one spinlock (s_lock), so line FSMs are only ever touched
 * with interrupts off on core 1. Core 0 talks to the task through a command queue and lock-free triple buffers.
 */
#include "pm_port.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/uart.h"
#include "esp_attr.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"
#include "hal/uart_ll.h"
#include "pm_artnet_packets.h"
#include "pm_port_line.h"
#include "pm_port_tbuf.h"
#include "sdkconfig.h"
#include "soc/uart_periph.h"

static const char *TAG = "pm_port";

#define EVENT_QUEUE_LEN 32
#define CMD_QUEUE_LEN 24
#define UART_RX_FULL_THR_BULK 16  /* DMX input: one interrupt per 0.7 ms */
#define UART_RX_FULL_THR_RDM 1    /* RDM response window: every byte at once (DUB/inter-slot timing) */
#define UART_TX_EMPTY_THR 32
#define UART_RX_TOUT_BITS 11        /* deliver the last bytes one character time after the line went idle */
#define INPUT_LOST_US 1000000u      /* E1.11: no frame for 1 s = no signal */
#define MBS_MAX_US 1000u
#define BG_INTERVAL_DEFAULT_MS 3000u

#if CONFIG_PM_PORT_ISR_CACHE_SAFE
#define ISR_FLAGS (ESP_INTR_FLAG_IRAM)
#else
#define ISR_FLAGS 0
#endif

/* ------------------------------------------------------------------------------------------------ state */

typedef enum { CMD_CONFIGURE, CMD_RDM, CMD_TOD_CONTROL, CMD_CATCH_START, CMD_CATCH_ABORT } cmd_kind_t;

typedef struct {
    uint8_t kind;
    pm_port_idx_t port;
    uint8_t arg;
    pm_uid_t uid;
    pm_port_cfg_t cfg;
    pm_rdm_request_t req;
} cmd_t;

typedef struct {
    pm_port_idx_t idx;
    const pm_board_port_pins_t *pins;
    uart_dev_t *hw;
    bool hw_ok;
    intr_handle_t intr;

    /* line (core 1, under s_lock) */
    pm_port_line_t line;
    const uint8_t *tx_data;
    uint16_t tx_len, tx_pos;
    uint8_t in_last[PM_PORT_FRAME_MAX];   /* last published input frame (ISR side), for change detection */
    uint16_t in_last_len;

    /* cross-core buffers */
    pm_port_tbuf_t out;                   /* core 0 writes, line reads */
    pm_port_tbuf_t in;                    /* line writes, core 0 reads */
    atomic_bool in_changed;
    atomic_bool rdm_done;

    /* core 0 side of the output (under s_out_lock) */
    uint8_t shadow[PM_PORT_FRAME_MAX];
    uint16_t shadow_len;
    bool deferred;
    pm_port_cfg_t cfg0;                   /* configuration as seen by core 0 (for argument checks) */

    /* task side (core 1) */
    pm_port_cfg_t cfg;
    bool configured;
    pm_rdm_ctrl_t ctrl;
    pm_rdm_txn_t txn;                     /* handed to the line by pointer */
    bool signal;
    uint32_t fps_last_frames;

    /* snapshots for core 0 (under s_snap_lock) */
    pm_port_stats_t stats;
    pm_uid_t tod[PM_RDM_TOD_MAX];
    uint16_t tod_count;
    pm_rdm_boot_catch_t catch_status;

    atomic_int rdm_pending;               /* submitted on core 0, not yet in the controller queue */
    atomic_int rdm_ctrl_free;             /* free slots of the controller queue */
} port_t;

static struct {
    bool inited;
    const pm_board_t *board;
    pm_uid_t node_uid;
    QueueHandle_t events;
    QueueHandle_t cmds;
    TaskHandle_t task;
    gptimer_handle_t timer;
    volatile pm_port_idx_t catch_port;
    atomic_uint events_dropped;
} s;

static DRAM_ATTR port_t s_port[PM_NUM_PORTS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;      /* line FSMs, core 1 only */
static portMUX_TYPE s_out_lock = portMUX_INITIALIZER_UNLOCKED;  /* output shadow, core 0 writers */
static portMUX_TYPE s_in_lock = portMUX_INITIALIZER_UNLOCKED;   /* input tbuf reader side */
static portMUX_TYPE s_snap_lock = portMUX_INITIALIZER_UNLOCKED; /* snapshots + core-0 config view */
static DRAM_ATTR bool s_isr_yield;

/* ------------------------------------------------------------------------------------------------ time */

static IRAM_ATTR uint32_t now_us(void)
{
    uint64_t v = 0;
    if (s.timer) gptimer_get_raw_count(s.timer, &v);
    return (uint32_t)v;
}

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static inline uint32_t bg_interval(const pm_port_cfg_t *c)
{
    return c->rdm_bg_interval_ms ? c->rdm_bg_interval_ms : BG_INTERVAL_DEFAULT_MS;
}

/* ------------------------------------------------------------------------------------------------ timer */

/* Run due lines and program the alarm for the earliest deadline. Called with s_lock held. */
static IRAM_ATTR void rearm(void)
{
    if (!s.timer) return;
    for (int round = 0; round < 8; round++) {
        uint64_t now64 = 0;
        gptimer_get_raw_count(s.timer, &now64);
        uint32_t now = (uint32_t)now64;
        bool any = false, ran = false;
        int32_t best = INT32_MAX;
        for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
            uint32_t dl;
            if (!pm_port_line_deadline(&s_port[i].line, &dl)) continue;
            int32_t d = (int32_t)(dl - now);
            if (d <= 1) {
                pm_port_line_timer(&s_port[i].line, now);
                ran = true;
                continue;
            }
            any = true;
            if (d < best) best = d;
        }
        if (ran) continue; /* deadlines may have moved: recompute */
        gptimer_alarm_config_t a = {
            .alarm_count = now64 + (any ? (uint64_t)best : 1000000u),
            .reload_count = 0,
            .flags.auto_reload_on_alarm = false,
        };
        gptimer_set_alarm_action(s.timer, &a);
        return;
    }
    /* still busy after 8 rounds: come back in 2 us */
    uint64_t now64 = 0;
    gptimer_get_raw_count(s.timer, &now64);
    gptimer_alarm_config_t a = {.alarm_count = now64 + 2, .flags.auto_reload_on_alarm = false};
    gptimer_set_alarm_action(s.timer, &a);
}

static IRAM_ATTR bool on_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *ev, void *arg)
{
    (void)timer;
    (void)ev;
    (void)arg;
    portENTER_CRITICAL_ISR(&s_lock);
    s_isr_yield = false;
    rearm();
    bool y = s_isr_yield;
    portEXIT_CRITICAL_ISR(&s_lock);
    return y;
}

static IRAM_ATTR void notify_task(void)
{
    if (!s.task) return;
    if (xPortInIsrContext()) {
        BaseType_t hp = pdFALSE;
        vTaskNotifyGiveFromISR(s.task, &hp);
        if (hp) s_isr_yield = true;
    }
    /* task context: the task checks all flags after every critical section anyway */
}

/* ------------------------------------------------------------------------------------------------ line ops */

static IRAM_ATTR void op_set_de(void *ctx, bool on)
{
    port_t *p = ctx;
    if (p->pins->de >= 0) gpio_ll_set_level(&GPIO, (uint32_t)p->pins->de, on ? 1 : 0);
    if (p->pins->re >= 0) gpio_ll_set_level(&GPIO, (uint32_t)p->pins->re, on ? 1 : 0);
}

static IRAM_ATTR void op_set_break(void *ctx, bool on)
{
    port_t *p = ctx;
    if (p->hw_ok) uart_ll_inverse_signal(p->hw, on ? UART_SIGNAL_TXD_INV : 0);
}

static IRAM_ATTR void tx_feed(port_t *p)
{
    uint32_t space = uart_ll_get_txfifo_len(p->hw); /* P4 LL: returns the FREE space */
    uint32_t n = (uint32_t)(p->tx_len - p->tx_pos);
    if (n > space) n = space;
    if (n) {
        uart_ll_write_txfifo(p->hw, p->tx_data + p->tx_pos, n);
        p->tx_pos = (uint16_t)(p->tx_pos + n);
    }
    if (p->tx_pos >= p->tx_len) {
        uart_ll_disable_intr_mask(p->hw, UART_INTR_TXFIFO_EMPTY);
        uart_ll_clr_intsts_mask(p->hw, UART_INTR_TX_DONE); /* a mid-frame underrun may have set it */
        uart_ll_ena_intr_mask(p->hw, UART_INTR_TX_DONE);
    } else {
        uart_ll_clr_intsts_mask(p->hw, UART_INTR_TXFIFO_EMPTY);
        uart_ll_ena_intr_mask(p->hw, UART_INTR_TXFIFO_EMPTY);
    }
}

static IRAM_ATTR void op_tx_start(void *ctx, const uint8_t *data, uint16_t len)
{
    port_t *p = ctx;
    if (!p->hw_ok) return; /* the line watchdog completes the packet */
    p->tx_data = data;
    p->tx_len = len;
    p->tx_pos = 0;
    uart_ll_clr_intsts_mask(p->hw, UART_INTR_TX_DONE);
    tx_feed(p);
}

static IRAM_ATTR void op_tx_abort(void *ctx)
{
    port_t *p = ctx;
    p->tx_data = NULL;
    if (!p->hw_ok) return;
    uart_ll_disable_intr_mask(p->hw, UART_INTR_TXFIFO_EMPTY | UART_INTR_TX_DONE);
    uart_ll_txfifo_rst(p->hw);
    uart_ll_clr_intsts_mask(p->hw, UART_INTR_TXFIFO_EMPTY | UART_INTR_TX_DONE);
}

static IRAM_ATTR void op_rx_start(void *ctx, bool per_byte)
{
    port_t *p = ctx;
    if (!p->hw_ok) return;
    uart_ll_set_rxfifo_full_thr(p->hw, per_byte ? UART_RX_FULL_THR_RDM : UART_RX_FULL_THR_BULK);
    uart_ll_rxfifo_rst(p->hw);
    uart_ll_clr_intsts_mask(p->hw, UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_BRK_DET |
                                       UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR | UART_INTR_RXFIFO_OVF);
}

static IRAM_ATTR uint16_t op_dmx_frame(void *ctx, const uint8_t **frame)
{
    port_t *p = ctx;
    uint16_t len;
    *frame = pm_port_tbuf_acquire(&p->out, &len);
    return len;
}

static IRAM_ATTR bool op_dmx_fresh(void *ctx) { return pm_port_tbuf_fresh(&((port_t *)ctx)->out); }

static IRAM_ATTR void op_rdm_done(void *ctx)
{
    port_t *p = ctx;
    atomic_store(&p->rdm_done, true);
    notify_task();
}

static IRAM_ATTR void op_input_frame(void *ctx, const uint8_t *frame, uint16_t len)
{
    port_t *p = ctx;
    bool changed = len != p->in_last_len;
    for (uint16_t i = 0; i < len && !changed; i++) changed = frame[i] != p->in_last[i];
    uint8_t *dst = pm_port_tbuf_back(&p->in);
    for (uint16_t i = 0; i < len; i++) dst[i] = frame[i];
    pm_port_tbuf_set_len(&p->in, len);
    pm_port_tbuf_publish(&p->in);
    if (changed) {
        for (uint16_t i = 0; i < len; i++) p->in_last[i] = frame[i];
        p->in_last_len = len;
        atomic_store(&p->in_changed, true);
        notify_task();
    }
}

/* ------------------------------------------------------------------------------------------------ UART ISR */

#define RX_INTS (UART_INTR_RXFIFO_FULL | UART_INTR_RXFIFO_TOUT | UART_INTR_BRK_DET | UART_INTR_FRAM_ERR | \
                 UART_INTR_PARITY_ERR | UART_INTR_RXFIFO_OVF)

static IRAM_ATTR void uart_isr(void *arg)
{
    port_t *p = arg;
    portENTER_CRITICAL_ISR(&s_lock);
    s_isr_yield = false;
    uint32_t now = now_us();
    uint32_t st = uart_ll_get_intsts_mask(p->hw);
    uart_ll_clr_intsts_mask(p->hw, st);
    /* the break itself is a character with a framing error: not an error of the data */
    if (st & UART_INTR_BRK_DET) st &= ~(uint32_t)(UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR);

    if (st & RX_INTS) {
        /* bytes before a break belong to the previous packet: deliver them first */
        uint8_t buf[UART_LL_FIFO_DEF_LEN];
        uint32_t n = uart_ll_get_rxfifo_len(p->hw);
        if (n > sizeof buf) n = sizeof buf;
        if (n) {
            uart_ll_read_rxfifo(p->hw, buf, n);
            pm_port_line_rx_bytes(&p->line, now, buf, (uint16_t)n);
        }
        if (st & UART_INTR_RXFIFO_OVF) uart_ll_rxfifo_rst(p->hw);
        if (st & (UART_INTR_FRAM_ERR | UART_INTR_PARITY_ERR | UART_INTR_RXFIFO_OVF)) {
            pm_port_line_rx_error(&p->line, now);
        }
        if (st & UART_INTR_BRK_DET) pm_port_line_rx_break(&p->line, now);
    }
    if ((st & UART_INTR_TXFIFO_EMPTY) && p->tx_data) tx_feed(p);
    if (st & UART_INTR_TX_DONE) {
        /* TX_DONE may come while the last stop bit is still shifting out (IDF's RS-485 path checks the same);
         * dropping DE then would cut the checksum. Wait for the transmitter FSM, at most ~1.5 characters. */
        for (int i = 0; i < 64 && !uart_ll_is_tx_idle(p->hw); i++) esp_rom_delay_us(1);
        uart_ll_disable_intr_mask(p->hw, UART_INTR_TX_DONE);
        p->tx_data = NULL;
        pm_port_line_tx_done(&p->line, now);
    }
    rearm();
    bool y = s_isr_yield;
    portEXIT_CRITICAL_ISR(&s_lock);
    if (y) portYIELD_FROM_ISR();
}

/* ------------------------------------------------------------------------------------------------ RDM callbacks */

static void emit(const pm_port_event_t *ev)
{
    if (xQueueSend(s.events, ev, 0) != pdTRUE) atomic_fetch_add(&s.events_dropped, 1);
}

static void cb_response(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    static pm_port_event_t ev; /* port task only; too big for its stack */
    memset(&ev, 0, sizeof ev);
    ev.type = PM_PORT_EV_RDM_RESPONSE;
    ev.port = port;
    ev.token = token;
    ev.len = len > sizeof ev.data ? (uint16_t)sizeof ev.data : len;
    memcpy(ev.data, data, ev.len);
    emit(&ev);
}

static void snapshot_tod(port_t *p)
{
    static pm_uid_t tmp[PM_RDM_TOD_MAX];
    uint16_t n = pm_rdm_ctrl_tod(&p->ctrl, tmp, PM_RDM_TOD_MAX);
    if (n > PM_RDM_TOD_MAX) n = PM_RDM_TOD_MAX;
    portENTER_CRITICAL(&s_snap_lock);
    memcpy(p->tod, tmp, n * sizeof(pm_uid_t));
    p->tod_count = n;
    portEXIT_CRITICAL(&s_snap_lock);
}

static void cb_tod_changed(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    snapshot_tod(&s_port[port]);
    pm_port_event_t ev = {.type = PM_PORT_EV_TOD_CHANGED, .port = port};
    emit(&ev);
}

static void cb_boot_catch(void *ctx, pm_port_idx_t port, uint8_t state)
{
    (void)ctx;
    pm_port_event_t ev = {.type = PM_PORT_EV_BOOT_CATCH, .port = port, .value = state};
    emit(&ev);
}

/* ------------------------------------------------------------------------------------------------ hardware init */

static esp_err_t uart_hw_init(port_t *p)
{
    const pm_board_port_pins_t *pins = p->pins;
    int n = pins->uart_num;
    if (n < 1 || n >= SOC_UART_HP_NUM || pins->tx < 0 || pins->rx < 0) return ESP_ERR_INVALID_ARG;
    uart_config_t uc = {
        .baud_rate = PM_PORT_DMX_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_2,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT, /* PLL_F80M: 80 MHz / 250 kHz = 320, exact */
    };
    esp_err_t err = uart_param_config(n, &uc);
    if (err == ESP_OK) err = uart_set_pin(n, pins->tx, pins->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) return err;
    gpio_set_pull_mode(pins->rx, GPIO_PULLUP_ONLY); /* RO is high-Z while DE (= /RE) is high */

    uart_dev_t *hw = UART_LL_GET_HW(n);
    p->hw = hw;
    uart_ll_set_mode(hw, UART_MODE_UART);
    uart_ll_discard_error_data(hw, true); /* break NUL and collision bytes never reach the FIFO */
    uart_ll_set_rxfifo_full_thr(hw, UART_RX_FULL_THR_BULK);
    uart_ll_set_txfifo_empty_thr(hw, UART_TX_EMPTY_THR);
    uart_ll_set_rx_tout(hw, UART_RX_TOUT_BITS);
    uart_ll_set_tx_idle_num(hw, 0);
    uart_ll_inverse_signal(hw, 0);
    uart_ll_rxfifo_rst(hw);
    uart_ll_txfifo_rst(hw);
    uart_ll_disable_intr_mask(hw, UART_LL_INTR_MASK);
    uart_ll_clr_intsts_mask(hw, UART_LL_INTR_MASK);

    err = esp_intr_alloc(uart_periph_signal[n].irq, ISR_FLAGS, uart_isr, p, &p->intr);
    if (err != ESP_OK) return err;
    uart_ll_ena_intr_mask(hw, RX_INTS);
    p->hw_ok = true;
    return ESP_OK;
}

static esp_err_t timer_init(void)
{
    gptimer_config_t tc = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    gptimer_handle_t t;
    esp_err_t err = gptimer_new_timer(&tc, &t);
    if (err != ESP_OK) return err;
    gptimer_event_callbacks_t cbs = {.on_alarm = on_alarm};
    err = gptimer_register_event_callbacks(t, &cbs, NULL); /* allocates the interrupt on this core (1) */
    if (err == ESP_OK) err = gptimer_enable(t);
    if (err == ESP_OK) err = gptimer_start(t);
    if (err != ESP_OK) return err;
    s.timer = t;
    return ESP_OK;
}

/* ------------------------------------------------------------------------------------------------ task */

static pm_port_line_mode_t line_mode(pm_port_mode_t m)
{
    switch (m) {
    case PM_PORT_MODE_OUTPUT: return PM_PORT_LINE_MODE_OUTPUT;
    case PM_PORT_MODE_INPUT: return PM_PORT_LINE_MODE_INPUT;
    default: return PM_PORT_LINE_MODE_OFF;
    }
}

static pm_port_line_timing_t line_timing(const pm_port_cfg_t *c)
{
    pm_port_line_timing_t t = {.break_us = c->break_us, .mab_us = c->mab_us, .rate_hz = c->rate_hz,
                               .continuous = c->continuous, .mbs_us = c->mbs_us > MBS_MAX_US ? MBS_MAX_US
                                                                                             : c->mbs_us};
    if (t.break_us == 0) t.break_us = PM_PORT_BREAK_DEFAULT_US;
    if (t.mab_us == 0) t.mab_us = PM_PORT_MAB_DEFAULT_US;
    if (t.rate_hz == 0) t.rate_hz = PM_PORT_RATE_DEFAULT_HZ;
    return t;
}

static void apply_cfg(port_t *p, const pm_port_cfg_t *c, uint32_t ms)
{
    bool mode_changed = !p->configured || c->mode != p->cfg.mode;
    bool rdm_on = c->mode == PM_PORT_MODE_OUTPUT && c->rdm_enabled;
    bool rdm_was = p->configured && p->cfg.mode == PM_PORT_MODE_OUTPUT && p->cfg.rdm_enabled;
    pm_port_line_timing_t t = line_timing(c);

    portENTER_CRITICAL(&s_lock);
    if (mode_changed) {
        pm_port_line_configure(&p->line, line_mode(c->mode), &t, now_us());
    } else {
        pm_port_line_set_timing(&p->line, &t);
    }
    if (p->hw_ok) uart_ll_set_tx_idle_num(p->hw, (t.mbs_us + 3u) / 4u); /* bit times at 4 us */
    pm_rdm_rx_t rx;
    bool aborted = pm_port_line_rdm_take(&p->line, &rx); /* transaction aborted by a mode change */
    rearm();
    portEXIT_CRITICAL(&s_lock);

    /* hand the aborted result to the controller before it is reset */
    if (aborted) pm_rdm_ctrl_done(&p->ctrl, ms, &rx);
    if (mode_changed || rdm_on != rdm_was) {
        pm_rdm_ctrl_set_enabled(&p->ctrl, false, ms); /* flushes the TOD */
        if (rdm_on) pm_rdm_ctrl_set_enabled(&p->ctrl, true, ms); /* full discovery */
        snapshot_tod(p);
    }
    pm_rdm_ctrl_set_background(&p->ctrl, c->rdm_bg_discovery, bg_interval(c));
    if (mode_changed) {
        p->signal = false;
        p->fps_last_frames = 0;
    }
    p->cfg = *c;
    p->configured = true;
}

static void handle_cmd(const cmd_t *c, uint32_t ms)
{
    if (c->port >= PM_NUM_PORTS) return;
    port_t *p = &s_port[c->port];
    switch (c->kind) {
    case CMD_CONFIGURE:
        apply_cfg(p, &c->cfg, ms);
        break;
    case CMD_RDM: {
        pm_err_t e = pm_rdm_ctrl_submit(&p->ctrl, &c->req);
        /* refresh the free count before releasing the pending slot: queue_free() never over-reports */
        atomic_store(&p->rdm_ctrl_free, pm_rdm_ctrl_queue_free(&p->ctrl));
        atomic_fetch_sub(&p->rdm_pending, 1);
        if (e != PM_OK) {
            ESP_LOGW(TAG, "port %u: RDM queue full, request dropped", c->port + 1u);
            pm_port_event_t ev = {.type = PM_PORT_EV_ERROR, .port = c->port, .token = c->req.token,
                                  .value = ESP_ERR_NO_MEM};
            emit(&ev);
        }
        break;
    }
    case CMD_TOD_CONTROL:
        switch (c->arg) {
        case PM_ATC_FLUSH: pm_rdm_ctrl_flush(&p->ctrl, ms); break;
        case PM_ATC_INC_ON: pm_rdm_ctrl_set_background(&p->ctrl, true, bg_interval(&p->cfg)); break;
        case PM_ATC_INC_OFF: pm_rdm_ctrl_set_background(&p->ctrl, false, bg_interval(&p->cfg)); break;
        case PM_ATC_END: /* pm_rdm_ctrl has no "stop discovery" (interface wish in the report) */ break;
        default: break;
        }
        break;
    case CMD_CATCH_START:
        for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
            if (i != c->port) pm_rdm_ctrl_boot_catch_abort(&s_port[i].ctrl);
        }
        if (pm_rdm_ctrl_boot_catch_start(&p->ctrl, &c->uid, ms) == PM_OK) s.catch_port = c->port;
        break;
    case CMD_CATCH_ABORT:
        for (unsigned i = 0; i < PM_NUM_PORTS; i++) pm_rdm_ctrl_boot_catch_abort(&s_port[i].ctrl);
        break;
    default:
        break;
    }
}

static void service_port(port_t *p, uint32_t ms)
{
    /* RDM: collect the result, hand out the next transaction */
    pm_rdm_rx_t rx;
    atomic_store(&p->rdm_done, false);
    portENTER_CRITICAL(&s_lock);
    bool got = pm_port_line_rdm_take(&p->line, &rx);
    bool busy = pm_port_line_rdm_busy(&p->line);
    portEXIT_CRITICAL(&s_lock);
    /* rx.data points into the line and stays untouched until the next offer */
    if (got) pm_rdm_ctrl_done(&p->ctrl, ms, &rx);

    if (!busy && p->cfg.mode == PM_PORT_MODE_OUTPUT && pm_rdm_ctrl_next(&p->ctrl, ms, &p->txn)) {
        portENTER_CRITICAL(&s_lock);
        bool ok = pm_port_line_rdm_offer(&p->line, &p->txn, now_us());
        rearm();
        portEXIT_CRITICAL(&s_lock);
        if (!ok) {
            pm_rdm_rx_t none = {.status = PM_RDM_RX_TIMEOUT};
            pm_rdm_ctrl_done(&p->ctrl, ms, &none);
        }
    }
    atomic_store(&p->rdm_ctrl_free, pm_rdm_ctrl_queue_free(&p->ctrl));

    /* input */
    if (atomic_exchange(&p->in_changed, false)) {
        pm_port_event_t ev = {.type = PM_PORT_EV_INPUT_FRAME, .port = p->idx};
        emit(&ev);
    }
    if (p->cfg.mode == PM_PORT_MODE_INPUT) {
        portENTER_CRITICAL(&s_lock);
        uint32_t last = p->line.last_in_us;
        bool seen = p->line.in_seen;
        uint32_t now = now_us();
        portEXIT_CRITICAL(&s_lock);
        bool sig = seen && (now - last) < INPUT_LOST_US;
        if (sig != p->signal) {
            p->signal = sig;
            pm_port_event_t ev = {.type = PM_PORT_EV_INPUT_SIGNAL, .port = p->idx, .value = sig ? 1 : 0};
            emit(&ev);
        }
    }
}

static void update_stats(port_t *p, uint32_t elapsed_ms)
{
    portENTER_CRITICAL(&s_lock);
    pm_port_line_stats_t ls = p->line.stats;
    portEXIT_CRITICAL(&s_lock);
    uint32_t frames = p->cfg.mode == PM_PORT_MODE_INPUT ? ls.frames_in : ls.frames_out;
    uint32_t fps = elapsed_ms ? ((frames - p->fps_last_frames) * 1000u + elapsed_ms / 2) / elapsed_ms : 0;
    p->fps_last_frames = frames;
    bool disc = pm_rdm_ctrl_discovery_running(&p->ctrl);

    portENTER_CRITICAL(&s_snap_lock);
    p->stats.frames_out = ls.frames_out;
    p->stats.frames_in = ls.frames_in;
    p->stats.fps = (uint16_t)(p->cfg.mode == PM_PORT_MODE_OFF ? 0 : fps);
    p->stats.rdm_requests = ls.rdm_txns;
    p->stats.rdm_timeouts = ls.rdm_timeouts;
    p->stats.rdm_collisions = ls.rdm_garbage;
    p->stats.tod_count = p->tod_count;
    p->stats.discovery_running = disc;
    p->stats.input_signal = p->signal;
    p->stats.rx_errors = ls.rx_errors;
    p->stats.events_dropped = atomic_load(&s.events_dropped);
    portEXIT_CRITICAL(&s_snap_lock);
}

static void update_catch_status(uint32_t ms)
{
    pm_port_idx_t pi = s.catch_port;
    pm_rdm_boot_catch_t bc;
    pm_rdm_ctrl_boot_catch_status(&s_port[pi].ctrl, ms, &bc);
    portENTER_CRITICAL(&s_snap_lock);
    s_port[pi].catch_status = bc;
    portEXIT_CRITICAL(&s_snap_lock);
}

static void port_task(void *arg)
{
    (void)arg;
    esp_err_t err = timer_init();
    if (err != ESP_OK) ESP_LOGE(TAG, "gptimer: %s - no DMX timing", esp_err_to_name(err));
    for (unsigned i = 0; i < s.board->num_ports && i < PM_NUM_PORTS; i++) {
        err = uart_hw_init(&s_port[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "port %u (UART%d): %s", i + 1, s_port[i].pins->uart_num, esp_err_to_name(err));
            pm_port_event_t ev = {.type = PM_PORT_EV_ERROR, .port = (pm_port_idx_t)i, .value = err};
            emit(&ev);
        }
    }
    ESP_LOGI(TAG, "port engine on core %d", xPortGetCoreID());

    uint32_t last_stats = now_ms();
    static cmd_t cmd; /* large: keep off the stack */
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1));
        uint32_t ms = now_ms();
        while (xQueueReceive(s.cmds, &cmd, 0) == pdTRUE) handle_cmd(&cmd, ms);
        for (unsigned i = 0; i < PM_NUM_PORTS; i++) service_port(&s_port[i], ms);
        update_catch_status(ms);
        if (ms - last_stats >= 1000) {
            for (unsigned i = 0; i < PM_NUM_PORTS; i++) update_stats(&s_port[i], ms - last_stats);
            last_stats = ms;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ API */

esp_err_t pm_port_init(const pm_board_t *board, const pm_uid_t *node_uid)
{
    if (board == NULL || node_uid == NULL) return ESP_ERR_INVALID_ARG;
    if (s.inited) return ESP_ERR_INVALID_STATE;
    s.board = board;
    s.node_uid = *node_uid;
    s.events = xQueueCreate(EVENT_QUEUE_LEN, sizeof(pm_port_event_t));
    s.cmds = xQueueCreate(CMD_QUEUE_LEN, sizeof(cmd_t));
    if (!s.events || !s.cmds) return ESP_ERR_NO_MEM;

    static const uint8_t zero_frame[PM_PORT_FRAME_MAX] = {0};
    pm_rdm_ctrl_cb_t cb = {.ctx = NULL, .on_response = cb_response, .on_tod_changed = cb_tod_changed,
                           .on_boot_catch = cb_boot_catch};
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        port_t *p = &s_port[i];
        memset(p, 0, sizeof(*p));
        p->idx = (pm_port_idx_t)i;
        p->pins = &board->port[i];
        pm_port_line_ops_t ops = {
            .ctx = p,
            .set_de = op_set_de,
            .set_break = op_set_break,
            .tx_start = op_tx_start,
            .rx_start = op_rx_start,
            .tx_abort = op_tx_abort,
            .dmx_frame = op_dmx_frame,
            .dmx_fresh = op_dmx_fresh,
            .rdm_done = op_rdm_done,
            .input_frame = op_input_frame,
        };
        pm_port_line_init(&p->line, &ops);
        pm_port_tbuf_init(&p->out, zero_frame, PM_PORT_FRAME_MAX);
        pm_port_tbuf_init(&p->in, NULL, 0);
        p->shadow_len = PM_PORT_FRAME_MAX;
        pm_rdm_ctrl_init(&p->ctrl, (pm_port_idx_t)i, node_uid, &cb);
        atomic_store(&p->rdm_ctrl_free, pm_rdm_ctrl_queue_free(&p->ctrl));
        p->cfg0.mode = PM_PORT_MODE_OFF;
    }
    s.inited = true;
    return ESP_OK;
}

esp_err_t pm_port_start(void)
{
    if (!s.inited) return ESP_ERR_INVALID_STATE;
    if (s.task) return ESP_OK;
    BaseType_t ok = xTaskCreatePinnedToCore(port_task, "pm_port", 6144, NULL, CONFIG_PM_PORT_TASK_PRIORITY,
                                            &s.task, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t send_cmd(const cmd_t *c)
{
    if (!s.inited) return ESP_ERR_INVALID_STATE;
    if (xQueueSend(s.cmds, c, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    if (s.task) xTaskNotifyGive(s.task);
    return ESP_OK;
}

/* cmd_t is ~300 bytes: build it on the heap, not on the (possibly small) caller stack */
static esp_err_t send_new_cmd(cmd_t *proto, const pm_rdm_request_t *req)
{
    cmd_t *c = malloc(sizeof(cmd_t));
    if (!c) return ESP_ERR_NO_MEM;
    *c = *proto;
    if (req) c->req = *req;
    esp_err_t err = send_cmd(c);
    free(c);
    return err;
}

esp_err_t pm_port_configure(pm_port_idx_t port, const pm_port_cfg_t *cfg)
{
    if (port >= PM_NUM_PORTS || cfg == NULL) return ESP_ERR_INVALID_ARG;
    if (cfg->mode == PM_PORT_MODE_PIXEL) return ESP_ERR_NOT_SUPPORTED;
    if (cfg->mode > PM_PORT_MODE_PIXEL) return ESP_ERR_INVALID_ARG;
    if (!s.inited) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_snap_lock);
    s_port[port].cfg0 = *cfg;
    portEXIT_CRITICAL(&s_snap_lock);
    cmd_t proto = {.kind = CMD_CONFIGURE, .port = port, .cfg = *cfg};
    return send_new_cmd(&proto, NULL);
}

static void publish_output(port_t *p)
{
    uint8_t *dst = pm_port_tbuf_back(&p->out);
    memcpy(dst, p->shadow, p->shadow_len);
    pm_port_tbuf_set_len(&p->out, p->shadow_len);
    pm_port_tbuf_publish(&p->out);
}

void pm_port_output_write(pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    if (port >= PM_NUM_PORTS || !s.inited) return;
    if (len > PM_PORT_FRAME_MAX - 1) len = PM_PORT_FRAME_MAX - 1;
    port_t *p = &s_port[port];
    portENTER_CRITICAL(&s_out_lock);
    p->shadow[0] = 0x00;
    if (len && data) memcpy(&p->shadow[1], data, len);
    p->shadow_len = (uint16_t)(len + 1u);
    if (defer) {
        p->deferred = true;
    } else {
        p->deferred = false;
        publish_output(p);
    }
    portEXIT_CRITICAL(&s_out_lock);
}

void pm_port_commit(void)
{
    if (!s.inited) return;
    portENTER_CRITICAL(&s_out_lock);
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        if (s_port[i].deferred) {
            s_port[i].deferred = false;
            publish_output(&s_port[i]);
        }
    }
    portEXIT_CRITICAL(&s_out_lock);
}

void pm_port_output_clear(pm_port_idx_t port)
{
    if (port >= PM_NUM_PORTS || !s.inited) return;
    port_t *p = &s_port[port];
    portENTER_CRITICAL(&s_out_lock);
    memset(p->shadow, 0, sizeof p->shadow);
    p->deferred = false;
    publish_output(p);
    portEXIT_CRITICAL(&s_out_lock);
}

uint16_t pm_port_output_read(pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    if (port >= PM_NUM_PORTS || !s.inited || out == NULL) return 0;
    port_t *p = &s_port[port];
    portENTER_CRITICAL(&s_out_lock);
    uint16_t n = p->shadow_len ? (uint16_t)(p->shadow_len - 1u) : 0;
    if (n > cap) n = cap;
    memcpy(out, &p->shadow[1], n);
    portEXIT_CRITICAL(&s_out_lock);
    return n;
}

uint16_t pm_port_input_read(pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    if (port >= PM_NUM_PORTS || !s.inited || out == NULL) return 0;
    port_t *p = &s_port[port];
    uint16_t len;
    portENTER_CRITICAL(&s_in_lock);
    const uint8_t *f = pm_port_tbuf_acquire(&p->in, &len);
    uint16_t n = len ? (uint16_t)(len - 1u) : 0;
    if (n > cap) n = cap;
    memcpy(out, f + 1, n);
    portEXIT_CRITICAL(&s_in_lock);
    return n;
}

static bool rdm_allowed(pm_port_idx_t port)
{
    portENTER_CRITICAL(&s_snap_lock);
    bool ok = s_port[port].cfg0.mode == PM_PORT_MODE_OUTPUT && s_port[port].cfg0.rdm_enabled;
    portEXIT_CRITICAL(&s_snap_lock);
    return ok;
}

uint8_t pm_port_rdm_queue_free(pm_port_idx_t port)
{
    if (port >= PM_NUM_PORTS || !s.inited) return 0;
    int f = atomic_load(&s_port[port].rdm_ctrl_free) - atomic_load(&s_port[port].rdm_pending);
    return f > 0 ? (uint8_t)f : 0;
}

esp_err_t pm_port_rdm_submit(pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    if (port >= PM_NUM_PORTS || data == NULL || len == 0 || len > PM_RDM_MAX_MSG_LEN) return ESP_ERR_INVALID_ARG;
    if (!s.inited) return ESP_ERR_INVALID_STATE;
    if (!rdm_allowed(port)) return ESP_ERR_INVALID_STATE;
    if (pm_port_rdm_queue_free(port) == 0) return ESP_ERR_NO_MEM;
    pm_rdm_request_t *req = malloc(sizeof(*req));
    if (!req) return ESP_ERR_NO_MEM;
    req->token = token;
    req->len = len;
    memcpy(req->data, data, len);
    port_t *p = &s_port[port];
    atomic_fetch_add(&p->rdm_pending, 1);
    cmd_t proto = {.kind = CMD_RDM, .port = port};
    esp_err_t err = send_new_cmd(&proto, req);
    if (err != ESP_OK) atomic_fetch_sub(&p->rdm_pending, 1);
    free(req);
    return err;
}

void pm_port_tod_control(pm_port_idx_t port, uint8_t command)
{
    if (port >= PM_NUM_PORTS) return;
    cmd_t proto = {.kind = CMD_TOD_CONTROL, .port = port, .arg = command};
    send_new_cmd(&proto, NULL);
}

uint16_t pm_port_tod_get(pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    if (port >= PM_NUM_PORTS || !s.inited) return 0;
    port_t *p = &s_port[port];
    portENTER_CRITICAL(&s_snap_lock);
    uint16_t total = p->tod_count;
    uint16_t n = total < cap ? total : cap;
    if (out && n) memcpy(out, p->tod, n * sizeof(pm_uid_t));
    portEXIT_CRITICAL(&s_snap_lock);
    return total;
}

esp_err_t pm_port_boot_catch_start(pm_port_idx_t port, const pm_uid_t *target)
{
    if (port >= PM_NUM_PORTS || target == NULL) return ESP_ERR_INVALID_ARG;
    if (pm_uid_is_vendorcast(target)) return ESP_ERR_INVALID_ARG;
    if (!rdm_allowed(port)) return ESP_ERR_INVALID_STATE;
    cmd_t proto = {.kind = CMD_CATCH_START, .port = port, .uid = *target};
    return send_new_cmd(&proto, NULL);
}

void pm_port_boot_catch_abort(void)
{
    cmd_t proto = {.kind = CMD_CATCH_ABORT};
    send_new_cmd(&proto, NULL);
}

void pm_port_boot_catch_status(pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    pm_port_idx_t pi = s.catch_port < PM_NUM_PORTS ? s.catch_port : 0;
    portENTER_CRITICAL(&s_snap_lock);
    *out = s_port[pi].catch_status;
    portEXIT_CRITICAL(&s_snap_lock);
    *port = pi;
}

void pm_port_get_stats(pm_port_idx_t port, pm_port_stats_t *out)
{
    if (port >= PM_NUM_PORTS) {
        memset(out, 0, sizeof(*out));
        return;
    }
    portENTER_CRITICAL(&s_snap_lock);
    *out = s_port[port].stats;
    portEXIT_CRITICAL(&s_snap_lock);
}

bool pm_port_event_receive(pm_port_event_t *out, uint32_t timeout_ms)
{
    if (!s.events) return false;
    return xQueueReceive(s.events, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
