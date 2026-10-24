/*
 * pm_artnet_packets.c - Art-Net 4 codec. Phase 0: header, ArtPoll, ArtDmx, ArtSync implemented (reference for the
 * style and the vector tests); the other packets return PM_ERR_NOT_IMPLEMENTED until Phase 1 fills them in.
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

static int put_header(uint8_t *buf, size_t cap, uint16_t op, size_t total)
{
    if (buf == NULL) return PM_ERR_INVALID_ARG;
    if (cap < total) return PM_ERR_NO_SPACE;
    memset(buf, 0, total);
    memcpy(buf, ARTNET_ID, sizeof(ARTNET_ID));
    pm_put_le16(buf + 8, op);
    pm_put_be16(buf + 10, PM_ARTNET_PROT_VER);
    return (int)total;
}

/* ------------------------------------------------------------------------------------------------ ArtPoll */

pm_err_t pm_artpoll_decode(const uint8_t *pkt, size_t len, pm_artpoll_t *out)
{
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

/* ------------------------------------------------------------------------------------------------ Phase 1 */

pm_err_t pm_artpollreply_decode(const uint8_t *pkt, size_t len, pm_artpollreply_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artpollreply_encode(const pm_artpollreply_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_artaddress_decode(const uint8_t *pkt, size_t len, pm_artaddress_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artaddress_encode(const pm_artaddress_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_artinput_decode(const uint8_t *pkt, size_t len, pm_artinput_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artinput_encode(const pm_artinput_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_artipprog_decode(const uint8_t *pkt, size_t len, pm_artipprog_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artipprog_encode(const pm_artipprog_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_artipprogreply_decode(const uint8_t *pkt, size_t len, pm_artipprogreply_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artipprogreply_encode(const pm_artipprogreply_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_arttodrequest_decode(const uint8_t *pkt, size_t len, pm_arttodrequest_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_arttodrequest_encode(const pm_arttodrequest_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_arttoddata_decode(const uint8_t *pkt, size_t len, pm_arttoddata_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_arttoddata_encode(const pm_arttoddata_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_arttodcontrol_decode(const uint8_t *pkt, size_t len, pm_arttodcontrol_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_arttodcontrol_encode(const pm_arttodcontrol_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_artrdm_decode(const uint8_t *pkt, size_t len, pm_artrdm_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artrdm_encode(const pm_artrdm_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_artfirmwaremaster_decode(const uint8_t *pkt, size_t len, pm_artfirmwaremaster_t *out)
{
    (void)pkt; (void)len; (void)out;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artfirmwaremaster_encode(const pm_artfirmwaremaster_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artfirmwarereply_encode(uint8_t type, uint8_t *buf, size_t cap)
{
    (void)type; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
pm_err_t pm_artfirmwarereply_decode(const uint8_t *pkt, size_t len, uint8_t *type)
{
    (void)pkt; (void)len; (void)type;
    return PM_ERR_NOT_IMPLEMENTED;
}
int pm_artdiagdata_encode(const pm_artdiagdata_t *in, uint8_t *buf, size_t cap)
{
    (void)in; (void)buf; (void)cap;
    return PM_ERR_NOT_IMPLEMENTED;
}
