/*
 * Direct-GPIO backend with Binary Code Modulation (BCM/BAM).
 *
 * Timer1 runs in CTC mode at F_CPU; each compare interrupt latches the port
 * pattern of one bit plane and programs the length of that plane
 * (BASE << bit). Bits 0 and 1 are too short for an interrupt each, so they are
 * emitted in one ISR with a cycle-exact busy wait of BASE cycles in between.
 * -> 7 interrupts per period, period = 255 * BCM_BASE_US (490 Hz @ 8 us).
 */
#include "config.h"
#include "output.h"

#if OUTPUT_BACKEND == OUTPUT_BACKEND_GPIO_BCM

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <string.h>

#define BASE ((uint16_t)((F_CPU / 1000000UL) * BCM_BASE_US))   /* cycles */

_Static_assert((F_CPU / 1000000UL) * BCM_BASE_US * 128UL <= 65536UL, "BCM_BASE_US too long for Timer1");
_Static_assert((F_CPU / 1000000UL) * BCM_BASE_US >= 48, "BCM_BASE_US too short for this F_CPU");

typedef struct { uint8_t port, bit, inv; } led_ch_t;
static const led_ch_t ch_map[] PROGMEM = LED_CHANNEL_MAP;
_Static_assert(sizeof(ch_map) / sizeof(ch_map[0]) == LED_CHANNELS, "LED_CHANNEL_MAP must have 3*LED_SEGMENTS entries");

static uint8_t bcm[2][8][3];          /* [buffer][bit plane][B,C,D] */
static uint8_t keep[3];               /* ~mask of LED pins per port */
static volatile uint8_t front;
static volatile uint8_t pending;

static inline __attribute__((always_inline)) void apply(const uint8_t *v)
{
    PORTB = (PORTB & keep[0]) | v[0];
    PORTC = (PORTC & keep[1]) | v[1];
    PORTD = (PORTD & keep[2]) | v[2];
}

ISR(TIMER1_COMPA_vect)
{
    static uint8_t phase;     /* 0: bits 0+1, 1..6: bits 2..7 */

    if (phase == 0) {
        OCR1A = 3 * BASE - 1;
        if (pending) { front ^= 1; pending = 0; }
        const uint8_t *v = bcm[front][0];
        apply(v);
        __builtin_avr_delay_cycles(BASE - 16);  /* minus apply() overhead */
        apply(v + 3);
        phase = 1;
    } else {
        uint8_t b = phase + 1;
        OCR1A = (BASE << b) - 1;
        apply(bcm[front][b]);
        if (++phase == 7) phase = 0;
    }
}

void out_write(const uint8_t lv[LED_CHANNELS])
{
    pending = 0;                          /* ISR keeps showing front buffer */
    uint8_t (*p)[3] = bcm[front ^ 1];
    memset(p, 0, sizeof(bcm[0]));
    for (uint8_t c = 0; c < LED_CHANNELS; c++) {
        uint8_t port = pgm_read_byte(&ch_map[c].port);
        uint8_t m = 1 << pgm_read_byte(&ch_map[c].bit);
        uint8_t v = lv[c];
        if (pgm_read_byte(&ch_map[c].inv)) v = ~v;
        for (uint8_t b = 0; b < 8; b++, v >>= 1)
            if (v & 1) p[b][port] |= m;
    }
    pending = 1;
}

void out_init(void)
{
    uint8_t mask[3] = { 0, 0, 0 };
    for (uint8_t c = 0; c < LED_CHANNELS; c++)
        mask[pgm_read_byte(&ch_map[c].port)] |= 1 << pgm_read_byte(&ch_map[c].bit);
    for (uint8_t i = 0; i < 3; i++) keep[i] = ~mask[i];

    static const uint8_t zero[LED_CHANNELS];
    out_write(zero);
    front ^= 1; pending = 0;
    apply(bcm[front][0]);                 /* "off" levels before enabling outputs */
    DDRB |= mask[0];
    DDRC |= mask[1];
    DDRD |= mask[2];

    TCCR1A = 0;
    OCR1A = 3 * BASE - 1;
    TCNT1 = 0;
    TCCR1B = (1 << WGM12) | (1 << CS10);  /* CTC, clk/1 */
    TIMSK |= (1 << OCIE1A);
}

#endif
