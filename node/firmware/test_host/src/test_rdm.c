/* pm_rdm codec against protocol/vectors/rdm. */
#include "pm_rdm.h"
#include "test.h"
#include "vectors.h"

void test_rdm_responder(void);
void test_rdm_ctrl(void); /* test_rdm_responder.c */

static void check_msg(const pm_vec_t *v)
{
    pm_rdm_msg_t m;
    pm_err_t e = pm_rdm_decode(v->bytes, v->len, &m);
    if (!v->valid) {
        const char *err = vec_str(v, "error");
        CHECK_EQ(e, err && strcmp(err, "bad_checksum") == 0 ? PM_ERR_BAD_CHECKSUM : PM_ERR_BAD_FORMAT);
        return;
    }
    CHECK_EQ(e, PM_OK);
    char s[PM_UID_STR_LEN];
    CHECK(strcmp(pm_uid_format(&m.dest, s), vec_str(v, "dest_uid")) == 0);
    CHECK(strcmp(pm_uid_format(&m.src, s), vec_str(v, "src_uid")) == 0);
    CHECK_EQ(m.tn, vec_num(v, "transaction_number"));
    CHECK_EQ(m.port_or_resp, vec_has(v, "port_id") ? vec_num(v, "port_id") : vec_num(v, "response_type"));
    CHECK_EQ(m.message_count, vec_num(v, "message_count"));
    CHECK_EQ(m.sub_device, vec_num(v, "sub_device"));
    CHECK_EQ(m.cc, vec_num(v, "command_class"));
    CHECK_EQ(m.pid, vec_num(v, "parameter_id"));
    CHECK_EQ(m.pdl, vec_num(v, "parameter_data_length"));
    uint8_t buf[PM_RDM_MAX_MSG_LEN];
    int n = pm_rdm_encode(&m, buf, sizeof(buf));
    CHECK_EQ(n, v->len);
    CHECK(n == (int)v->len && memcmp(buf, v->bytes, v->len) == 0);
    /* ArtRdm form: without start code */
    pm_rdm_msg_t m2;
    CHECK_EQ(pm_rdm_decode_nosc(v->bytes + 1, v->len - 1, &m2), PM_OK);
    CHECK_EQ(m2.pid, m.pid);
}

static void check_dub(const pm_vec_t *v)
{
    pm_uid_t u;
    pm_err_t e = pm_rdm_dub_decode(v->bytes, v->len, &u);
    if (!v->valid) {
        CHECK(e != PM_OK);
        return;
    }
    CHECK_EQ(e, PM_OK);
    char s[PM_UID_STR_LEN];
    CHECK(strcmp(pm_uid_format(&u, s), vec_str(v, "uid")) == 0);
    uint8_t buf[32];
    int n = pm_rdm_dub_encode(&u, (unsigned)vec_num(v, "preamble_length"), buf, sizeof(buf));
    CHECK_EQ(n, v->len);
    CHECK(n == (int)v->len && memcmp(buf, v->bytes, v->len) == 0);
}

void test_rdm(void)
{
    int done = 0;
    for (size_t i = 0; i < pm_vectors_count; i++) {
        const pm_vec_t *v = &pm_vectors[i];
        if (strcmp(v->protocol, "rdm") != 0) continue;
        g_context = v->name;
        if (strcmp(v->packet, "RDM") == 0) { check_msg(v); done++; }
        else if (strcmp(v->packet, "DUB_RESPONSE") == 0) { check_dub(v); done++; }
    }
    g_context = NULL;
    CHECK(done >= 20);
    test_rdm_responder();
    test_rdm_ctrl();
}
