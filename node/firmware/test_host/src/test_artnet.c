/* pm_artnet codec against protocol/vectors/artnet (every vector, dispatched on v->packet) plus focused unit tests. */
#include "pm_artnet_packets.h"
#include "test.h"
#include "vectors.h"

void test_artnet_node(void);

/* ------------------------------------------------------------------------------------------------ helpers */

static pm_err_t expected_error(const pm_vec_t *v)
{
    const char *e = vec_str(v, "error");
    if (e == NULL) return PM_OK;
    if (strcmp(e, "short") == 0) return PM_ERR_SHORT;
    if (strcmp(e, "bad_id") == 0) return PM_ERR_BAD_ID;
    return PM_ERR_BAD_FORMAT;
}

/** Returns 1 when the vector is invalid (and checks the decoder's error) or decoding failed, 0 to go on. */
static int check_invalid(const pm_vec_t *v, pm_err_t e)
{
    if (!v->valid) {
        CHECK_EQ(e, expected_error(v));
        return 1;
    }
    CHECK_EQ(e, PM_OK);
    return e != PM_OK;
}

/** "a.b.c.d" -> host-order uint32 (0 on parse error, which then fails the comparison). */
static uint32_t parse_ip(const char *s)
{
    unsigned a, b, c, d;
    if (s == NULL || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

static void check_ip(const pm_vec_t *v, const char *key, uint32_t got)
{
    if (!vec_has(v, key)) return;
    CHECK_EQ(got, parse_ip(vec_str(v, key)));
}

static void check_num(const pm_vec_t *v, const char *key, long long got)
{
    if (!vec_has(v, key)) return;
    CHECK_EQ(got, vec_num(v, key));
}

static void check_str(const pm_vec_t *v, const char *key, const char *got)
{
    if (!vec_has(v, key)) return;
    const char *want = vec_str(v, key);
    CHECK(want != NULL && strcmp(got, want) == 0);
}

static void check_uid(const pm_vec_t *v, const char *key, const pm_uid_t *got)
{
    if (!vec_has(v, key)) return;
    pm_uid_t want;
    CHECK_EQ(pm_uid_parse(vec_str(v, key), &want), PM_OK);
    CHECK(pm_uid_equal(got, &want));
}

/** name[0..n-1] against a byte array (only the indices present in the vector). */
static void check_arr(const pm_vec_t *v, const char *name, const uint8_t *got, size_t n)
{
    char key[64];
    for (size_t i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "%s[%zu]", name, i);
        check_num(v, key, got[i]);
    }
}

static void check_hex(const pm_vec_t *v, const char *key, const uint8_t *got, size_t got_len)
{
    if (!vec_has(v, key)) return;
    static uint8_t want[2048];
    int n = vec_hex(vec_str(v, key), want, sizeof(want));
    CHECK_EQ(n, got_len);
    CHECK(n == (int)got_len && memcmp(want, got, got_len) == 0);
}

/** ProtVer of packets that have one (bytes 10/11, BE). */
static void check_prot_ver(const pm_vec_t *v)
{
    check_num(v, "prot_ver", pm_get_be16(v->bytes + 10));
}

/** Re-encoded bytes must equal the vector bytes. */
static void check_reencode(const pm_vec_t *v, int n, const uint8_t *buf)
{
    CHECK_EQ(n, v->len);
    CHECK(n == (int)v->len && memcmp(buf, v->bytes, v->len) == 0);
}

static uint8_t g_buf[PM_ARTNET_MAX_PACKET + 300];

/* ------------------------------------------------------------------------------------------------ per packet */

static void check_poll(const pm_vec_t *v)
{
    pm_artpoll_t p;
    if (check_invalid(v, pm_artpoll_decode(v->bytes, v->len, &p))) return;
    check_prot_ver(v);
    CHECK_EQ(p.flags, vec_num(v, "flags"));
    CHECK_EQ(p.diag_priority, vec_num(v, "diag_priority"));
    CHECK_EQ(p.target_top, vec_num(v, "target_port_address_top"));
    CHECK_EQ(p.target_bottom, vec_num(v, "target_port_address_bottom"));
    CHECK_EQ(p.esta_man, vec_num(v, "esta_man"));
    CHECK_EQ(p.oem, vec_num(v, "oem"));
    check_num(v, "targeted_mode", (p.flags & PM_POLL_FLAG_TARGETED) != 0);
    check_num(v, "reply_on_change", (p.flags & PM_POLL_FLAG_REPLY_ON_CHANGE) != 0);
    if (v->len == 22) check_reencode(v, pm_artpoll_encode(&p, g_buf, sizeof(g_buf)), g_buf);
}

static void check_poll_reply(const pm_vec_t *v)
{
    pm_artpollreply_t r;
    if (check_invalid(v, pm_artpollreply_decode(v->bytes, v->len, &r))) return;
    check_ip(v, "ip", r.ip);
    check_num(v, "port", pm_get_le16(v->bytes + 14));
    check_num(v, "vers_info", r.vers_info);
    check_num(v, "net_switch", r.net_switch);
    check_num(v, "sub_switch", r.sub_switch);
    check_num(v, "oem", r.oem);
    check_num(v, "ubea_version", r.ubea_version);
    check_num(v, "status1", r.status1);
    check_num(v, "esta_man", r.esta_man);
    check_str(v, "short_name", r.short_name);
    check_str(v, "long_name", r.long_name);
    check_str(v, "node_report", r.node_report);
    check_num(v, "num_ports", r.num_ports);
    check_arr(v, "port_types", r.port_types, 4);
    check_arr(v, "good_input", r.good_input, 4);
    check_arr(v, "good_output_a", r.good_output_a, 4);
    check_arr(v, "sw_in", r.sw_in, 4);
    check_arr(v, "sw_out", r.sw_out, 4);
    for (size_t i = 0; i < 4; i++) {
        char key[32];
        snprintf(key, sizeof(key), "port_addresses_out[%zu]", i);
        check_num(v, key, PM_PA_MAKE(r.net_switch, r.sub_switch, r.sw_out[i]));
    }
    /* one-port pages: Port-Address of port 0 in its current direction (PortTypes bit 7 = output) */
    check_num(v, "port_address",
              PM_PA_MAKE(r.net_switch, r.sub_switch, (r.port_types[0] & 0x80u) ? r.sw_out[0] : r.sw_in[0]));
    check_num(v, "acn_priority", r.acn_priority);
    check_num(v, "sw_macro", r.sw_macro);
    check_num(v, "sw_remote", r.sw_remote);
    check_num(v, "style", r.style);
    if (vec_has(v, "mac")) {
        char mac[18];
        snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", r.mac[0], r.mac[1], r.mac[2], r.mac[3],
                 r.mac[4], r.mac[5]);
        check_str(v, "mac", mac);
    }
    check_ip(v, "bind_ip", r.bind_ip);
    check_num(v, "bind_index", r.bind_index);
    check_num(v, "status2", r.status2);
    check_arr(v, "good_output_b", r.good_output_b, 4);
    check_num(v, "status3", r.status3);
    check_uid(v, "default_resp_uid", &r.default_resp_uid);
    check_num(v, "user", r.user);
    check_num(v, "refresh_rate", r.refresh_rate);
    check_num(v, "background_queue_policy", r.background_queue_policy);
    /* min_207 is truncated: no re-encode */
    if (v->len == PM_POLL_REPLY_LEN) check_reencode(v, pm_artpollreply_encode(&r, g_buf, sizeof(g_buf)), g_buf);
}

static void check_dmx(const pm_vec_t *v)
{
    pm_artdmx_t d;
    if (check_invalid(v, pm_artdmx_decode(v->bytes, v->len, &d))) return;
    check_prot_ver(v);
    CHECK_EQ(d.sequence, vec_num(v, "sequence"));
    CHECK_EQ(d.physical, vec_num(v, "physical"));
    CHECK_EQ(d.port_address, vec_num(v, "port_address"));
    check_num(v, "sub_uni", d.port_address & 0xFFu);
    CHECK_EQ(PM_PA_NET(d.port_address), vec_num(v, "net"));
    CHECK_EQ(PM_PA_SUB(d.port_address), vec_num(v, "sub_net"));
    CHECK_EQ(PM_PA_UNI(d.port_address), vec_num(v, "universe"));
    CHECK_EQ(d.length, vec_num(v, "length"));
    check_hex(v, "data", d.data, d.length);
    check_reencode(v, pm_artdmx_encode(&d, g_buf, sizeof(g_buf)), g_buf);
}

static void check_sync(const pm_vec_t *v)
{
    if (check_invalid(v, pm_artsync_decode(v->bytes, v->len))) return;
    check_prot_ver(v);
    check_num(v, "aux1", v->bytes[12]);
    check_num(v, "aux2", v->bytes[13]);
    check_reencode(v, pm_artsync_encode(g_buf, sizeof(g_buf)), g_buf);
}

static void check_address(const pm_vec_t *v)
{
    pm_artaddress_t a;
    if (check_invalid(v, pm_artaddress_decode(v->bytes, v->len, &a))) return;
    check_prot_ver(v);
    check_num(v, "net_switch", a.net_switch);
    check_num(v, "bind_index", a.bind_index);
    check_str(v, "short_name", a.short_name);
    check_str(v, "long_name", a.long_name);
    check_arr(v, "sw_in", a.sw_in, 4);
    check_arr(v, "sw_out", a.sw_out, 4);
    check_num(v, "sub_switch", a.sub_switch);
    check_num(v, "acn_priority", a.acn_priority);
    check_num(v, "command", a.command);
    for (size_t i = 0; i < 4; i++) {
        char key[32];
        snprintf(key, sizeof(key), "port_addresses_out[%zu]", i);
        check_num(v, key, PM_PA_MAKE(a.net_switch, a.sub_switch, a.sw_out[i]));
    }
    check_reencode(v, pm_artaddress_encode(&a, g_buf, sizeof(g_buf)), g_buf);
}

static void check_input(const pm_vec_t *v)
{
    pm_artinput_t in;
    if (check_invalid(v, pm_artinput_decode(v->bytes, v->len, &in))) return;
    check_prot_ver(v);
    check_num(v, "bind_index", in.bind_index);
    check_num(v, "num_ports", in.num_ports);
    check_arr(v, "input", in.input, 4);
    check_reencode(v, pm_artinput_encode(&in, g_buf, sizeof(g_buf)), g_buf);
}

static void check_ip_prog(const pm_vec_t *v)
{
    pm_artipprog_t p;
    if (check_invalid(v, pm_artipprog_decode(v->bytes, v->len, &p))) return;
    check_prot_ver(v);
    check_num(v, "command", p.command);
    check_ip(v, "prog_ip", p.ip);
    check_ip(v, "prog_sm", p.mask);
    check_num(v, "prog_port", p.port);
    check_ip(v, "prog_dg", p.gateway);
    check_reencode(v, pm_artipprog_encode(&p, g_buf, sizeof(g_buf)), g_buf);
}

static void check_ip_prog_reply(const pm_vec_t *v)
{
    pm_artipprogreply_t p;
    if (check_invalid(v, pm_artipprogreply_decode(v->bytes, v->len, &p))) return;
    check_prot_ver(v);
    check_ip(v, "prog_ip", p.ip);
    check_ip(v, "prog_sm", p.mask);
    check_num(v, "prog_port", p.port);
    check_num(v, "status", p.status);
    check_num(v, "dhcp_enabled", (p.status & 0x40u) != 0);
    check_ip(v, "prog_dg", p.gateway);
    check_reencode(v, pm_artipprogreply_encode(&p, g_buf, sizeof(g_buf)), g_buf);
}

static void check_tod_request(const pm_vec_t *v)
{
    pm_arttodrequest_t r;
    if (check_invalid(v, pm_arttodrequest_decode(v->bytes, v->len, &r))) return;
    check_prot_ver(v);
    check_num(v, "net", r.net);
    check_num(v, "command", r.command);
    check_num(v, "add_count", r.adcount);
    check_arr(v, "address", r.address, r.adcount);
    for (size_t i = 0; i < r.adcount; i++) {
        char key[32];
        snprintf(key, sizeof(key), "port_addresses[%zu]", i);
        check_num(v, key, ((r.net & 0x7Fu) << 8) | r.address[i]);
    }
    check_reencode(v, pm_arttodrequest_encode(&r, g_buf, sizeof(g_buf)), g_buf);
}

static void check_tod_data(const pm_vec_t *v)
{
    pm_arttoddata_t t;
    if (check_invalid(v, pm_arttoddata_decode(v->bytes, v->len, &t))) return;
    check_prot_ver(v);
    check_num(v, "rdm_ver", t.rdm_ver);
    check_num(v, "port", t.port);
    check_num(v, "bind_index", t.bind_index);
    check_num(v, "net", t.net);
    check_num(v, "command_response", t.command_response);
    check_num(v, "address", t.address);
    check_num(v, "port_address", ((t.net & 0x7Fu) << 8) | t.address);
    check_num(v, "uid_total", t.uid_total);
    check_num(v, "block_count", t.block_count);
    check_num(v, "uid_count", t.uid_count);
    char key[32];
    for (size_t i = 0; i < t.uid_count; i++) {
        snprintf(key, sizeof(key), "tod[%zu]", i);
        CHECK(vec_has(v, key));
        check_uid(v, key, &t.uids[i]);
    }
    snprintf(key, sizeof(key), "tod[%u]", (unsigned)t.uid_count);
    CHECK(!vec_has(v, key)); /* no more UIDs in the vector than decoded */
    check_reencode(v, pm_arttoddata_encode(&t, g_buf, sizeof(g_buf)), g_buf);
}

static void check_tod_control(const pm_vec_t *v)
{
    pm_arttodcontrol_t c;
    if (check_invalid(v, pm_arttodcontrol_decode(v->bytes, v->len, &c))) return;
    check_prot_ver(v);
    check_num(v, "net", c.net);
    check_num(v, "command", c.command);
    check_num(v, "address", c.address);
    check_num(v, "port_address", ((c.net & 0x7Fu) << 8) | c.address);
    check_reencode(v, pm_arttodcontrol_encode(&c, g_buf, sizeof(g_buf)), g_buf);
}

static void check_rdm(const pm_vec_t *v)
{
    pm_artrdm_t r;
    if (check_invalid(v, pm_artrdm_decode(v->bytes, v->len, &r))) return;
    check_prot_ver(v);
    check_num(v, "rdm_ver", r.rdm_ver);
    check_num(v, "fifo_avail", r.fifo_avail);
    check_num(v, "fifo_max", r.fifo_max);
    check_num(v, "net", r.net);
    check_num(v, "command", r.command);
    check_num(v, "address", r.address);
    check_num(v, "port_address", ((r.net & 0x7Fu) << 8) | r.address);
    check_hex(v, "rdm_packet", r.rdm, r.rdm_len);
    if (r.rdm_len >= 2) {
        check_num(v, "rdm_message_length", r.rdm[1]);
        check_num(v, "rdm_checksum", pm_get_be16(r.rdm + r.rdm_len - 2));
        /* E1.20 checksum = sum of 0xCC (not in the payload) + all bytes before the checksum */
        unsigned sum = 0xCC;
        for (size_t i = 0; i + 2 < r.rdm_len; i++) sum += r.rdm[i];
        CHECK_EQ(sum & 0xFFFFu, pm_get_be16(r.rdm + r.rdm_len - 2));
        CHECK_EQ(r.rdm_len, r.rdm[1] + 1u); /* Message Length - 1 (no start code) + 2 (checksum) */
    }
    check_reencode(v, pm_artrdm_encode(&r, g_buf, sizeof(g_buf)), g_buf);
}

static void check_firmware_master(const pm_vec_t *v)
{
    pm_artfirmwaremaster_t f;
    if (check_invalid(v, pm_artfirmwaremaster_decode(v->bytes, v->len, &f))) return;
    check_prot_ver(v);
    check_num(v, "type", f.type);
    check_num(v, "block_id", f.block_id);
    check_num(v, "firmware_length", f.firmware_length_words);
    check_hex(v, "data", f.data, PM_FIRM_BLOCK_BYTES);
    /* .alf header fields inside Data[] (digest 6.3): block 0 starts with the header, block 1 holds Length at 32 */
    check_num(v, "file_checksum", pm_get_be16(f.data));
    check_num(v, "file_vers_info", pm_get_be16(f.data + 2));
    if (vec_has(v, "file_user_name")) {
        char name[31];
        memcpy(name, f.data + 4, 30);
        name[30] = '\0';
        check_str(v, "file_user_name", name);
    }
    for (size_t i = 0; i < 256; i++) {
        char key[32];
        snprintf(key, sizeof(key), "file_oem[%zu]", i);
        check_num(v, key, pm_get_be16(f.data + 34 + 2 * i));
    }
    check_num(v, "file_length", pm_get_be32(f.data + (PM_ALF_HEADER_LEN - 4u - PM_FIRM_BLOCK_BYTES)));
    check_reencode(v, pm_artfirmwaremaster_encode(&f, g_buf, sizeof(g_buf)), g_buf);
}

static void check_firmware_reply(const pm_vec_t *v)
{
    uint8_t type;
    if (check_invalid(v, pm_artfirmwarereply_decode(v->bytes, v->len, &type))) return;
    check_prot_ver(v);
    check_num(v, "type", type);
    check_reencode(v, pm_artfirmwarereply_encode(type, g_buf, sizeof(g_buf)), g_buf);
}

/** Test-only ArtDiagData decoder (the node only sends ArtDiagData). */
static pm_err_t diag_decode(const uint8_t *pkt, size_t len, pm_artdiagdata_t *out, uint16_t *dlen)
{
    uint16_t op;
    pm_err_t e = pm_artnet_peek_opcode(pkt, len, &op);
    if (e != PM_OK) return e;
    if (op != PM_OP_DIAG_DATA) return PM_ERR_BAD_ID;
    if (len < 18) return PM_ERR_SHORT;
    if (pm_get_be16(pkt + 10) < PM_ARTNET_PROT_VER) return PM_ERR_BAD_FORMAT;
    *dlen = pm_get_be16(pkt + 16);
    if (*dlen == 0 || *dlen > 512) return PM_ERR_BAD_FORMAT;
    if (len < 18u + *dlen) return PM_ERR_SHORT;
    if (pkt[18 + *dlen - 1] != 0) return PM_ERR_BAD_FORMAT; /* must be NUL-terminated */
    out->priority = pkt[13];
    out->logical_port = pkt[14];
    out->text = (const char *)(pkt + 18);
    return PM_OK;
}

static void check_diag(const pm_vec_t *v)
{
    pm_artdiagdata_t d;
    uint16_t dlen = 0;
    if (check_invalid(v, diag_decode(v->bytes, v->len, &d, &dlen))) return;
    check_prot_ver(v);
    check_num(v, "diag_priority", d.priority);
    check_num(v, "logical_port", d.logical_port);
    check_num(v, "length", dlen);
    check_str(v, "data", d.text);
    check_reencode(v, pm_artdiagdata_encode(&d, g_buf, sizeof(g_buf)), g_buf);
}

typedef struct {
    const char *packet;
    void (*fn)(const pm_vec_t *v);
} packet_check_t;

static const packet_check_t PACKET_CHECKS[] = {
    {"ArtPoll", check_poll},
    {"ArtPollReply", check_poll_reply},
    {"ArtDmx", check_dmx},
    {"ArtSync", check_sync},
    {"ArtAddress", check_address},
    {"ArtInput", check_input},
    {"ArtIpProg", check_ip_prog},
    {"ArtIpProgReply", check_ip_prog_reply},
    {"ArtTodRequest", check_tod_request},
    {"ArtTodData", check_tod_data},
    {"ArtTodControl", check_tod_control},
    {"ArtRdm", check_rdm},
    {"ArtFirmwareMaster", check_firmware_master},
    {"ArtFirmwareReply", check_firmware_reply},
    {"ArtDiagData", check_diag},
};

/* ------------------------------------------------------------------------------------------------ unit tests */

/** Zeroed packet of `len` bytes with ID, OpCode and ProtVer 14. */
static void make_pkt(uint8_t *buf, size_t len, uint16_t op)
{
    memset(buf, 0, len);
    memcpy(buf, "Art-Net", 8);
    pm_put_le16(buf + 8, op);
    if (len >= 12) pm_put_be16(buf + 10, PM_ARTNET_PROT_VER);
}

static void test_no_space(void)
{
    g_context = "no_space";
    static uint8_t buf[PM_ARTNET_MAX_PACKET];
    pm_artpollreply_t pr;
    memset(&pr, 0, sizeof(pr));
    CHECK_EQ(pm_artpollreply_encode(&pr, buf, PM_POLL_REPLY_LEN - 1), PM_ERR_NO_SPACE);
    CHECK_EQ(pm_artpollreply_encode(&pr, buf, PM_POLL_REPLY_LEN), PM_POLL_REPLY_LEN);
    pm_artaddress_t ad;
    memset(&ad, 0, sizeof(ad));
    CHECK_EQ(pm_artaddress_encode(&ad, buf, 106), PM_ERR_NO_SPACE);
    pm_artinput_t in = {0};
    CHECK_EQ(pm_artinput_encode(&in, buf, 19), PM_ERR_NO_SPACE);
    pm_artipprog_t ip = {0};
    CHECK_EQ(pm_artipprog_encode(&ip, buf, 33), PM_ERR_NO_SPACE);
    pm_artipprogreply_t ipr = {0};
    CHECK_EQ(pm_artipprogreply_encode(&ipr, buf, 33), PM_ERR_NO_SPACE);
    pm_arttodrequest_t tr;
    memset(&tr, 0, sizeof(tr));
    CHECK_EQ(pm_arttodrequest_encode(&tr, buf, 55), PM_ERR_NO_SPACE);
    pm_uid_t uids[2];
    uids[0] = pm_uid_make(0x7FF0, 1);
    uids[1] = pm_uid_make(0x7FF0, 2);
    pm_arttoddata_t td = {.uid_count = 2, .uids = uids};
    CHECK_EQ(pm_arttoddata_encode(&td, buf, 39), PM_ERR_NO_SPACE);
    CHECK_EQ(pm_arttoddata_encode(&td, buf, 40), 40);
    pm_arttodcontrol_t tc = {0};
    CHECK_EQ(pm_arttodcontrol_encode(&tc, buf, 23), PM_ERR_NO_SPACE);
    uint8_t rdm[25] = {0x01, 0x18};
    pm_artrdm_t r = {.rdm = rdm, .rdm_len = sizeof(rdm)};
    CHECK_EQ(pm_artrdm_encode(&r, buf, 48), PM_ERR_NO_SPACE);
    CHECK_EQ(pm_artrdm_encode(&r, buf, 49), 49);
    static const uint8_t block[PM_FIRM_BLOCK_BYTES];
    pm_artfirmwaremaster_t fm = {.data = block};
    CHECK_EQ(pm_artfirmwaremaster_encode(&fm, buf, 1063), PM_ERR_NO_SPACE);
    CHECK_EQ(pm_artfirmwaremaster_encode(&fm, buf, sizeof(buf)), 1064);
    CHECK_EQ(pm_artfirmwarereply_encode(PM_FIRM_ALL_GOOD, buf, 35), PM_ERR_NO_SPACE);
    pm_artdiagdata_t dd = {.priority = 0x10, .text = "hello"};
    CHECK_EQ(pm_artdiagdata_encode(&dd, buf, 23), PM_ERR_NO_SPACE);
    CHECK_EQ(pm_artdiagdata_encode(&dd, buf, 24), 24);
    pm_artpoll_t p;
    memset(&p, 0, sizeof(p));
    CHECK_EQ(pm_artpoll_encode(&p, buf, 21), PM_ERR_NO_SPACE);
    CHECK_EQ(pm_artsync_encode(buf, 13), PM_ERR_NO_SPACE);
}

static void test_null_args(void)
{
    g_context = "null_args";
    static uint8_t pkt[1100];
    pm_artpollreply_t pr;
    pm_artaddress_t ad;
    pm_artinput_t in;
    pm_artipprog_t ip;
    pm_artipprogreply_t ipr;
    pm_arttodrequest_t tr;
    pm_arttoddata_t td;
    pm_arttodcontrol_t tc;
    pm_artrdm_t r;
    pm_artfirmwaremaster_t fm;
    uint8_t type;
    make_pkt(pkt, sizeof(pkt), PM_OP_POLL_REPLY);
    CHECK_EQ(pm_artpollreply_decode(NULL, sizeof(pkt), &pr), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artpollreply_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artaddress_decode(NULL, sizeof(pkt), &ad), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artaddress_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artinput_decode(NULL, sizeof(pkt), &in), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artinput_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artipprog_decode(NULL, sizeof(pkt), &ip), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artipprog_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artipprogreply_decode(NULL, sizeof(pkt), &ipr), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artipprogreply_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttodrequest_decode(NULL, sizeof(pkt), &tr), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttodrequest_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttoddata_decode(NULL, sizeof(pkt), &td), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttoddata_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttodcontrol_decode(NULL, sizeof(pkt), &tc), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttodcontrol_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artrdm_decode(NULL, sizeof(pkt), &r), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artrdm_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artfirmwaremaster_decode(NULL, sizeof(pkt), &fm), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artfirmwaremaster_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artfirmwarereply_decode(NULL, sizeof(pkt), &type), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artfirmwarereply_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artpoll_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artdmx_decode(pkt, sizeof(pkt), NULL), PM_ERR_INVALID_ARG);

    static uint8_t buf[1100];
    CHECK_EQ(pm_artpollreply_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    memset(&pr, 0, sizeof(pr));
    CHECK_EQ(pm_artpollreply_encode(&pr, NULL, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artaddress_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artinput_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artipprog_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artipprogreply_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttodrequest_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttoddata_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    pm_arttoddata_t td_null = {.uid_count = 1, .uids = NULL};
    CHECK_EQ(pm_arttoddata_encode(&td_null, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_arttodcontrol_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artrdm_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    pm_artrdm_t r_null = {.rdm = NULL, .rdm_len = 5};
    CHECK_EQ(pm_artrdm_encode(&r_null, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artfirmwaremaster_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    pm_artfirmwaremaster_t fm_null = {.data = NULL};
    CHECK_EQ(pm_artfirmwaremaster_encode(&fm_null, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artfirmwarereply_encode(0, NULL, sizeof(buf)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_artdiagdata_encode(NULL, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    pm_artdiagdata_t dd_null = {.text = NULL};
    CHECK_EQ(pm_artdiagdata_encode(&dd_null, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
}

/** Decode a full-size packet of `op` with every decoder that has ProtVer. */
static pm_err_t decode_any(uint16_t op, const uint8_t *pkt, size_t len)
{
    union {
        pm_artaddress_t ad;
        pm_artinput_t in;
        pm_artipprog_t ip;
        pm_artipprogreply_t ipr;
        pm_arttodrequest_t tr;
        pm_arttoddata_t td;
        pm_arttodcontrol_t tc;
        pm_artrdm_t r;
        pm_artfirmwaremaster_t fm;
        uint8_t type;
    } o;
    switch (op) {
    case PM_OP_ADDRESS: return pm_artaddress_decode(pkt, len, &o.ad);
    case PM_OP_INPUT: return pm_artinput_decode(pkt, len, &o.in);
    case PM_OP_IP_PROG: return pm_artipprog_decode(pkt, len, &o.ip);
    case PM_OP_IP_PROG_REPLY: return pm_artipprogreply_decode(pkt, len, &o.ipr);
    case PM_OP_TOD_REQUEST: return pm_arttodrequest_decode(pkt, len, &o.tr);
    case PM_OP_TOD_DATA: return pm_arttoddata_decode(pkt, len, &o.td);
    case PM_OP_TOD_CONTROL: return pm_arttodcontrol_decode(pkt, len, &o.tc);
    case PM_OP_RDM: return pm_artrdm_decode(pkt, len, &o.r);
    case PM_OP_FIRMWARE_MASTER: return pm_artfirmwaremaster_decode(pkt, len, &o.fm);
    default: return pm_artfirmwarereply_decode(pkt, len, &o.type);
    }
}

static void test_prot_ver_13(void)
{
    g_context = "prot_ver_13";
    static uint8_t pkt[1100];
    static const uint16_t ops[] = {PM_OP_ADDRESS, PM_OP_INPUT, PM_OP_IP_PROG, PM_OP_IP_PROG_REPLY,
                                   PM_OP_TOD_REQUEST, PM_OP_TOD_DATA, PM_OP_TOD_CONTROL, PM_OP_RDM,
                                   PM_OP_FIRMWARE_MASTER, PM_OP_FIRMWARE_REPLY};
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
        make_pkt(pkt, sizeof(pkt), ops[i]);
        pkt[24] = 0x01; /* ArtRdm: sub-start code */
        CHECK_EQ(decode_any(ops[i], pkt, sizeof(pkt)), PM_OK);
        pm_put_be16(pkt + 10, 13);
        CHECK_EQ(decode_any(ops[i], pkt, sizeof(pkt)), PM_ERR_BAD_FORMAT);
        pm_put_be16(pkt + 10, 14);
        pkt[0] = 'a';
        CHECK_EQ(decode_any(ops[i], pkt, sizeof(pkt)), PM_ERR_BAD_ID);
        pkt[0] = 'A';
        pm_put_le16(pkt + 8, PM_OP_POLL);
        CHECK_EQ(decode_any(ops[i], pkt, sizeof(pkt)), PM_ERR_BAD_ID);
    }
    /* ArtPollReply has no ProtVer: bytes 10/11 are the IP and never checked */
    pm_artpollreply_t pr;
    make_pkt(pkt, PM_POLL_REPLY_LEN, PM_OP_POLL_REPLY);
    pkt[10] = 0;
    pkt[11] = 0;
    CHECK_EQ(pm_artpollreply_decode(pkt, PM_POLL_REPLY_LEN, &pr), PM_OK);
}

static void test_trailing_bytes(void)
{
    static uint8_t pkt[2048];
    for (size_t i = 0; i < pm_vectors_count; i++) {
        const pm_vec_t *v = &pm_vectors[i];
        if (!v->valid || strcmp(v->protocol, "artnet") != 0) continue;
        if (v->len + 8 > sizeof(pkt)) continue;
        g_context = v->name;
        memcpy(pkt, v->bytes, v->len);
        memset(pkt + v->len, 0xA5, 8);
        size_t len = v->len + 8;
        if (strcmp(v->packet, "ArtAddress") == 0) {
            pm_artaddress_t a, b;
            CHECK_EQ(pm_artaddress_decode(v->bytes, v->len, &a), PM_OK);
            CHECK_EQ(pm_artaddress_decode(pkt, len, &b), PM_OK);
            CHECK(memcmp(&a, &b, sizeof(a)) == 0);
        } else if (strcmp(v->packet, "ArtTodData") == 0) {
            pm_arttoddata_t a, b;
            CHECK_EQ(pm_arttoddata_decode(v->bytes, v->len, &a), PM_OK);
            CHECK_EQ(pm_arttoddata_decode(pkt, len, &b), PM_OK);
            CHECK_EQ(a.uid_count, b.uid_count);
        } else if (strcmp(v->packet, "ArtRdm") == 0) {
            pm_artrdm_t a, b;
            CHECK_EQ(pm_artrdm_decode(v->bytes, v->len, &a), PM_OK);
            CHECK_EQ(pm_artrdm_decode(pkt, len, &b), PM_OK);
            CHECK_EQ(a.rdm_len, b.rdm_len); /* padding after the RDM checksum is trimmed */
        } else if (strcmp(v->packet, "ArtPollReply") == 0 && v->len == PM_POLL_REPLY_LEN) {
            pm_artpollreply_t a, b;
            CHECK_EQ(pm_artpollreply_decode(v->bytes, v->len, &a), PM_OK);
            CHECK_EQ(pm_artpollreply_decode(pkt, len, &b), PM_OK);
            CHECK(memcmp(&a, &b, sizeof(a)) == 0);
        } else if (strcmp(v->packet, "ArtTodRequest") == 0) {
            pm_arttodrequest_t a, b;
            CHECK_EQ(pm_arttodrequest_decode(v->bytes, v->len, &a), PM_OK);
            CHECK_EQ(pm_arttodrequest_decode(pkt, len, &b), PM_OK);
            CHECK(memcmp(&a, &b, sizeof(a)) == 0);
        } else if (strcmp(v->packet, "ArtFirmwareMaster") == 0) {
            pm_artfirmwaremaster_t a;
            CHECK_EQ(pm_artfirmwaremaster_decode(pkt, len, &a), PM_OK);
        } else if (strcmp(v->packet, "ArtIpProg") == 0) {
            pm_artipprog_t a;
            CHECK_EQ(pm_artipprog_decode(pkt, len, &a), PM_OK);
        } else if (strcmp(v->packet, "ArtDmx") == 0) {
            pm_artdmx_t a;
            CHECK_EQ(pm_artdmx_decode(pkt, len, &a), PM_OK);
        }
    }
}

static void test_short_and_bounds(void)
{
    g_context = "short_and_bounds";
    static uint8_t pkt[1100];
    /* ArtPollReply: 207 bytes decode with the tail read as 0 even if the buffer holds more */
    pm_artpollreply_t pr;
    make_pkt(pkt, sizeof(pkt), PM_OP_POLL_REPLY);
    memset(pkt + 207, 0xEE, 32);
    CHECK_EQ(pm_artpollreply_decode(pkt, 207, &pr), PM_OK);
    CHECK_EQ(pr.bind_index, 0);
    CHECK_EQ(pr.refresh_rate, 0);
    CHECK_EQ(pr.status3, 0);
    CHECK_EQ(pr.bind_ip, 0);
    CHECK_EQ(pm_artpollreply_decode(pkt, 206, &pr), PM_ERR_SHORT);
    CHECK_EQ(pm_artpollreply_decode(pkt, 9, &pr), PM_ERR_SHORT);
    /* unterminated name fields -> 17 / 63 chars + NUL */
    memset(pkt + 26, 'X', 18);
    memset(pkt + 44, 'Y', 64);
    memset(pkt + 108, 'Z', 64);
    CHECK_EQ(pm_artpollreply_decode(pkt, 207, &pr), PM_OK);
    CHECK_EQ(strlen(pr.short_name), 17);
    CHECK_EQ(strlen(pr.long_name), 63);
    CHECK_EQ(strlen(pr.node_report), 63);

    /* ArtTodRequest: AddCount 33 -> BAD_FORMAT, 24 + AddCount required */
    pm_arttodrequest_t tr;
    make_pkt(pkt, 56, PM_OP_TOD_REQUEST);
    pkt[23] = 33;
    CHECK_EQ(pm_arttodrequest_decode(pkt, 56, &tr), PM_ERR_BAD_FORMAT);
    pkt[23] = 4;
    CHECK_EQ(pm_arttodrequest_decode(pkt, 27, &tr), PM_ERR_SHORT);
    CHECK_EQ(pm_arttodrequest_decode(pkt, 28, &tr), PM_OK);
    CHECK_EQ(tr.adcount, 4);
    pkt[23] = 0;
    CHECK_EQ(pm_arttodrequest_decode(pkt, 24, &tr), PM_OK);
    tr.adcount = 33;
    CHECK_EQ(pm_arttodrequest_encode(&tr, pkt, sizeof(pkt)), PM_ERR_INVALID_ARG);

    /* ArtFirmwareMaster: the full 1064 bytes are needed */
    pm_artfirmwaremaster_t fm;
    make_pkt(pkt, 1064, PM_OP_FIRMWARE_MASTER);
    CHECK_EQ(pm_artfirmwaremaster_decode(pkt, 1063, &fm), PM_ERR_SHORT);
    CHECK_EQ(pm_artfirmwaremaster_decode(pkt, 1064, &fm), PM_OK);
    CHECK(fm.data == pkt + 40);

    /* ArtRdm: 24 bytes (no RDM data) is short; 1 byte is passed through as is */
    pm_artrdm_t r;
    make_pkt(pkt, 30, PM_OP_RDM);
    CHECK_EQ(pm_artrdm_decode(pkt, 24, &r), PM_ERR_SHORT);
    pkt[24] = 0x01;
    CHECK_EQ(pm_artrdm_decode(pkt, 25, &r), PM_OK);
    CHECK_EQ(r.rdm_len, 1);
    /* Message Length larger than the payload: hand over everything */
    pkt[25] = 0x30;
    CHECK_EQ(pm_artrdm_decode(pkt, 30, &r), PM_OK);
    CHECK_EQ(r.rdm_len, 6);
}

static void test_tod_data_max(void)
{
    g_context = "tod_data_max";
    static pm_uid_t uids[PM_TOD_DATA_MAX_UIDS + 1];
    for (uint32_t i = 0; i <= PM_TOD_DATA_MAX_UIDS; i++) uids[i] = pm_uid_make(0x7FF0, 0x1000u + i);
    static uint8_t buf[1300];
    pm_arttoddata_t t = {.rdm_ver = 1, .port = 1, .bind_index = 3, .net = 0x12, .address = 0x34,
                         .uid_total = 450, .block_count = 1, .uid_count = 200, .uids = uids};
    int n = pm_arttoddata_encode(&t, buf, sizeof(buf));
    CHECK_EQ(n, 1228);
    pm_arttoddata_t d;
    CHECK_EQ(pm_arttoddata_decode(buf, (size_t)n, &d), PM_OK);
    CHECK_EQ(d.uid_count, 200);
    CHECK_EQ(d.uid_total, 450);
    CHECK_EQ(d.block_count, 1);
    CHECK_EQ(d.bind_index, 3);
    CHECK_EQ(d.net, 0x12);
    CHECK_EQ(d.address, 0x34);
    CHECK((const void *)d.uids == (const void *)(buf + 28));
    CHECK(pm_uid_equal(&d.uids[0], &uids[0]));
    CHECK(pm_uid_equal(&d.uids[199], &uids[199]));
    CHECK_EQ(pm_arttoddata_decode(buf, (size_t)n - 1, &d), PM_ERR_SHORT);
    /* 201 UIDs: rejected on encode and on decode */
    t.uid_count = 201;
    CHECK_EQ(pm_arttoddata_encode(&t, buf, sizeof(buf)), PM_ERR_INVALID_ARG);
    static uint8_t big[28 + 6 * 201];
    make_pkt(big, sizeof(big), PM_OP_TOD_DATA);
    big[27] = 201;
    CHECK_EQ(pm_arttoddata_decode(big, sizeof(big), &d), PM_ERR_BAD_FORMAT);
}

static void test_rdm_padding(void)
{
    g_context = "rdm_padding";
    /* RDM GET DEVICE_INFO without start code (25 bytes, Message Length 0x18) followed by 3 padding bytes */
    static const uint8_t rdm[] = {0x01, 0x18, 0x7f, 0xf0, 0x00, 0x00, 0x00, 0x01, 0x7f, 0xf0, 0x80, 0x12, 0x34,
                                  0x56, 0x05, 0x01, 0x00, 0x00, 0x00, 0x20, 0x00, 0x60, 0x00, 0x05, 0x66};
    uint8_t buf[64];
    pm_artrdm_t in = {.rdm_ver = 1, .fifo_avail = 2, .fifo_max = 4, .net = 1, .address = 0x23, .rdm = rdm,
                      .rdm_len = sizeof(rdm)};
    int n = pm_artrdm_encode(&in, buf, sizeof(buf));
    CHECK_EQ(n, 24 + 25);
    buf[49] = 0;
    buf[50] = 0;
    buf[51] = 0;
    pm_artrdm_t out;
    CHECK_EQ(pm_artrdm_decode(buf, 52, &out), PM_OK);
    CHECK_EQ(out.rdm_len, 25);
    CHECK(out.rdm == buf + 24);
    CHECK_EQ(out.rdm_ver, 1);
    CHECK_EQ(out.fifo_avail, 2);
    CHECK_EQ(out.fifo_max, 4);
    CHECK_EQ(out.net, 1);
    CHECK_EQ(out.address, 0x23);
    /* one byte missing: the message does not fit -> all remaining bytes */
    CHECK_EQ(pm_artrdm_decode(buf, 48, &out), PM_OK);
    CHECK_EQ(out.rdm_len, 24);
}

static void test_string_truncation(void)
{
    g_context = "string_truncation";
    uint8_t buf[PM_POLL_REPLY_LEN];
    pm_artpollreply_t r;
    memset(&r, 0, sizeof(r));
    memset(r.short_name, 'S', sizeof(r.short_name)); /* not NUL-terminated: encode must stop at 17 / 63 */
    memset(r.long_name, 'L', sizeof(r.long_name));
    memset(r.node_report, 'N', sizeof(r.node_report));
    CHECK_EQ(pm_artpollreply_encode(&r, buf, sizeof(buf)), PM_POLL_REPLY_LEN);
    CHECK_EQ(buf[26 + 16], 'S');
    CHECK_EQ(buf[26 + 17], 0);
    CHECK_EQ(buf[44 + 62], 'L');
    CHECK_EQ(buf[44 + 63], 0);
    CHECK_EQ(buf[108 + 62], 'N');
    CHECK_EQ(buf[108 + 63], 0);
    CHECK_EQ(pm_get_le16(buf + 14), PM_ARTNET_PORT);
    pm_artpollreply_t d;
    CHECK_EQ(pm_artpollreply_decode(buf, sizeof(buf), &d), PM_OK);
    CHECK_EQ(strlen(d.short_name), 17);
    CHECK_EQ(strlen(d.long_name), 63);
    CHECK_EQ(strlen(d.node_report), 63);

    uint8_t abuf[107];
    pm_artaddress_t a;
    memset(&a, 0, sizeof(a));
    memset(a.short_name, 'x', sizeof(a.short_name));
    memset(a.long_name, 'y', sizeof(a.long_name));
    CHECK_EQ(pm_artaddress_encode(&a, abuf, sizeof(abuf)), 107);
    CHECK_EQ(abuf[14 + 16], 'x');
    CHECK_EQ(abuf[14 + 17], 0);
    CHECK_EQ(abuf[32 + 63], 0);
    pm_artaddress_t ad;
    CHECK_EQ(pm_artaddress_decode(abuf, sizeof(abuf), &ad), PM_OK);
    CHECK_EQ(strlen(ad.short_name), 17);
    CHECK_EQ(strlen(ad.long_name), 63);

    /* ArtDiagData: text longer than 511 chars is cut to 511 + NUL */
    static char text[700];
    memset(text, 'd', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    static uint8_t dbuf[600];
    pm_artdiagdata_t dd = {.priority = 0xE0, .logical_port = 3, .text = text};
    CHECK_EQ(pm_artdiagdata_encode(&dd, dbuf, sizeof(dbuf)), 18 + 512);
    CHECK_EQ(pm_get_be16(dbuf + 16), 512);
    CHECK_EQ(dbuf[18 + 510], 'd');
    CHECK_EQ(dbuf[18 + 511], 0);
    CHECK_EQ(dbuf[13], 0xE0);
    CHECK_EQ(dbuf[14], 3);
    dd.text = "";
    CHECK_EQ(pm_artdiagdata_encode(&dd, dbuf, sizeof(dbuf)), 19);
    CHECK_EQ(pm_get_be16(dbuf + 16), 1);
}

/* ------------------------------------------------------------------------------------------------ entry */

void test_artnet(void)
{
    enum { NCHECKS = sizeof(PACKET_CHECKS) / sizeof(PACKET_CHECKS[0]) };
    int seen[NCHECKS] = {0};
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
        size_t k = 0;
        while (k < NCHECKS && strcmp(PACKET_CHECKS[k].packet, v->packet) != 0) k++;
        CHECK(k < NCHECKS); /* every artnet vector file has a checker */
        if (k == NCHECKS) continue;
        PACKET_CHECKS[k].fn(v);
        seen[k]++;
        done++;
    }
    g_context = "coverage";
    for (size_t k = 0; k < NCHECKS; k++) CHECK(seen[k] > 0); /* every packet type has vectors */
    CHECK(done >= 50);

    test_no_space();
    test_null_args();
    test_prot_ver_13();
    test_trailing_bytes();
    test_short_and_bounds();
    test_tod_data_max();
    test_rdm_padding();
    test_string_truncation();
    g_context = NULL;

    test_artnet_node();
}
