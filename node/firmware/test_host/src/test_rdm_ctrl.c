/* pm_rdm_ctrl against the simulated RS-485 bus (test_rdm_sim.c): discovery, background search, presence check,
 * request queue, BOOT_CATCH, DMX rate. Plus the node vectors of protocol/vectors/rdm/RDM_NODE.json. */
#include <stdlib.h>

#include "pm_pids.h"
#include "pm_rdm_ctrl.h"
#include "pm_rdm_responder.h"
#include "test.h"
#include "sim_bus.h"
#include "vectors.h"

typedef struct {
    int tod_events;
    int responses;
    uint32_t last_token;
    uint8_t last_resp[PM_RDM_MAX_MSG_LEN];
    uint16_t last_len;
    int catch_events;
    uint8_t catch_states[16];
} rec_t;

static rec_t g_rec;
static sim_bus_t g_bus; /* large: static */
static pm_rdm_ctrl_t g_ctrl;

static const pm_uid_t NODE_UID = {{0x7F, 0xF0, 0x80, 0x12, 0x34, 0x56}};

static void cb_response(void *ctx, pm_port_idx_t port, uint32_t token, const uint8_t *data, uint16_t len)
{
    rec_t *r = ctx;
    (void)port;
    r->responses++;
    r->last_token = token;
    r->last_len = len;
    memcpy(r->last_resp, data, len);
}

static void cb_tod(void *ctx, pm_port_idx_t port)
{
    (void)port;
    ((rec_t *)ctx)->tod_events++;
}

static void cb_catch(void *ctx, pm_port_idx_t port, uint8_t state)
{
    rec_t *r = ctx;
    (void)port;
    if (r->catch_events < 16) r->catch_states[r->catch_events] = state;
    r->catch_events++;
}

static void setup(uint32_t seed)
{
    memset(&g_rec, 0, sizeof(g_rec));
    sim_init(&g_bus, seed);
    pm_rdm_ctrl_cb_t cb = {&g_rec, cb_response, cb_tod, cb_catch};
    pm_rdm_ctrl_init(&g_ctrl, 0, &NODE_UID, &cb);
}

static bool disc_done(void *ctx) { return !pm_rdm_ctrl_discovery_running((const pm_rdm_ctrl_t *)ctx); }

/* TOD == present devices (that ACK mute), sorted, no duplicates */
static bool tod_matches_bus(void)
{
    static pm_uid_t tod[PM_RDM_TOD_MAX];
    uint16_t n = pm_rdm_ctrl_tod(&g_ctrl, tod, PM_RDM_TOD_MAX);
    int expect = 0;
    for (int i = 0; i < g_bus.n; i++) {
        const sim_dev_t *d = &g_bus.dev[i];
        if (d->present && !d->never_ack_mute) expect++;
    }
    if (n != expect) {
        fprintf(stderr, "TOD %u, bus %d\n", n, expect);
        for (int i = 0; i < g_bus.n; i++) {
            char sb[PM_UID_STR_LEN];
            if (g_bus.dev[i].present && !g_bus.dev[i].never_ack_mute && !pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[i].uid, NULL))
                fprintf(stderr, "  missing %s muted=%d\n", pm_uid_format(&g_bus.dev[i].uid, sb), g_bus.dev[i].muted);
        }
        return false;
    }
    for (uint16_t i = 0; i < n; i++) {
        if (i && pm_uid_to_u64(&tod[i - 1]) >= pm_uid_to_u64(&tod[i])) return false;
        int k = sim_find(&g_bus, &tod[i]);
        if (k < 0 || !g_bus.dev[k].present || g_bus.dev[k].never_ack_mute) return false;
    }
    return true;
}

static void full_discovery(uint32_t limit_ms, sim_port_stats_t *st)
{
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, g_bus.now_ms);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + limit_ms, 512, disc_done, &g_ctrl, st);
}

/* ------------------------------------------------------------------------------------------------ discovery */

static void test_full_counts(void)
{
    static const int counts[] = {0, 1, 2, 3, 17, 100, 300};
    for (unsigned k = 0; k < sizeof(counts) / sizeof(counts[0]); k++) {
        for (uint32_t seed = 1; seed <= 4; seed++) {
            setup(seed * 7919u + k);
            g_bus.garbage_percent = (seed % 2u) ? 50u : 0u;
            sim_add_random(&g_bus, counts[k]);
            sim_port_stats_t st = {0};
            full_discovery(600000, &st);
            g_context = "full discovery";
            CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));
            CHECK(tod_matches_bus());
            CHECK(st.max_period_us <= 1000000u / 30u); /* DMX >= 30 Hz throughout */
            CHECK_EQ(st.est_violations, 0);
            CHECK(g_rec.tod_events >= (counts[k] ? 1 : 0));
            pm_rdm_ctrl_stats_t s;
            pm_rdm_ctrl_get_stats(&g_ctrl, &s);
            CHECK_EQ(s.full_discoveries, 1);
            if (counts[k] >= 17) CHECK(s.collisions > 0);
        }
    }
    g_context = NULL;
}

static void test_full_edges(void)
{
    g_context = "full discovery edge UIDs";
    setup(42);
    sim_add(&g_bus, pm_uid_make(0x0000, 0x00000000));
    sim_add(&g_bus, pm_uid_make(0x0000, 0x00000001));
    sim_add(&g_bus, pm_uid_make(0x7FFF, 0xFFFFFFFE));
    sim_add(&g_bus, pm_uid_make(0x7FFF, 0xFFFFFFFD));
    sim_add(&g_bus, pm_uid_make(0x7FF0, 0x7FFFFFFF)); /* unsigned 48-bit compare */
    sim_add(&g_bus, pm_uid_make(0x7FF0, 0x80000000));
    sim_add(&g_bus, pm_uid_make(0x8000, 0x00000001)); /* not a valid ESTA ID, still a UID */
    for (int i = 0; i < g_bus.n; i++) g_bus.dev[i].preamble = 7; /* aligned collisions: wired-AND phantoms */
    full_discovery(120000, NULL);
    CHECK(tod_matches_bus());
    g_context = NULL;
}

static void test_flags_and_quirks(void)
{
    g_context = "control field flags";
    setup(5);
    int a = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x100));
    int p = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x200));
    int s = sim_add(&g_bus, pm_uid_make(0x0123, 0x300));
    int t = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x400));
    g_bus.dev[p].control = PM_RDM_MUTE_MANAGED_PROXY;
    g_bus.dev[p].binding = true;
    g_bus.dev[s].control = PM_RDM_MUTE_SUB_DEVICE;
    g_bus.dev[t].tube = true;
    g_bus.dev[t].app_valid = false;
    g_bus.dev[t].mode = SIM_BOOT; /* bootloader without valid app: boot-loader flag */
    full_discovery(60000, NULL);
    CHECK(tod_matches_bus());
    uint16_t cf = 0xFFFF;
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[a].uid, &cf));
    CHECK_EQ(cf, 0);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[p].uid, &cf));
    CHECK_EQ(cf, PM_RDM_MUTE_MANAGED_PROXY | PM_RDM_TOD_HAS_BINDING_UID);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[s].uid, &cf));
    CHECK_EQ(cf, PM_RDM_MUTE_SUB_DEVICE);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[t].uid, &cf));
    CHECK_EQ(cf, PM_RDM_MUTE_BOOT_LOADER);
    pm_uid_t none = pm_uid_make(0x7FF0, 0x999);
    CHECK(!pm_rdm_ctrl_tod_find(&g_ctrl, &none, NULL));

    g_context = "broken responders";
    for (uint32_t seed = 1; seed <= 3; seed++) {
        setup(100 + seed);
        sim_add_random(&g_bus, 40);
        int phantom = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x12340040u)); /* answers DUB, never ACKs mute */
        g_bus.dev[phantom].never_ack_mute = true;
        int deaf = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x12340041u)); /* ACKs mute, keeps answering DUB */
        g_bus.dev[deaf].ignore_mute = true;
        full_discovery(600000, NULL);
        CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));
        CHECK(tod_matches_bus());
        CHECK(!pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[phantom].uid, NULL));
        CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[deaf].uid, NULL));
    }

    g_context = "responder answering every DUB";
    setup(150);
    sim_add_random(&g_bus, 20);
    int wild = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x77777777));
    g_bus.dev[wild].ignore_range = true;
    g_bus.dev[wild].ignore_mute = true;
    full_discovery(600000, NULL);
    CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));
    pm_rdm_ctrl_stats_t ws;
    pm_rdm_ctrl_get_stats(&g_ctrl, &ws);
    CHECK(ws.dubs < 20000);
    CHECK_EQ(ws.corrupt_runs, 1);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[wild].uid, NULL));

    g_context = "noisy line";
    for (uint32_t seed = 1; seed <= 3; seed++) {
        setup(200 + seed);
        g_bus.noise_permille = 20;
        g_bus.garbage_percent = 30;
        sim_add_random(&g_bus, 60);
        full_discovery(600000, NULL);
        CHECK(tod_matches_bus());
    }
    g_context = NULL;
}

static void test_tod_limit(void)
{
    g_context = "TOD 512";
    setup(77);
    sim_add_random(&g_bus, 520);
    full_discovery(3600000, NULL);
    CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));
    static pm_uid_t tod[PM_RDM_TOD_MAX + 8];
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, tod, PM_RDM_TOD_MAX + 8), PM_RDM_TOD_MAX);
    pm_rdm_ctrl_stats_t s;
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    CHECK_EQ(s.tod_overflow, 8);
    int muted = 0;
    for (int i = 0; i < g_bus.n; i++) muted += g_bus.dev[i].muted;
    CHECK_EQ(muted, 520); /* the overflow devices are muted too (no endless DUB answers) */
    for (unsigned i = 1; i < PM_RDM_TOD_MAX; i++) CHECK(pm_uid_to_u64(&tod[i - 1]) < pm_uid_to_u64(&tod[i]));

    g_context = "TOD full + broken responders";
    setup(78);
    sim_add_random(&g_bus, 515);
    for (int i = 0; i < 8; i++) g_bus.dev[i * 60].ignore_mute = true;
    full_discovery(3600000, NULL);
    CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, NULL, 0), PM_RDM_TOD_MAX);
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    CHECK_EQ(s.tod_overflow, 3);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ background */

static uint32_t count_kind_until(uint32_t until_ms, pm_rdm_txn_kind_t kind)
{
    uint32_t before = kind == PM_RDM_TXN_DUB ? g_bus.dubs : g_bus.txns;
    sim_run(&g_bus, &g_ctrl, until_ms, 512, NULL, NULL, NULL);
    return (kind == PM_RDM_TXN_DUB ? g_bus.dubs : g_bus.txns) - before;
}

static void test_background(void)
{
    g_context = "background";
    setup(300);
    sim_add_random(&g_bus, 20);
    pm_rdm_ctrl_set_background(&g_ctrl, true, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    sim_run(&g_bus, &g_ctrl, 60000, 512, disc_done, &g_ctrl, NULL);
    CHECK(tod_matches_bus());
    uint32_t t_end = g_bus.now_ms;
    int ev = g_rec.tod_events;

    /* nothing before the interval has passed */
    CHECK_EQ(count_kind_until(t_end + 2900, PM_RDM_TXN_REQUEST), 0);
    /* quiet bus: background runs, TOD unchanged, no events, all devices stay muted */
    sim_run(&g_bus, &g_ctrl, t_end + 10000, 512, NULL, NULL, NULL);
    pm_rdm_ctrl_stats_t s;
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    CHECK(s.bg_discoveries >= 2);
    CHECK_EQ(g_rec.tod_events, ev);
    CHECK(tod_matches_bus());
    for (int i = 0; i < g_bus.n; i++) CHECK(g_bus.dev[i].muted);

    /* new device appears */
    int nd = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x55555555));
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 8000, 512, NULL, NULL, NULL);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[nd].uid, NULL));
    CHECK(g_rec.tod_events > ev);
    CHECK(tod_matches_bus());

    /* un-mute 3x per background run (OLA incremental discovery) */
    uint32_t bc = g_bus.broadcasts;
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    uint32_t runs0 = s.bg_discoveries;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 7000, 512, NULL, NULL, NULL);
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    CHECK(s.bg_discoveries > runs0);
    CHECK(g_bus.broadcasts - bc >= PM_RDM_UNMUTE_REPEATS * (s.bg_discoveries - runs0 - 1));

    /* device disappears: gone after its presence check */
    ev = g_rec.tod_events;
    g_bus.dev[3].present = false;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 8000, 512, NULL, NULL, NULL);
    CHECK(!pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[3].uid, NULL));
    CHECK(g_rec.tod_events > ev);
    /* ... and comes back when replugged although it never lost power (still muted) */
    CHECK(g_bus.dev[3].muted);
    g_bus.dev[3].present = true;
    ev = g_rec.tod_events;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 8000, 512, NULL, NULL, NULL);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[3].uid, NULL));
    CHECK(g_rec.tod_events > ev);
    CHECK(tod_matches_bus());

    /* device power-cycles (comes back un-muted): stays listed, re-muted, no TOD change */
    sim_power_cycle(&g_bus, 5, 500);
    g_bus.dev[5].tube = false;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 600, 512, NULL, NULL, NULL);
    ev = g_rec.tod_events;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 8000, 512, NULL, NULL, NULL);
    CHECK(g_bus.dev[5].muted);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[5].uid, NULL));
    CHECK(tod_matches_bus());

    /* one missed DISC_MUTE is not enough to drop a device: drop answers for a short time only */
    CHECK_EQ(g_rec.tod_events, ev);

    /* background off: no discovery traffic at all */
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 200, 512, NULL, NULL, NULL); /* let a running run be stopped */
    uint32_t tx = g_bus.txns;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 20000, 512, NULL, NULL, NULL);
    CHECK_EQ(g_bus.txns, tx);
    CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));

    /* interval: 1 s -> roughly 1 run per second */
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    uint32_t runs = s.bg_discoveries;
    pm_rdm_ctrl_set_background(&g_ctrl, true, 1000);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 10000, 512, NULL, NULL, NULL);
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    CHECK(s.bg_discoveries - runs >= 6 && s.bg_discoveries - runs <= 10);
    g_context = NULL;
}

static void test_presence_misses(void)
{
    g_context = "presence misses";
    setup(400);
    int a = sim_add(&g_bus, pm_uid_make(0x7FF0, 0xA));
    sim_add(&g_bus, pm_uid_make(0x7FF0, 0xB));
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    sim_run(&g_bus, &g_ctrl, 10000, 512, disc_done, &g_ctrl, NULL);
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, NULL, 0), 2);
    /* drive the presence check by hand: first DISC_MUTE to A unanswered twice, third answered -> stays */
    pm_rdm_ctrl_set_background(&g_ctrl, true, 100);
    uint32_t now = g_bus.now_ms;
    pm_rdm_txn_t t;
    CHECK(!pm_rdm_ctrl_next(&g_ctrl, now, &t)); /* schedules the first background run */
    now += 100;
    pm_rdm_rx_t none = {PM_RDM_RX_TIMEOUT, 0, NULL};
    for (unsigned k = 0; k < PM_RDM_UNMUTE_REPEATS; k++) { /* background runs start with un-mute too (OLA) */
        CHECK(pm_rdm_ctrl_next(&g_ctrl, now, &t));
        CHECK_EQ(t.kind, PM_RDM_TXN_BROADCAST);
        CHECK_EQ(pm_get_be16(t.data + 21), PM_RDM_PID_DISC_UN_MUTE);
        sim_transact(&g_bus, &t, &none);
        pm_rdm_ctrl_done(&g_ctrl, now, NULL);
    }
    none.status = PM_RDM_RX_TIMEOUT;
    none.len = 0;
    for (int k = 0; k < (int)PM_RDM_PRESENCE_MAX_MISSES - 1; k++) {
        CHECK(pm_rdm_ctrl_next(&g_ctrl, now, &t));
        CHECK_EQ(t.kind, PM_RDM_TXN_REQUEST);
        CHECK(memcmp(t.data + 3, g_bus.dev[a].uid.b, 6) == 0);
        CHECK_EQ(pm_get_be16(t.data + 21), PM_RDM_PID_DISC_MUTE);
        pm_rdm_ctrl_done(&g_ctrl, now, &none);
    }
    CHECK(pm_rdm_ctrl_next(&g_ctrl, now, &t));
    CHECK(memcmp(t.data + 3, g_bus.dev[a].uid.b, 6) == 0);
    pm_rdm_rx_t rx;
    sim_transact(&g_bus, &t, &rx);
    pm_rdm_ctrl_done(&g_ctrl, now, &rx);
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, NULL, 0), 2);
    /* B: three misses -> removed */
    for (int k = 0; k < (int)PM_RDM_PRESENCE_MAX_MISSES; k++) {
        CHECK(pm_rdm_ctrl_next(&g_ctrl, now, &t));
        CHECK_EQ(pm_get_be16(t.data + 21), PM_RDM_PID_DISC_MUTE);
        pm_rdm_ctrl_done(&g_ctrl, now, &none);
    }
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, NULL, 0), 1);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ requests */

static pm_rdm_request_t make_req(uint32_t token, pm_uid_t dest, uint8_t cc, uint16_t pid, uint8_t tn)
{
    pm_rdm_request_t r;
    memset(&r, 0, sizeof(r));
    pm_rdm_msg_t m;
    memset(&m, 0, sizeof(m));
    m.dest = dest;
    m.src = pm_uid_make(0x7FF0, 0xA0000001);
    m.tn = tn;
    m.port_or_resp = 1;
    m.cc = cc;
    m.pid = pid;
    uint8_t one = 1;
    if (cc == PM_RDM_CC_SET) {
        m.pd = &one;
        m.pdl = 1;
    }
    r.token = token;
    r.len = (uint16_t)pm_rdm_encode(&m, r.data, sizeof(r.data));
    return r;
}

static void test_requests(void)
{
    g_context = "request queue";
    setup(500);
    int a = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x1001));
    sim_add(&g_bus, pm_uid_make(0x7FF0, 0x1002));
    int other = sim_add(&g_bus, pm_uid_make(0x0123, 0x1003));
    pm_rdm_request_t r = make_req(1, g_bus.dev[a].uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, 0x42);
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_ERR_STATE); /* disabled */
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);

    /* requests come before discovery steps, TN and source kept */
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    CHECK_EQ(pm_rdm_ctrl_queue_free(&g_ctrl), PM_RDM_QUEUE_LEN - 1);
    pm_rdm_txn_t t;
    CHECK(pm_rdm_ctrl_next(&g_ctrl, 0, &t));
    CHECK_EQ(t.kind, PM_RDM_TXN_REQUEST);
    CHECK_EQ(t.len, r.len);
    CHECK(memcmp(t.data, r.data, r.len) == 0);
    CHECK(t.est_us > PM_RDM_RESPONSE_TIMEOUT_US);
    pm_rdm_rx_t rx;
    sim_transact(&g_bus, &t, &rx);
    pm_rdm_ctrl_done(&g_ctrl, 5, &rx);
    CHECK_EQ(g_rec.responses, 1);
    CHECK_EQ(g_rec.last_token, 1);
    pm_rdm_msg_t m;
    CHECK_EQ(pm_rdm_decode(g_rec.last_resp, g_rec.last_len, &m), PM_OK);
    CHECK_EQ(m.tn, 0x42);
    CHECK_EQ(m.pid, PM_RDM_PID_DEVICE_INFO);
    CHECK_EQ(m.pdl, 19);

    /* finish discovery, then a mix of requests */
    sim_run(&g_bus, &g_ctrl, 20000, 512, disc_done, &g_ctrl, NULL);
    CHECK(tod_matches_bus());
    g_rec.responses = 0;

    /* full queue */
    for (unsigned i = 0; i < PM_RDM_QUEUE_LEN; i++) {
        r = make_req(10 + i, g_bus.dev[a].uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, (uint8_t)i);
        CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    }
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_ERR_FULL);
    CHECK_EQ(pm_rdm_ctrl_queue_free(&g_ctrl), 0);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 1000, 512, NULL, NULL, NULL);
    CHECK_EQ(g_rec.responses, (int)PM_RDM_QUEUE_LEN);
    CHECK_EQ(g_rec.last_token, 10 + PM_RDM_QUEUE_LEN - 1);
    CHECK_EQ(pm_rdm_ctrl_queue_free(&g_ctrl), PM_RDM_QUEUE_LEN);

    /* UID outside the TOD: forwarded, no answer -> no callback */
    g_rec.responses = 0;
    uint32_t sent = g_bus.txns;
    r = make_req(30, pm_uid_make(0x7FF0, 0xDEAD), PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, 1);
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 100, 512, NULL, NULL, NULL);
    CHECK_EQ(g_bus.txns, sent + 1);
    CHECK_EQ(g_rec.responses, 0);

    /* broadcast and vendorcast SET: BROADCAST kind, executed by the devices, no response */
    uint32_t req_before[3];
    for (int i = 0; i < 3; i++) req_before[i] = g_bus.dev[i].requests;
    pm_uid_t bc = pm_uid_make(0xFFFF, 0xFFFFFFFF);
    r = make_req(31, bc, PM_RDM_CC_SET, PM_RDM_PID_IDENTIFY_DEVICE, 2);
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    CHECK(pm_rdm_ctrl_next(&g_ctrl, g_bus.now_ms, &t));
    CHECK_EQ(t.kind, PM_RDM_TXN_BROADCAST);
    CHECK(t.est_us < 3000);
    sim_transact(&g_bus, &t, &rx);
    pm_rdm_ctrl_done(&g_ctrl, g_bus.now_ms, &rx);
    for (int i = 0; i < 3; i++) CHECK_EQ(g_bus.dev[i].requests, req_before[i] + 1);
    r = make_req(32, pm_uid_make(0x7FF0, 0xFFFFFFFF), PM_RDM_CC_SET, PM_RDM_PID_IDENTIFY_DEVICE, 3);
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    CHECK(pm_rdm_ctrl_next(&g_ctrl, g_bus.now_ms, &t));
    CHECK_EQ(t.kind, PM_RDM_TXN_BROADCAST);
    sim_transact(&g_bus, &t, &rx);
    pm_rdm_ctrl_done(&g_ctrl, g_bus.now_ms, &rx);
    CHECK_EQ(g_bus.dev[other].requests, req_before[other] + 1); /* foreign manufacturer ignores 7FF0 vendorcast */
    CHECK_EQ(g_bus.dev[a].requests, req_before[a] + 2);
    CHECK_EQ(g_rec.responses, 0);

    /* forwarded DUB: DUB kind, raw answer not forwarded */
    uint8_t pd[12];
    memset(pd, 0, 6);
    memset(pd + 6, 0xFF, 6);
    pm_rdm_msg_t dm;
    memset(&dm, 0, sizeof(dm));
    dm.dest = bc;
    dm.src = pm_uid_make(0x7FF0, 0xA0000001);
    dm.cc = PM_RDM_CC_DISCOVERY;
    dm.pid = PM_RDM_PID_DISC_UNIQUE_BRANCH;
    dm.pd = pd;
    dm.pdl = 12;
    r.token = 33;
    r.len = (uint16_t)pm_rdm_encode(&dm, r.data, sizeof(r.data));
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    CHECK(pm_rdm_ctrl_next(&g_ctrl, g_bus.now_ms, &t));
    CHECK_EQ(t.kind, PM_RDM_TXN_DUB);
    pm_rdm_ctrl_done(&g_ctrl, g_bus.now_ms, &rx);
    CHECK_EQ(g_rec.responses, 0);

    /* responses that are not for this request are dropped: wrong TN, bad checksum, other source */
    r = make_req(34, g_bus.dev[a].uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, 9);
    for (int variant = 0; variant < 3; variant++) {
        CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
        CHECK(pm_rdm_ctrl_next(&g_ctrl, g_bus.now_ms, &t));
        sim_transact(&g_bus, &t, &rx);
        uint8_t bad[PM_RDM_MAX_MSG_LEN];
        memcpy(bad, rx.data, rx.len);
        if (variant == 0) bad[15] ^= 1; /* TN */
        if (variant == 1) bad[30] ^= 1; /* checksum */
        if (variant == 2) bad[14] ^= 1; /* source UID */
        if (variant != 1) pm_put_be16(bad + bad[2], pm_rdm_checksum(bad, bad[2]));
        pm_rdm_rx_t brx = {PM_RDM_RX_OK, rx.len, bad};
        pm_rdm_ctrl_done(&g_ctrl, g_bus.now_ms, &brx);
    }
    CHECK_EQ(g_rec.responses, 0);
    pm_rdm_ctrl_stats_t s;
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    CHECK(s.timeouts >= 4);

    /* malformed submits */
    r.len = 10;
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_ERR_INVALID_ARG);
    r = make_req(35, g_bus.dev[a].uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, 9);
    r.data[0] = 0x00;
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, NULL), PM_ERR_INVALID_ARG);

    /* disable: queue dropped, TOD cleared (event) */
    r = make_req(36, g_bus.dev[a].uid, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, 9);
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    int ev = g_rec.tod_events;
    pm_rdm_ctrl_set_enabled(&g_ctrl, false, g_bus.now_ms);
    CHECK_EQ(pm_rdm_ctrl_queue_free(&g_ctrl), PM_RDM_QUEUE_LEN);
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, NULL, 0), 0);
    CHECK_EQ(g_rec.tod_events, ev + 1);
    CHECK(!pm_rdm_ctrl_next(&g_ctrl, g_bus.now_ms, &t));
    g_context = NULL;
}

static void test_next_max_and_flush(void)
{
    g_context = "next_max / flush";
    setup(600);
    sim_add_random(&g_bus, 5);
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    pm_rdm_ctrl_set_timing(&g_ctrl, 176, 16);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    pm_rdm_txn_t t, t2;
    /* first step: DISC_UN_MUTE broadcast x3 */
    CHECK(!pm_rdm_ctrl_next_max(&g_ctrl, 0, 10, &t)); /* does not fit: nothing consumed */
    for (unsigned i = 0; i < PM_RDM_UNMUTE_REPEATS; i++) {
        CHECK(pm_rdm_ctrl_next_max(&g_ctrl, 0, 100000, &t));
        CHECK_EQ(t.kind, PM_RDM_TXN_BROADCAST);
        CHECK_EQ(pm_get_be16(t.data + 21), PM_RDM_PID_DISC_UN_MUTE);
        CHECK_EQ(t.est_us, 176 + 16 + 26 * PM_RDM_BYTE_US + PM_RDM_BROADCAST_GAP_US);
        pm_rdm_rx_t none = {PM_RDM_RX_TIMEOUT, 0, NULL};
        pm_rdm_ctrl_done(&g_ctrl, 0, &none);
    }
    CHECK(pm_rdm_ctrl_next(&g_ctrl, 0, &t));
    CHECK_EQ(t.kind, PM_RDM_TXN_DUB);
    CHECK_EQ(t.est_us, 176 + 16 + 38 * PM_RDM_BYTE_US + PM_RDM_DUB_GAP_US);
    CHECK(t.est_us < 1000000u / 30u - (176 + 16 + 513 * PM_RDM_BYTE_US)); /* fits next to a 512-slot frame */
    pm_rdm_msg_t m;
    CHECK_EQ(pm_rdm_decode(t.data, t.len, &m), PM_OK);
    CHECK(pm_uid_is_broadcast(&m.dest));
    CHECK(pm_uid_equal(&m.src, &NODE_UID));
    CHECK_EQ(m.port_or_resp, 1);
    /* flush restarts: the old DUB result is ignored */
    pm_rdm_ctrl_flush(&g_ctrl, 1);
    pm_rdm_rx_t garbage = {PM_RDM_RX_GARBAGE, 3, (const uint8_t *)"\x01\x02\x03"};
    pm_rdm_ctrl_done(&g_ctrl, 1, &garbage);
    CHECK(pm_rdm_ctrl_next(&g_ctrl, 1, &t2));
    CHECK_EQ(pm_get_be16(t2.data + 21), PM_RDM_PID_DISC_UN_MUTE);
    pm_rdm_ctrl_stats_t s;
    pm_rdm_ctrl_get_stats(&g_ctrl, &s);
    CHECK_EQ(s.collisions, 0);
    CHECK_EQ(s.full_discoveries, 2);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ BOOT_CATCH */

static bool caught(void *ctx)
{
    pm_rdm_boot_catch_t bc;
    pm_rdm_ctrl_boot_catch_status((const pm_rdm_ctrl_t *)ctx, g_bus.now_ms, &bc);
    return bc.state != PM_BOOT_CATCH_STATE_RUNNING;
}

static void test_boot_catch(void)
{
    g_context = "boot catch";
    setup(700);
    sim_add_random(&g_bus, 10);
    int tube = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x12345678));
    g_bus.dev[tube].tube = true;
    pm_uid_t target = g_bus.dev[tube].uid;
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &target, 0), PM_ERR_STATE); /* RDM disabled */
    pm_rdm_ctrl_set_background(&g_ctrl, true, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    sim_run(&g_bus, &g_ctrl, 60000, 512, disc_done, &g_ctrl, NULL);
    CHECK(tod_matches_bus());
    pm_uid_t bc = pm_uid_make(0xFFFF, 0xFFFFFFFF), vc = pm_uid_make(0x7FF0, 0xFFFFFFFF);
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &bc, g_bus.now_ms), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &vc, g_bus.now_ms), PM_ERR_INVALID_ARG);

    /* running app answers BOOT_INFO with NACK: not caught */
    uint32_t t0 = g_bus.now_ms;
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &target, t0), PM_OK);
    CHECK_EQ(g_rec.catch_events, 1);
    CHECK_EQ(g_rec.catch_states[0], PM_BOOT_CATCH_STATE_RUNNING);
    sim_port_stats_t st = {0};
    uint32_t dubs = g_bus.dubs;
    sim_run(&g_bus, &g_ctrl, t0 + 2000, 512, NULL, NULL, &st);
    CHECK_EQ(g_bus.dubs, dubs); /* background discovery paused during the catch */
    pm_rdm_boot_catch_t s;
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK_EQ(s.state, PM_BOOT_CATCH_STATE_RUNNING);
    CHECK(s.attempts >= 2000 / 34 && s.attempts <= 2000 / PM_BOOT_CATCH_PERIOD_MS + 1);
    CHECK(s.elapsed_ms >= 1990 && s.elapsed_ms <= 2100);
    CHECK(pm_uid_equal(&s.target, &target));
    CHECK(st.max_period_us <= 1000000u / 30u);
    CHECK(g_bus.dev[tube].boot_info_gets >= s.attempts - 1);

    /* power-cycle the tube: caught inside its rescue window, stays in the bootloader */
    sim_power_cycle(&g_bus, tube, 1500);
    uint32_t on = g_bus.now_ms + 1500;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 10000, 512, caught, &g_ctrl, NULL);
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK_EQ(s.state, PM_BOOT_CATCH_STATE_CAUGHT);
    CHECK_EQ(g_rec.catch_events, 2);
    CHECK_EQ(g_rec.catch_states[1], PM_BOOT_CATCH_STATE_CAUGHT);
    CHECK(g_bus.now_ms - on <= 225 && g_bus.now_ms - on >= 26);
    CHECK_EQ(g_bus.dev[tube].mode, SIM_BOOT);
    uint32_t el = s.elapsed_ms;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 5000, 512, NULL, NULL, NULL);
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK_EQ(s.elapsed_ms, el); /* frozen */
    /* background discovery resumed; the tube (un-muted after its reset) is still listed */
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &target, NULL));
    CHECK(g_bus.dev[tube].muted);
    CHECK(tod_matches_bus());

    /* timeout: target never answers */
    pm_uid_t ghost = pm_uid_make(0x7FF0, 0x0BAD0BAD);
    t0 = g_bus.now_ms;
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &ghost, t0), PM_OK);
    sim_run(&g_bus, &g_ctrl, t0 + PM_BOOT_CATCH_DURATION_MS + 1000, 512, caught, &g_ctrl, NULL);
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK_EQ(s.state, PM_BOOT_CATCH_STATE_TIMEOUT);
    CHECK(s.elapsed_ms >= PM_BOOT_CATCH_DURATION_MS && s.elapsed_ms < PM_BOOT_CATCH_DURATION_MS + 100);
    CHECK(s.attempts > 1500);
    CHECK_EQ(g_rec.catch_states[g_rec.catch_events - 1], PM_BOOT_CATCH_STATE_TIMEOUT);

    /* abort, replace, disable */
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &ghost, g_bus.now_ms), PM_OK);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 500, 512, NULL, NULL, NULL);
    int ev = g_rec.catch_events;
    pm_rdm_ctrl_boot_catch_abort(&g_ctrl);
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK_EQ(s.state, PM_BOOT_CATCH_STATE_ABORTED);
    CHECK(s.elapsed_ms >= 450 && s.elapsed_ms <= 550);
    CHECK_EQ(g_rec.catch_events, ev + 1);
    pm_rdm_ctrl_boot_catch_abort(&g_ctrl); /* idle: no event */
    CHECK_EQ(g_rec.catch_events, ev + 1);

    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &ghost, g_bus.now_ms), PM_OK);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 100, 512, NULL, NULL, NULL);
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &target, g_bus.now_ms), PM_OK); /* replaces */
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK(pm_uid_equal(&s.target, &target));
    CHECK_EQ(s.attempts, 0);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 100, 512, caught, &g_ctrl, NULL);
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK_EQ(s.state, PM_BOOT_CATCH_STATE_CAUGHT); /* tube is still in its bootloader */

    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &ghost, g_bus.now_ms), PM_OK);
    pm_rdm_ctrl_set_enabled(&g_ctrl, false, g_bus.now_ms);
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &s);
    CHECK_EQ(s.state, PM_BOOT_CATCH_STATE_ABORTED);

    /* requests keep priority over the catch */
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, g_bus.now_ms);
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &ghost, g_bus.now_ms), PM_OK);
    pm_rdm_request_t r = make_req(77, target, PM_RDM_CC_GET, PM_RDM_PID_DEVICE_INFO, 5);
    CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    pm_rdm_txn_t t;
    CHECK(pm_rdm_ctrl_next(&g_ctrl, g_bus.now_ms, &t));
    CHECK(memcmp(t.data, r.data, r.len) == 0);
    pm_rdm_ctrl_done(&g_ctrl, g_bus.now_ms, NULL);
    CHECK(pm_rdm_ctrl_next(&g_ctrl, g_bus.now_ms, &t));
    CHECK_EQ(pm_get_be16(t.data + 21), PM_PID_BOOT_INFO);
    g_context = NULL;
}

/* ------------------------------------------------------------------------------------------------ node vectors */

static const pm_vec_t *find_vec(const char *name)
{
    for (size_t i = 0; i < pm_vectors_count; i++) {
        if (strcmp(pm_vectors[i].name, name) == 0) return &pm_vectors[i];
    }
    return NULL;
}

static void fake_settings(void *ctx, pm_settings_t *out)
{
    (void)ctx;
    pm_settings_defaults(out);
}
static pm_err_t fake_apply(void *ctx, const pm_settings_t *s, pm_change_mask_t m)
{
    (void)ctx; (void)s; (void)m;
    return PM_OK;
}
static uint32_t fake_now(void *ctx)
{
    (void)ctx;
    return 0;
}
static pm_err_t fake_catch(void *ctx, pm_port_idx_t port, const pm_uid_t *target)
{
    (void)port;
    return pm_rdm_ctrl_boot_catch_start((pm_rdm_ctrl_t *)ctx, target, 0);
}

static void test_node_vectors(void)
{
    g_context = "node vectors (controller)";
    setup(800);
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    pm_uid_t tube = pm_uid_make(0x7FF0, 0x12345678);
    pm_rdm_txn_t t;
    const pm_vec_t *v = find_vec("node_disc_un_mute_broadcast");
    CHECK(v != NULL);
    g_ctrl.port = 1; /* the vector was taken on port 2 */
    CHECK(pm_rdm_ctrl_next(&g_ctrl, 0, &t));
    CHECK(v && t.len == v->len && memcmp(t.data, v->bytes, v->len) == 0);
    g_ctrl.port = 0;
    pm_rdm_ctrl_done(&g_ctrl, 0, NULL);
    pm_rdm_ctrl_next(&g_ctrl, 0, &t);
    pm_rdm_ctrl_done(&g_ctrl, 0, NULL);
    pm_rdm_ctrl_next(&g_ctrl, 0, &t);
    pm_rdm_ctrl_done(&g_ctrl, 0, NULL);
    v = find_vec("node_dub_full_range"); /* TN 3 */
    CHECK(pm_rdm_ctrl_next(&g_ctrl, 0, &t));
    CHECK(v && t.len == v->len && memcmp(t.data, v->bytes, v->len) == 0);
    uint8_t dub[32];
    int n = pm_rdm_dub_encode(&tube, 7, dub, sizeof(dub));
    pm_rdm_rx_t rx = {PM_RDM_RX_OK, (uint16_t)n, dub};
    pm_rdm_ctrl_done(&g_ctrl, 0, &rx);
    v = find_vec("node_disc_mute_tube"); /* TN 4 */
    CHECK(pm_rdm_ctrl_next(&g_ctrl, 0, &t));
    CHECK(v && t.len == v->len && memcmp(t.data, v->bytes, v->len) == 0);

    setup(801);
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    g_ctrl.disc_mode = PM_RDM_DISC_IDLE; /* only the catch */
    g_ctrl.tn = 0;
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &tube, 0), PM_OK);
    v = find_vec("node_boot_catch_get_boot_info");
    CHECK(pm_rdm_ctrl_next(&g_ctrl, 0, &t));
    CHECK(v && t.len == v->len && memcmp(t.data, v->bytes, v->len) == 0);
    v = find_vec("ack_response"); /* RDM_TUBE_BOOT_INFO.json, TN 0x0B: not ours -> ignored */
    if (v) {
        rx.status = PM_RDM_RX_OK;
        rx.len = (uint16_t)v->len;
        rx.data = v->bytes;
        pm_rdm_ctrl_done(&g_ctrl, 1, &rx);
        CHECK_EQ(g_ctrl.boot_catch.state, PM_BOOT_CATCH_STATE_RUNNING);
    }

    /* responder side: the node vectors are what pm_rdm_responder produces */
    g_context = "node vectors (responder)";
    static uint8_t mem[65536];
    CHECK(pm_rdm_responder_size() <= sizeof(mem));
    pm_rdm_responder_identity_t id = {NODE_UID, PM_MODEL_NODE, 0x000100, "0.1.0", 0, "", "Pixelmapper Node",
                                      "Prototype", 0};
    pm_rdm_responder_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.ctx = &g_ctrl;
    ops.now_ms = fake_now;
    ops.settings_get = fake_settings;
    ops.settings_apply = fake_apply;
    ops.boot_catch_start = fake_catch;
    pm_rdm_responder_t *r = pm_rdm_responder_init(mem, &id, &ops);
    CHECK(r != NULL);
    static const char *pairs[][2] = {
        {"node_get_device_info_request", "node_device_info_response"},
        {"node_boot_catch_start_request", "node_boot_catch_start_ack"},
    };
    uint8_t out[PM_RDM_MAX_MSG_LEN];
    for (unsigned i = 0; i < 2 && r; i++) {
        const pm_vec_t *q = find_vec(pairs[i][0]), *a = find_vec(pairs[i][1]);
        CHECK(q && a);
        if (!q || !a) continue;
        if (i == 1) pm_rdm_responder_boot_catch_event(r, 0, PM_BOOT_CATCH_STATE_RUNNING);
        int len = pm_rdm_responder_handle(r, q->bytes, q->len, out, sizeof(out));
        CHECK_EQ(len, a->len);
        for (int k = 0; k < len; k++) if (out[k] != a->bytes[k]) fprintf(stderr, "%s byte %d: %02x vs %02x\n", pairs[i][1], k, out[k], a->bytes[k]);
        CHECK(len == (int)a->len && memcmp(out, a->bytes, a->len) == 0);
    }
    /* NACK SUB_DEVICE_OUT_OF_RANGE: request = the DEVICE_INFO request with sub-device 1, TN 0x22, port 2 */
    const pm_vec_t *q = find_vec("node_get_device_info_request"), *a = find_vec("node_nack_sub_device_out_of_range");
    r = pm_rdm_responder_init(mem, &id, &ops); /* empty queue: message count 0 as in the vector */
    if (q && a && r) {
        uint8_t req[64];
        memcpy(req, q->bytes, q->len);
        req[15] = 0x22;
        req[16] = 2;
        pm_put_be16(req + 18, 1);
        pm_put_be16(req + 24, pm_rdm_checksum(req, 24));
        int len = pm_rdm_responder_handle(r, req, q->len, out, sizeof(out));
        for (int k = 0; k < len; k++) if (out[k] != a->bytes[k]) fprintf(stderr, "nack byte %d: %02x vs %02x\n", k, out[k], a->bytes[k]);
        CHECK(len == (int)a->len && memcmp(out, a->bytes, a->len) == 0);
    }
    g_context = NULL;
}

static void test_wraparound(void)
{
    g_context = "uint32 ms wraparound";
    setup(900);
    g_bus.now_us = (uint64_t)0xFFFFF000u * 1000u; /* ms clock wraps after ~4 s */
    g_bus.now_ms = (uint32_t)(g_bus.now_us / 1000u);
    sim_add_random(&g_bus, 30);
    int tube = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x0000ABCD));
    g_bus.dev[tube].tube = true;
    pm_rdm_ctrl_set_background(&g_ctrl, true, 1000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, g_bus.now_ms);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 30000, 512, disc_done, &g_ctrl, NULL);
    CHECK(tod_matches_bus());
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &g_bus.dev[tube].uid, g_bus.now_ms), PM_OK);
    sim_power_cycle(&g_bus, tube, 1000);
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 10000, 512, caught, &g_ctrl, NULL);
    CHECK(g_bus.now_ms < 0x10000000u); /* really wrapped */
    pm_rdm_boot_catch_t bc;
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, g_bus.now_ms, &bc);
    CHECK_EQ(bc.state, PM_BOOT_CATCH_STATE_CAUGHT);
    CHECK(bc.elapsed_ms < 2000);
    int extra = sim_add(&g_bus, pm_uid_make(0x7FF0, 0x0000ABCE));
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 5000, 512, NULL, NULL, NULL);
    CHECK(pm_rdm_ctrl_tod_find(&g_ctrl, &g_bus.dev[extra].uid, NULL));
    CHECK(tod_matches_bus());
    g_context = NULL;
}

static void test_discovery_stop(void)
{
    g_context = "discovery stop (AtcEnd)";
    setup(950);
    sim_add_random(&g_bus, 50);
    pm_rdm_ctrl_set_background(&g_ctrl, true, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    sim_run(&g_bus, &g_ctrl, 300, 512, NULL, NULL, NULL);
    CHECK(pm_rdm_ctrl_discovery_running(&g_ctrl));
    int ev = g_rec.tod_events;
    uint16_t found = pm_rdm_ctrl_tod(&g_ctrl, NULL, 0);
    pm_rdm_ctrl_discovery_stop(&g_ctrl, g_bus.now_ms);
    CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, NULL, 0), found); /* kept */
    CHECK_EQ(g_rec.tod_events, ev + 1);
    uint32_t tx = g_bus.txns;
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 2900, 512, NULL, NULL, NULL);
    CHECK_EQ(g_bus.txns, tx); /* quiet until the next background run */
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 200, 512, NULL, NULL, NULL);
    CHECK(pm_rdm_ctrl_discovery_running(&g_ctrl));
    sim_run(&g_bus, &g_ctrl, g_bus.now_ms + 60000, 512, disc_done, &g_ctrl, NULL);
    CHECK(tod_matches_bus()); /* background run finds the rest */
    pm_rdm_ctrl_discovery_stop(&g_ctrl, g_bus.now_ms); /* idle: no-op */
    g_context = NULL;
}

/* The port engine finishes every transaction it could not complete (port reconfigured, line refused it, driver
 * error) with done(PM_RDM_RX_TIMEOUT, rx NULL): discovery must still end, the queue drain, BOOT_CATCH go on. */
static void test_abandoned_txns(void)
{
    g_context = "abandoned transactions";
    setup(970);
    pm_rdm_ctrl_set_background(&g_ctrl, false, 3000);
    pm_rdm_ctrl_set_enabled(&g_ctrl, true, 0);
    for (uint8_t i = 0; i < 3; i++) {
        pm_rdm_request_t r = make_req(100u + i, pm_uid_make(0x7FF0, 0x2000u + i), PM_RDM_CC_GET,
                                      PM_RDM_PID_DEVICE_INFO, i);
        CHECK_EQ(pm_rdm_ctrl_submit(&g_ctrl, &r), PM_OK);
    }
    pm_rdm_txn_t t;
    unsigned n = 0;
    uint32_t now = 0;
    for (; n < 200 && pm_rdm_ctrl_next(&g_ctrl, now, &t); n++, now++) pm_rdm_ctrl_done(&g_ctrl, now, NULL);
    CHECK(n < 200);
    CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));
    CHECK_EQ(pm_rdm_ctrl_queue_free(&g_ctrl), PM_RDM_QUEUE_LEN);
    CHECK_EQ(g_rec.responses, 0);
    pm_rdm_ctrl_stats_t st;
    pm_rdm_ctrl_get_stats(&g_ctrl, &st);
    CHECK_EQ(st.requests, 3);
    CHECK_EQ(st.timeouts, 3);
    CHECK_EQ(pm_rdm_ctrl_tod(&g_ctrl, NULL, 0), 0);

    /* abandoned in the middle of a run: the next step continues (a DUB counts as an empty branch) */
    pm_rdm_ctrl_flush(&g_ctrl, now);
    CHECK(pm_rdm_ctrl_discovery_running(&g_ctrl));
    pm_rdm_rx_t to = {PM_RDM_RX_TIMEOUT, 0, NULL};
    for (n = 0; n < 200 && pm_rdm_ctrl_next(&g_ctrl, now, &t); n++, now++) pm_rdm_ctrl_done(&g_ctrl, now, &to);
    CHECK(!pm_rdm_ctrl_discovery_running(&g_ctrl));

    /* BOOT_CATCH keeps polling when its GETs are abandoned */
    pm_uid_t target = pm_uid_make(0x7FF0, 0x3000);
    CHECK_EQ(pm_rdm_ctrl_boot_catch_start(&g_ctrl, &target, now), PM_OK);
    unsigned gets = 0;
    for (uint32_t ms = now; ms < now + 200; ms++) {
        if (pm_rdm_ctrl_next(&g_ctrl, ms, &t)) {
            gets++;
            pm_rdm_ctrl_done(&g_ctrl, ms, NULL);
        }
    }
    CHECK(gets >= 9);
    pm_rdm_boot_catch_t bc;
    pm_rdm_ctrl_boot_catch_status(&g_ctrl, now + 200, &bc);
    CHECK_EQ(bc.state, PM_BOOT_CATCH_STATE_RUNNING);
    g_context = NULL;
}

void test_rdm_ctrl(void)
{
    test_abandoned_txns();
    test_wraparound();
    test_discovery_stop();
    test_full_counts();
    test_full_edges();
    test_flags_and_quirks();
    test_tod_limit();
    test_background();
    test_presence_misses();
    test_requests();
    test_next_max_and_flush();
    test_boot_catch();
    test_node_vectors();
}
