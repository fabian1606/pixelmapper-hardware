/* sim_model.c - full device models for the simulated RDM bus (see sim_model.h). */
#include "sim_model.h"

#include <stdio.h>
#include <string.h>

#include "pm_pids.h"

#define PRODUCT_CATEGORY_FIXTURE 0x0101u
#define TUBE_APP_MAX (SIM_TUBE_TRAILER)
#define BOOT_REQUEST "BOOT"
#define PD_TYPE_ASCII 0x02u
#define PD_CC_SET 0x02u

static const uint16_t TUBE_APP_PIDS[] = {
    PM_RDM_PID_DEVICE_MODEL_DESCRIPTION, PM_RDM_PID_MANUFACTURER_LABEL, PM_RDM_PID_DEVICE_LABEL,
    PM_RDM_PID_DMX_START_ADDRESS, PM_PID_ENTER_BOOTLOADER,
};
static const uint16_t FIXTURE_PIDS[] = {
    PM_RDM_PID_DEVICE_MODEL_DESCRIPTION, PM_RDM_PID_MANUFACTURER_LABEL, PM_RDM_PID_DEVICE_LABEL,
    PM_RDM_PID_DMX_START_ADDRESS,
};

/* ------------------------------------------------------------------------------------------------ image */

static uint16_t trailer_crc(const uint8_t *flash, uint16_t length)
{
    uint16_t crc = pm_crc16_ccitt(0xFFFFu, flash, length);
    return pm_crc16_ccitt(crc, flash + SIM_TUBE_TRAILER, 12);
}

uint8_t sim_model_trailer_check(const uint8_t *flash, uint16_t manufacturer, uint16_t model)
{
    const uint8_t *t = flash + SIM_TUBE_TRAILER;
    if (pm_get_le16(t) != PM_TUBE_TRAILER_MAGIC || t[2] != 1u) return PM_BOOT_ERR_FORMAT;
    if (pm_get_le16(t + 4) != manufacturer || pm_get_le16(t + 6) != model) return PM_BOOT_ERR_MODEL;
    uint16_t len = pm_get_le16(t + 12);
    if (len == 0 || len > TUBE_APP_MAX || trailer_crc(flash, len) != pm_get_le16(t + 14)) {
        return PM_BOOT_ERR_APP_INVALID;
    }
    return PM_BOOT_ERR_NONE;
}

void sim_model_build_tube_image(uint8_t *img, uint16_t app_len, uint16_t manufacturer, uint16_t model,
                                uint32_t version)
{
    memset(img, 0xFF, SIM_TUBE_APP_END);
    if (app_len > TUBE_APP_MAX) app_len = TUBE_APP_MAX;
    for (uint16_t i = 0; i < app_len; i++) img[i] = (uint8_t)(i * 13u + version);
    uint16_t len = (uint16_t)((app_len + 1u) & ~1u);
    uint8_t *t = img + SIM_TUBE_TRAILER;
    pm_put_le16(t, PM_TUBE_TRAILER_MAGIC);
    t[2] = 1;
    t[3] = 0xFF;
    pm_put_le16(t + 4, manufacturer);
    pm_put_le16(t + 6, model);
    pm_put_le32(t + 8, version);
    pm_put_le16(t + 12, len);
    pm_put_le16(t + 14, trailer_crc(img, len));
}

/* ------------------------------------------------------------------------------------------------ state changes */

static uint8_t check(const sim_dev_t *d)
{
    return sim_model_trailer_check(d->model->flash, pm_uid_manufacturer(&d->uid), d->model->model_id);
}

/** Bootloader app_check(): check + cache BOOT_INFO app_valid/app_fw_version. */
static uint8_t snapshot(sim_dev_t *d)
{
    sim_model_t *m = d->model;
    uint8_t e = check(d);
    m->cached_valid = e == 0;
    m->cached_version = pm_get_le32(m->flash + SIM_TUBE_TRAILER + 8);
    d->app_valid = m->cached_valid;
    return e;
}

static void silence(sim_model_t *m, uint32_t now, uint32_t ms) { m->silent_until_ms = ms ? now + ms : 0; }

static void start_app(sim_bus_t *b, sim_dev_t *d)
{
    sim_model_t *m = d->model;
    d->mode = SIM_APP;
    d->muted = false;
    m->busy = 0;
    m->identify = false;
    m->running_version = pm_get_le32(m->flash + SIM_TUBE_TRAILER + 8);
    silence(m, b->now_ms, m->app_start_ms);
}

static void enter_boot(sim_bus_t *b, sim_dev_t *d)
{
    sim_model_t *m = d->model;
    d->mode = SIM_BOOT;
    d->muted = false;
    m->busy = 0;
    m->last_error = 0;
    m->identify = false;
    m->idle_deadline_ms = b->now_ms + SIM_TUBE_BOOT_TIMEOUT_MS;
    m->enters++;
    snapshot(d);
    silence(m, b->now_ms, SIM_TUBE_ENTER_BOOT_MS);
}

void sim_model_tube(sim_bus_t *b, int idx, sim_model_t *m, const uint8_t *img)
{
    sim_dev_t *d = &b->dev[idx];
    memset(m, 0, sizeof(*m));
    m->tube = true;
    m->model_id = PM_MODEL_TUBE;
    m->start_address = 1;
    m->app_start_ms = SIM_TUBE_APP_START_MS;
    if (img) {
        memcpy(m->flash, img, SIM_TUBE_APP_END);
    } else {
        memset(m->flash, 0xFF, SIM_TUBE_APP_END);
    }
    d->model = m;
    d->tube = true;
    if (snapshot(d) == 0) {
        d->mode = SIM_APP;
        m->running_version = m->cached_version;
    } else {
        d->mode = SIM_BOOT;
        m->idle_deadline_ms = b->now_ms + SIM_TUBE_BOOT_TIMEOUT_MS;
    }
}

void sim_model_fixture(sim_bus_t *b, int idx, sim_model_t *m, uint16_t model_id, uint16_t footprint)
{
    sim_dev_t *d = &b->dev[idx];
    memset(m, 0, sizeof(*m));
    m->model_id = model_id;
    m->footprint = footprint;
    m->fixture_version = 0x00010203u;
    m->start_address = 1;
    d->model = m;
    d->tube = false;
    d->app_valid = true;
    d->mode = SIM_APP;
}

void sim_model_power_on(sim_bus_t *b, sim_dev_t *d)
{
    (void)b;
    sim_model_t *m = d->model;
    m->busy = 0;
    m->last_error = 0;
    m->identify = false;
    m->silent_until_ms = 0;
    if (m->tube) snapshot(d);
}

void sim_model_window_end(sim_bus_t *b, sim_dev_t *d)
{
    if (snapshot(d) == 0) {
        start_app(b, d);
    } else {
        d->mode = SIM_BOOT;
        d->model->idle_deadline_ms = b->now_ms + SIM_TUBE_BOOT_TIMEOUT_MS;
    }
}

bool sim_model_update(sim_bus_t *b, sim_dev_t *d)
{
    sim_model_t *m = d->model;
    if (m->tube && d->mode == SIM_BOOT && (int32_t)(b->now_ms - m->idle_deadline_ms) >= 0) {
        m->busy = 0;
        if (snapshot(d) == 0) {
            start_app(b, d);
        } else {
            m->idle_deadline_ms = b->now_ms + SIM_TUBE_BOOT_TIMEOUT_MS;
        }
    }
    if (m->silent_until_ms != 0) {
        if ((int32_t)(b->now_ms - m->silent_until_ms) < 0) return false;
        m->silent_until_ms = 0;
    }
    return true;
}

/* ------------------------------------------------------------------------------------------------ answers */

static int answer(const sim_dev_t *d, const pm_rdm_msg_t *req, uint8_t rt, const uint8_t *pd, uint8_t pdl,
                  uint8_t *out)
{
    pm_rdm_msg_t r;
    pm_rdm_make_response(req, &d->uid, rt, 0, &r);
    r.pd = pd;
    r.pdl = pdl;
    return pm_rdm_encode(&r, out, PM_RDM_MAX_MSG_LEN);
}

static int ack(const sim_dev_t *d, const pm_rdm_msg_t *req, const uint8_t *pd, uint8_t pdl, uint8_t *out)
{
    return answer(d, req, PM_RDM_RESP_ACK, pd, pdl, out);
}

static int nack(const sim_dev_t *d, const pm_rdm_msg_t *req, uint16_t reason, uint8_t *out)
{
    uint8_t pd[2];
    pm_put_be16(pd, reason);
    return answer(d, req, PM_RDM_RESP_NACK_REASON, pd, 2, out);
}

static int label(const sim_dev_t *d, const pm_rdm_msg_t *req, const char *s, uint8_t *out)
{
    size_t n = strlen(s);
    if (n > 32) n = 32;
    return ack(d, req, (const uint8_t *)s, (uint8_t)n, out);
}

static int pid_list(const sim_dev_t *d, const pm_rdm_msg_t *req, const uint16_t *pids, unsigned n, uint8_t *out)
{
    uint8_t pd[64];
    for (unsigned i = 0; i < n; i++) pm_put_be16(pd + 2 * i, pids[i]);
    return ack(d, req, pd, (uint8_t)(2 * n), out);
}

static int device_info(const sim_dev_t *d, const pm_rdm_msg_t *req, uint16_t category, uint32_t sw,
                       uint16_t footprint, uint8_t pers, uint8_t pers_count, uint16_t start, uint8_t *out)
{
    uint8_t pd[19];
    memset(pd, 0, sizeof(pd));
    pm_put_be16(pd + 0, 0x0100u);
    pm_put_be16(pd + 2, d->model->model_id);
    pm_put_be16(pd + 4, category);
    pm_put_be32(pd + 6, sw);
    pm_put_be16(pd + 10, footprint);
    pd[12] = pers;
    pd[13] = pers_count;
    pm_put_be16(pd + 14, start);
    return ack(d, req, pd, sizeof(pd), out);
}

/** PDL rule: -1 = unknown PID, -2 = wrong command class, -3 = wrong PDL, 0 = ok. */
typedef struct {
    uint16_t pid;
    uint8_t cc;
    uint8_t pdl;
    bool max; /* pdl is a maximum */
} rule_t;

static const rule_t APP_RULES[] = {
    {PM_RDM_PID_SUPPORTED_PARAMETERS, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_PARAMETER_DESCRIPTION, PM_RDM_CC_GET, 2, false},
    {PM_RDM_PID_DEVICE_INFO, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_SOFTWARE_VERSION_LABEL, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_DEVICE_MODEL_DESCRIPTION, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_MANUFACTURER_LABEL, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_DEVICE_LABEL, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_DEVICE_LABEL, PM_RDM_CC_SET, 32, true},
    {PM_RDM_PID_DMX_START_ADDRESS, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_DMX_START_ADDRESS, PM_RDM_CC_SET, 2, false},
    {PM_RDM_PID_IDENTIFY_DEVICE, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_IDENTIFY_DEVICE, PM_RDM_CC_SET, 1, false},
    {PM_PID_ENTER_BOOTLOADER, PM_RDM_CC_SET, 4, false},
};

static const rule_t BOOT_RULES[] = {
    {PM_RDM_PID_SUPPORTED_PARAMETERS, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_DEVICE_INFO, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_SOFTWARE_VERSION_LABEL, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_IDENTIFY_DEVICE, PM_RDM_CC_GET, 0, false},
    {PM_RDM_PID_IDENTIFY_DEVICE, PM_RDM_CC_SET, 1, false},
    {PM_PID_BOOT_INFO, PM_RDM_CC_GET, 0, false},
    {PM_PID_BOOT_WRITE_PAGE, PM_RDM_CC_SET, 2 + PM_BOOT_RDM_PAGE_SIZE, false},
    {PM_PID_BOOT_APP_CRC, PM_RDM_CC_GET, 4, false},
    {PM_PID_BOOT_FINISH, PM_RDM_CC_SET, 0, false},
};

static int rules_check(const rule_t *rules, unsigned n, const pm_rdm_msg_t *m)
{
    bool known = false;
    for (unsigned i = 0; i < n; i++) {
        if (rules[i].pid != m->pid) continue;
        known = true;
        if (rules[i].cc != m->cc) continue;
        bool ok = rules[i].max ? m->pdl <= rules[i].pdl : m->pdl == rules[i].pdl;
        return ok ? 0 : -3;
    }
    return known ? -2 : -1;
}

static int rules_nack(const sim_dev_t *d, const pm_rdm_msg_t *m, int r, uint8_t *out)
{
    uint16_t reason = r == -1 ? PM_NR_UNKNOWN_PID : (r == -2 ? PM_NR_UNSUPPORTED_COMMAND_CLASS : PM_NR_FORMAT_ERROR);
    return nack(d, m, reason, out);
}

static int common_settings(sim_dev_t *d, const pm_rdm_msg_t *m, uint16_t footprint, uint8_t *out)
{
    sim_model_t *s = d->model;
    switch (m->pid) {
    case PM_RDM_PID_DEVICE_LABEL:
        if (m->cc == PM_RDM_CC_GET) return ack(d, m, (const uint8_t *)s->label, s->label_len, out);
        s->label_len = 0;
        while (s->label_len < m->pdl && m->pd[s->label_len] != 0) {
            s->label[s->label_len] = (char)m->pd[s->label_len];
            s->label_len++;
        }
        return ack(d, m, NULL, 0, out);
    case PM_RDM_PID_DMX_START_ADDRESS: {
        uint8_t pd[2];
        if (m->cc == PM_RDM_CC_GET) {
            pm_put_be16(pd, s->start_address);
            return ack(d, m, pd, 2, out);
        }
        uint16_t a = pm_get_be16(m->pd);
        if (a < 1 || (uint32_t)a + footprint > 513u) return nack(d, m, PM_NR_DATA_OUT_OF_RANGE, out);
        s->start_address = a;
        return ack(d, m, NULL, 0, out);
    }
    case PM_RDM_PID_IDENTIFY_DEVICE: {
        if (m->cc == PM_RDM_CC_GET) {
            uint8_t v = s->identify;
            return ack(d, m, &v, 1, out);
        }
        if (m->pd[0] > 1) return nack(d, m, PM_NR_DATA_OUT_OF_RANGE, out);
        s->identify = m->pd[0] != 0;
        return ack(d, m, NULL, 0, out);
    }
    default:
        return -1;
    }
}

static int respond_app(sim_bus_t *b, sim_dev_t *d, const pm_rdm_msg_t *m, bool bcast, uint8_t *out)
{
    sim_model_t *s = d->model;
    int r = rules_check(APP_RULES, sizeof(APP_RULES) / sizeof(APP_RULES[0]), m);
    if (r < 0) return rules_nack(d, m, r, out);
    const uint16_t fp = 12; /* 4 RGB segments */
    char buf[33];
    switch (m->pid) {
    case PM_RDM_PID_SUPPORTED_PARAMETERS:
        return pid_list(d, m, TUBE_APP_PIDS, sizeof(TUBE_APP_PIDS) / sizeof(TUBE_APP_PIDS[0]), out);
    case PM_RDM_PID_PARAMETER_DESCRIPTION: {
        if (pm_get_be16(m->pd) != PM_PID_ENTER_BOOTLOADER) return nack(d, m, PM_NR_DATA_OUT_OF_RANGE, out);
        static const char DESC[] = "Enter bootloader (SET \"BOOT\")";
        uint8_t pd[20 + 32];
        memset(pd, 0, sizeof(pd));
        pm_put_be16(pd, PM_PID_ENTER_BOOTLOADER);
        pd[2] = PM_PDL_ENTER_BOOTLOADER_SET_REQUEST;
        pd[3] = PD_TYPE_ASCII;
        pd[4] = PD_CC_SET;
        memcpy(pd + 20, DESC, sizeof(DESC) - 1);
        return ack(d, m, pd, (uint8_t)(20 + sizeof(DESC) - 1), out);
    }
    case PM_RDM_PID_DEVICE_INFO:
        return device_info(d, m, PRODUCT_CATEGORY_FIXTURE, s->running_version, fp, 1, 4, s->start_address, out);
    case PM_RDM_PID_SOFTWARE_VERSION_LABEL:
        snprintf(buf, sizeof(buf), "tube_fw %u.%u.%u", (unsigned)(s->running_version >> 16) & 0xFFu,
                 (unsigned)(s->running_version >> 8) & 0xFFu, (unsigned)s->running_version & 0xFFu);
        return label(d, m, buf, out);
    case PM_RDM_PID_DEVICE_MODEL_DESCRIPTION:
        return label(d, m, "XBAR500 LED tube (custom fw)", out);
    case PM_RDM_PID_MANUFACTURER_LABEL:
        return label(d, m, "Prototype", out);
    case PM_PID_ENTER_BOOTLOADER: {
        if (memcmp(m->pd, BOOT_REQUEST, 4) != 0) return nack(d, m, PM_NR_DATA_OUT_OF_RANGE, out);
        int n = ack(d, m, NULL, 0, out);
        if (!bcast) enter_boot(b, d); /* never all tubes at once */
        return n;
    }
    default:
        return common_settings(d, m, fp, out);
    }
}

static int respond_boot(sim_bus_t *b, sim_dev_t *d, const pm_rdm_msg_t *m, uint8_t *out)
{
    sim_model_t *s = d->model;
    bool was_busy = s->busy > 0;
    if (s->busy > 0) s->busy--;
    int r = rules_check(BOOT_RULES, sizeof(BOOT_RULES) / sizeof(BOOT_RULES[0]), m);
    if (r < 0) return rules_nack(d, m, r, out);
    if (m->pid >= PM_PID_BOOT_INFO && m->pid <= PM_PID_BOOT_FINISH) {
        d->mode = SIM_BOOT; /* a bootloader PID keeps it (rescue window), restarts the idle timeout */
        s->idle_deadline_ms = b->now_ms + SIM_TUBE_BOOT_TIMEOUT_MS;
        d->boot_info_gets += m->pid == PM_PID_BOOT_INFO;
    }
    char buf[33];
    switch (m->pid) {
    case PM_RDM_PID_SUPPORTED_PARAMETERS:
        return ack(d, m, NULL, 0, out);
    case PM_RDM_PID_DEVICE_INFO:
        return device_info(d, m, PRODUCT_CATEGORY_FIXTURE, PM_BOOT_RDM_SW_VERSION_ID, 0, 0, 0, 0xFFFFu, out);
    case PM_RDM_PID_SOFTWARE_VERSION_LABEL:
        snprintf(buf, sizeof(buf), "tube_boot_rdm %u", (unsigned)PM_BOOT_RDM_VERSION);
        return label(d, m, buf, out);
    case PM_PID_BOOT_INFO: {
        uint8_t pd[19];
        pd[0] = PM_BOOT_RDM_VERSION;
        pd[1] = PM_BOOT_RDM_PAGE_SIZE;
        pm_put_be16(pd + 2, SIM_TUBE_APP_END);
        pd[4] = was_busy;
        pd[5] = s->last_error;
        pm_put_be16(pd + 6, pm_uid_manufacturer(&d->uid));
        pm_put_be16(pd + 8, s->model_id);
        pm_put_be32(pd + 10, PM_BOOT_RDM_SW_VERSION_ID);
        pd[14] = s->cached_valid;
        pm_put_be32(pd + 15, s->cached_version);
        s->last_error = 0; /* read clears */
        return ack(d, m, pd, sizeof(pd), out);
    }
    case PM_PID_BOOT_WRITE_PAGE: {
        uint16_t a = pm_get_be16(m->pd);
        if (a >= SIM_TUBE_APP_END || a % PM_BOOT_RDM_PAGE_SIZE) return nack(d, m, PM_NR_DATA_OUT_OF_RANGE, out);
        if (was_busy) return nack(d, m, PM_NR_BUFFER_FULL, out);
        memcpy(s->flash + a, m->pd + 2, PM_BOOT_RDM_PAGE_SIZE);
        s->cached_valid = false;
        d->app_valid = false;
        s->busy = s->busy_requests;
        s->pages_written++;
        return ack(d, m, NULL, 0, out);
    }
    case PM_PID_BOOT_APP_CRC: {
        uint16_t a = pm_get_be16(m->pd), n = pm_get_be16(m->pd + 2);
        if (n > PM_BOOT_RDM_CRC_MAX_LEN || a > SIM_TUBE_APP_END - n) return nack(d, m, PM_NR_DATA_OUT_OF_RANGE, out);
        if (was_busy) return nack(d, m, PM_NR_BUFFER_FULL, out);
        uint8_t pd[2];
        pm_put_be16(pd, pm_crc16_ccitt(0xFFFFu, s->flash + a, n));
        return ack(d, m, pd, 2, out);
    }
    case PM_PID_BOOT_FINISH: {
        if (was_busy) return nack(d, m, PM_NR_BUFFER_FULL, out);
        int len = ack(d, m, NULL, 0, out);
        s->finishes++;
        uint8_t e = snapshot(d);
        if (e == 0) {
            start_app(b, d);
        } else {
            s->last_error = e;
        }
        return len;
    }
    default:
        return common_settings(d, m, 0, out);
    }
}

static int respond_fixture(sim_dev_t *d, const pm_rdm_msg_t *m, uint8_t *out)
{
    sim_model_t *s = d->model;
    int r = rules_check(APP_RULES, sizeof(APP_RULES) / sizeof(APP_RULES[0]) - 1u, m); /* no ENTER_BOOTLOADER */
    if (r == 0 && m->pid == PM_RDM_PID_PARAMETER_DESCRIPTION) r = -1;
    if (r < 0) return rules_nack(d, m, r, out);
    switch (m->pid) {
    case PM_RDM_PID_SUPPORTED_PARAMETERS:
        return pid_list(d, m, FIXTURE_PIDS, sizeof(FIXTURE_PIDS) / sizeof(FIXTURE_PIDS[0]), out);
    case PM_RDM_PID_DEVICE_INFO:
        return device_info(d, m, PRODUCT_CATEGORY_FIXTURE, s->fixture_version, s->footprint, 1, 1,
                           s->footprint ? s->start_address : 0xFFFFu, out);
    case PM_RDM_PID_SOFTWARE_VERSION_LABEL:
        return label(d, m, "fixture 1.2.3", out);
    case PM_RDM_PID_DEVICE_MODEL_DESCRIPTION:
        return label(d, m, "Simulated fixture", out);
    case PM_RDM_PID_MANUFACTURER_LABEL:
        return label(d, m, "Other", out);
    default:
        return common_settings(d, m, s->footprint, out);
    }
}

int sim_model_respond(sim_bus_t *b, sim_dev_t *d, const pm_rdm_msg_t *m, bool bcast, uint8_t *out)
{
    if (m->sub_device != 0 && !(m->cc == PM_RDM_CC_SET && m->sub_device == 0xFFFFu)) {
        return bcast ? 0 : nack(d, m, PM_NR_SUB_DEVICE_OUT_OF_RANGE, out);
    }
    /* manufacturer PIDs sent to FFFF:FFFFFFFF are ignored (E1.20 6.2.10.2) */
    if (bcast && m->pid >= 0x8000u && pm_uid_is_broadcast(&m->dest)) return 0;
    int n;
    if (!d->model->tube) {
        n = respond_fixture(d, m, out);
    } else if (d->mode == SIM_APP) {
        n = respond_app(b, d, m, bcast, out);
    } else {
        n = respond_boot(b, d, m, out);
    }
    return bcast ? 0 : n;
}
