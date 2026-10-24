/*
 * pm_artnet_packets.h - Art-Net 4 packet codec (pure C11, no ESP-IDF, host-testable).
 *
 * Byte layouts: protocol/artnet4-digest.md (Art-Net 4 rev 1.4dp). Test vectors: protocol/vectors/artnet/.
 * Conventions:
 *  - decode functions take the complete UDP payload, check ID + OpCode + minimum length (+ ProtVer >= 14 where the
 *    packet has one) and return PM_ERR_SHORT / PM_ERR_BAD_ID / PM_ERR_BAD_FORMAT. Extra trailing bytes are ignored
 *    (spec: only a minimum length is checked).
 *  - encode functions write into `buf` (capacity `cap`) and return the packet length (> 0) or a negative pm_err_t.
 *  - all multi-byte struct fields are host order; IPv4 addresses are uint32_t host order (2.0.0.10 = 0x0200000A).
 *  - strings are NUL-terminated C strings, truncated to the field size on encode.
 */
#pragma once

#include "pm_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PM_ARTNET_PORT 6454u
#define PM_ARTNET_PROT_VER 14u
#define PM_ARTNET_HEADER_LEN 12u /* ID[8] + OpCode + ProtVer (packets that have ProtVer) */
#define PM_ARTNET_MAX_PACKET 1100u /* ArtFirmwareMaster is the largest packet we handle (1064) */

/** OpCodes (digest §1). */
typedef enum {
    PM_OP_POLL = 0x2000,
    PM_OP_POLL_REPLY = 0x2100,
    PM_OP_DIAG_DATA = 0x2300,
    PM_OP_COMMAND = 0x2400,
    PM_OP_DATA_REQUEST = 0x2700,
    PM_OP_DATA_REPLY = 0x2800,
    PM_OP_DMX = 0x5000,
    PM_OP_NZS = 0x5100,
    PM_OP_SYNC = 0x5200,
    PM_OP_ADDRESS = 0x6000,
    PM_OP_INPUT = 0x7000,
    PM_OP_TOD_REQUEST = 0x8000,
    PM_OP_TOD_DATA = 0x8100,
    PM_OP_TOD_CONTROL = 0x8200,
    PM_OP_RDM = 0x8300,
    PM_OP_RDM_SUB = 0x8400,
    PM_OP_TIME_CODE = 0x9700,
    PM_OP_TIME_SYNC = 0x9800,
    PM_OP_TRIGGER = 0x9900,
    PM_OP_DIRECTORY = 0x9A00,
    PM_OP_DIRECTORY_REPLY = 0x9B00,
    PM_OP_FIRMWARE_MASTER = 0xF200,
    PM_OP_FIRMWARE_REPLY = 0xF300,
    PM_OP_IP_PROG = 0xF800,
    PM_OP_IP_PROG_REPLY = 0xF900,
} pm_artnet_opcode_t;

/** Check "Art-Net\0" and return the OpCode (LE) of any Art-Net packet. len >= 10 required. */
pm_err_t pm_artnet_peek_opcode(const uint8_t *pkt, size_t len, uint16_t *opcode);

/* ------------------------------------------------------------------------------------------------ ArtPoll */

/** ArtPoll Flags bits (digest §2.1). */
enum {
    PM_POLL_FLAG_REPLY_ON_CHANGE = 1u << 1,
    PM_POLL_FLAG_DIAG_ENABLE = 1u << 2,
    PM_POLL_FLAG_DIAG_UNICAST = 1u << 3,
    PM_POLL_FLAG_VLC_DISABLE = 1u << 4,
    PM_POLL_FLAG_TARGETED = 1u << 5,
};

typedef struct {
    uint8_t flags;
    uint8_t diag_priority;
    pm_port_address_t target_top;    /**< only valid with PM_POLL_FLAG_TARGETED (and len >= 18) */
    pm_port_address_t target_bottom;
    uint16_t esta_man;               /**< 0 when the packet is shorter (min. length 14) */
    uint16_t oem;
} pm_artpoll_t;

pm_err_t pm_artpoll_decode(const uint8_t *pkt, size_t len, pm_artpoll_t *out);
int pm_artpoll_encode(const pm_artpoll_t *in, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ ArtPollReply */

#define PM_POLL_REPLY_LEN 239u
#define PM_POLL_REPLY_MIN_LEN 207u

typedef struct {
    uint32_t ip;
    uint16_t vers_info;
    uint8_t net_switch;              /**< bits 14-8 of the Port-Address in bits 6-0 */
    uint8_t sub_switch;              /**< bits 7-4 in bits 3-0 */
    uint16_t oem;
    uint8_t ubea_version;
    uint8_t status1;
    uint16_t esta_man;               /**< encoded LO byte first (spec) */
    char short_name[18];
    char long_name[64];
    char node_report[64];
    uint16_t num_ports;
    uint8_t port_types[4];
    uint8_t good_input[4];
    uint8_t good_output_a[4];
    uint8_t sw_in[4];
    uint8_t sw_out[4];
    uint8_t acn_priority;
    uint8_t sw_macro;
    uint8_t sw_remote;
    uint8_t style;
    uint8_t mac[6];
    uint32_t bind_ip;
    uint8_t bind_index;
    uint8_t status2;
    uint8_t good_output_b[4];
    uint8_t status3;
    pm_uid_t default_resp_uid;
    uint16_t user;
    uint16_t refresh_rate;
    uint8_t background_queue_policy;
} pm_artpollreply_t;

pm_err_t pm_artpollreply_decode(const uint8_t *pkt, size_t len, pm_artpollreply_t *out);
int pm_artpollreply_encode(const pm_artpollreply_t *in, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ ArtDmx / ArtSync */

typedef struct {
    uint8_t sequence;                /**< 0 = disabled */
    uint8_t physical;
    pm_port_address_t port_address;
    uint16_t length;                 /**< 2..512 (decode accepts 1..512, odd lengths are tolerated) */
    const uint8_t *data;             /**< points into the decoded packet (decode) / caller buffer (encode) */
} pm_artdmx_t;

pm_err_t pm_artdmx_decode(const uint8_t *pkt, size_t len, pm_artdmx_t *out);
int pm_artdmx_encode(const pm_artdmx_t *in, uint8_t *buf, size_t cap);

pm_err_t pm_artsync_decode(const uint8_t *pkt, size_t len);
int pm_artsync_encode(uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ ArtAddress */

/** ArtAddress Command values (digest §4.1). Port-specific commands: base + port (only port 0 in Art-Net 4,
 *  the port is selected by BindIndex). */
enum {
    PM_AC_NONE = 0x00,
    PM_AC_CANCEL_MERGE = 0x01,
    PM_AC_LED_NORMAL = 0x02,
    PM_AC_LED_MUTE = 0x03,
    PM_AC_LED_LOCATE = 0x04,
    PM_AC_RESET_RX_FLAGS = 0x05,
    PM_AC_ANALYSIS_ON = 0x06,
    PM_AC_ANALYSIS_OFF = 0x07,
    PM_AC_FAIL_HOLD = 0x08,
    PM_AC_FAIL_ZERO = 0x09,
    PM_AC_FAIL_FULL = 0x0A,
    PM_AC_FAIL_SCENE = 0x0B,
    PM_AC_FAIL_RECORD = 0x0C,
    PM_AC_MERGE_LTP0 = 0x10,
    PM_AC_DIRECTION_TX0 = 0x20,
    PM_AC_DIRECTION_RX0 = 0x30,
    PM_AC_MERGE_HTP0 = 0x50,
    PM_AC_ARTNET_SEL0 = 0x60,
    PM_AC_ACN_SEL0 = 0x70,
    PM_AC_CLEAR_OP0 = 0x90,
    PM_AC_STYLE_DELTA0 = 0xA0,
    PM_AC_STYLE_CONST0 = 0xB0,
    PM_AC_RDM_ENABLE0 = 0xC0,
    PM_AC_RDM_DISABLE0 = 0xD0,
    PM_AC_BQP0 = 0xE0,
};

/** Raw ArtAddress fields; interpretation (bit 7 = program, 0x00 = reset, 0x7F = no change) is node logic. */
typedef struct {
    uint8_t net_switch;
    uint8_t bind_index;
    char short_name[18];             /**< "" = no change */
    char long_name[64];
    uint8_t sw_in[4];
    uint8_t sw_out[4];
    uint8_t sub_switch;
    uint8_t acn_priority;            /**< 255 = no change */
    uint8_t command;
} pm_artaddress_t;

pm_err_t pm_artaddress_decode(const uint8_t *pkt, size_t len, pm_artaddress_t *out);
int pm_artaddress_encode(const pm_artaddress_t *in, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ ArtInput */

typedef struct {
    uint8_t bind_index;
    uint16_t num_ports;
    uint8_t input[4];                /**< bit 0 = disable this input */
} pm_artinput_t;

pm_err_t pm_artinput_decode(const uint8_t *pkt, size_t len, pm_artinput_t *out);
int pm_artinput_encode(const pm_artinput_t *in, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ ArtIpProg(Reply) */

/** ArtIpProg Command bits (digest §4.3). */
enum {
    PM_IPPROG_PROG_PORT = 1u << 0,   /**< deprecated */
    PM_IPPROG_PROG_MASK = 1u << 1,
    PM_IPPROG_PROG_IP = 1u << 2,
    PM_IPPROG_RESET = 1u << 3,       /**< return parameters to default */
    PM_IPPROG_PROG_GATEWAY = 1u << 4,
    PM_IPPROG_DHCP = 1u << 6,
    PM_IPPROG_ENABLE = 1u << 7,      /**< any programming */
};

typedef struct {
    uint8_t command;
    uint32_t ip;
    uint32_t mask;
    uint16_t port;                   /**< deprecated, ignored */
    uint32_t gateway;
} pm_artipprog_t;

typedef struct {
    uint32_t ip;
    uint32_t mask;
    uint16_t port;
    uint8_t status;                  /**< bit 6 = DHCP enabled */
    uint32_t gateway;
} pm_artipprogreply_t;

pm_err_t pm_artipprog_decode(const uint8_t *pkt, size_t len, pm_artipprog_t *out);
int pm_artipprog_encode(const pm_artipprog_t *in, uint8_t *buf, size_t cap);
pm_err_t pm_artipprogreply_decode(const uint8_t *pkt, size_t len, pm_artipprogreply_t *out);
int pm_artipprogreply_encode(const pm_artipprogreply_t *in, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ TOD */

#define PM_TOD_MAX_ADDRESSES 32u
#define PM_TOD_DATA_MAX_UIDS 200u /**< per ArtTodData packet */

typedef struct {
    uint8_t net;
    uint8_t command;                 /**< 0x00 TodFull */
    uint8_t adcount;                 /**< 0..32 */
    uint8_t address[PM_TOD_MAX_ADDRESSES]; /**< low byte of the Port-Address (Sub-Net + Universe) */
} pm_arttodrequest_t;

/** ArtTodData CommandResponse */
enum { PM_TOD_FULL = 0x00, PM_TOD_NAK = 0xFF };

typedef struct {
    uint8_t rdm_ver;                 /**< 0x01 = RDM standard V1.0 */
    uint8_t port;                    /**< 1..4 (1 with one port per BindIndex) */
    uint8_t bind_index;
    uint8_t net;
    uint8_t command_response;        /**< PM_TOD_FULL / PM_TOD_NAK */
    uint8_t address;                 /**< low byte of the Port-Address */
    uint16_t uid_total;
    uint8_t block_count;
    uint8_t uid_count;               /**< <= PM_TOD_DATA_MAX_UIDS */
    const pm_uid_t *uids;            /**< points into the packet (decode) / caller array (encode) */
} pm_arttoddata_t;

/** ArtTodControl commands */
enum { PM_ATC_NONE = 0x00, PM_ATC_FLUSH = 0x01, PM_ATC_END = 0x02, PM_ATC_INC_ON = 0x03, PM_ATC_INC_OFF = 0x04 };

typedef struct {
    uint8_t net;
    uint8_t command;
    uint8_t address;
} pm_arttodcontrol_t;

pm_err_t pm_arttodrequest_decode(const uint8_t *pkt, size_t len, pm_arttodrequest_t *out);
int pm_arttodrequest_encode(const pm_arttodrequest_t *in, uint8_t *buf, size_t cap);
pm_err_t pm_arttoddata_decode(const uint8_t *pkt, size_t len, pm_arttoddata_t *out);
int pm_arttoddata_encode(const pm_arttoddata_t *in, uint8_t *buf, size_t cap);
pm_err_t pm_arttodcontrol_decode(const uint8_t *pkt, size_t len, pm_arttodcontrol_t *out);
int pm_arttodcontrol_encode(const pm_arttodcontrol_t *in, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ ArtRdm */

typedef struct {
    uint8_t rdm_ver;                 /**< 0x01 */
    uint8_t fifo_avail;              /**< node -> controller: free request slots of this port */
    uint8_t fifo_max;
    uint8_t net;
    uint8_t command;                 /**< 0x00 ArProcess */
    uint8_t address;                 /**< low byte of the Port-Address */
    const uint8_t *rdm;              /**< RDM message WITHOUT start code 0xCC (sub-start code first) */
    size_t rdm_len;
} pm_artrdm_t;

pm_err_t pm_artrdm_decode(const uint8_t *pkt, size_t len, pm_artrdm_t *out);
int pm_artrdm_encode(const pm_artrdm_t *in, uint8_t *buf, size_t cap);

/* ------------------------------------------------------------------------------------------------ firmware */

enum { PM_FIRM_FIRST = 0x00, PM_FIRM_CONT = 0x01, PM_FIRM_LAST = 0x02 };
enum { PM_FIRM_BLOCK_GOOD = 0x00, PM_FIRM_ALL_GOOD = 0x01, PM_FIRM_FAIL = 0xFF };

#define PM_FIRM_BLOCK_BYTES 1024u /**< Data[512] Int16 (digest §6.1, interpretation) */
#define PM_ALF_HEADER_LEN 1060u

typedef struct {
    uint8_t type;                    /**< PM_FIRM_FIRST / CONT / LAST (UBEA types are rejected) */
    uint8_t block_id;
    uint32_t firmware_length_words;  /**< whole file incl. header, in 16-bit words */
    const uint8_t *data;             /**< PM_FIRM_BLOCK_BYTES bytes */
} pm_artfirmwaremaster_t;

pm_err_t pm_artfirmwaremaster_decode(const uint8_t *pkt, size_t len, pm_artfirmwaremaster_t *out);
int pm_artfirmwaremaster_encode(const pm_artfirmwaremaster_t *in, uint8_t *buf, size_t cap);
int pm_artfirmwarereply_encode(uint8_t type, uint8_t *buf, size_t cap);
pm_err_t pm_artfirmwarereply_decode(const uint8_t *pkt, size_t len, uint8_t *type);

/* ------------------------------------------------------------------------------------------------ ArtDiagData */

typedef struct {
    uint8_t priority;                /**< DpLow 0x10 .. DpVolatile 0xF0 */
    uint8_t logical_port;
    const char *text;                /**< NUL-terminated, max 512 bytes incl. NUL */
} pm_artdiagdata_t;

int pm_artdiagdata_encode(const pm_artdiagdata_t *in, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif
