/*
 * pm_artnet_packets.c - Art-Net 4 codec (byte layouts: protocol/artnet4-digest.md, offsets in the comments).
 * Decoders never read past `len`, ignore trailing bytes and NUL-terminate every string.
 */
#include "pm_artnet_packets.h"

#include <string.h>

static const uint8_t ARTNET_ID[8] = {'A', 'r', 't', '-', 'N', 'e', 't', 0};

pm_err_t pm_artnet_peek_opcode(const uint8_t *pkt, size_t len, uint16_t *opcode)
{
    if (pkt == NULL || opcode == NULL) return PM_ERR_INVALID_ARG;
    if (len < 10) return PM_ERR_SHORT;
    if (memcmp(pkt, ARTNET_ID, sizeof(ARTNET_ID)) != 0) return PM_ERR_BAD_ID;
    *opcode = pm_get_le16(pkt + 8);
    return PM_OK;
}

/** Common check for packets with ProtVer: ID, opcode, minimum length, ProtVer >= 14. */
static pm_err_t check_header(const uint8_t *pkt, size_t len, uint16_t op, size_t min_len)
{
    uint16_t got;
    pm_err_t e = pm_artnet_peek_opcode(pkt, len, &got);
    if (e != PM_OK) return e;
    if (got != op) return PM_ERR_BAD_ID;
    if (len < min_len) return PM_ERR_SHORT;
    if (pm_get_be16(pkt + 10) < PM_ARTNET_PROT_VER) return PM_ERR_BAD_FORMAT;
    return PM_OK;
}

/** Zero `total` bytes and write ID + OpCode (packets without ProtVer, i.e. ArtPollReply). */
static int put_id(uint8_t *buf, size_t cap, uint16_t op, size_t total)
{
    if (buf == NULL) return PM_ERR_INVALID_ARG;
    if (cap < total) return PM_ERR_NO_SPACE;
    memset(buf, 0, total);
    memcpy(buf, ARTNET_ID, sizeof(ARTNET_ID));
    pm_put_le16(buf + 8, op);
    return (int)total;
}

/** Zero `total` bytes and write ID + OpCode + ProtVer. */
static int put_header(uint8_t *buf, size_t cap, uint16_t op, size_t total)
{
    int n = put_id(buf, cap, op, total);
    if (n > 0) pm_put_be16(buf + 10, PM_ARTNET_PROT_VER);
    return n;
}

/** Copy a fixed-size, possibly unterminated packet string field into dst (always NUL-terminated). */
static void get_str(char *dst, size_t dst_size, const uint8_t *src, size_t field_size)
{
    size_t max = (field_size < dst_size ? field_size : dst_size) - 1;
    size_t n = 0;
    while (n < max && src[n] != 0) {
        dst[n] = (char)src[n];
        n++;
    }
    memset(dst + n, 0, dst_size - n);
}

/** Write a C string into a zeroed packet field of field_size bytes, truncated to field_size - 1 chars. */
static void put_str(uint8_t *dst, size_t field_size, const char *src)
{
    size_t n = 0;
    while (n < field_size - 1 && src[n] != '\0') {
        dst[n] = (uint8_t)src[n];
        n++;
    }
}

/* ------------------------------------------------------------------------------------------------ ArtPoll */

pm_err_t pm_artpoll_decode(const uint8_t *pkt, size_t len, pm_artpoll_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_POLL, 14);
    if (e != PM_OK) return e;
    memset(out, 0, sizeof(*out));
    out->flags = pkt[12];
    out->diag_priority = pkt[13];
    if (len >= 18) {
        out->target_top = pm_get_be16(pkt + 14);
        out->target_bottom = pm_get_be16(pkt + 16);
    }
    if (len >= 20) out->esta_man = pm_get_be16(pkt + 18);
    if (len >= 22) out->oem = pm_get_be16(pkt + 20);
    return PM_OK;
}

int pm_artpoll_encode(const pm_artpoll_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_POLL, 22);
    if (n < 0) return n;
    buf[12] = in->flags;
    buf[13] = in->diag_priority;
    pm_put_be16(buf + 14, in->target_top);
    pm_put_be16(buf + 16, in->target_bottom);
    pm_put_be16(buf + 18, in->esta_man);
    pm_put_be16(buf + 20, in->oem);
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtDmx */

pm_err_t pm_artdmx_decode(const uint8_t *pkt, size_t len, pm_artdmx_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_DMX, 18);
    if (e != PM_OK) return e;
    uint16_t dlen = pm_get_be16(pkt + 16);
    if (dlen == 0 || dlen > 512) return PM_ERR_BAD_FORMAT;
    if (len < 18u + dlen) return PM_ERR_SHORT;
    out->sequence = pkt[12];
    out->physical = pkt[13];
    out->port_address = (pm_port_address_t)(((pkt[15] & 0x7Fu) << 8) | pkt[14]);
    out->length = dlen;
    out->data = pkt + 18;
    return PM_OK;
}

int pm_artdmx_encode(const pm_artdmx_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL || in->length == 0 || in->length > 512 || in->data == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_DMX, 18u + in->length);
    if (n < 0) return n;
    buf[12] = in->sequence;
    buf[13] = in->physical;
    buf[14] = (uint8_t)(in->port_address & 0xFFu);
    buf[15] = (uint8_t)((in->port_address >> 8) & 0x7Fu);
    pm_put_be16(buf + 16, in->length);
    memcpy(buf + 18, in->data, in->length);
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtSync */

pm_err_t pm_artsync_decode(const uint8_t *pkt, size_t len)
{
    return check_header(pkt, len, PM_OP_SYNC, 14); /* spec gives no minimum: we require the full 14 bytes */
}

int pm_artsync_encode(uint8_t *buf, size_t cap) { return put_header(buf, cap, PM_OP_SYNC, 14); }


/* ------------------------------------------------------------------------------------------------ ArtPollReply */

pm_err_t pm_artpollreply_decode(const uint8_t *pkt, size_t len, pm_artpollreply_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    uint16_t op;
    pm_err_t e = pm_artnet_peek_opcode(pkt, len, &op);
    if (e != PM_OK) return e;
    if (op != PM_OP_POLL_REPLY) return PM_ERR_BAD_ID;
    if (len < PM_POLL_REPLY_MIN_LEN) return PM_ERR_SHORT;

    /* No ProtVer in ArtPollReply. Fields beyond len (207..238) read as 0: parse a zero-padded copy. */
    uint8_t p[PM_POLL_REPLY_LEN];
    size_t n = len < sizeof(p) ? len : sizeof(p);
    memset(p, 0, sizeof(p));
    memcpy(p, pkt, n);

    memset(out, 0, sizeof(*out));
    out->ip = pm_get_be32(p + 10);
    /* 14: Port 0x1936 (LE) - informational, not checked */
    out->vers_info = pm_get_be16(p + 16);
    out->net_switch = p[18];
    out->sub_switch = p[19];
    out->oem = pm_get_be16(p + 20);
    out->ubea_version = p[22];
    out->status1 = p[23];
    out->esta_man = pm_get_le16(p + 24); /* EstaManLo first */
    get_str(out->short_name, sizeof(out->short_name), p + 26, 18);
    get_str(out->long_name, sizeof(out->long_name), p + 44, 64);
    get_str(out->node_report, sizeof(out->node_report), p + 108, 64);
    out->num_ports = pm_get_be16(p + 172);
    memcpy(out->port_types, p + 174, 4);
    memcpy(out->good_input, p + 178, 4);
    memcpy(out->good_output_a, p + 182, 4);
    memcpy(out->sw_in, p + 186, 4);
    memcpy(out->sw_out, p + 190, 4);
    out->acn_priority = p[194];
    out->sw_macro = p[195];
    out->sw_remote = p[196];
    out->style = p[200];
    memcpy(out->mac, p + 201, 6);
    out->bind_ip = pm_get_be32(p + 207);
    out->bind_index = p[211];
    out->status2 = p[212];
    memcpy(out->good_output_b, p + 213, 4);
    out->status3 = p[217];
    memcpy(out->default_resp_uid.b, p + 218, 6);
    out->user = pm_get_be16(p + 224);
    out->refresh_rate = pm_get_be16(p + 226);
    out->background_queue_policy = p[228];
    return PM_OK;
}

int pm_artpollreply_encode(const pm_artpollreply_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL) return PM_ERR_INVALID_ARG;
    int n = put_id(buf, cap, PM_OP_POLL_REPLY, PM_POLL_REPLY_LEN);
    if (n < 0) return n;
    pm_put_be32(buf + 10, in->ip);
    pm_put_le16(buf + 14, PM_ARTNET_PORT);
    pm_put_be16(buf + 16, in->vers_info);
    buf[18] = in->net_switch;
    buf[19] = in->sub_switch;
    pm_put_be16(buf + 20, in->oem);
    buf[22] = in->ubea_version;
    buf[23] = in->status1;
    pm_put_le16(buf + 24, in->esta_man);
    put_str(buf + 26, 18, in->short_name);
    put_str(buf + 44, 64, in->long_name);
    put_str(buf + 108, 64, in->node_report);
    pm_put_be16(buf + 172, in->num_ports);
    memcpy(buf + 174, in->port_types, 4);
    memcpy(buf + 178, in->good_input, 4);
    memcpy(buf + 182, in->good_output_a, 4);
    memcpy(buf + 186, in->sw_in, 4);
    memcpy(buf + 190, in->sw_out, 4);
    buf[194] = in->acn_priority;
    buf[195] = in->sw_macro;
    buf[196] = in->sw_remote;
    buf[200] = in->style;
    memcpy(buf + 201, in->mac, 6);
    pm_put_be32(buf + 207, in->bind_ip);
    buf[211] = in->bind_index;
    buf[212] = in->status2;
    memcpy(buf + 213, in->good_output_b, 4);
    buf[217] = in->status3;
    memcpy(buf + 218, in->default_resp_uid.b, 6);
    pm_put_be16(buf + 224, in->user);
    pm_put_be16(buf + 226, in->refresh_rate);
    buf[228] = in->background_queue_policy;
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtAddress */

#define ARTADDRESS_LEN 107u

pm_err_t pm_artaddress_decode(const uint8_t *pkt, size_t len, pm_artaddress_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_ADDRESS, ARTADDRESS_LEN);
    if (e != PM_OK) return e;
    memset(out, 0, sizeof(*out));
    out->net_switch = pkt[12];
    out->bind_index = pkt[13];
    get_str(out->short_name, sizeof(out->short_name), pkt + 14, 18);
    get_str(out->long_name, sizeof(out->long_name), pkt + 32, 64);
    memcpy(out->sw_in, pkt + 96, 4);
    memcpy(out->sw_out, pkt + 100, 4);
    out->sub_switch = pkt[104];
    out->acn_priority = pkt[105];
    out->command = pkt[106];
    return PM_OK;
}

int pm_artaddress_encode(const pm_artaddress_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_ADDRESS, ARTADDRESS_LEN);
    if (n < 0) return n;
    buf[12] = in->net_switch;
    buf[13] = in->bind_index;
    put_str(buf + 14, 18, in->short_name);
    put_str(buf + 32, 64, in->long_name);
    memcpy(buf + 96, in->sw_in, 4);
    memcpy(buf + 100, in->sw_out, 4);
    buf[104] = in->sub_switch;
    buf[105] = in->acn_priority;
    buf[106] = in->command;
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtInput */

#define ARTINPUT_LEN 20u

pm_err_t pm_artinput_decode(const uint8_t *pkt, size_t len, pm_artinput_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_INPUT, ARTINPUT_LEN);
    if (e != PM_OK) return e;
    out->bind_index = pkt[13];
    out->num_ports = pm_get_be16(pkt + 14);
    memcpy(out->input, pkt + 16, 4);
    return PM_OK;
}

int pm_artinput_encode(const pm_artinput_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_INPUT, ARTINPUT_LEN);
    if (n < 0) return n;
    buf[13] = in->bind_index;
    pm_put_be16(buf + 14, in->num_ports);
    memcpy(buf + 16, in->input, 4);
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtIpProg(Reply) */

#define ARTIPPROG_LEN 34u /* the trailing Spare[4] is included in the minimum (digest 4.3) */

pm_err_t pm_artipprog_decode(const uint8_t *pkt, size_t len, pm_artipprog_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_IP_PROG, ARTIPPROG_LEN);
    if (e != PM_OK) return e;
    out->command = pkt[14];
    out->ip = pm_get_be32(pkt + 16);
    out->mask = pm_get_be32(pkt + 20);
    out->port = pm_get_be16(pkt + 24);
    out->gateway = pm_get_be32(pkt + 26);
    return PM_OK;
}

int pm_artipprog_encode(const pm_artipprog_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_IP_PROG, ARTIPPROG_LEN);
    if (n < 0) return n;
    buf[14] = in->command;
    pm_put_be32(buf + 16, in->ip);
    pm_put_be32(buf + 20, in->mask);
    pm_put_be16(buf + 24, in->port);
    pm_put_be32(buf + 26, in->gateway);
    return n;
}

#define ARTIPPROGREPLY_LEN 34u

pm_err_t pm_artipprogreply_decode(const uint8_t *pkt, size_t len, pm_artipprogreply_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_IP_PROG_REPLY, ARTIPPROGREPLY_LEN);
    if (e != PM_OK) return e;
    out->ip = pm_get_be32(pkt + 16);
    out->mask = pm_get_be32(pkt + 20);
    out->port = pm_get_be16(pkt + 24);
    out->status = pkt[26];
    out->gateway = pm_get_be32(pkt + 28);
    return PM_OK;
}

int pm_artipprogreply_encode(const pm_artipprogreply_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_IP_PROG_REPLY, ARTIPPROGREPLY_LEN);
    if (n < 0) return n;
    pm_put_be32(buf + 16, in->ip);
    pm_put_be32(buf + 20, in->mask);
    pm_put_be16(buf + 24, in->port);
    buf[26] = in->status;
    pm_put_be32(buf + 28, in->gateway);
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtTodRequest */

#define ARTTODREQUEST_MIN_LEN 24u
#define ARTTODREQUEST_LEN (24u + PM_TOD_MAX_ADDRESSES) /* 56 */

pm_err_t pm_arttodrequest_decode(const uint8_t *pkt, size_t len, pm_arttodrequest_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_TOD_REQUEST, ARTTODREQUEST_MIN_LEN);
    if (e != PM_OK) return e;
    uint8_t count = pkt[23];
    if (count > PM_TOD_MAX_ADDRESSES) return PM_ERR_BAD_FORMAT;
    if (len < ARTTODREQUEST_MIN_LEN + count) return PM_ERR_SHORT;
    memset(out, 0, sizeof(*out));
    out->net = pkt[21];
    out->command = pkt[22];
    out->adcount = count;
    memcpy(out->address, pkt + 24, count);
    return PM_OK;
}

int pm_arttodrequest_encode(const pm_arttodrequest_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL || in->adcount > PM_TOD_MAX_ADDRESSES) return PM_ERR_INVALID_ARG;
    /* always the full 56 bytes (unused Address[] entries 0), like the spec table */
    int n = put_header(buf, cap, PM_OP_TOD_REQUEST, ARTTODREQUEST_LEN);
    if (n < 0) return n;
    buf[21] = in->net;
    buf[22] = in->command;
    buf[23] = in->adcount;
    memcpy(buf + 24, in->address, in->adcount);
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtTodData */

#define ARTTODDATA_HDR_LEN 28u

/* uids point straight into the packet: pm_uid_t must be 6 plain bytes without padding or alignment needs */
_Static_assert(sizeof(pm_uid_t) == 6, "pm_uid_t must be exactly 6 bytes");
_Static_assert(_Alignof(pm_uid_t) == 1, "pm_uid_t must be byte-aligned");

pm_err_t pm_arttoddata_decode(const uint8_t *pkt, size_t len, pm_arttoddata_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_TOD_DATA, ARTTODDATA_HDR_LEN);
    if (e != PM_OK) return e;
    uint8_t count = pkt[27];
    if (count > PM_TOD_DATA_MAX_UIDS) return PM_ERR_BAD_FORMAT;
    if (len < ARTTODDATA_HDR_LEN + 6u * count) return PM_ERR_SHORT;
    out->rdm_ver = pkt[12];
    out->port = pkt[13];
    out->bind_index = pkt[20];
    out->net = pkt[21];
    out->command_response = pkt[22];
    out->address = pkt[23];
    out->uid_total = pm_get_be16(pkt + 24);
    out->block_count = pkt[26];
    out->uid_count = count;
    out->uids = (const pm_uid_t *)(const void *)(pkt + ARTTODDATA_HDR_LEN);
    return PM_OK;
}

int pm_arttoddata_encode(const pm_arttoddata_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL || in->uid_count > PM_TOD_DATA_MAX_UIDS) return PM_ERR_INVALID_ARG;
    if (in->uid_count > 0 && in->uids == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_TOD_DATA, ARTTODDATA_HDR_LEN + 6u * in->uid_count);
    if (n < 0) return n;
    buf[12] = in->rdm_ver;
    buf[13] = in->port;
    buf[20] = in->bind_index;
    buf[21] = in->net;
    buf[22] = in->command_response;
    buf[23] = in->address;
    pm_put_be16(buf + 24, in->uid_total);
    buf[26] = in->block_count;
    buf[27] = in->uid_count;
    for (size_t i = 0; i < in->uid_count; i++) memcpy(buf + ARTTODDATA_HDR_LEN + 6u * i, in->uids[i].b, 6);
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtTodControl */

#define ARTTODCONTROL_LEN 24u

pm_err_t pm_arttodcontrol_decode(const uint8_t *pkt, size_t len, pm_arttodcontrol_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_TOD_CONTROL, ARTTODCONTROL_LEN);
    if (e != PM_OK) return e;
    out->net = pkt[21];
    out->command = pkt[22];
    out->address = pkt[23];
    return PM_OK;
}

int pm_arttodcontrol_encode(const pm_arttodcontrol_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_TOD_CONTROL, ARTTODCONTROL_LEN);
    if (n < 0) return n;
    buf[21] = in->net;
    buf[22] = in->command;
    buf[23] = in->address;
    return n;
}

/* ------------------------------------------------------------------------------------------------ ArtRdm */

#define ARTRDM_HDR_LEN 24u
#define RDM_MIN_MESSAGE_LEN 24u /* E1.20 Message Length without parameter data (counts the 0xCC start code) */

pm_err_t pm_artrdm_decode(const uint8_t *pkt, size_t len, pm_artrdm_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    /* an ArtRdm without at least one RDM byte carries nothing: require 24 + 1 */
    pm_err_t e = check_header(pkt, len, PM_OP_RDM, ARTRDM_HDR_LEN + 1u);
    if (e != PM_OK) return e;
    out->rdm_ver = pkt[12];
    out->fifo_avail = pkt[19];
    out->fifo_max = pkt[20];
    out->net = pkt[21];
    out->command = pkt[22];
    out->address = pkt[23];
    out->rdm = pkt + ARTRDM_HDR_LEN;
    out->rdm_len = len - ARTRDM_HDR_LEN;
    /* RdmPacket has no start code; Message Length (rdm[1]) counts it but not the 2 checksum bytes, so the RDM
     * message occupies Message Length - 1 + 2 bytes here. Trim trailing padding when that fits; otherwise hand
     * over everything and let pm_rdm judge the frame. */
    if (out->rdm_len >= 2 && out->rdm[1] >= RDM_MIN_MESSAGE_LEN) {
        size_t msg = (size_t)out->rdm[1] + 1u;
        if (msg <= out->rdm_len) out->rdm_len = msg;
    }
    return PM_OK;
}

int pm_artrdm_encode(const pm_artrdm_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL || (in->rdm_len > 0 && in->rdm == NULL)) return PM_ERR_INVALID_ARG;
    if (in->rdm_len > PM_ARTNET_MAX_PACKET - ARTRDM_HDR_LEN) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_RDM, ARTRDM_HDR_LEN + in->rdm_len);
    if (n < 0) return n;
    buf[12] = in->rdm_ver;
    buf[19] = in->fifo_avail;
    buf[20] = in->fifo_max;
    buf[21] = in->net;
    buf[22] = in->command;
    buf[23] = in->address;
    if (in->rdm_len > 0) memcpy(buf + ARTRDM_HDR_LEN, in->rdm, in->rdm_len);
    return n;
}

/* ------------------------------------------------------------------------------------------------ firmware */

#define ARTFIRMWAREMASTER_HDR_LEN 40u
#define ARTFIRMWAREMASTER_LEN (ARTFIRMWAREMASTER_HDR_LEN + PM_FIRM_BLOCK_BYTES) /* 1064 */
#define ARTFIRMWAREREPLY_LEN 36u

pm_err_t pm_artfirmwaremaster_decode(const uint8_t *pkt, size_t len, pm_artfirmwaremaster_t *out)
{
    if (out == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_FIRMWARE_MASTER, ARTFIRMWAREMASTER_LEN);
    if (e != PM_OK) return e;
    out->type = pkt[14]; /* UBEA types (0x03..0x05) are passed through; the node answers FirmFail */
    out->block_id = pkt[15];
    out->firmware_length_words = pm_get_be32(pkt + 16);
    out->data = pkt + ARTFIRMWAREMASTER_HDR_LEN;
    return PM_OK;
}

int pm_artfirmwaremaster_encode(const pm_artfirmwaremaster_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL || in->data == NULL) return PM_ERR_INVALID_ARG;
    int n = put_header(buf, cap, PM_OP_FIRMWARE_MASTER, ARTFIRMWAREMASTER_LEN);
    if (n < 0) return n;
    buf[14] = in->type;
    buf[15] = in->block_id;
    pm_put_be32(buf + 16, in->firmware_length_words);
    memcpy(buf + ARTFIRMWAREMASTER_HDR_LEN, in->data, PM_FIRM_BLOCK_BYTES);
    return n;
}

int pm_artfirmwarereply_encode(uint8_t type, uint8_t *buf, size_t cap)
{
    int n = put_header(buf, cap, PM_OP_FIRMWARE_REPLY, ARTFIRMWAREREPLY_LEN);
    if (n < 0) return n;
    buf[14] = type;
    return n;
}

pm_err_t pm_artfirmwarereply_decode(const uint8_t *pkt, size_t len, uint8_t *type)
{
    if (type == NULL) return PM_ERR_INVALID_ARG;
    pm_err_t e = check_header(pkt, len, PM_OP_FIRMWARE_REPLY, ARTFIRMWAREREPLY_LEN);
    if (e != PM_OK) return e;
    *type = pkt[14];
    return PM_OK;
}

/* ------------------------------------------------------------------------------------------------ ArtDiagData */

#define ARTDIAGDATA_HDR_LEN 18u
#define ARTDIAGDATA_MAX_DATA 512u /* incl. NUL */

int pm_artdiagdata_encode(const pm_artdiagdata_t *in, uint8_t *buf, size_t cap)
{
    if (in == NULL || in->text == NULL) return PM_ERR_INVALID_ARG;
    size_t tlen = 0;
    while (tlen < ARTDIAGDATA_MAX_DATA - 1u && in->text[tlen] != '\0') tlen++;
    size_t dlen = tlen + 1u; /* Length counts the terminating NUL */
    int n = put_header(buf, cap, PM_OP_DIAG_DATA, ARTDIAGDATA_HDR_LEN + dlen);
    if (n < 0) return n;
    buf[13] = in->priority;
    buf[14] = in->logical_port;
    pm_put_be16(buf + 16, (uint16_t)dlen);
    memcpy(buf + ARTDIAGDATA_HDR_LEN, in->text, tlen); /* NUL already there from put_header */
    return n;
}
