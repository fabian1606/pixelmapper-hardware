/*
 * Manufacturer-specific RDM PIDs of tube_fw / tube_boot_rdm.
 * Shared by the firmware (C) and the host uploader: flash_rdm.py parses the
 * "#define NAME value" lines of this file - keep them one per line, values
 * as plain hex/decimal literals.
 *
 * App (tube_fw):
 *   0x8000 ENTER_BOOTLOADER  SET  pd "BOOT" -> ACK, RAM magic, watchdog reset
 *                            (not executed when sent to a broadcast UID)
 *
 * Bootloader (tube_boot_rdm, DEVICE_INFO software version BOOT_RDM_SW_VERSION_ID):
 *   0x8010 BOOT_INFO   GET  -> 19 bytes, multi-byte values big-endian:
 *                              0 version (BOOT_RDM_VERSION)  1 page size
 *                              2-3 app space end (0x1800)    4 busy
 *                              5 last error (BOOT_ERR_*, reading clears it)
 *                              6-7 manufacturer ID  8-9 device model ID
 *                              10-13 bootloader sw version ID
 *                              14 installed app trailer valid (0/1)
 *                              15-18 FW_VERSION_ID from that trailer
 *                              (14..18: state of the last check at boot /
 *                              FINISH / idle timeout; WRITE_PAGE clears 14)
 *   0x8011 WRITE_PAGE  SET  pd = page byte address (2, BE) + 64 bytes
 *                           -> ACK: page buffered, erase/write/verify run in
 *                              the background (BOOT_INFO busy = 1 meanwhile)
 *                           -> NACK BUFFER_FULL: previous page still being
 *                              written, request dropped - send it again
 *                           -> NACK DATA_OUT_OF_RANGE: address >= app max or
 *                              not page aligned
 *   0x8012 APP_CRC     GET  pd = address (2, BE) + length (2, BE, 0..256)
 *                           -> CRC16-CCITT (2, BE; 0x1021, init 0xFFFF) of
 *                              flash address..address+length-1. Chunked so the
 *                              answer stays within the E1.20 2 ms limit
 *                              (256 B ~1.05 ms).
 *                              NACK BUFFER_FULL while a page is written.
 *   0x8013 FINISH      SET  -> ACK (NACK BUFFER_FULL if busy); afterwards the
 *                           trailer (boot_shared.h) is checked: valid ->
 *                           watchdog reset into the app, else it stays with
 *                           last error FORMAT (magic/format), MODEL
 *                           (manufacturer/model ID) or APP_INVALID (length/CRC)
 * No ACK_TIMER anywhere: message count is always 0, so QUEUED_MESSAGE /
 * STATUS_MESSAGES are not needed (E1.20 10.3).
 * App: 0x8000 is listed in SUPPORTED_PARAMETERS and described by
 * PARAMETER_DESCRIPTION. Bootloader (v3): its manufacturer PIDs are not listed
 * (E1.20 10.4.1 "may or may not be included"), so it needs no
 * PARAMETER_DESCRIPTION (Table A-3); SUPPORTED_PARAMETERS answers an empty list.
 * Broadcast SET is executed (never answered), broadcast GET is ignored;
 * manufacturer PIDs sent to FFFF:FFFFFFFF are ignored (E1.20 6.2.10.2), only
 * the vendorcast 7FF0:FFFFFFFF reaches them.
 * Rescue: GET BOOT_INFO to the UID within ~200 ms after reset keeps the
 * bootloader active even with a valid app. Without a known UID: any SET of a
 * bootloader PID to the vendorcast 7FF0:FFFFFFFF in that window (e.g. SET
 * 0x8010 -> no effect but "stay") keeps every tube in the bootloader for 60 s.
 */
#pragma once

#define PID_ENTER_BOOTLOADER   0x8000
#define PID_BOOT_INFO          0x8010
#define PID_BOOT_WRITE_PAGE    0x8011
#define PID_BOOT_APP_CRC       0x8012
#define PID_BOOT_FINISH        0x8013

#define BOOT_RDM_VERSION       3       /* = low byte of the SW version ID */
#define BOOT_RDM_SW_VERSION_ID 0xB0000003
#define BOOT_RDM_PROTOCOL      2       /* oldest BOOT_INFO byte 0 with this 19-byte format and these PIDs */
#define BOOT_RDM_PAGE_SIZE     64
#define BOOT_RDM_CRC_MAX_LEN   256     /* APP_CRC chunk limit */

/* BOOT_INFO last error */
#define BOOT_ERR_NONE          0
#define BOOT_ERR_VERIFY        1       /* page content differs after write */
#define BOOT_ERR_APP_INVALID   2       /* trailer length/CRC wrong */
#define BOOT_ERR_MODEL         3       /* trailer manufacturer/model ID not ours */
#define BOOT_ERR_FORMAT        4       /* trailer magic/format unknown (or blank) */
