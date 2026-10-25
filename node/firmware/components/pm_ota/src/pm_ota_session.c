/* pm_ota_session.c - one firmware update session (image-format.md §2.3 steps 1-7), pure C11. */
#include "pm_ota_session.h"

#include <string.h>

#define DESC_END (PM_IMAGE_DESC_OFFSET + PM_IMAGE_DESC_SIZE)

static void emit(pm_ota_session_t *s)
{
    if (s->on_event != NULL) s->on_event(s->event_ctx, &s->st);
}

static void set_state(pm_ota_session_t *s, pm_fw_state_t state, pm_fw_error_t err)
{
    s->st.state = state;
    s->st.last_error = err;
    emit(s);
}

static void close_backend(pm_ota_session_t *s)
{
    if (s->backend_open) {
        s->backend_open = false;
        s->be->abort(s->be->ctx);
    }
}

/* Error in an active session: abort the backend, FAILED + last_error. */
static pm_fw_error_t fail(pm_ota_session_t *s, pm_fw_error_t err)
{
    close_backend(s);
    set_state(s, PM_FW_STATE_FAILED, err);
    return err;
}

static bool timed_out(const pm_ota_session_t *s)
{
    return (uint32_t)(s->be->now_ms(s->be->ctx) - s->last_ms) > PM_OTA_SESSION_TIMEOUT_MS;
}

void pm_ota_session_init(pm_ota_session_t *s, const pm_ota_backend_t *be, pm_ota_session_event_fn on_event,
                         void *event_ctx)
{
    memset(s, 0, sizeof(*s));
    s->be = be;
    s->on_event = on_event;
    s->event_ctx = event_ctx;
    s->st.state = PM_FW_STATE_IDLE;
    s->st.last_error = PM_FW_ERROR_NONE;
}

pm_fw_error_t pm_ota_session_begin(pm_ota_session_t *s, const pm_image_desc_t *announced, uint16_t own_esta,
                                   uint8_t own_hw_rev, uint32_t max_len)
{
    if (announced == NULL) return PM_FW_ERROR_DESC_MAGIC;

    if (s->st.state == PM_FW_STATE_READY) return PM_FW_ERROR_SEQUENCE; /* reboot into the new image pending */
    if (s->st.state == PM_FW_STATE_RECEIVING) {
        uint8_t a[PM_IMAGE_DESC_SIZE], b[PM_IMAGE_DESC_SIZE];
        pm_image_desc_encode(announced, false, a);
        pm_image_desc_encode(&s->announced, false, b);
        if (timed_out(s)) {
            fail(s, PM_FW_ERROR_TIMEOUT);
        } else if (memcmp(a, b, sizeof(a)) == 0) {
            close_backend(s); /* same image again: restart from offset 0 */
        } else {
            return PM_FW_ERROR_SEQUENCE; /* busy: another source owns the session, leave it alone */
        }
    }

    s->st.received = 0;
    s->st.total = 0;
    pm_fw_error_t err = pm_image_desc_check(announced, own_esta, own_hw_rev, max_len);
    if (err != PM_FW_ERROR_NONE) {
        set_state(s, PM_FW_STATE_FAILED, err);
        return err;
    }

    s->announced = *announced;
    s->crc = 0;
    s->desc_checked = false;
    s->last_pct = 0;
    memset(s->desc_buf, 0, sizeof(s->desc_buf));
    s->st.total = announced->image_length;

    err = s->be->begin(s->be->ctx, announced->image_length);
    if (err != PM_FW_ERROR_NONE) {
        set_state(s, PM_FW_STATE_FAILED, err);
        return err;
    }
    s->backend_open = true;
    s->last_ms = s->be->now_ms(s->be->ctx); /* after begin: the erase may take a while */
    set_state(s, PM_FW_STATE_RECEIVING, PM_FW_ERROR_NONE);
    return PM_FW_ERROR_NONE;
}

pm_fw_error_t pm_ota_session_write(pm_ota_session_t *s, uint32_t offset, const uint8_t *data, size_t len)
{
    if (s->st.state != PM_FW_STATE_RECEIVING) return PM_FW_ERROR_SEQUENCE;
    if (timed_out(s)) return fail(s, PM_FW_ERROR_TIMEOUT);
    if (data == NULL && len != 0) return fail(s, PM_FW_ERROR_SEQUENCE);
    if ((uint64_t)offset + len > s->st.total) return fail(s, PM_FW_ERROR_TOO_LARGE);
    if (offset > s->st.received) return fail(s, PM_FW_ERROR_SEQUENCE);

    s->last_ms = s->be->now_ms(s->be->ctx);
    uint32_t end = offset + (uint32_t)len;
    if (end <= s->st.received) return PM_FW_ERROR_NONE; /* complete retransmission */

    /* New bytes: [received, end). */
    uint32_t start = s->st.received;
    const uint8_t *p = data + (start - offset);
    uint32_t n = end - start;

    /* Collect the in-image block (288..351) and check it as soon as it is complete (step 6). */
    if (!s->desc_checked && start < DESC_END && end > PM_IMAGE_DESC_OFFSET) {
        uint32_t lo = start > PM_IMAGE_DESC_OFFSET ? start : PM_IMAGE_DESC_OFFSET;
        uint32_t hi = end < DESC_END ? end : DESC_END;
        memcpy(s->desc_buf + (lo - PM_IMAGE_DESC_OFFSET), p + (lo - start), hi - lo);
        if (hi == DESC_END) {
            pm_image_desc_t in_image;
            if (pm_image_desc_decode(s->desc_buf, sizeof(s->desc_buf), &in_image) != PM_OK ||
                !pm_image_desc_matches(&in_image, &s->announced)) {
                return fail(s, PM_FW_ERROR_DESC_MISMATCH);
            }
            s->desc_checked = true;
        }
    }

    pm_fw_error_t err = s->be->write(s->be->ctx, p, n);
    if (err != PM_FW_ERROR_NONE) return fail(s, err);
    s->crc = pm_crc32(s->crc, p, n);
    s->st.received = end;

    uint8_t pct = (uint8_t)(((uint64_t)s->st.received * 100u) / s->st.total);
    if (pct != s->last_pct) {
        s->last_pct = pct;
        emit(s);
    }
    return PM_FW_ERROR_NONE;
}

pm_fw_error_t pm_ota_session_finish(pm_ota_session_t *s)
{
    if (s->st.state == PM_FW_STATE_READY) return PM_FW_ERROR_NONE; /* retried FINISH */
    if (s->st.state != PM_FW_STATE_RECEIVING) return PM_FW_ERROR_SEQUENCE;
    if (s->st.received != s->st.total) return fail(s, PM_FW_ERROR_SEQUENCE);
    if (!s->desc_checked) return fail(s, PM_FW_ERROR_DESC_MISMATCH); /* image shorter than 352 bytes */
    if (s->crc != s->announced.image_crc32) return fail(s, PM_FW_ERROR_CRC);

    set_state(s, PM_FW_STATE_VERIFYING, PM_FW_ERROR_NONE);
    s->backend_open = false; /* end() releases the backend in any case */
    pm_fw_error_t err = s->be->end(s->be->ctx);
    if (err != PM_FW_ERROR_NONE) return fail(s, err);
    set_state(s, PM_FW_STATE_READY, PM_FW_ERROR_NONE);
    return PM_FW_ERROR_NONE;
}

void pm_ota_session_abort(pm_ota_session_t *s, pm_fw_error_t reason)
{
    if (s->st.state != PM_FW_STATE_RECEIVING) return;
    close_backend(s);
    set_state(s, reason == PM_FW_ERROR_NONE ? PM_FW_STATE_IDLE : PM_FW_STATE_FAILED, reason);
}

void pm_ota_session_poll(pm_ota_session_t *s)
{
    if (s->st.state == PM_FW_STATE_RECEIVING && timed_out(s)) fail(s, PM_FW_ERROR_TIMEOUT);
}

void pm_ota_session_status(const pm_ota_session_t *s, pm_fw_status_t *out) { *out = s->st; }
