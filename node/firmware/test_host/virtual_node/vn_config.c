/* vn_config.c - settings in RAM (pm_config without NVS): atomic read-modify-write, change masks per source,
 * delivered later from the event loop like main.c (on_settings -> work_settings_changed). */
#include <string.h>

#include "pm_config_store.h"
#include "vn.h"

static pm_settings_t s_cfg;
static pm_change_mask_t s_pending[PM_SRC_SYSTEM + 1];

void vn_config_init(void)
{
    pm_settings_defaults(&s_cfg);
    pm_config_store_normalize(&s_cfg);
    memset(s_pending, 0, sizeof(s_pending));
}

void vn_config_get(pm_settings_t *out) { *out = s_cfg; }

static void store(pm_settings_t *next, pm_change_source_t src)
{
    pm_change_mask_t m = pm_config_store_prepare(next, &s_cfg); /* sanitize + normalize + diff */
    if (m == 0) return;
    s_cfg = *next;
    s_pending[src <= PM_SRC_SYSTEM ? src : PM_SRC_SYSTEM] |= m;
}

pm_err_t vn_config_modify(pm_node_modify_fn fn, void *fctx, pm_change_source_t src)
{
    static pm_settings_t t;
    t = s_cfg;
    fn(fctx, &t);
    store(&t, src);
    return PM_OK;
}

pm_err_t vn_config_factory_reset(pm_change_source_t src)
{
    static pm_settings_t t;
    pm_settings_defaults(&t);
    t.hw_revision_override = s_cfg.hw_revision_override;
    store(&t, src);
    return PM_OK;
}

void vn_config_flush(pm_node_t *node)
{
    for (int src = 0; src <= PM_SRC_SYSTEM; src++) {
        pm_change_mask_t m = s_pending[src];
        s_pending[src] = 0;
        if (m != 0 && node != NULL) pm_node_settings_changed(node, m, (pm_change_source_t)src);
    }
}
