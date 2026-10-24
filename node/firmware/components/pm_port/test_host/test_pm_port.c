/*
 * test_pm_port.c - host tests of the pm_port line FSM, frame scheduler and triple buffer.
 *
 * A small discrete-event simulator plays the UART (tx_done after len * 44 us) and scripted responders
 * (break + bytes at given times), so the wire timing of pm_port_line can be checked without hardware.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pm_board.h"
#include "pm_port_line.h"
#include "pm_port_tbuf.h"

static int g_fail, g_checks;

#define CHECK(cond)                                                                     \
    do {                                                                                \
        g_checks++;                                                                     \
        if (!(cond)) {                                                                  \
            g_fail++;                                                                   \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                       \
        }                                                                               \
    } while (0)
#define CHECK_EQ(a, b)                                                                  \
    do {                                                                                \
        g_checks++;                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                             \
        if (_a != _b) {                                                                 \
            g_fail++;                                                                   \
            printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, _a, _b); \
        }                                                                               \
    } while (0)

/* ------------------------------------------------------------------------------------------------ simulator */

typedef enum { EV_DE, EV_BRK, EV_TX, EV_TXDONE, EV_RDMDONE, EV_INFRAME } ev_kind_t;

typedef struct {
    ev_kind_t kind;
    uint32_t t;
    int val;          /* DE/BRK level, tx length, input frame length */
    uint8_t first;    /* first byte sent / received */
} logent_t;

typedef enum { RX_BREAK, RX_BYTE, RX_ERR } rx_kind_t;

typedef struct {
    uint32_t t;
    rx_kind_t kind;
    uint8_t b;
} rxev_t;

typedef enum { RESP_NONE, RESP_ANSWER, RESP_DUB, RESP_DUB_COLLISION } resp_mode_t;

typedef struct {
    pm_port_line_t line;
    uint32_t now;
    bool de, brk;

    logent_t log[4096];
    unsigned nlog;

    bool tx_busy;
    uint32_t tx_done_at;

    rxev_t rx[2048];
    unsigned nrx, rx_pos;

    resp_mode_t resp;
    uint32_t resp_delay_us;  /* end of request -> response break / first DUB byte */
    uint16_t resp_len;       /* response length for RESP_ANSWER */

    uint8_t frame[PM_PORT_FRAME_MAX];
    uint16_t frame_len;
    bool fresh;

    unsigned rdm_done;
    uint8_t in_frame[PM_PORT_FRAME_MAX];
    uint16_t in_len;
} sim_t;

static sim_t S;

static void logit(ev_kind_t k, int val, uint8_t first)
{
    if (S.nlog < sizeof S.log / sizeof S.log[0]) S.log[S.nlog++] = (logent_t){k, S.now, val, first};
}

static void rx_push(uint32_t t, rx_kind_t k, uint8_t b)
{
    if (S.nrx < sizeof S.rx / sizeof S.rx[0]) S.rx[S.nrx++] = (rxev_t){t, k, b};
}

/* Schedule a response starting at `t` (break 176 + MAB 16 when with_break). */
static void rx_push_packet(uint32_t t, bool with_break, const uint8_t *data, uint16_t len)
{
    if (with_break) {
        rx_push(t + 44, RX_BREAK, 0); /* UART flags the break after one character time */
        t += 176 + 16;
    }
    for (uint16_t i = 0; i < len; i++) rx_push(t + (uint32_t)(i + 1) * 44u, RX_BYTE, data[i]);
}

static void op_set_de(void *ctx, bool on) { (void)ctx; S.de = on; logit(EV_DE, on, 0); }
static void op_set_break(void *ctx, bool on) { (void)ctx; S.brk = on; logit(EV_BRK, on, 0); }

static void op_tx_start(void *ctx, const uint8_t *data, uint16_t len)
{
    (void)ctx;
    S.tx_busy = true;
    S.tx_done_at = S.now + (uint32_t)len * PM_PORT_US_PER_SLOT;
    logit(EV_TX, len, data[0]);
    if (data[0] == PM_RDM_START_CODE && S.resp != RESP_NONE) {
        uint32_t t0 = S.tx_done_at + S.resp_delay_us;
        bool dub = len >= 24 && data[20] == 0x10 && data[21] == 0x00 && data[22] == 0x01;
        if (S.resp == RESP_ANSWER && !dub) {
            uint8_t r[PM_RDM_MAX_MSG_LEN];
            memset(r, 0x55, sizeof r);
            r[0] = PM_RDM_START_CODE;
            r[1] = PM_RDM_SUB_START_CODE;
            r[2] = (uint8_t)(S.resp_len - 2u);
            rx_push_packet(t0, true, r, S.resp_len);
        } else if (S.resp == RESP_DUB && dub) {
            uint8_t r[24];
            memset(r, 0xFE, 7);
            r[7] = 0xAA;
            memset(r + 8, 0xFF, 16);
            rx_push_packet(t0, false, r, 24);
        } else if (S.resp == RESP_DUB_COLLISION && dub) {
            uint8_t r[24];
            memset(r, 0xFE, sizeof r);
            rx_push_packet(t0, false, r, 10);
            rx_push(t0 + 300, RX_ERR, 0);
        }
    }
}

static void op_rx_flush(void *ctx) { (void)ctx; }

static uint16_t op_dmx_frame(void *ctx, const uint8_t **frame)
{
    (void)ctx;
    S.fresh = false;
    *frame = S.frame;
    return S.frame_len;
}

static bool op_dmx_fresh(void *ctx) { (void)ctx; return S.fresh; }
static void op_rdm_done(void *ctx) { (void)ctx; S.rdm_done++; logit(EV_RDMDONE, 0, 0); }

static void op_input_frame(void *ctx, const uint8_t *frame, uint16_t len)
{
    (void)ctx;
    memcpy(S.in_frame, frame, len);
    S.in_len = len;
    logit(EV_INFRAME, len, frame[0]);
}

static const pm_port_line_ops_t OPS = {
    .ctx = NULL,
    .set_de = op_set_de,
    .set_break = op_set_break,
    .tx_start = op_tx_start,
    .rx_flush = op_rx_flush,
    .dmx_frame = op_dmx_frame,
    .dmx_fresh = op_dmx_fresh,
    .rdm_done = op_rdm_done,
    .input_frame = op_input_frame,
};

static void sim_reset(void)
{
    memset(&S, 0, sizeof S);
    S.now = 1000;
    S.frame_len = PM_PORT_FRAME_MAX;
    pm_port_line_init(&S.line, &OPS);
}

static void sim_configure(pm_port_line_mode_t mode, uint16_t brk, uint16_t mab, uint8_t rate, bool continuous)
{
    pm_port_line_timing_t t = {.break_us = brk, .mab_us = mab, .rate_hz = rate, .continuous = continuous};
    pm_port_line_configure(&S.line, mode, &t, S.now);
}

/* Advance simulated time to `until`, dispatching all events in order. */
static void sim_run(uint32_t until)
{
    for (int guard = 0; guard < 1000000; guard++) {
        uint32_t next = until;
        int which = 0; /* 0 = until, 1 = deadline, 2 = tx_done, 3 = rx */
        uint32_t dl;
        if (pm_port_line_deadline(&S.line, &dl) && !pm_port_t_ge(dl, next)) { next = dl; which = 1; }
        if (S.tx_busy && !pm_port_t_ge(S.tx_done_at, next)) { next = S.tx_done_at; which = 2; }
        if (S.rx_pos < S.nrx && !pm_port_t_ge(S.rx[S.rx_pos].t, next)) { next = S.rx[S.rx_pos].t; which = 3; }
        if (pm_port_line_deadline(&S.line, &dl) && dl == next && which == 0) which = 1;
        if (pm_port_t_ge(S.now, next) == false) S.now = next;
        switch (which) {
        case 1:
            pm_port_line_timer(&S.line, S.now);
            break;
        case 2:
            S.tx_busy = false;
            logit(EV_TXDONE, 0, 0);
            pm_port_line_tx_done(&S.line, S.now);
            break;
        case 3: {
            rxev_t e = S.rx[S.rx_pos++];
            if (e.kind == RX_BREAK) pm_port_line_rx_break(&S.line, S.now);
            else if (e.kind == RX_ERR) pm_port_line_rx_error(&S.line, S.now);
            else pm_port_line_rx_bytes(&S.line, S.now, &e.b, 1);
            break;
        }
        default:
            return;
        }
    }
    printf("sim_run: guard hit\n");
    g_fail++;
}

/* n-th log entry of a kind (from index `from`), -1 if none */
static int find(ev_kind_t k, int val, unsigned from)
{
    for (unsigned i = from; i < S.nlog; i++) {
        if (S.log[i].kind == k && (val < 0 || S.log[i].val == val)) return (int)i;
    }
    return -1;
}

static pm_rdm_txn_t make_txn(pm_rdm_txn_kind_t kind, uint16_t len)
{
    pm_rdm_txn_t t;
    memset(&t, 0, sizeof t);
    t.kind = kind;
    t.len = len;
    t.data[0] = PM_RDM_START_CODE;
    t.data[1] = PM_RDM_SUB_START_CODE;
    t.data[2] = (uint8_t)(len - 2);
    if (kind == PM_RDM_TXN_DUB) {
        t.data[20] = 0x10;
        t.data[21] = 0x00;
        t.data[22] = 0x01;
    } else {
        t.data[20] = 0x20;
    }
    return t;
}

/* ------------------------------------------------------------------------------------------------ tests */

static void test_dmx_output_timing(void)
{
    sim_reset();
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
    sim_run(S.now + 200000);

    int b0 = find(EV_BRK, 1, 0);
    CHECK(b0 >= 0);
    int b0e = find(EV_BRK, 0, (unsigned)b0);
    int tx0 = find(EV_TX, -1, (unsigned)b0);
    CHECK_EQ(S.log[b0e].t - S.log[b0].t, 176);
    CHECK_EQ(S.log[tx0].t - S.log[b0e].t, 16);
    CHECK_EQ(S.log[tx0].val, 513);
    CHECK_EQ(S.log[tx0].first, 0x00);
    CHECK(S.de);

    /* break-to-break = 25 ms at 40 Hz, for all frames */
    unsigned frames = 0;
    uint32_t prev = 0;
    for (unsigned i = 0; i < S.nlog; i++) {
        if (S.log[i].kind == EV_BRK && S.log[i].val == 1) {
            if (frames) CHECK_EQ(S.log[i].t - prev, 25000);
            prev = S.log[i].t;
            frames++;
        }
    }
    CHECK(frames >= 7 && frames <= 9);
    CHECK_EQ(S.line.stats.frames_out, frames - (S.line.state != PM_PORT_LINE_BACKOFF &&
                                                S.line.state != PM_PORT_LINE_GAP ? 1u : 0u));

    /* change break/MAB on the fly, clamped to the contract ranges */
    pm_port_line_timing_t t = {.break_us = 400, .mab_us = 5, .rate_hz = 44, .continuous = true};
    pm_port_line_set_timing(&S.line, &t);
    CHECK_EQ(S.line.timing.break_us, 352);
    CHECK_EQ(S.line.timing.mab_us, 12);
    unsigned from = S.nlog;
    sim_run(S.now + 60000);
    int b1 = find(EV_BRK, 1, from);
    int b1e = find(EV_BRK, 0, (unsigned)b1);
    int tx1 = find(EV_TX, -1, (unsigned)b1);
    CHECK_EQ(S.log[b1e].t - S.log[b1].t, 352);
    CHECK_EQ(S.log[tx1].t - S.log[b1e].t, 12);

    /* 44 Hz with 512 slots: frame (352+12+513*44 = 22.9 ms) + gap fits in 22.7 ms? no -> back-to-back */
    int b2 = find(EV_BRK, 1, (unsigned)tx1);
    CHECK(b2 >= 0);
    CHECK((S.log[b2].t - S.log[b1].t) >= 22727);
}

static void test_short_frame_rate(void)
{
    sim_reset();
    S.frame_len = 25; /* SC + 24 slots */
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 44, true);
    sim_run(S.now + 1000000);
    CHECK(S.line.stats.frames_out >= 43 && S.line.stats.frames_out <= 45);
}

static void test_delta_mode(void)
{
    sim_reset();
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, false);
    sim_run(S.now + 100000);
    CHECK_EQ(S.line.stats.frames_out, 1); /* first frame, then nothing new */
    S.fresh = true;
    sim_run(S.now + 30000);
    CHECK_EQ(S.line.stats.frames_out, 2);
    sim_run(S.now + 1000000);
    CHECK_EQ(S.line.stats.frames_out, 3); /* refresh after 800 ms */
}

static void test_rdm_request_response(void)
{
    sim_reset();
    S.frame_len = 513;
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
    sim_run(S.now + 30000); /* first frame done, waiting */
    S.resp = RESP_ANSWER;
    S.resp_delay_us = 300;
    S.resp_len = 30;
    pm_rdm_txn_t t = make_txn(PM_RDM_TXN_REQUEST, 26);
    unsigned from = S.nlog;
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    CHECK(!pm_port_line_rdm_offer(&S.line, &t, S.now)); /* busy */
    sim_run(S.now + 60000);

    int tx = find(EV_TX, 26, from);
    CHECK(tx >= 0);
    int done = find(EV_TXDONE, -1, (unsigned)tx);
    int de_off = find(EV_DE, 0, (unsigned)tx);
    CHECK_EQ(S.log[de_off].t, S.log[done].t); /* turnaround: release immediately */
    CHECK_EQ(S.rdm_done, 1);
    pm_rdm_rx_t rx;
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_OK);
    CHECK_EQ(rx.len, 30);
    CHECK_EQ(rx.data[0], PM_RDM_START_CODE);
    CHECK(!pm_port_line_rdm_busy(&S.line));

    /* the packet after the response starts >= 176 us after its last byte */
    int rd = find(EV_RDMDONE, -1, (unsigned)tx);
    int nb = find(EV_BRK, 1, (unsigned)rd);
    CHECK(S.log[nb].t - S.log[rd].t >= 176);
    /* RDM inserted before the floor deadline: DMX break-to-break stays <= 1/30 s */
    int pb = -1;
    for (int i = tx; i >= 0; i--) {
        if (S.log[i].kind == EV_BRK && S.log[i].val == 1 && i < tx - 2) { pb = i; break; }
    }
    CHECK(pb >= 0);
    CHECK(S.log[nb].t - S.log[pb].t <= 33333);
}

static void test_rdm_timeout_and_broadcast(void)
{
    sim_reset();
    S.frame_len = 100;
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
    sim_run(S.now + 10000);
    pm_rdm_txn_t t = make_txn(PM_RDM_TXN_REQUEST, 26);
    unsigned from = S.nlog;
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 30000);
    int tx = find(EV_TX, 26, from);
    int done = find(EV_TXDONE, -1, (unsigned)tx);
    int rd = find(EV_RDMDONE, -1, (unsigned)tx);
    CHECK_EQ(S.log[rd].t - S.log[done].t, 2800);
    pm_rdm_rx_t rx;
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_TIMEOUT);
    CHECK_EQ(S.line.stats.rdm_timeouts, 1);
    int nb = find(EV_BRK, 1, (unsigned)rd);
    CHECK(S.log[nb].t - S.log[done].t >= 3000);

    /* broadcast: DE stays on, no wait for a response, 176 us gap */
    t = make_txn(PM_RDM_TXN_BROADCAST, 26);
    from = S.nlog;
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 30000);
    tx = find(EV_TX, 26, from);
    done = find(EV_TXDONE, -1, (unsigned)tx);
    rd = find(EV_RDMDONE, -1, (unsigned)tx);
    CHECK_EQ(S.log[rd].t, S.log[done].t);
    CHECK(find(EV_DE, 0, (unsigned)tx) < 0);
    nb = find(EV_BRK, 1, (unsigned)rd);
    CHECK(S.log[nb].t - S.log[done].t >= 176);
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_TIMEOUT);
    CHECK_EQ(S.line.stats.rdm_timeouts, 1); /* broadcasts are not timeouts */
}

static void test_rdm_late_and_noise(void)
{
    /* response that starts after the 2.8 ms window -> timeout */
    sim_reset();
    S.frame_len = 50;
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
    sim_run(S.now + 5000);
    S.resp = RESP_ANSWER;
    S.resp_delay_us = 2900;
    S.resp_len = 26;
    pm_rdm_txn_t t = make_txn(PM_RDM_TXN_REQUEST, 26);
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 20000);
    pm_rdm_rx_t rx;
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_TIMEOUT);

    /* truncated response (inter-slot timeout) -> garbage */
    sim_reset();
    S.frame_len = 50;
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
    sim_run(S.now + 5000);
    t = make_txn(PM_RDM_TXN_REQUEST, 26);
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 3000); /* request on the wire / done */
    uint8_t part[5] = {PM_RDM_START_CODE, 0x01, 40, 1, 2};
    rx_push_packet(S.now + 100, true, part, 5);
    sim_run(S.now + 20000);
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_GARBAGE);
    CHECK_EQ(S.line.stats.rdm_garbage, 1);
}

static void test_dub(void)
{
    sim_reset();
    S.frame_len = 50;
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
    sim_run(S.now + 5000);
    S.resp = RESP_DUB;
    S.resp_delay_us = 500;
    pm_rdm_txn_t t = make_txn(PM_RDM_TXN_DUB, 38);
    unsigned from = S.nlog;
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 30000);
    pm_rdm_rx_t rx;
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_OK);
    CHECK_EQ(rx.len, 24);
    CHECK_EQ(rx.data[0], 0xFE);
    CHECK_EQ(rx.data[7], 0xAA);
    int tx = find(EV_TX, 38, from);
    int done = find(EV_TXDONE, -1, (unsigned)tx);
    int nb = find(EV_BRK, 1, (unsigned)done);
    CHECK(S.log[nb].t - S.log[done].t >= 5800);

    /* collision: framing error -> garbage */
    S.resp = RESP_DUB_COLLISION;
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 40000);
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_GARBAGE);

    /* empty branch: nothing -> timeout after the 2.9 ms window, next packet >= 5.8 ms */
    S.resp = RESP_NONE;
    from = S.nlog;
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 40000);
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_TIMEOUT);
    tx = find(EV_TX, 38, from);
    done = find(EV_TXDONE, -1, (unsigned)tx);
    int rd = find(EV_RDMDONE, -1, (unsigned)tx);
    CHECK_EQ(S.log[rd].t - S.log[done].t, 2900);
    nb = find(EV_BRK, 1, (unsigned)rd);
    CHECK(S.log[nb].t - S.log[done].t >= 5800);
}

/* Discovery-like load: an RDM transaction always pending. DMX must stay >= 30 Hz, RDM must make progress. */
static void test_rdm_load_keeps_dmx_rate(void)
{
    static const struct { uint16_t slots; pm_rdm_txn_kind_t kind; resp_mode_t resp; } cases[] = {
        {513, PM_RDM_TXN_REQUEST, RESP_NONE},
        {513, PM_RDM_TXN_REQUEST, RESP_ANSWER},
        {513, PM_RDM_TXN_DUB, RESP_DUB},
        {100, PM_RDM_TXN_DUB, RESP_NONE},
    };
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        sim_reset();
        S.frame_len = cases[c].slots;
        S.resp = cases[c].resp;
        S.resp_delay_us = 1000;
        S.resp_len = 60;
        sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
        pm_rdm_txn_t t = make_txn(cases[c].kind, cases[c].kind == PM_RDM_TXN_DUB ? 38 : 26);
        uint32_t end = S.now + 2000000;
        unsigned txns = 0;
        while (pm_port_t_ge(end, S.now)) {
            if (!pm_port_line_rdm_busy(&S.line)) pm_port_line_rdm_offer(&S.line, &t, S.now);
            uint32_t before = S.rdm_done;
            sim_run(S.now + 500);
            pm_rdm_rx_t rx;
            if (S.rdm_done != before && pm_port_line_rdm_take(&S.line, &rx)) txns++;
        }
        /* DMX rate */
        unsigned frames = 0;
        uint32_t prev = 0, worst = 0;
        for (unsigned i = 0; i < S.nlog; i++) {
            if (S.log[i].kind == EV_TX && S.log[i].first == 0x00) {
                /* break of this frame = tx - mab - break */
                uint32_t b = S.log[i].t - 16 - 176;
                if (frames && b - prev > worst) worst = b - prev;
                prev = b;
                frames++;
            }
        }
        printf("  load case %u: %u DMX frames/2 s (worst break-to-break %u us), %u RDM txns\n", c, frames,
               (unsigned)worst, txns);
        CHECK(frames >= 60);
        CHECK(txns >= 60);
        /* only forced (starvation) transactions may stretch a frame interval beyond the floor */
        CHECK(worst <= 33333u + 20000u);
    }
}

static void test_mode_change_aborts_rdm(void)
{
    sim_reset();
    sim_configure(PM_PORT_LINE_MODE_OUTPUT, 176, 16, 40, true);
    pm_rdm_txn_t t = make_txn(PM_RDM_TXN_REQUEST, 26);
    CHECK(pm_port_line_rdm_offer(&S.line, &t, S.now));
    sim_run(S.now + 300);
    sim_configure(PM_PORT_LINE_MODE_INPUT, 176, 16, 40, true);
    CHECK_EQ(S.rdm_done, 1);
    pm_rdm_rx_t rx;
    CHECK(pm_port_line_rdm_take(&S.line, &rx));
    CHECK_EQ(rx.status, PM_RDM_RX_TIMEOUT);
    CHECK(!S.de);
    CHECK(!S.brk);
    CHECK(!pm_port_line_rdm_offer(&S.line, &t, S.now)); /* input: no RDM */
}

static void test_input(void)
{
    sim_reset();
    sim_configure(PM_PORT_LINE_MODE_INPUT, 176, 16, 40, true);
    CHECK(!S.de);
    uint8_t f[PM_PORT_FRAME_MAX];
    for (unsigned i = 0; i < sizeof f; i++) f[i] = (uint8_t)i;
    f[0] = 0;
    /* garbage before the first break is ignored */
    uint8_t junk[3] = {1, 2, 3};
    rx_push_packet(S.now + 10, false, junk, 3);
    uint32_t t = S.now + 1000;
    rx_push_packet(t, true, f, 101);              /* frame 1: SC + 100 slots */
    t += 192 + 102 * 44 + 1000;
    uint8_t rdm[30] = {PM_RDM_START_CODE, 1};
    rx_push_packet(t, true, rdm, sizeof rdm);     /* non-zero start code */
    t += 192 + 31 * 44 + 100;
    rx_push_packet(t, true, f, PM_PORT_FRAME_MAX); /* full frame: delivered without waiting for a break */
    t += 192 + 514 * 44 + 50;
    rx_push_packet(t, true, f, 10);               /* frame 4, finished by the break after it */
    t += 192 + 11 * 44 + 50;
    rx_push(t + 44, RX_BREAK, 0);
    sim_run(t + 1000);

    CHECK_EQ(S.line.stats.frames_in, 3);
    CHECK_EQ(S.line.stats.frames_in_other_sc, 1);
    int a = find(EV_INFRAME, -1, 0);
    CHECK_EQ(S.log[a].val, 101);
    int b = find(EV_INFRAME, -1, (unsigned)a + 1);
    CHECK_EQ(S.log[b].val, 513);
    int c = find(EV_INFRAME, -1, (unsigned)b + 1);
    CHECK_EQ(S.log[c].val, 10);
    CHECK_EQ(S.in_len, 10);
    CHECK_EQ(S.in_frame[9], 9);
}

static void test_tbuf(void)
{
    static pm_port_tbuf_t b;
    uint8_t z[1] = {0};
    pm_port_tbuf_init(&b, z, 1);
    uint16_t len;
    const uint8_t *p = pm_port_tbuf_acquire(&b, &len);
    CHECK_EQ(len, 1);
    CHECK(!pm_port_tbuf_fresh(&b));

    for (int round = 0; round < 10; round++) {
        uint8_t *w = pm_port_tbuf_back(&b);
        w[0] = 0;
        w[1] = (uint8_t)round;
        pm_port_tbuf_set_len(&b, 2);
        pm_port_tbuf_publish(&b);
        CHECK(pm_port_tbuf_fresh(&b));
        if (round % 3 == 2) continue; /* reader skips: newest wins */
        p = pm_port_tbuf_acquire(&b, &len);
        CHECK_EQ(len, 2);
        CHECK_EQ(p[1], round);
        CHECK(!pm_port_tbuf_fresh(&b));
    }
    /* two publishes before one acquire -> the second */
    for (int k = 0; k < 2; k++) {
        uint8_t *w = pm_port_tbuf_back(&b);
        w[1] = (uint8_t)(100 + k);
        pm_port_tbuf_set_len(&b, 2);
        pm_port_tbuf_publish(&b);
    }
    p = pm_port_tbuf_acquire(&b, &len);
    CHECK_EQ(p[1], 101);
    /* the reader's buffer is never handed to the writer */
    const uint8_t *cur = pm_port_tbuf_current(&b, &len);
    for (int k = 0; k < 5; k++) {
        CHECK(pm_port_tbuf_back(&b) != cur);
        pm_port_tbuf_publish(&b);
    }
}

static void test_sched(void)
{
    pm_port_sched_in_t in = {.now = 1000, .dmx_started = false, .period_us = 25000, .continuous = true};
    uint32_t wake = 0;
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_DMX);
    in.rdm_pending = true;
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_DMX); /* first frame first */

    in.dmx_started = true;
    in.last_dmx_start = 0;
    in.now = 23500;            /* 512-slot frame just ended */
    in.rdm_est_us = 5000;      /* ends at 28.5 ms < 33.3 ms */
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_RDM);
    in.rdm_est_us = 12000;     /* would end at 35.5 ms: DMX first, wait until due */
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_WAIT);
    CHECK_EQ(wake, 25000);
    in.now = 25000;
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_DMX);
    in.frames_since_rdm = PM_PORT_RDM_STARVE_FRAMES;
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_RDM);

    /* wrap-around of the us counter */
    in.frames_since_rdm = 0;
    in.rdm_pending = false;
    in.last_dmx_start = 0xFFFFF000u;
    in.now = 0xFFFFF000u + 10000u;
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_WAIT);
    CHECK_EQ(wake, 0xFFFFF000u + 25000u);

    /* rate below the floor: the floor follows the period */
    in.period_us = 100000;
    in.rdm_pending = true;
    in.rdm_est_us = 60000;
    in.now = in.last_dmx_start + 30000;
    CHECK_EQ(pm_port_sched_decide(&in, &wake), PM_PORT_SCHED_RDM);

    pm_rdm_txn_t t = make_txn(PM_RDM_TXN_DUB, 38);
    CHECK_EQ(pm_port_rdm_estimate_us(&t, 176, 16), 176 + 16 + 38 * 44 + 5800);
}

static void test_board_rev_code(void)
{
    uint8_t rev = 99;
    CHECK(pm_board_decode_rev_code(NULL, NULL, 0, &rev));
    CHECK_EQ(rev, 0);
    const uint8_t fl_up[3] = {1, 1, 1}, fl_dn[3] = {0, 0, 0};
    CHECK(pm_board_decode_rev_code(fl_up, fl_dn, 3, &rev)); /* all floating = rev 0 */
    CHECK_EQ(rev, 0);
    const uint8_t c_up[3] = {0, 0, 0}, c_dn[3] = {0, 0, 0};
    CHECK(pm_board_decode_rev_code(c_up, c_dn, 3, &rev)); /* code 0 = rev 1 */
    CHECK_EQ(rev, 1);
    const uint8_t d_up[3] = {1, 0, 1}, d_dn[3] = {1, 0, 1};
    CHECK(pm_board_decode_rev_code(d_up, d_dn, 3, &rev)); /* code 0b101 = rev 6 */
    CHECK_EQ(rev, 6);
    const uint8_t m_up[3] = {1, 1, 0}, m_dn[3] = {0, 1, 0};
    CHECK(!pm_board_decode_rev_code(m_up, m_dn, 3, &rev)); /* one floating, two driven */
    const uint8_t x_up[1] = {0}, x_dn[1] = {1};
    CHECK(!pm_board_decode_rev_code(x_up, x_dn, 1, &rev));
    CHECK(!pm_board_decode_rev_code(fl_up, fl_dn, PM_BOARD_REV_CODE_MAX_PINS + 1, &rev));
}

int main(void)
{
    test_board_rev_code();
    test_sched();
    test_tbuf();
    test_dmx_output_timing();
    test_short_frame_rate();
    test_delta_mode();
    test_rdm_request_response();
    test_rdm_timeout_and_broadcast();
    test_rdm_late_and_noise();
    test_dub();
    test_rdm_load_keeps_dmx_rate();
    test_mode_change_aborts_rdm();
    test_input();
    printf("pm_port host tests: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? EXIT_FAILURE : EXIT_SUCCESS;
}
