/* pm_rdm.c - E1.20 message codec + DUB encoding (implemented in Phase 0, reference for vector tests). */
#include "pm_rdm.h"

#include <string.h>

uint16_t pm_rdm_checksum(const uint8_t *data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) sum = (uint16_t)(sum + data[i]);
    return sum;
}

pm_err_t pm_rdm_decode(const uint8_t *buf, size_t len, pm_rdm_msg_t *out)
{
    if (buf == NULL || out == NULL) return PM_ERR_INVALID_ARG;
    if (len < PM_RDM_HEADER_LEN + 2u) return PM_ERR_SHORT;
    if (buf[0] != PM_RDM_START_CODE || buf[1] != PM_RDM_SUB_START_CODE) return PM_ERR_BAD_ID;
    uint8_t mlen = buf[2];
    uint8_t pdl = buf[23];
    if (pdl > PM_RDM_MAX_PDL || mlen != PM_RDM_HEADER_LEN + pdl) return PM_ERR_BAD_FORMAT;
    if (len < (size_t)mlen + 2u) return PM_ERR_SHORT;
    if (pm_get_be16(buf + mlen) != pm_rdm_checksum(buf, mlen)) return PM_ERR_BAD_CHECKSUM;
    memcpy(out->dest.b, buf + 3, 6);
    memcpy(out->src.b, buf + 9, 6);
    out->tn = buf[15];
    out->port_or_resp = buf[16];
    out->message_count = buf[17];
    out->sub_device = pm_get_be16(buf + 18);
    out->cc = buf[20];
    out->pid = pm_get_be16(buf + 21);
    out->pdl = pdl;
    out->pd = buf + PM_RDM_HEADER_LEN;
    return PM_OK;
}

pm_err_t pm_rdm_decode_nosc(const uint8_t *buf, size_t len, pm_rdm_msg_t *out)
{
    uint8_t tmp[PM_RDM_MAX_MSG_LEN];
    if (buf == NULL || out == NULL) return PM_ERR_INVALID_ARG;
    if (len + 1u > sizeof(tmp)) len = sizeof(tmp) - 1u;
    tmp[0] = PM_RDM_START_CODE;
    memcpy(tmp + 1, buf, len);
    pm_err_t e = pm_rdm_decode(tmp, len + 1u, out);
    if (e == PM_OK) out->pd = buf + (PM_RDM_HEADER_LEN - 1u); /* point into the caller's buffer */
    return e;
}

int pm_rdm_encode(const pm_rdm_msg_t *m, uint8_t *buf, size_t cap)
{
    if (m == NULL || buf == NULL || m->pdl > PM_RDM_MAX_PDL || (m->pdl && m->pd == NULL)) return PM_ERR_INVALID_ARG;
    size_t mlen = PM_RDM_HEADER_LEN + m->pdl;
    if (cap < mlen + 2u) return PM_ERR_NO_SPACE;
    buf[0] = PM_RDM_START_CODE;
    buf[1] = PM_RDM_SUB_START_CODE;
    buf[2] = (uint8_t)mlen;
    memcpy(buf + 3, m->dest.b, 6);
    memcpy(buf + 9, m->src.b, 6);
    buf[15] = m->tn;
    buf[16] = m->port_or_resp;
    buf[17] = m->message_count;
    pm_put_be16(buf + 18, m->sub_device);
    buf[20] = m->cc;
    pm_put_be16(buf + 21, m->pid);
    buf[23] = m->pdl;
    if (m->pdl) memmove(buf + PM_RDM_HEADER_LEN, m->pd, m->pdl);
    pm_put_be16(buf + mlen, pm_rdm_checksum(buf, mlen));
    return (int)(mlen + 2u);
}

int pm_rdm_encode_nosc(const pm_rdm_msg_t *m, uint8_t *buf, size_t cap)
{
    uint8_t tmp[PM_RDM_MAX_MSG_LEN];
    int n = pm_rdm_encode(m, tmp, sizeof(tmp));
    if (n < 0) return n;
    if (cap < (size_t)n - 1u) return PM_ERR_NO_SPACE;
    memcpy(buf, tmp + 1, (size_t)n - 1u);
    return n - 1;
}

void pm_rdm_make_response(const pm_rdm_msg_t *req, const pm_uid_t *self, uint8_t response_type,
                          uint8_t message_count, pm_rdm_msg_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->dest = req->src;
    resp->src = *self;
    resp->tn = req->tn;
    resp->port_or_resp = response_type;
    resp->message_count = message_count;
    resp->sub_device = req->sub_device;
    resp->cc = (uint8_t)(req->cc + 1u);
    resp->pid = req->pid;
}

int pm_rdm_dub_encode(const pm_uid_t *uid, unsigned preamble_len, uint8_t *buf, size_t cap)
{
    if (uid == NULL || buf == NULL || preamble_len > 7) return PM_ERR_INVALID_ARG;
    size_t total = preamble_len + 1u + 12u + 4u;
    if (cap < total) return PM_ERR_NO_SPACE;
    uint8_t *p = buf;
    for (unsigned i = 0; i < preamble_len; i++) *p++ = 0xFE;
    *p++ = 0xAA;
    uint16_t sum = 0;
    for (unsigned i = 0; i < 6; i++) {
        p[0] = uid->b[i] | 0xAAu;
        p[1] = uid->b[i] | 0x55u;
        sum = (uint16_t)(sum + p[0] + p[1]);
        p += 2;
    }
    p[0] = (uint8_t)((sum >> 8) | 0xAAu);
    p[1] = (uint8_t)((sum >> 8) | 0x55u);
    p[2] = (uint8_t)((sum & 0xFFu) | 0xAAu);
    p[3] = (uint8_t)((sum & 0xFFu) | 0x55u);
    return (int)total;
}

pm_err_t pm_rdm_dub_decode(const uint8_t *buf, size_t len, pm_uid_t *out)
{
    if (buf == NULL || out == NULL) return PM_ERR_INVALID_ARG;
    size_t i = 0;
    while (i < len && i < 7 && buf[i] == 0xFE) i++;
    if (i >= len || buf[i] != 0xAA) return len - i < 17u ? PM_ERR_SHORT : PM_ERR_BAD_CHECKSUM;
    i++;
    if (len - i < 16u) return PM_ERR_SHORT;
    const uint8_t *e = buf + i;
    uint16_t sum = 0;
    for (unsigned k = 0; k < 12; k++) sum = (uint16_t)(sum + e[k]);
    uint16_t ecs = (uint16_t)(((e[12] & e[13]) << 8) | (e[14] & e[15]));
    if (sum != ecs) return PM_ERR_BAD_CHECKSUM;
    for (unsigned k = 0; k < 6; k++) out->b[k] = e[2 * k] & e[2 * k + 1];
    return PM_OK;
}
