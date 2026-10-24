/* 1 ms system tick on Timer2 (CTC, TIMER2_COMP_vect). */
#pragma once
#include <stdint.h>

void tb_init(void);
uint16_t tb_now(void);              /* ms, wraps every 65.5 s */
void tb_wait_ms(uint16_t ms);       /* busy wait, kicks the watchdog */
