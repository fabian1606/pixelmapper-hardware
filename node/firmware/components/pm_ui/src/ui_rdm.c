/* ui_rdm.c - RDM device cache of the front panel (pure C11). */
#include "ui_rdm.h"

#include <string.h>

#include "pm_rdm.h"

#define QLEN 16
#define REFRESH_MS 30000u

typedef struct {
    uint8_t port;
    pm_uid_t uid;
    uint8_t cc;
    uint16_t pid;
    uint8_t pdl;
    uint8_t pd[32];
} req_t;

static struct {
    pm_uid_t self;
    ui_rdm_send_fn send;
    void *ctx;
    ui_rdm_dev_t dev[UI_RDM_CACHE];
    uint32_t stamp;
    req_t q[QLEN];
    uint8_t q_n;
    bool inflight;
    req_t cur;
    uint32_t cur_token, cur_ms, token;
    uint8_t tn;
    uint32_t now;
    ui_rdm_pers_t pers[UI_RDM_PERS_MAX];
    pm_uid_t pers_uid;
    ui_rdm_res_t result;
    uint16_t result_pid;
} R;

void ui_rdm_init(const pm_uid_t *self, ui_rdm_send_fn send, void *ctx)
{
    memset(&R, 0, sizeof(R));
    if (self) R.self = *self;
    R.send = send;
    R.ctx = ctx;
}

void ui_rdm_set_self(const pm_uid_t *self) { R.self = *self; }

static bool uid_eq(const pm_uid_t *a, const pm_uid_t *b) { return memcmp(a->b, b->b, 6) == 0; }

ui_rdm_dev_t *ui_rdm_find(uint8_t port, const pm_uid_t *uid)
{
    for (int i = 0; i < UI_RDM_CACHE; i++) {
        ui_rdm_dev_t *d = &R.dev[i];
        if (d->used && d->port == port && uid_eq(&d->uid, uid)) return d;
    }
    return NULL;
}

const ui_rdm_dev_t *ui_rdm_cache_at(int i)
{
    if (i < 0 || i >= UI_RDM_CACHE || !R.dev[i].used) return NULL;
    return &R.dev[i];
}

static bool queued(uint8_t port, const pm_uid_t *uid, uint8_t cc, uint16_t pid)
{
    for (int i = 0; i < R.q_n; i++) {
        req_t *r = &R.q[i];
        if (r->port == port && r->cc == cc && r->pid == pid && uid_eq(&r->uid, uid)) return true;
    }
    return R.inflight && R.cur.port == port && R.cur.cc == cc && R.cur.pid == pid && uid_eq(&R.cur.uid, uid);
}

static void enqueue(uint8_t port, const pm_uid_t *uid, uint8_t cc, uint16_t pid, const void *pd, uint8_t pdl)
{
    if (cc == PM_RDM_CC_GET && queued(port, uid, cc, pid) && pdl == 0) return;
    if (R.q_n >= QLEN) return;
    req_t *r = &R.q[R.q_n++];
    r->port = port;
    r->uid = *uid;
    r->cc = cc;
    r->pid = pid;
    r->pdl = pdl > sizeof(r->pd) ? (uint8_t)sizeof(r->pd) : pdl;
    if (pd && r->pdl) memcpy(r->pd, pd, r->pdl);
}

ui_rdm_dev_t *ui_rdm_dev(uint8_t port, const pm_uid_t *uid)
{
    ui_rdm_dev_t *d = ui_rdm_find(port, uid);
    if (!d) {
        d = &R.dev[0];
        for (int i = 1; i < UI_RDM_CACHE; i++) {
            if (R.dev[i].used < d->used) d = &R.dev[i];
        }
        memset(d, 0, sizeof(*d));
        d->uid = *uid;
        d->port = port;
        d->start = 0xFFFF;
    }
    d->used = ++R.stamp;
    bool stale = d->fetched_ms && R.now - d->fetched_ms > REFRESH_MS;
    if (!(d->flags & UI_RDM_HAVE_INFO) || stale) {
        if (!(d->flags & UI_RDM_FAILED) || stale) enqueue(port, uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
    }
    if (!(d->flags & UI_RDM_HAVE_LABEL) || stale) {
        if (!(d->flags & UI_RDM_FAILED) || stale) enqueue(port, uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_LABEL, NULL, 0);
    }
    if (stale) d->fetched_ms = R.now;
    return d;
}

void ui_rdm_set_identify(ui_rdm_dev_t *d, bool on)
{
    uint8_t v = on ? 1 : 0;
    enqueue(d->port, &d->uid, PM_RDM_CC_SET, PM_RDM_PID_IDENTIFY_DEVICE, &v, 1);
}

void ui_rdm_set_start(ui_rdm_dev_t *d, uint16_t start)
{
    uint8_t pd[2];
    pm_put_be16(pd, start);
    enqueue(d->port, &d->uid, PM_RDM_CC_SET, PM_RDM_PID_DMX_START_ADDRESS, pd, 2);
}

void ui_rdm_set_personality(ui_rdm_dev_t *d, uint8_t pers)
{
    enqueue(d->port, &d->uid, PM_RDM_CC_SET, PM_RDM_PID_DMX_PERSONALITY, &pers, 1);
}

void ui_rdm_set_label(ui_rdm_dev_t *d, const char *label)
{
    size_t n = strlen(label);
    if (n > 32) n = 32;
    enqueue(d->port, &d->uid, PM_RDM_CC_SET, PM_RDM_PID_DEVICE_LABEL, label, (uint8_t)n);
}

void ui_rdm_load_personalities(ui_rdm_dev_t *d)
{
    if (!uid_eq(&R.pers_uid, &d->uid)) {
        memset(R.pers, 0, sizeof(R.pers));
        R.pers_uid = d->uid;
    }
    for (uint8_t p = 1; p <= d->pers_count && p <= UI_RDM_PERS_MAX; p++) {
        if (!R.pers[p - 1].valid) {
            enqueue(d->port, &d->uid, PM_RDM_CC_GET, PM_RDM_PID_DMX_PERSONALITY_DESCRIPTION, &p, 1);
        }
    }
}

const ui_rdm_pers_t *ui_rdm_personality(uint8_t pers)
{
    if (pers < 1 || pers > UI_RDM_PERS_MAX || !R.pers[pers - 1].valid) return NULL;
    return &R.pers[pers - 1];
}

bool ui_rdm_busy(const ui_rdm_dev_t *d, uint16_t pid)
{
    for (int i = 0; i < R.q_n; i++) {
        if (R.q[i].cc == PM_RDM_CC_SET && R.q[i].pid == pid && uid_eq(&R.q[i].uid, &d->uid)) return true;
    }
    return R.inflight && R.cur.cc == PM_RDM_CC_SET && R.cur.pid == pid && uid_eq(&R.cur.uid, &d->uid);
}

void ui_rdm_forget(uint8_t port)
{
    for (int i = 0; i < UI_RDM_CACHE; i++) {
        if (port == 0xFF || R.dev[i].port == port) memset(&R.dev[i], 0, sizeof(R.dev[i]));
    }
    uint8_t k = 0;
    for (uint8_t i = 0; i < R.q_n; i++) {
        if (port != 0xFF && R.q[i].port != port) R.q[k++] = R.q[i];
    }
    R.q_n = k;
}

ui_rdm_res_t ui_rdm_take_result(uint16_t *pid)
{
    ui_rdm_res_t r = R.result;
    if (pid) *pid = R.result_pid;
    R.result = UI_RDM_RES_NONE;
    return r;
}

/* ------------------------------------------------------------------------------------------------ wire */

static uint16_t encode(const req_t *r, uint8_t tn, uint8_t *buf)
{
    uint8_t len = (uint8_t)(24u + r->pdl);
    buf[0] = PM_RDM_START_CODE;
    buf[1] = PM_RDM_SUB_START_CODE;
    buf[2] = len;
    memcpy(&buf[3], r->uid.b, 6);
    memcpy(&buf[9], R.self.b, 6);
    buf[15] = tn;
    buf[16] = (uint8_t)(r->port + 1u);
    buf[17] = 0;
    buf[18] = 0;
    buf[19] = 0;
    buf[20] = r->cc;
    pm_put_be16(&buf[21], r->pid);
    buf[23] = r->pdl;
    memcpy(&buf[24], r->pd, r->pdl);
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) sum = (uint16_t)(sum + buf[i]);
    pm_put_be16(&buf[len], sum);
    return (uint16_t)(len + 2u);
}

static void finish(ui_rdm_res_t res)
{
    R.inflight = false;
    ui_rdm_dev_t *d = ui_rdm_find(R.cur.port, &R.cur.uid);
    if (res != UI_RDM_RES_OK && d) {
        d->flags |= UI_RDM_FAILED;
        if (!d->fetched_ms) d->fetched_ms = R.now ? R.now : 1;
    }
    if (R.cur.cc == PM_RDM_CC_SET) {
        R.result = res;
        R.result_pid = R.cur.pid;
        if (res == UI_RDM_RES_OK && d) {
            if (R.cur.pid == PM_RDM_PID_IDENTIFY_DEVICE) {
                if (R.cur.pd[0]) d->flags |= UI_RDM_IDENTIFY;
                else d->flags &= (uint8_t)~UI_RDM_IDENTIFY;
            }
            if (R.cur.pid == PM_RDM_PID_DMX_START_ADDRESS || R.cur.pid == PM_RDM_PID_DMX_PERSONALITY) {
                enqueue(d->port, &d->uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, NULL, 0);
            }
            if (R.cur.pid == PM_RDM_PID_DEVICE_LABEL) {
                memset(d->label, 0, sizeof(d->label));
                memcpy(d->label, R.cur.pd, R.cur.pdl);
                d->flags |= UI_RDM_HAVE_LABEL;
            }
        }
    }
}

static void copy_text(char *dst, const uint8_t *src, uint8_t n)
{
    if (n > 32) n = 32;
    for (uint8_t i = 0; i < n; i++) {
        char c = (char)src[i];
        dst[i] = (c >= 0x20 && c < 0x7F) ? c : '?';
    }
    dst[n] = '\0';
}

void ui_rdm_on_response(uint32_t token, const uint8_t *buf, uint16_t len)
{
    if (!R.inflight || token != R.cur_token) return;
    if (len < 26 || buf[0] != PM_RDM_START_CODE || buf[2] < 24 || buf[2] + 2u > len) {
        finish(UI_RDM_RES_NACK);
        return;
    }
    uint8_t mlen = buf[2];
    uint16_t sum = 0;
    for (int i = 0; i < mlen; i++) sum = (uint16_t)(sum + buf[i]);
    if (sum != pm_get_be16(&buf[mlen])) {
        finish(UI_RDM_RES_NACK);
        return;
    }
    uint8_t resp = buf[16], pdl = buf[23];
    uint16_t pid = pm_get_be16(&buf[21]);
    const uint8_t *pd = &buf[24];
    if (pdl + 24u > mlen || pid != R.cur.pid || resp != PM_RDM_RESP_ACK) {
        finish(UI_RDM_RES_NACK);
        return;
    }
    ui_rdm_dev_t *d = ui_rdm_find(R.cur.port, &R.cur.uid);
    if (d && R.cur.cc == PM_RDM_CC_GET) {
        d->flags &= (uint8_t)~UI_RDM_FAILED;
        if (!d->fetched_ms) d->fetched_ms = R.now ? R.now : 1;
        if (pid == PM_RDM_PID_DEVICE_INFO && pdl >= 19) {
            d->model = pm_get_be16(&pd[2]);
            d->sw_version = pm_get_be32(&pd[6]);
            d->footprint = pm_get_be16(&pd[10]);
            d->pers = pd[12];
            d->pers_count = pd[13];
            d->start = pm_get_be16(&pd[14]);
            d->flags |= UI_RDM_HAVE_INFO;
        } else if (pid == PM_RDM_PID_DEVICE_LABEL) {
            copy_text(d->label, pd, pdl);
            d->flags |= UI_RDM_HAVE_LABEL;
        } else if (pid == PM_RDM_PID_DMX_PERSONALITY_DESCRIPTION && pdl >= 3 && uid_eq(&d->uid, &R.pers_uid)) {
            uint8_t p = pd[0];
            if (p >= 1 && p <= UI_RDM_PERS_MAX) {
                R.pers[p - 1].footprint = pm_get_be16(&pd[1]);
                copy_text(R.pers[p - 1].name, &pd[3], (uint8_t)(pdl - 3));
                R.pers[p - 1].valid = true;
            }
        }
    }
    finish(UI_RDM_RES_OK);
}

void ui_rdm_tick(uint32_t now)
{
    R.now = now;
    if (R.inflight && now - R.cur_ms > UI_RDM_TIMEOUT_MS) finish(UI_RDM_RES_TIMEOUT);
    if (R.inflight || R.q_n == 0 || !R.send) return;
    R.cur = R.q[0];
    memmove(&R.q[0], &R.q[1], sizeof(req_t) * (size_t)(R.q_n - 1));
    R.q_n--;
    uint8_t buf[PM_RDM_MAX_MSG_LEN];
    uint16_t n = encode(&R.cur, R.tn++, buf);
    R.cur_token = 0x55490000u | (++R.token & 0xFFFFu); /* PM_UI_RDM_TOKEN_TAG: main routes it back */
    R.cur_ms = now;
    R.inflight = true;
    if (R.send(R.ctx, R.cur.port, R.cur_token, buf, n) != 0) finish(UI_RDM_RES_TIMEOUT);
}
