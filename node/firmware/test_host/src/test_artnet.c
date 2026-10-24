/* pm_artnet codec against protocol/vectors/artnet. Phase 0: ArtPoll, ArtDmx, ArtSync (others follow in Phase 1). */
#include "pm_artnet_packets.h"
#include "test.h"
#include "vectors.h"

static pm_err_t expected_error(const pm_vec_t *v)
{
    const char *e = vec_str(v, "error");
    if (e == NULL) return PM_OK;
    if (strcmp(e, "short") == 0) return PM_ERR_SHORT;
    if (strcmp(e, "bad_id") == 0) return PM_ERR_BAD_ID;
    return PM_ERR_BAD_FORMAT;
}

static void check_poll(const pm_vec_t *v)
{
    pm_artpoll_t p;
    pm_err_t e = pm_artpoll_decode(v->bytes, v->len, &p);
    if (!v->valid) {
        CHECK_EQ(e, expected_error(v));
        return;
    }
    CHECK_EQ(e, PM_OK);
    CHECK_EQ(p.flags, vec_num(v, "flags"));
    CHECK_EQ(p.diag_priority, vec_num(v, "diag_priority"));
    CHECK_EQ(p.target_top, vec_num(v, "target_port_address_top"));
    CHECK_EQ(p.target_bottom, vec_num(v, "target_port_address_bottom"));
    CHECK_EQ(p.esta_man, vec_num(v, "esta_man"));
    CHECK_EQ(p.oem, vec_num(v, "oem"));
    if (v->len == 22) { /* full packet: re-encode must give the same bytes */
        uint8_t buf[64];
        int n = pm_artpoll_encode(&p, buf, sizeof(buf));
        CHECK_EQ(n, 22);
        CHECK(n == 22 && memcmp(buf, v->bytes, 22) == 0);
    }
}

static void check_dmx(const pm_vec_t *v)
{
    pm_artdmx_t d;
    pm_err_t e = pm_artdmx_decode(v->bytes, v->len, &d);
    if (!v->valid) {
        CHECK_EQ(e, expected_error(v));
        return;
    }
    CHECK_EQ(e, PM_OK);
    CHECK_EQ(d.sequence, vec_num(v, "sequence"));
    CHECK_EQ(d.physical, vec_num(v, "physical"));
    CHECK_EQ(d.port_address, vec_num(v, "port_address"));
    CHECK_EQ(PM_PA_NET(d.port_address), vec_num(v, "net"));
    CHECK_EQ(PM_PA_SUB(d.port_address), vec_num(v, "sub_net"));
    CHECK_EQ(PM_PA_UNI(d.port_address), vec_num(v, "universe"));
    CHECK_EQ(d.length, vec_num(v, "length"));
    uint8_t want[512];
    int n = vec_hex(vec_str(v, "data"), want, sizeof(want));
    CHECK_EQ(n, d.length);
    CHECK(n == d.length && memcmp(want, d.data, d.length) == 0);
    uint8_t buf[600];
    int len = pm_artdmx_encode(&d, buf, sizeof(buf));
    CHECK_EQ(len, v->len);
    CHECK(len == (int)v->len && memcmp(buf, v->bytes, v->len) == 0);
}

static void check_sync(const pm_vec_t *v)
{
    pm_err_t e = pm_artsync_decode(v->bytes, v->len);
    if (!v->valid) {
        CHECK_EQ(e, expected_error(v));
        return;
    }
    CHECK_EQ(e, PM_OK);
}

void test_artnet(void)
{
    int done = 0;
    for (size_t i = 0; i < pm_vectors_count; i++) {
        const pm_vec_t *v = &pm_vectors[i];
        if (strcmp(v->protocol, "artnet") != 0) continue;
        g_context = v->name;
        uint16_t op;
        if (v->valid) {
            CHECK_EQ(pm_artnet_peek_opcode(v->bytes, v->len, &op), PM_OK);
            if (vec_has(v, "op_code")) CHECK_EQ(op, vec_num(v, "op_code"));
        }
        if (strcmp(v->packet, "ArtPoll") == 0) { check_poll(v); done++; }
        else if (strcmp(v->packet, "ArtDmx") == 0) { check_dmx(v); done++; }
        else if (strcmp(v->packet, "ArtSync") == 0) { check_sync(v); done++; }
    }
    g_context = NULL;
    CHECK(done >= 10);
}
