/* test_ota_session.c - host tests for pm_ota_session (image-format.md §2.3) with a RAM fake backend. */
#include <stdint.h>
#include <string.h>

#include "pm_ota_session.h"
#include "test.h"

void test_ota_session(void);

#define IMG_LEN 4096u
#define HW_REV 0u
#define MAX_LEN 8192u

/* ------------------------------------------------------------------------------------------------ fake backend */

typedef struct {
    uint8_t flash[MAX_LEN];
    uint32_t len;
    uint32_t now;
    int begins, ends, aborts;
    bool open;
    pm_fw_error_t begin_err, write_err, end_err;
} fake_t;

static pm_fw_error_t fk_begin(void *ctx, uint32_t image_len)
{
    fake_t *f = ctx;
    f->begins++;
    if (f->begin_err != PM_FW_ERROR_NONE) return f->begin_err;
    (void)image_len;
    f->len = 0;
    f->open = true;
    return PM_FW_ERROR_NONE;
}

static pm_fw_error_t fk_write(void *ctx, const uint8_t *data, size_t len)
{
    fake_t *f = ctx;
    if (!f->open) return PM_FW_ERROR_FLASH;
    if (f->write_err != PM_FW_ERROR_NONE) return f->write_err;
    if (f->len + len > sizeof(f->flash)) return PM_FW_ERROR_FLASH;
    memcpy(f->flash + f->len, data, len);
    f->len += (uint32_t)len;
    return PM_FW_ERROR_NONE;
}

static pm_fw_error_t fk_end(void *ctx)
{
    fake_t *f = ctx;
    f->ends++;
    f->open = false;
    return f->end_err;
}

static void fk_abort(void *ctx)
{
    fake_t *f = ctx;
    f->aborts++;
    f->open = false;
}

static uint32_t fk_now(void *ctx) { return ((fake_t *)ctx)->now; }

/* ------------------------------------------------------------------------------------------------ events */

typedef struct {
    int count;
    int progress; /* events while RECEIVING */
    pm_fw_status_t last;
} events_t;

static void on_event(void *ctx, const pm_fw_status_t *st)
{
    events_t *e = ctx;
    e->count++;
    if (st->state == PM_FW_STATE_RECEIVING) e->progress++;
    e->last = *st;
}

/* ------------------------------------------------------------------------------------------------ fixtures */

static uint8_t s_img[IMG_LEN];
static fake_t s_fake;
static events_t s_ev;
static pm_ota_backend_t s_be;
static pm_ota_session_t s_s;

static pm_image_desc_t base_desc(void)
{
    pm_image_desc_t d;
    memset(&d, 0, sizeof(d));
    d.magic = PM_NODE_IMAGE_MAGIC;
    d.format = PM_NODE_IMAGE_FORMAT;
    d.size = PM_IMAGE_DESC_SIZE;
    d.esta_manufacturer = PM_ESTA_MANUFACTURER;
    d.device_model_id = PM_MODEL_NODE;
    d.fw_version_id = 0x00010203u;
    d.hw_rev_mask = 1u << HW_REV;
    strcpy(d.version_label, "1.2.3");
    return d;
}

/* Re-encode an announced copy so desc_crc32 is valid (after modifying fields). */
static pm_image_desc_t sealed(pm_image_desc_t d)
{
    uint8_t raw[PM_IMAGE_DESC_SIZE];
    pm_image_desc_encode(&d, true, raw);
    pm_image_desc_decode(raw, sizeof(raw), &d);
    return d;
}

/* Image with the in-image block (length/crc/desc_crc 0) at 288, and the matching announced copy. */
static pm_image_desc_t make_image(void)
{
    for (uint32_t i = 0; i < IMG_LEN; i++) s_img[i] = (uint8_t)(i * 7u + 3u);
    pm_image_desc_t in_image = base_desc();
    pm_image_desc_encode(&in_image, false, s_img + PM_IMAGE_DESC_OFFSET);
    pm_image_desc_t a = in_image;
    a.image_length = IMG_LEN;
    a.image_crc32 = pm_crc32(0, s_img, IMG_LEN);
    return sealed(a);
}

static void setup(void)
{
    memset(&s_fake, 0, sizeof(s_fake));
    memset(&s_ev, 0, sizeof(s_ev));
    s_be = (pm_ota_backend_t){.ctx = &s_fake, .begin = fk_begin, .write = fk_write, .end = fk_end,
                              .abort = fk_abort, .now_ms = fk_now};
    pm_ota_session_init(&s_s, &s_be, on_event, &s_ev);
}

static pm_fw_error_t begin(const pm_image_desc_t *a)
{
    return pm_ota_session_begin(&s_s, a, PM_ESTA_MANUFACTURER, HW_REV, MAX_LEN);
}

/* Write image bytes [from, to) in chunks of `chunk`. */
static pm_fw_error_t send(uint32_t from, uint32_t to, uint32_t chunk)
{
    for (uint32_t o = from; o < to; o += chunk) {
        uint32_t n = to - o < chunk ? to - o : chunk;
        pm_fw_error_t e = pm_ota_session_write(&s_s, o, s_img + o, n);
        if (e != PM_FW_ERROR_NONE) return e;
    }
    return PM_FW_ERROR_NONE;
}

static void check_failed(pm_fw_error_t err)
{
    pm_fw_status_t st;
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.state, PM_FW_STATE_FAILED);
    CHECK_EQ(st.last_error, err);
}

/* ------------------------------------------------------------------------------------------------ tests */

static void t_desc_checks(void)
{
    g_context = "desc checks 1-5";
    pm_image_desc_t good = make_image();
    struct {
        pm_image_desc_t d;
        pm_fw_error_t err;
    } cases[7];
    int n = 0;
    cases[n].d = good, cases[n].d.magic ^= 1u, cases[n].d = sealed(cases[n].d), cases[n++].err = PM_FW_ERROR_DESC_MAGIC;
    cases[n].d = good, cases[n].d.desc_crc32 ^= 1u, cases[n++].err = PM_FW_ERROR_DESC_MAGIC;
    cases[n].d = good, cases[n].d.esta_manufacturer = 0x1234, cases[n].d = sealed(cases[n].d),
    cases[n++].err = PM_FW_ERROR_DESC_MANUFACTURER;
    cases[n].d = good, cases[n].d.device_model_id = PM_MODEL_TUBE, cases[n].d = sealed(cases[n].d),
    cases[n++].err = PM_FW_ERROR_DESC_MODEL;
    cases[n].d = good, cases[n].d.hw_rev_mask = 1u << 3, cases[n].d = sealed(cases[n].d),
    cases[n++].err = PM_FW_ERROR_DESC_HW_REV;
    cases[n].d = good, cases[n].d.image_length = MAX_LEN + 1, cases[n].d = sealed(cases[n].d),
    cases[n++].err = PM_FW_ERROR_TOO_LARGE;
    cases[n].d = good, cases[n].d.image_length = 0, cases[n].d = sealed(cases[n].d),
    cases[n++].err = PM_FW_ERROR_TOO_LARGE;
    for (int i = 0; i < n; i++) {
        setup();
        CHECK_EQ(begin(&cases[i].d), cases[i].err);
        CHECK_EQ(s_fake.begins, 0); /* no backend call before checks 1-5 pass */
        check_failed(cases[i].err);
    }
    setup();
    s_fake.begin_err = PM_FW_ERROR_FLASH;
    CHECK_EQ(begin(&good), PM_FW_ERROR_FLASH);
    check_failed(PM_FW_ERROR_FLASH);
    CHECK_EQ(pm_ota_session_write(&s_s, 0, s_img, 16), PM_FW_ERROR_SEQUENCE);
}

static void t_success(void)
{
    g_context = "success path + progress";
    setup();
    pm_image_desc_t a = make_image();
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_SEQUENCE); /* no session */
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(s_ev.count, 1);
    CHECK_EQ(s_ev.last.state, PM_FW_STATE_RECEIVING);
    CHECK_EQ(s_ev.last.total, IMG_LEN);
    CHECK_EQ(send(0, IMG_LEN, 7), PM_FW_ERROR_NONE); /* odd chunk size: split desc block */
    CHECK_EQ(s_ev.progress, 1 + 100);                /* begin + one per whole percent */
    pm_fw_status_t st;
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.received, IMG_LEN);
    CHECK_EQ(st.state, PM_FW_STATE_RECEIVING);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_NONE);
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.state, PM_FW_STATE_READY);
    CHECK_EQ(st.last_error, PM_FW_ERROR_NONE);
    CHECK_EQ(s_ev.last.state, PM_FW_STATE_READY);
    CHECK_EQ(s_fake.ends, 1);
    CHECK_EQ(s_fake.aborts, 0);
    CHECK_EQ(s_fake.len, IMG_LEN);
    CHECK(memcmp(s_fake.flash, s_img, IMG_LEN) == 0);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_NONE); /* retried finish */
    CHECK_EQ(s_fake.ends, 1);
    CHECK_EQ(begin(&a), PM_FW_ERROR_SEQUENCE); /* reboot pending */
    CHECK_EQ(pm_ota_session_write(&s_s, 0, s_img, 1), PM_FW_ERROR_SEQUENCE);
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.state, PM_FW_STATE_READY);
}

static void t_retransmission(void)
{
    g_context = "retransmission / overlap / gap";
    setup();
    pm_image_desc_t a = make_image();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, 224, 224), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_write(&s_s, 0, s_img, 224), PM_FW_ERROR_NONE);   /* full repeat: ACK */
    CHECK_EQ(pm_ota_session_write(&s_s, 100, s_img + 100, 50), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_write(&s_s, 200, s_img + 200, 200), PM_FW_ERROR_NONE); /* overlap: 224..399 new */
    CHECK_EQ(s_fake.len, 400);
    CHECK_EQ(pm_ota_session_write(&s_s, 400, s_img + 400, 0), PM_FW_ERROR_NONE); /* empty */
    CHECK_EQ(send(400, IMG_LEN, 224), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_NONE);
    CHECK(memcmp(s_fake.flash, s_img, IMG_LEN) == 0);

    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, 224, 224), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_write(&s_s, 448, s_img + 448, 224), PM_FW_ERROR_SEQUENCE); /* gap */
    check_failed(PM_FW_ERROR_SEQUENCE);
    CHECK_EQ(s_fake.aborts, 1);

    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, IMG_LEN - 10, 512), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_write(&s_s, IMG_LEN - 10, s_img, 11), PM_FW_ERROR_TOO_LARGE); /* beyond end */
    check_failed(PM_FW_ERROR_TOO_LARGE);
    CHECK_EQ(s_fake.aborts, 1);
}

static void t_desc_in_image(void)
{
    g_context = "in-image desc at 288";
    pm_image_desc_t a = make_image();
    /* split exactly inside the block: 0..300, 300..340, 340..end */
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, 300, 300), PM_FW_ERROR_NONE);
    CHECK_EQ(send(300, 340, 40), PM_FW_ERROR_NONE);
    CHECK_EQ(send(340, IMG_LEN, 1024), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_NONE);

    /* in-image block differs in the version label -> mismatch as soon as byte 351 arrives */
    s_img[PM_IMAGE_DESC_OFFSET + 28] = 'X';
    a.image_crc32 = pm_crc32(0, s_img, IMG_LEN);
    a = sealed(a);
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, 351, 351), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_write(&s_s, 351, s_img + 351, 1), PM_FW_ERROR_DESC_MISMATCH);
    check_failed(PM_FW_ERROR_DESC_MISMATCH);
    CHECK_EQ(s_fake.aborts, 1);
    CHECK_EQ(s_fake.len, 351); /* the mismatching chunk is not written */

    /* differences in bytes 20..27 / 60..63 of the in-image block are ignored */
    a = make_image();
    s_img[PM_IMAGE_DESC_OFFSET + 20] = 0x55;
    s_img[PM_IMAGE_DESC_OFFSET + 62] = 0x66;
    a.image_crc32 = pm_crc32(0, s_img, IMG_LEN);
    a = sealed(a);
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, IMG_LEN, 64), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_NONE);

    /* image too short to contain the block */
    a = make_image();
    a.image_length = 300;
    a.image_crc32 = pm_crc32(0, s_img, 300);
    a = sealed(a);
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, 300, 100), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_DESC_MISMATCH);
}

static void t_finish_errors(void)
{
    g_context = "finish errors";
    pm_image_desc_t a = make_image();
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, IMG_LEN - 1, 1000), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_SEQUENCE); /* short */
    check_failed(PM_FW_ERROR_SEQUENCE);
    CHECK_EQ(s_fake.aborts, 1);
    CHECK_EQ(s_fake.ends, 0);

    pm_image_desc_t bad = a;
    bad.image_crc32 ^= 0x80000000u;
    bad = sealed(bad);
    setup();
    CHECK_EQ(begin(&bad), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, IMG_LEN, 1000), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_CRC);
    check_failed(PM_FW_ERROR_CRC);
    CHECK_EQ(s_fake.aborts, 1);
    CHECK_EQ(s_fake.ends, 0);

    setup();
    s_fake.end_err = PM_FW_ERROR_IMAGE_INVALID;
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, IMG_LEN, 1000), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_IMAGE_INVALID);
    check_failed(PM_FW_ERROR_IMAGE_INVALID);
    CHECK_EQ(s_fake.ends, 1);
    CHECK_EQ(s_fake.aborts, 0); /* end() released the backend itself */

    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    s_fake.write_err = PM_FW_ERROR_FLASH;
    CHECK_EQ(pm_ota_session_write(&s_s, 0, s_img, 10), PM_FW_ERROR_FLASH);
    check_failed(PM_FW_ERROR_FLASH);
    CHECK_EQ(s_fake.aborts, 1);
}

static void t_busy_timeout(void)
{
    g_context = "busy / timeout / abort";
    pm_image_desc_t a = make_image();
    pm_image_desc_t other = a;
    other.fw_version_id = 0x00090000u;
    other = sealed(other);

    setup();
    s_fake.now = 0xFFFFF000u; /* wrap-around during the session */
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, 1000, 500), PM_FW_ERROR_NONE);
    s_fake.now += PM_OTA_SESSION_TIMEOUT_MS; /* exactly 30 s: still active */
    CHECK_EQ(begin(&other), PM_FW_ERROR_SEQUENCE);
    pm_fw_status_t st;
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.state, PM_FW_STATE_RECEIVING); /* running session untouched */
    CHECK_EQ(st.received, 1000);
    CHECK_EQ(s_fake.begins, 1);
    CHECK_EQ(send(1000, 2000, 500), PM_FW_ERROR_NONE); /* refreshes activity */
    s_fake.now += PM_OTA_SESSION_TIMEOUT_MS + 1;
    CHECK_EQ(begin(&other), PM_FW_ERROR_NONE); /* idle > 30 s: taken over */
    CHECK_EQ(s_fake.aborts, 1);
    CHECK_EQ(s_fake.begins, 2);
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.state, PM_FW_STATE_RECEIVING);
    CHECK_EQ(st.received, 0);

    /* same announced block again: restart from 0 */
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(send(0, 1000, 500), PM_FW_ERROR_NONE);
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    CHECK_EQ(s_fake.aborts, 1);
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.received, 0);
    CHECK_EQ(send(0, IMG_LEN, 1024), PM_FW_ERROR_NONE);
    CHECK_EQ(pm_ota_session_finish(&s_s), PM_FW_ERROR_NONE);

    /* write after timeout, poll */
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    s_fake.now += PM_OTA_SESSION_TIMEOUT_MS + 1;
    CHECK_EQ(pm_ota_session_write(&s_s, 0, s_img, 10), PM_FW_ERROR_TIMEOUT);
    check_failed(PM_FW_ERROR_TIMEOUT);
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    pm_ota_session_poll(&s_s);
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.state, PM_FW_STATE_RECEIVING);
    s_fake.now += PM_OTA_SESSION_TIMEOUT_MS + 1;
    pm_ota_session_poll(&s_s);
    check_failed(PM_FW_ERROR_TIMEOUT);
    CHECK_EQ(s_fake.aborts, 1);
    CHECK_EQ(s_ev.last.state, PM_FW_STATE_FAILED);

    /* explicit abort */
    setup();
    CHECK_EQ(begin(&a), PM_FW_ERROR_NONE);
    pm_ota_session_abort(&s_s, PM_FW_ERROR_NONE);
    pm_ota_session_status(&s_s, &st);
    CHECK_EQ(st.state, PM_FW_STATE_IDLE);
    CHECK_EQ(s_fake.aborts, 1);
    CHECK_EQ(begin(&other), PM_FW_ERROR_NONE); /* free again */
    pm_ota_session_abort(&s_s, PM_FW_ERROR_TIMEOUT);
    check_failed(PM_FW_ERROR_TIMEOUT);
    pm_ota_session_abort(&s_s, PM_FW_ERROR_NONE); /* no session: no-op */
    CHECK_EQ(s_fake.aborts, 2);
    check_failed(PM_FW_ERROR_TIMEOUT);
}

void test_ota_session(void)
{
    t_desc_checks();
    t_success();
    t_retransmission();
    t_desc_in_image();
    t_finish_errors();
    t_busy_timeout();
    g_context = NULL;
}
