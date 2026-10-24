/*
 * pm_artnet_node.c - Art-Net node logic. Phase 0 stub: compiles, stores ops/settings, does nothing else.
 * Phase 1 implements the behaviour documented in pm_artnet.h / protocol/contract.md.
 */
#include "pm_artnet.h"

#include <string.h>

struct pm_artnet_node {
    pm_artnet_identity_t id;
    pm_artnet_ops_t ops;
    pm_settings_t settings;
    pm_artnet_merge_t merge[PM_NUM_PORTS];
};

size_t pm_artnet_node_size(void) { return sizeof(struct pm_artnet_node); }

pm_artnet_node_t *pm_artnet_node_init(void *mem, const pm_artnet_identity_t *id, const pm_artnet_ops_t *ops,
                                      const pm_settings_t *settings)
{
    if (mem == NULL || id == NULL || ops == NULL || settings == NULL || id->num_ports > PM_NUM_PORTS) return NULL;
    pm_artnet_node_t *n = mem;
    memset(n, 0, sizeof(*n));
    n->id = *id;
    n->ops = *ops;
    n->settings = *settings;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) pm_artnet_merge_init(&n->merge[i], settings->port[i].merge);
    return n;
}

void pm_artnet_handle_packet(pm_artnet_node_t *n, uint32_t src_ip, uint16_t src_port, const uint8_t *data,
                             size_t len)
{
    (void)n; (void)src_ip; (void)src_port; (void)data; (void)len;
}

void pm_artnet_tick(pm_artnet_node_t *n) { (void)n; }

void pm_artnet_settings_changed(pm_artnet_node_t *n, const pm_settings_t *s, pm_change_mask_t mask)
{
    (void)mask;
    n->settings = *s;
}

void pm_artnet_net_changed(pm_artnet_node_t *n) { (void)n; }
void pm_artnet_port_status_changed(pm_artnet_node_t *n, pm_port_idx_t port) { (void)n; (void)port; }

void pm_artnet_input_frame(pm_artnet_node_t *n, pm_port_idx_t port, const uint8_t *data, uint16_t len)
{
    (void)n; (void)port; (void)data; (void)len;
}

void pm_artnet_tod_changed(pm_artnet_node_t *n, pm_port_idx_t port) { (void)n; (void)port; }

void pm_artnet_rdm_response(pm_artnet_node_t *n, uint32_t token, const uint8_t *rdm, size_t rdm_len)
{
    (void)n; (void)token; (void)rdm; (void)rdm_len;
}

void pm_artnet_diag(pm_artnet_node_t *n, uint8_t priority, const char *text)
{
    (void)n; (void)priority; (void)text;
}

void pm_artnet_set_node_report(pm_artnet_node_t *n, uint16_t code, const char *text)
{
    (void)n; (void)code; (void)text;
}

void pm_artnet_merge_init(pm_artnet_merge_t *m, pm_merge_mode_t mode)
{
    memset(m, 0, sizeof(*m));
    m->mode = mode;
}

pm_err_t pm_artnet_merge_feed(pm_artnet_merge_t *m, uint32_t ip, uint8_t physical, const uint8_t *data,
                              uint16_t len, uint32_t now_ms)
{
    (void)m; (void)ip; (void)physical; (void)data; (void)len; (void)now_ms;
    return PM_ERR_NOT_IMPLEMENTED;
}

bool pm_artnet_merge_expire(pm_artnet_merge_t *m, uint32_t now_ms)
{
    (void)m; (void)now_ms;
    return false;
}

unsigned pm_artnet_merge_sources(const pm_artnet_merge_t *m)
{
    return (unsigned)m->src[0].active + (unsigned)m->src[1].active;
}

void pm_artnet_merge_cancel(pm_artnet_merge_t *m) { (void)m; }
