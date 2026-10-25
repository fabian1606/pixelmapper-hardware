/* pm_settings.c - defaults, range checks and diff of pm_settings_t (pure C11). */
#include "pm_settings.h"

#include <string.h>

#include "pm_pids.h"

void pm_settings_defaults(pm_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = PM_SETTINGS_VERSION;
    strcpy(s->short_name, "PM Node");
    strcpy(s->long_name, "Pixelmapper Node");
    s->ip.dhcp = true;
    s->ip.static_mask = 0xFF000000u;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        pm_port_settings_t *p = &s->port[i];
        p->enabled = true;
        p->direction = PM_PORT_DIR_OUTPUT;
        p->address = (pm_port_address_t)(i + 1u); /* Port-Address 0 is deprecated in Art-Net 4 */
        p->merge = PM_MERGE_HTP;
        p->rdm_enabled = true;
        p->rdm_bg_discovery = true;
        p->output_style = PM_OUTPUT_CONTINUOUS;
        p->failsafe = PM_FAILSAFE_HOLD;
        p->dmx_break_us = 176;
        p->dmx_mab_us = 16;
        p->dmx_rate_hz = 40;
        p->dmx_mbs_us = 0;
    }
    s->rdm_bg_interval_s = 3;
    s->display_level = 255;
    s->display_dim_timeout_s = 60;
    s->address_format = PM_ADDRESS_FORMAT_NET_SUB_UNI;
    s->panel_lock = false;
    s->hw_revision_override = 0xFF;
}

#define CLAMP(field, lo, hi)                     \
    do {                                         \
        if ((field) < (lo)) { (field) = (lo); changed = true; } \
        if ((field) > (hi)) { (field) = (hi); changed = true; } \
    } while (0)

bool pm_settings_sanitize(pm_settings_t *s)
{
    bool changed = false;
    s->short_name[PM_SHORT_NAME_LEN - 1] = '\0';
    s->long_name[PM_LONG_NAME_LEN - 1] = '\0';
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        pm_port_settings_t *p = &s->port[i];
        if (p->address > PM_PA_MAX) { p->address &= PM_PA_MAX; changed = true; }
        if (p->direction > PM_PORT_DIR_INPUT) { p->direction = PM_PORT_DIR_OUTPUT; changed = true; }
        if (p->merge > PM_MERGE_LTP) { p->merge = PM_MERGE_HTP; changed = true; }
        if (p->failsafe > PM_FAILSAFE_SCENE) { p->failsafe = PM_FAILSAFE_HOLD; changed = true; }
        if (p->output_style > PM_OUTPUT_DELTA) { p->output_style = PM_OUTPUT_CONTINUOUS; changed = true; }
        CLAMP(p->dmx_break_us, 176, 352);
        CLAMP(p->dmx_mab_us, 12, 88);
        CLAMP(p->dmx_rate_hz, 1, 44);
        CLAMP(p->dmx_mbs_us, 0, 100);
        if (p->scene_len > 512) { p->scene_len = 512; changed = true; }
    }
    CLAMP(s->rdm_bg_interval_s, 1, 600);
    CLAMP(s->display_dim_timeout_s, 0, 3600);
    if (s->address_format > PM_ADDRESS_FORMAT_PORT_ADDRESS) { s->address_format = 0; changed = true; }
    return changed;
}

pm_change_mask_t pm_settings_diff(const pm_settings_t *a, const pm_settings_t *b)
{
    pm_change_mask_t m = 0;
    if (strcmp(a->short_name, b->short_name) != 0 || strcmp(a->long_name, b->long_name) != 0) m |= PM_CHG_NAMES;
    /* field by field: memcmp would also compare the padding bytes after `dhcp` */
    if (a->ip.dhcp != b->ip.dhcp || a->ip.static_ip != b->ip.static_ip || a->ip.static_mask != b->ip.static_mask ||
        a->ip.gateway != b->ip.gateway) {
        m |= PM_CHG_IP;
    }
    if (a->rdm_bg_interval_s != b->rdm_bg_interval_s) m |= PM_CHG_RDM_INTERVAL;
    if (a->display_level != b->display_level || a->display_dim_timeout_s != b->display_dim_timeout_s ||
        a->address_format != b->address_format) {
        m |= PM_CHG_DISPLAY;
    }
    if (a->panel_lock != b->panel_lock) m |= PM_CHG_PANEL_LOCK;
    if (a->hw_revision_override != b->hw_revision_override) m |= PM_CHG_HW_REV;
    for (unsigned i = 0; i < PM_NUM_PORTS; i++) {
        const pm_port_settings_t *x = &a->port[i], *y = &b->port[i];
        if (x->enabled != y->enabled) m |= PM_CHG_PORT(PM_CHG_P_ENABLED, i);
        if (x->direction != y->direction) m |= PM_CHG_PORT(PM_CHG_P_DIRECTION, i);
        if (x->address != y->address) m |= PM_CHG_PORT(PM_CHG_P_ADDRESS, i);
        if (x->merge != y->merge) m |= PM_CHG_PORT(PM_CHG_P_MERGE, i);
        if (x->rdm_enabled != y->rdm_enabled || x->rdm_bg_discovery != y->rdm_bg_discovery) {
            m |= PM_CHG_PORT(PM_CHG_P_RDM, i);
        }
        if (x->output_style != y->output_style || x->failsafe != y->failsafe || x->scene_len != y->scene_len ||
            memcmp(x->scene, y->scene, sizeof(x->scene)) != 0) {
            m |= PM_CHG_PORT(PM_CHG_P_OUTPUT, i);
        }
        if (x->dmx_break_us != y->dmx_break_us || x->dmx_mab_us != y->dmx_mab_us || x->dmx_rate_hz != y->dmx_rate_hz ||
            x->dmx_mbs_us != y->dmx_mbs_us) {
            m |= PM_CHG_PORT(PM_CHG_P_TIMING, i);
        }
    }
    return m;
}
