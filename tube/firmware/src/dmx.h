/* DMX512 receiver + RS-485 half-duplex transmit on the USART. */
#pragma once
#include <stdint.h>
#include "config.h"

#define DMX_MAX_FOOTPRINT (3 * LED_SEGMENTS + 1)
/* Every legal request is received completely (message length <= 255, PD <= 231,
 * E1.20 6.2.3), so an over-long request is answered with a NACK instead of being
 * dropped (E1.20 6.2.10.1: GET/SET to a unicast UID always get a response). */
#define RDM_RX_MAX_PD     231
#define RDM_TX_MAX_PD     (5 * DMX_MAX_FOOTPRINT)   /* largest response: SLOT_INFO */
#if RDM_TX_MAX_PD > RDM_RX_MAX_PD
#error "RDM response PD too large (max 231)"
#endif
/* header + PD + checksum; shared RX/TX buffer (257 B) */
#define RDM_BUF_SIZE      (24 + RDM_RX_MAX_PD + 2)

void rs485_init(void);                                   /* driver off, TXD idle - first thing after reset */
void dmx_init(void);
void dmx_configure(uint16_t start, uint8_t footprint);   /* start 1..512 */
uint8_t dmx_take_frame(uint8_t *dst);                    /* 1 if a new frame was copied */

#if RDM_ENABLED
extern uint8_t rdm_buf[RDM_BUF_SIZE];
extern volatile uint8_t rdm_ready;                       /* complete RDM packet in rdm_buf */
void rs485_tx(const uint8_t *buf, uint8_t len, uint8_t with_break);
#endif
