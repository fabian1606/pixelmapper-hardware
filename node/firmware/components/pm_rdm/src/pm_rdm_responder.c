/*
 * pm_rdm_responder.c - node RDM responder. Phase 0 stub (no answers); Phase 1 implements pm_rdm_responder.h.
 */
#include "pm_rdm_responder.h"

#include <string.h>

#include "pm_pids.h"

struct pm_rdm_responder {
    pm_rdm_responder_identity_t id;
    pm_rdm_responder_ops_t ops;
};

size_t pm_rdm_responder_size(void) { return sizeof(struct pm_rdm_responder); }

pm_rdm_responder_t *pm_rdm_responder_init(void *mem, const pm_rdm_responder_identity_t *id,
                                          const pm_rdm_responder_ops_t *ops)
{
    if (mem == NULL || id == NULL || ops == NULL) return NULL;
    pm_rdm_responder_t *r = mem;
    memset(r, 0, sizeof(*r));
    r->id = *id;
    r->ops = *ops;
    return r;
}

int pm_rdm_responder_handle(pm_rdm_responder_t *r, const uint8_t *req, size_t len, uint8_t *out, size_t cap)
{
    (void)r; (void)req; (void)len; (void)out; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}

void pm_rdm_responder_settings_changed(pm_rdm_responder_t *r, pm_change_mask_t mask, pm_change_source_t src)
{
    (void)r; (void)mask; (void)src;
}

void pm_rdm_responder_queue_pid(pm_rdm_responder_t *r, uint16_t pid) { (void)r; (void)pid; }

void pm_rdm_responder_status(pm_rdm_responder_t *r, uint16_t status_id, uint8_t type, int16_t data1, int16_t data2)
{
    (void)r; (void)status_id; (void)type; (void)data1; (void)data2;
}

void pm_rdm_responder_local_set(pm_rdm_responder_t *r, pm_port_idx_t port, const pm_uid_t *uid,
                                uint16_t sub_device, uint16_t pid)
{
    (void)r; (void)port; (void)uid; (void)sub_device; (void)pid;
}
