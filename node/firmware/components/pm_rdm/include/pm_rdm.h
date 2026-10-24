/*
 * pm_rdm.h - ANSI E1.20 RDM message codec and constants (pure C11, host-testable).
 *
 * Reference: protocol/rdm-digest.md (sections in brackets). Vectors: protocol/vectors/rdm/.
 * Wire format = start code 0xCC first. ArtRdm carries the message WITHOUT the start code: use the *_nosc variants
 * or pass buf+1.
 */
#pragma once

#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------------------------ constants [§1, §4] */

#define PM_RDM_START_CODE 0xCCu
#define PM_RDM_SUB_START_CODE 0x01u
#define PM_RDM_HEADER_LEN 24u        /**< start code .. PDL */
#define PM_RDM_MAX_PDL 231u
#define PM_RDM_MAX_MSG_LEN (PM_RDM_HEADER_LEN + PM_RDM_MAX_PDL + 2u) /**< 257 incl. checksum */
#define PM_RDM_DUB_RESPONSE_LEN 24u  /**< 7 preamble + separator + 12 EUID + 4 ECS */

/** Command classes */
enum {
    PM_RDM_CC_DISCOVERY = 0x10,
    PM_RDM_CC_DISCOVERY_RESPONSE = 0x11,
    PM_RDM_CC_GET = 0x20,
    PM_RDM_CC_GET_RESPONSE = 0x21,
    PM_RDM_CC_SET = 0x30,
    PM_RDM_CC_SET_RESPONSE = 0x31,
};

/** Response types (in the port-ID slot of responses) */
enum {
    PM_RDM_RESP_ACK = 0x00,
    PM_RDM_RESP_ACK_TIMER = 0x01,
    PM_RDM_RESP_NACK_REASON = 0x02,
    PM_RDM_RESP_ACK_OVERFLOW = 0x03,
};

/** NACK reason codes [§4.2] */
enum {
    PM_NR_UNKNOWN_PID = 0x0000,
    PM_NR_FORMAT_ERROR = 0x0001,
    PM_NR_HARDWARE_FAULT = 0x0002,
    PM_NR_PROXY_REJECT = 0x0003,
    PM_NR_WRITE_PROTECT = 0x0004,
    PM_NR_UNSUPPORTED_COMMAND_CLASS = 0x0005,
    PM_NR_DATA_OUT_OF_RANGE = 0x0006,
    PM_NR_BUFFER_FULL = 0x0007,
    PM_NR_PACKET_SIZE_UNSUPPORTED = 0x0008,
    PM_NR_SUB_DEVICE_OUT_OF_RANGE = 0x0009,
    PM_NR_PROXY_BUFFER_FULL = 0x000A,
};

/** Status types (STATUS_MESSAGES, QUEUED_MESSAGE request) */
enum {
    PM_RDM_STATUS_NONE = 0x00,
    PM_RDM_STATUS_GET_LAST_MESSAGE = 0x01,
    PM_RDM_STATUS_ADVISORY = 0x02,
    PM_RDM_STATUS_WARNING = 0x03,
    PM_RDM_STATUS_ERROR = 0x04,
    PM_RDM_STATUS_ADVISORY_CLEARED = 0x12,
    PM_RDM_STATUS_WARNING_CLEARED = 0x13,
    PM_RDM_STATUS_ERROR_CLEARED = 0x14,
};

/** DISC_MUTE / DISC_UN_MUTE control field bits [§3.2] */
enum {
    PM_RDM_MUTE_MANAGED_PROXY = 0x0001,
    PM_RDM_MUTE_SUB_DEVICE = 0x0002,
    PM_RDM_MUTE_BOOT_LOADER = 0x0004,
    PM_RDM_MUTE_PROXIED_DEVICE = 0x0008,
};

/** Standard PIDs used by the node (controller + responder). Manufacturer PIDs: pm_pids.h. */
enum {
    PM_RDM_PID_DISC_UNIQUE_BRANCH = 0x0001,
    PM_RDM_PID_DISC_MUTE = 0x0002,
    PM_RDM_PID_DISC_UN_MUTE = 0x0003,
    PM_RDM_PID_QUEUED_MESSAGE = 0x0020,
    PM_RDM_PID_STATUS_MESSAGES = 0x0030,
    PM_RDM_PID_STATUS_ID_DESCRIPTION = 0x0031,
    PM_RDM_PID_CLEAR_STATUS_ID = 0x0032,
    PM_RDM_PID_SUPPORTED_PARAMETERS = 0x0050,
    PM_RDM_PID_PARAMETER_DESCRIPTION = 0x0051,
    PM_RDM_PID_DEVICE_INFO = 0x0060,
    PM_RDM_PID_PRODUCT_DETAIL_ID_LIST = 0x0070,
    PM_RDM_PID_DEVICE_MODEL_DESCRIPTION = 0x0080,
    PM_RDM_PID_MANUFACTURER_LABEL = 0x0081,
    PM_RDM_PID_DEVICE_LABEL = 0x0082,
    PM_RDM_PID_FACTORY_DEFAULTS = 0x0090,
    PM_RDM_PID_SOFTWARE_VERSION_LABEL = 0x00C0,
    PM_RDM_PID_BOOT_SOFTWARE_VERSION_ID = 0x00C1,
    PM_RDM_PID_BOOT_SOFTWARE_VERSION_LABEL = 0x00C2,
    PM_RDM_PID_DMX_PERSONALITY = 0x00E0,
    PM_RDM_PID_DMX_PERSONALITY_DESCRIPTION = 0x00E1,
    PM_RDM_PID_DMX_START_ADDRESS = 0x00F0,
    PM_RDM_PID_DISPLAY_LEVEL = 0x0501,
    PM_RDM_PID_IDENTIFY_DEVICE = 0x1000,
    PM_RDM_PID_RESET_DEVICE = 0x1001,
    PM_RDM_PID_LIST_INTERFACES = 0x0700,
    PM_RDM_PID_INTERFACE_LABEL = 0x0701,
    PM_RDM_PID_INTERFACE_HARDWARE_ADDRESS_TYPE1 = 0x0702,
    PM_RDM_PID_IPV4_DHCP_MODE = 0x0703,
    PM_RDM_PID_IPV4_CURRENT_ADDRESS = 0x0705,
    PM_RDM_PID_IPV4_STATIC_ADDRESS = 0x0706,
    PM_RDM_PID_INTERFACE_RENEW_DHCP = 0x0707,
    PM_RDM_PID_INTERFACE_APPLY_CONFIGURATION = 0x0709,
    PM_RDM_PID_IPV4_DEFAULT_ROUTE = 0x070A,
};

/* ------------------------------------------------------------------------------------------------ message */

/** Decoded RDM message. `pd` points into the decoded buffer (decode) or caller memory (encode). */
typedef struct {
    pm_uid_t dest;
    pm_uid_t src;
    uint8_t tn;                  /**< transaction number */
    uint8_t port_or_resp;        /**< requests: port ID (1..), responses: response type */
    uint8_t message_count;
    uint16_t sub_device;
    uint8_t cc;
    uint16_t pid;
    uint8_t pdl;
    const uint8_t *pd;
} pm_rdm_msg_t;

/** 16-bit additive checksum over `len` bytes. */
uint16_t pm_rdm_checksum(const uint8_t *data, size_t len);

/** Decode wire bytes (start code first). Checks start/sub-start code, message length, PDL <= 231, checksum.
 *  Trailing bytes after the checksum are ignored. */
pm_err_t pm_rdm_decode(const uint8_t *buf, size_t len, pm_rdm_msg_t *out);
/** Same for a message without start code (ArtRdm payload). */
pm_err_t pm_rdm_decode_nosc(const uint8_t *buf, size_t len, pm_rdm_msg_t *out);

/** Encode with start code and checksum. Returns length or negative pm_err_t. */
int pm_rdm_encode(const pm_rdm_msg_t *msg, uint8_t *buf, size_t cap);
/** Encode without start code (for ArtRdm). The checksum still covers the start code (E1.20 / Art-Net). */
int pm_rdm_encode_nosc(const pm_rdm_msg_t *msg, uint8_t *buf, size_t cap);

/** Build a response skeleton for `req` (dest/src swapped, same TN, CC + 1, same sub-device and PID). */
void pm_rdm_make_response(const pm_rdm_msg_t *req, const pm_uid_t *self, uint8_t response_type,
                          uint8_t message_count, pm_rdm_msg_t *resp);

/* ------------------------------------------------------------------------------------------------ discovery [§3] */

/** Encode a DISC_UNIQUE_BRANCH response for `uid` (preamble_len 0..7 bytes of 0xFE). Returns length. */
int pm_rdm_dub_encode(const pm_uid_t *uid, unsigned preamble_len, uint8_t *buf, size_t cap);

/** Decode a DUB response: skips up to 7 preamble bytes 0xFE, needs separator 0xAA, decodes EUID and checks ECS.
 *  PM_ERR_BAD_CHECKSUM = collision or noise (caller splits the branch). */
pm_err_t pm_rdm_dub_decode(const uint8_t *buf, size_t len, pm_uid_t *out);

#ifdef __cplusplus
}
#endif
