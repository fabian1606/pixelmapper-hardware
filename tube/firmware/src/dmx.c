#include "config.h"
#include "dmx.h"
#include <avr/interrupt.h>
#include <util/atomic.h>
#include <util/delay.h>
#include <string.h>

enum { RX_IDLE, RX_BREAK, RX_DMX, RX_RDM };

static volatile uint16_t start_slot = 1;
static volatile uint8_t footprint = 3;
static uint8_t buf[2][DMX_MAX_FOOTPRINT];
static volatile uint8_t wr;              /* buffer the ISR fills */
static volatile uint8_t new_frame;

static uint8_t rx_state;
static uint16_t rx_slot;

#if RDM_ENABLED
uint8_t rdm_buf[RDM_BUF_SIZE];
volatile uint8_t rdm_ready;
static uint16_t rdm_idx, rdm_len;        /* a request has up to 257 slots */

#if RS485_DE_ACTIVE_HIGH
#define DE_TX()  (RS485_DE_PORT |= (1 << RS485_DE_BIT))
#define DE_RX()  (RS485_DE_PORT &= ~(1 << RS485_DE_BIT))
#else
#define DE_TX()  (RS485_DE_PORT &= ~(1 << RS485_DE_BIT))
#define DE_RX()  (RS485_DE_PORT |= (1 << RS485_DE_BIT))
#endif
#endif

/* Called first in main(): the pins are inputs after a reset, a floating DE
 * could switch the transceiver to transmit during the ~2 s self-test
 * (E1.20 2.6: ports power up with the driver in high impedance). */
void rs485_init(void)
{
#if RDM_ENABLED
    DE_RX();
    RS485_DE_DDR |= (1 << RS485_DE_BIT);
#endif
    PORTD |= (1 << PD1);                  /* TXD idle high (used for break) */
    DDRD |= (1 << PD1);
}

void dmx_init(void)
{
    UBRRH = (uint8_t)(DMX_UBRR >> 8);     /* URSEL = 0 -> UBRRH */
    UBRRL = (uint8_t)DMX_UBRR;
    UCSRA = 0;
    UCSRC = (1 << URSEL) | (1 << USBS) | (1 << UCSZ1) | (1 << UCSZ0);  /* 8N2 */
    UCSRB = (1 << RXEN) | (1 << RXCIE);
}

void dmx_configure(uint16_t start, uint8_t fp)
{
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        start_slot = start;
        footprint = fp;
        new_frame = 0;
        rx_state = RX_IDLE;
    }
}

uint8_t dmx_take_frame(uint8_t *dst)
{
    uint8_t ok = 0;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (new_frame) {
            memcpy(dst, buf[wr ^ 1], footprint);
            new_frame = 0;
            ok = 1;
        }
    }
    return ok;
}

ISR(USART_RXC_vect)
{
    uint8_t st = UCSRA;                   /* status must be read before UDR */
    uint8_t d = UDR;

    if (st & (1 << FE)) {                 /* break = 0x00 with framing error */
        rx_state = d ? RX_IDLE : RX_BREAK;
        return;
    }
    switch (rx_state) {
    case RX_BREAK:
        if (d == 0x00) {
            rx_state = RX_DMX;
            rx_slot = 0;
        }
#if RDM_ENABLED
        else if (d == 0xCC && !rdm_ready) {
            rdm_buf[0] = d;
            rdm_idx = 1;
            rdm_len = 0xFFFF;
            rx_state = RX_RDM;
        }
#endif
        else
            rx_state = RX_IDLE;
        break;

    case RX_DMX: {
        uint16_t off = ++rx_slot - start_slot;   /* wraps big if before start */
        uint8_t fp = footprint;
        if (off < fp) {
            buf[wr][off] = d;
            if (off == (uint8_t)(fp - 1)) {
                wr ^= 1;
                new_frame = 1;
                rx_state = RX_IDLE;
            }
        } else if (rx_slot > 512) {
            rx_state = RX_IDLE;
        }
        break;
    }

#if RDM_ENABLED
    case RX_RDM:
        rdm_buf[rdm_idx] = d;
        if (rdm_idx == 2) {
            if (d < 24) { rx_state = RX_IDLE; break; }
            rdm_len = d + 2;                     /* + checksum, <= RDM_BUF_SIZE */
        }
        if (++rdm_idx == rdm_len) {
            rdm_ready = 1;
            rx_state = RX_IDLE;
        }
        break;
#endif
    default:
        break;
    }
}

#if RDM_ENABLED
void rs485_tx(const uint8_t *p, uint8_t len, uint8_t with_break)
{
    UCSRB &= ~((1 << RXEN) | (1 << RXCIE));  /* no echo */
    DE_TX();
    if (with_break) {
        PORTD &= ~(1 << PD1);
        _delay_us(RDM_BREAK_US);
        PORTD |= (1 << PD1);
        _delay_us(RDM_MAB_US);
    }
    UCSRB |= (1 << TXEN);
    UCSRA = (1 << TXC);                      /* clear TXC */
    while (len--) {
        while (!(UCSRA & (1 << UDRE)))
            ;
        UDR = *p++;
    }
    while (!(UCSRA & (1 << TXC)))
        ;
    UCSRB &= ~(1 << TXEN);                   /* PD1 back to GPIO (high) */
    DE_RX();
    rx_state = RX_IDLE;
    UCSRB |= (1 << RXEN) | (1 << RXCIE);
}
#endif
