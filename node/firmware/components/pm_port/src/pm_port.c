/*
 * pm_port.c - port engine. Phase 0 stub: keeps configuration and buffers, no UART traffic yet.
 */
#include "pm_port.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "pm_port";

#define PM_PORT_EVENT_QUEUE_LEN 16

static struct {
    const pm_board_t *board;
    pm_uid_t node_uid;
    pm_port_cfg_t cfg[PM_NUM_PORTS];
    QueueHandle_t events;
    TaskHandle_t task;
} s;

esp_err_t pm_port_init(const pm_board_t *board, const pm_uid_t *node_uid)
{
    s.board = board;
    s.node_uid = *node_uid;
    s.events = xQueueCreate(PM_PORT_EVENT_QUEUE_LEN, sizeof(pm_port_event_t));
    return s.events ? ESP_OK : ESP_ERR_NO_MEM;
}

static void port_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "port engine on core %d (stub)", xPortGetCoreID());
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t pm_port_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(port_task, "pm_port", 4096, NULL, configMAX_PRIORITIES - 2, &s.task, 1);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t pm_port_configure(pm_port_idx_t port, const pm_port_cfg_t *cfg)
{
    if (port >= PM_NUM_PORTS || cfg == NULL) return ESP_ERR_INVALID_ARG;
    s.cfg[port] = *cfg;
    return ESP_OK;
}

void pm_port_output_write(pm_port_idx_t port, const uint8_t *data, uint16_t len, bool defer)
{
    (void)port; (void)data; (void)len; (void)defer;
}

void pm_port_commit(void) {}
void pm_port_output_clear(pm_port_idx_t port) { (void)port; }

uint16_t pm_port_output_read(pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    (void)port; (void)out; (void)cap;
    return 0;
}

uint16_t pm_port_input_read(pm_port_idx_t port, uint8_t *out, uint16_t cap)
{
    (void)port; (void)out; (void)cap;
    return 0;
}

esp_err_t pm_port_rdm_submit(pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    (void)port; (void)token; (void)data; (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

uint8_t pm_port_rdm_queue_free(pm_port_idx_t port)
{
    (void)port;
    return 0;
}

void pm_port_tod_control(pm_port_idx_t port, uint8_t command) { (void)port; (void)command; }

uint16_t pm_port_tod_get(pm_port_idx_t port, pm_uid_t *out, uint16_t cap)
{
    (void)port; (void)out; (void)cap;
    return 0;
}

esp_err_t pm_port_boot_catch_start(pm_port_idx_t port, const pm_uid_t *target)
{
    (void)port; (void)target;
    return ESP_ERR_NOT_SUPPORTED;
}

void pm_port_boot_catch_abort(void) {}

void pm_port_boot_catch_status(pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    *port = 0;
    memset(out, 0, sizeof(*out));
}

void pm_port_get_stats(pm_port_idx_t port, pm_port_stats_t *out)
{
    (void)port;
    memset(out, 0, sizeof(*out));
}

bool pm_port_event_receive(pm_port_event_t *out, uint32_t timeout_ms)
{
    return xQueueReceive(s.events, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}
