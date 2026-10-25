/*
 * pm_port_tbuf.h - lock-free triple buffer for one DMX frame (pure C11, host-testable).
 *
 * One writer and one reader on different cores, neither ever blocks:
 *   writer: fill pm_port_tbuf_back(), then pm_port_tbuf_publish()  (the newest frame wins; older unread ones drop)
 *   reader: pm_port_tbuf_acquire() -> pointer to the newest published frame, stable until the next acquire
 * Used core 0 -> core 1 for output frames (pm_port_output_write/commit) and core 1 -> core 0 for input frames.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "pm_port_line.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_PORT_TBUF_DIRTY 0x4u

typedef struct {
    uint8_t data[3][PM_PORT_FRAME_MAX];
    uint16_t len[3];
    atomic_uint mid;      /**< index of the middle buffer | PM_PORT_TBUF_DIRTY */
    uint8_t back;         /**< writer-owned */
    uint8_t front;        /**< reader-owned */
} pm_port_tbuf_t;

/** All three buffers = `init_frame` (len bytes). */
void pm_port_tbuf_init(pm_port_tbuf_t *b, const uint8_t *init_frame, uint16_t len);

/** Writer: buffer to fill (PM_PORT_FRAME_MAX bytes) and its length field. */
__attribute__((always_inline)) static inline uint8_t *pm_port_tbuf_back(pm_port_tbuf_t *b)
{
    return b->data[b->back];
}
__attribute__((always_inline)) static inline void pm_port_tbuf_set_len(pm_port_tbuf_t *b, uint16_t len)
{
    b->len[b->back] = len;
}
/** Writer: make the back buffer the newest frame. */
void pm_port_tbuf_publish(pm_port_tbuf_t *b);

/** Reader: is there a frame newer than the last acquire? */
bool pm_port_tbuf_fresh(const pm_port_tbuf_t *b);
/** Reader: switch to the newest frame (if any) and return it; *len = its length. */
const uint8_t *pm_port_tbuf_acquire(pm_port_tbuf_t *b, uint16_t *len);
/** Reader: current frame without switching. */
static inline const uint8_t *pm_port_tbuf_current(const pm_port_tbuf_t *b, uint16_t *len)
{
    *len = b->len[b->front];
    return b->data[b->front];
}

#ifdef __cplusplus
}
#endif
