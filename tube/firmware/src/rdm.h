/* RDM responder (ANSI E1.20). */
#pragma once
#include <stdint.h>
#include "config.h"

#if RDM_ENABLED
extern uint8_t rdm_identify;
void rdm_init(uint32_t device_id);
void rdm_poll(void);            /* handle a received request, if any */
#else
#define rdm_identify 0
#define rdm_init(id) ((void)(id))
#define rdm_poll() ((void)0)
#endif
