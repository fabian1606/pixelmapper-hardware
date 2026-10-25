/* vn_fw.c - firmware sink of the virtual node: the real pm_ota_session (image-format.md §2.3 steps 1-7) on a RAM
 * "partition". esp_ota_end() is modelled by the ESP image magic check (0xE9 at offset 0 -> else IMAGE_INVALID). */
#include <string.h>

#include "pm_ota_session.h"
#include "vn.h"

static uint8_t s_part[VN_FW_PARTITION];
static uint32_t s_len;
static bool s_open;
static bool s_fail_begin;
static pm_ota_session_t s_session;
static uint8_t s_hw_rev;
static void (*s_listener)(const pm_fw_status_t *st);
static bool s_reboot;
static pm_image_desc_t s_installed;
static bool s_installed_valid;

static pm_fw_error_t be_begin(void *ctx, uint32_t image_len)
{
    (void)ctx;
    if (s_fail_begin) {
        s_fail_begin = false;
        return PM_FW_ERROR_FLASH;
    }
    if (image_len > sizeof(s_part)) return PM_FW_ERROR_TOO_LARGE;
    s_len = 0;
    s_open = true;
    return PM_FW_ERROR_NONE;
}

static pm_fw_error_t be_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!s_open || s_len + len > sizeof(s_part)) return PM_FW_ERROR_FLASH;
    memcpy(s_part + s_len, data, len);
    s_len += (uint32_t)len;
    return PM_FW_ERROR_NONE;
}

static pm_fw_error_t be_end(void *ctx)
{
    (void)ctx;
    s_open = false;
    if (s_len < PM_IMAGE_DESC_OFFSET + PM_IMAGE_DESC_SIZE || s_part[0] != 0xE9u) return PM_FW_ERROR_IMAGE_INVALID;
    s_installed_valid = pm_image_desc_decode(s_part + PM_IMAGE_DESC_OFFSET, PM_IMAGE_DESC_SIZE, &s_installed) == PM_OK;
    return PM_FW_ERROR_NONE;
}

static void be_abort(void *ctx)
{
    (void)ctx;
    s_open = false;
}

static uint32_t be_now(void *ctx)
{
    (void)ctx;
    return vn_now_ms();
}

static const pm_ota_backend_t s_backend = {
    .ctx = NULL, .begin = be_begin, .write = be_write, .end = be_end, .abort = be_abort, .now_ms = be_now,
};

static void on_event(void *ctx, const pm_fw_status_t *st)
{
    (void)ctx;
    if (s_listener) s_listener(st);
}

static pm_fw_error_t sink_begin(void *ctx, const pm_image_desc_t *a)
{
    (void)ctx;
    return pm_ota_session_begin(&s_session, a, PM_ESTA_MANUFACTURER, s_hw_rev, sizeof(s_part));
}

static pm_fw_error_t sink_write(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    (void)ctx;
    return pm_ota_session_write(&s_session, off, data, len);
}

static pm_fw_error_t sink_finish(void *ctx)
{
    (void)ctx;
    return pm_ota_session_finish(&s_session);
}

static void sink_abort(void *ctx, pm_fw_error_t reason)
{
    (void)ctx;
    pm_ota_session_abort(&s_session, reason);
}

static void sink_status(void *ctx, pm_fw_status_t *out)
{
    (void)ctx;
    pm_ota_session_status(&s_session, out);
}

static void sink_reboot(void *ctx)
{
    (void)ctx;
    s_reboot = true;
}

static const pm_fw_sink_t s_sink = {
    .ctx = NULL, .begin = sink_begin, .write = sink_write, .finish = sink_finish, .abort = sink_abort,
    .status = sink_status, .reboot = sink_reboot,
};

void vn_fw_init(uint8_t hw_rev)
{
    s_hw_rev = hw_rev;
    pm_ota_session_init(&s_session, &s_backend, on_event, NULL);
}

const pm_fw_sink_t *vn_fw_sink(void) { return &s_sink; }

void vn_fw_poll(void) { pm_ota_session_poll(&s_session); }

void vn_fw_set_listener(void (*fn)(const pm_fw_status_t *st)) { s_listener = fn; }

bool vn_fw_take_reboot(pm_image_desc_t *installed, bool *have_image)
{
    if (!s_reboot) return false;
    s_reboot = false;
    *have_image = s_installed_valid;
    if (s_installed_valid) *installed = s_installed;
    s_installed_valid = false;
    pm_ota_session_init(&s_session, &s_backend, on_event, NULL); /* fresh boot: FW_INFO idle */
    return true;
}

void vn_fw_fail_next_begin(bool on) { s_fail_begin = on; }
