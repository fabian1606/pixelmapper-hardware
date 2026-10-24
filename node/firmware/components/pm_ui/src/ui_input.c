/* ui_input.c - encoder/button gestures (pure C11). */
#include "ui_input.h"

#include <string.h>

static void post(ui_input_t *in, uint8_t type, int steps, uint8_t accel)
{
    uint8_t next = (uint8_t)((in->q_head + 1u) % (sizeof(in->q) / sizeof(in->q[0])));
    if (next == in->q_tail) return; /* full: drop */
    in->q[in->q_head] = (ui_key_t){.type = type, .steps = (int8_t)steps, .accel = accel};
    in->q_head = next;
}

void ui_input_init(ui_input_t *in) { memset(in, 0, sizeof(*in)); }

static uint8_t accel_of(ui_input_t *in, int dir, uint32_t now)
{
    if (dir != in->last_dir) {
        in->last_dir = dir;
        in->det_t[0] = in->det_t[1] = 0;
        in->det_t[2] = now;
        return 1;
    }
    in->det_t[0] = in->det_t[1];
    in->det_t[1] = in->det_t[2];
    in->det_t[2] = now;
    if (in->det_t[0] == 0) return 1;
    uint32_t avg = (in->det_t[2] - in->det_t[0]) / 2u; /* mean interval of the last 3 detents */
    if (avg < 30) return 100;
    if (avg <= 80) return 10;
    return 1;
}

void ui_input_feed(ui_input_t *in, int detents, bool push, bool back, uint32_t now)
{
    /* rotation: one event per call, accel from the detent rhythm */
    if (detents != 0 && !in->combo) {
        int dir = detents > 0 ? 1 : -1;
        uint8_t acc = 1;
        int n = detents > 0 ? detents : -detents;
        for (int i = 0; i < n; i++) acc = accel_of(in, dir, now);
        if (n > 1 && acc < 100) acc = n >= 3 ? 100 : 10; /* several detents within one poll = fast */
        if (detents > 100) detents = 100;
        if (detents < -100) detents = -100;
        post(in, UI_KEY_ROT, detents, acc);
    }

    bool push_edge = push != in->push, back_edge = back != in->back;
    if (push_edge && push) {
        in->push_t = now;
        in->push_consumed = false;
    }
    if (back_edge && back) {
        in->back_t = now;
        in->back_long_sent = false;
    }
    in->push = push;
    in->back = back;

    if (push && back && !in->combo) {
        in->combo = true;
        in->combo_sent = false;
        in->combo_t = now;
    }
    if (in->combo) {
        if (push && back) {
            if (!in->combo_sent && now - in->combo_t >= UI_COMBO_MS) {
                in->combo_sent = true;
                post(in, UI_KEY_COMBO, 0, 0);
            }
        } else if (!push && !back) {
            in->combo = false; /* swallow both releases */
        }
        return;
    }

    if (push_edge && !push) {
        if (!in->push_consumed && now - in->push_t < UI_PUSH_SHORT_MS) post(in, UI_KEY_PUSH, 0, 0);
    }
    if (back && !in->back_long_sent && now - in->back_t >= UI_BACK_LONG_MS) {
        in->back_long_sent = true;
        post(in, UI_KEY_BACK_LONG, 0, 0);
    }
    if (back_edge && !back && !in->back_long_sent) post(in, UI_KEY_BACK, 0, 0);
}

bool ui_input_pop(ui_input_t *in, ui_key_t *out)
{
    if (in->q_tail == in->q_head) return false;
    *out = in->q[in->q_tail];
    in->q_tail = (uint8_t)((in->q_tail + 1u) % (sizeof(in->q) / sizeof(in->q[0])));
    return true;
}

uint32_t ui_input_push_held(const ui_input_t *in, uint32_t now)
{
    if (!in->push || in->combo) return 0;
    return now - in->push_t;
}

void ui_input_consume_push(ui_input_t *in) { in->push_consumed = true; }

uint32_t ui_input_combo_ms(const ui_input_t *in, uint32_t now)
{
    if (!in->combo || !in->push || !in->back) return 0;
    uint32_t d = now - in->combo_t;
    return d > UI_COMBO_MS ? UI_COMBO_MS : d;
}
