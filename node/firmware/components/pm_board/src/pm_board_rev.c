/*
 * pm_board_rev.c - revision resistor code decoder (pure C11, no ESP-IDF; host-tested in pm_port/test_host).
 */
#include "pm_board.h"

bool pm_board_decode_rev_code(const uint8_t *level_pullup, const uint8_t *level_pulldown, uint8_t n_pins,
                              uint8_t *revision)
{
    if (n_pins == 0) {
        *revision = 0;
        return true;
    }
    if (n_pins > PM_BOARD_REV_CODE_MAX_PINS) return false;
    unsigned floating = 0, code = 0;
    for (uint8_t i = 0; i < n_pins; i++) {
        uint8_t up = level_pullup[i] ? 1u : 0u, down = level_pulldown[i] ? 1u : 0u;
        if (up && !down) {
            floating++;
        } else if (up && down) {
            code |= 1u << i;
        } else if (!up && !down) {
            /* bit 0 */
        } else {
            return false; /* reads low with pull-up and high with pull-down: impossible */
        }
    }
    if (floating == n_pins) {
        *revision = 0;
        return true;
    }
    if (floating != 0) return false;
    *revision = (uint8_t)(code + 1u);
    return true;
}
