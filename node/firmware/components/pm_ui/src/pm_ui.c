/*
 * pm_ui.c - front panel. Phase 0 stub.
 */
#include "pm_ui.h"

esp_err_t pm_ui_init(const pm_board_t *board, const pm_ui_model_t *model)
{
    (void)board; (void)model;
    return ESP_OK;
}

esp_err_t pm_ui_start(void) { return ESP_OK; }

void pm_ui_notify(const pm_ui_event_t *ev) { (void)ev; }

bool pm_ui_factory_reset_requested(void) { return false; }
