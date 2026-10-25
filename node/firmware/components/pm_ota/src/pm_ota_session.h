/*
 * pm_ota_session.h - one firmware update session (image-format.md §2.3 steps 1-7), pure C11.
 *
 * Platform-independent: the flash side is reached only through pm_ota_backend_t, so the logic runs in the host
 * tests with a RAM fake and on the target with esp_ota (pm_ota.c). Not thread-safe: all calls from one task.
 *
 * Semantics:
 *   - begin(): steps 1-5 (pm_image_desc_check) before any backend call. While a session is RECEIVING and was
 *     active within the last PM_OTA_SESSION_TIMEOUT_MS, a begin with a different announced block is rejected with
 *     SEQUENCE (busy, other source) and the running session is NOT touched. A begin with a byte-identical
 *     announced block restarts the session (lost FW_BEGIN ACK / sender restarted). An idle (> timeout) session
 *     is aborted with TIMEOUT first. A begin in READY (reboot pending) is rejected with SEQUENCE.
 *   - write(): offset == received appends; offset < received is a retransmission and is accepted (only the part
 *     beyond `received` is written); offset > received -> SEQUENCE; offset + len > image_length -> TOO_LARGE.
 *     The block at image offset 288..351 is collected (may be split) and compared with the announced copy as
 *     soon as complete (DESC_MISMATCH). A write while no session is RECEIVING returns SEQUENCE without changing
 *     the state.
 *   - finish(): received == image_length (else SEQUENCE), CRC-32 (CRC), backend end (its error code, normally
 *     IMAGE_INVALID or FLASH) -> READY. finish() in READY returns NONE again (idempotent retry).
 *   - Every error of an active session sets FAILED + last_error and aborts the backend.
 */
#pragma once

#include "pm_fw.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A session without begin/write for this long is dropped (TIMEOUT). image-format.md §2.4/§2.5. */
#define PM_OTA_SESSION_TIMEOUT_MS 30000u

/** Flash backend. All functions are mandatory. */
typedef struct {
    void *ctx;
    /** Open/prepare the inactive partition for `image_len` bytes. Returns NONE or an error (normally FLASH). */
    pm_fw_error_t (*begin)(void *ctx, uint32_t image_len);
    /** Append `len` bytes sequentially. Returns NONE or FLASH. */
    pm_fw_error_t (*write)(void *ctx, const uint8_t *data, size_t len);
    /** Close + validate the image and make it the boot image. Must release the backend even on failure.
     *  Returns NONE, IMAGE_INVALID or FLASH. */
    pm_fw_error_t (*end)(void *ctx);
    /** Drop an open (begun, not ended) backend session. */
    void (*abort)(void *ctx);
    /** Monotonic milliseconds (wrap-around is handled). */
    uint32_t (*now_ms)(void *ctx);
} pm_ota_backend_t;

/** Event callback: state changes and progress in whole-percent steps. Called in the caller's task. */
typedef void (*pm_ota_session_event_fn)(void *ctx, const pm_fw_status_t *st);

/** Session state. Treat as opaque; use the functions below. */
typedef struct {
    const pm_ota_backend_t *be;
    pm_ota_session_event_fn on_event;
    void *event_ctx;
    pm_fw_status_t st;
    pm_image_desc_t announced;
    uint32_t crc;
    uint32_t last_ms;
    uint8_t desc_buf[PM_IMAGE_DESC_SIZE];
    bool desc_checked;
    bool backend_open;
    uint8_t last_pct;
} pm_ota_session_t;

/** Initialise `s` (state IDLE). `on_event` may be NULL. */
void pm_ota_session_init(pm_ota_session_t *s, const pm_ota_backend_t *be, pm_ota_session_event_fn on_event,
                         void *event_ctx);

/** Start a session for `announced` (steps 1-5, then backend begin). `max_len` = inactive partition size. */
pm_fw_error_t pm_ota_session_begin(pm_ota_session_t *s, const pm_image_desc_t *announced, uint16_t own_esta,
                                   uint8_t own_hw_rev, uint32_t max_len);

/** Write image bytes at `offset` (see header comment for retransmission/gap rules). */
pm_fw_error_t pm_ota_session_write(pm_ota_session_t *s, uint32_t offset, const uint8_t *data, size_t len);

/** Step 7: length, CRC-32, backend end. NONE -> state READY. */
pm_fw_error_t pm_ota_session_finish(pm_ota_session_t *s);

/** Drop the session. reason NONE -> IDLE, otherwise FAILED with last_error = reason. No-op unless RECEIVING. */
void pm_ota_session_abort(pm_ota_session_t *s, pm_fw_error_t reason);

/** Drop a RECEIVING session idle for more than PM_OTA_SESSION_TIMEOUT_MS (TIMEOUT). Call periodically. */
void pm_ota_session_poll(pm_ota_session_t *s);

/** Copy the current status. */
void pm_ota_session_status(const pm_ota_session_t *s, pm_fw_status_t *out);

#ifdef __cplusplus
}
#endif
