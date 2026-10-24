#include "config.h"
#include "timebase.h"
#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/atomic.h>

#if (F_CPU / 64UL / 1000UL) <= 256
#define TB_CS   (1 << CS22)                         /* /64 */
#define TB_OCR  (F_CPU / 64UL / 1000UL - 1)
#else
#define TB_CS   ((1 << CS22) | (1 << CS21))         /* /256 */
#define TB_OCR  (F_CPU / 256UL / 1000UL - 1)
#endif

static volatile uint16_t tb_ms;

void tb_init(void)
{
    OCR2 = TB_OCR;
    TCCR2 = (1 << WGM21) | TB_CS;
    TIMSK |= (1 << OCIE2);
}

ISR(TIMER2_COMP_vect)
{
    tb_ms++;
}

uint16_t tb_now(void)
{
    uint16_t t;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { t = tb_ms; }
    return t;
}

void tb_wait_ms(uint16_t ms)
{
    uint16_t t0 = tb_now();
    while ((uint16_t)(tb_now() - t0) < ms)
        wdt_reset();
}
