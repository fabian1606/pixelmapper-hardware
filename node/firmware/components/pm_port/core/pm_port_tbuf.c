/*
 * pm_port_tbuf.c - lock-free triple buffer (see pm_port_tbuf.h).
 */
#include "pm_port_tbuf.h"

#include <string.h>

void pm_port_tbuf_init(pm_port_tbuf_t *b, const uint8_t *init_frame, uint16_t len)
{
    if (len > PM_PORT_FRAME_MAX) len = PM_PORT_FRAME_MAX;
    for (unsigned i = 0; i < 3; i++) {
        memset(b->data[i], 0, PM_PORT_FRAME_MAX);
        if (init_frame && len) memcpy(b->data[i], init_frame, len);
        b->len[i] = len;
    }
    b->front = 0;
    atomic_store(&b->mid, 1u);
    b->back = 2;
}

PM_PORT_IRAM void pm_port_tbuf_publish(pm_port_tbuf_t *b)
{
    unsigned old = atomic_exchange_explicit(&b->mid, (unsigned)b->back | PM_PORT_TBUF_DIRTY, memory_order_acq_rel);
    b->back = (uint8_t)(old & 3u);
}

PM_PORT_IRAM bool pm_port_tbuf_fresh(const pm_port_tbuf_t *b)
{
    return (atomic_load_explicit(&((pm_port_tbuf_t *)b)->mid, memory_order_acquire) & PM_PORT_TBUF_DIRTY) != 0;
}

PM_PORT_IRAM const uint8_t *pm_port_tbuf_acquire(pm_port_tbuf_t *b, uint16_t *len)
{
    if (atomic_load_explicit(&b->mid, memory_order_acquire) & PM_PORT_TBUF_DIRTY) {
        unsigned old = atomic_exchange_explicit(&b->mid, b->front, memory_order_acq_rel);
        b->front = (uint8_t)(old & 3u);
    }
    *len = b->len[b->front];
    return b->data[b->front];
}
