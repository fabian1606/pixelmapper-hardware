/* pm_rdm_responder: every PID, every NACK path, addressing, queued/status messages, BOOT_CATCH, FW_* (fake ops). */
#include <ctype.h>

#include "pm_pids.h"
#include "pm_rdm_responder.h"
#include "test.h"

void test_rdm_responder(void);

/* ------------------------------------------------------------------------------------------------ fake platform */

static struct {
    pm_settings_t s;
    uint32_t now;
    int apply_count;
    pm_change_mask_t last_mask;
    pm_err_t apply_result;
    pm_rdm_net_info_t net;
    int net_apply, renew, identify_on, identify_off, reset_count, factory;
    uint8_t reset_kind;
    /* boot catch */
    int bc_start, bc_abort;
    pm_port_idx_t bc_port;
    pm_uid_t bc_target;
    pm_err_t bc_result;
    pm_rdm_boot_catch_t bc;
    pm_port_idx_t bc_status_port;
    /* fw sink */
    pm_fw_status_t fw;
    pm_image_desc_t fw_desc;
    pm_fw_error_t fw_begin_result, fw_finish_result;
    int fw_begin, fw_writes, fw_finish, fw_abort, fw_reboot;
    pm_fw_error_t fw_abort_reason;
    uint8_t img[2048];
} F;

static pm_rdm_responder_t *R;
static _Alignas(8) uint8_t g_mem[16384];
static pm_uid_t NODE, CTRL;
static pm_rdm_msg_t g_resp;
static uint8_t g_out[PM_RDM_MAX_MSG_LEN + 8];
static int g_len;
static uint8_t g_tn;

static uint32_t f_now(void *ctx) { (void)ctx; return F.now; }
static void f_settings_get(void *ctx, pm_settings_t *out) { (void)ctx; *out = F.s; }
static pm_err_t f_settings_apply(void *ctx, const pm_settings_t *s, pm_change_mask_t mask)
{
    (void)ctx;
    F.apply_count++;
    F.last_mask = mask;
    if (F.apply_result != PM_OK) return F.apply_result;
    F.s = *s;
    pm_rdm_responder_settings_changed(R, mask, PM_SRC_RDM); /* as the platform does it */
    return PM_OK;
}
static void f_net_info(void *ctx, pm_rdm_net_info_t *out) { (void)ctx; *out = F.net; }
static void f_net_apply(void *ctx) { (void)ctx; F.net_apply++; }
static void f_renew(void *ctx) { (void)ctx; F.renew++; }
static void f_identify(void *ctx, bool on) { (void)ctx; if (on) F.identify_on++; else F.identify_off++; }
static void f_reset(void *ctx, uint8_t kind) { (void)ctx; F.reset_count++; F.reset_kind = kind; }
static void f_factory(void *ctx) { (void)ctx; F.factory++; }
static pm_err_t f_bc_start(void *ctx, pm_port_idx_t port, const pm_uid_t *target)
{
    (void)ctx;
    if (F.bc_result != PM_OK) return F.bc_result;
    F.bc_start++;
    F.bc_port = port;
    F.bc_target = *target;
    return PM_OK;
}
static void f_bc_abort(void *ctx) { (void)ctx; F.bc_abort++; }
static void f_bc_status(void *ctx, pm_port_idx_t *port, pm_rdm_boot_catch_t *out)
{
    (void)ctx;
    *port = F.bc_status_port;
    *out = F.bc;
}

static pm_fw_error_t fw_begin(void *ctx, const pm_image_desc_t *d)
{
    (void)ctx;
    F.fw_begin++;
    F.fw_desc = *d;
    if (F.fw_begin_result != PM_FW_ERROR_NONE) {
        F.fw.state = PM_FW_STATE_FAILED;
        F.fw.last_error = F.fw_begin_result;
        return F.fw_begin_result;
    }
    F.fw = (pm_fw_status_t){PM_FW_STATE_RECEIVING, PM_FW_ERROR_NONE, 0, d->image_length};
    return PM_FW_ERROR_NONE;
}
static pm_fw_error_t fw_write(void *ctx, uint32_t off, const uint8_t *data, size_t len)
{
    (void)ctx;
    F.fw_writes++;
    if (F.fw.state != PM_FW_STATE_RECEIVING || off > F.fw.received || off + len > sizeof(F.img)) {
        return PM_FW_ERROR_SEQUENCE;
    }
    memcpy(F.img + off, data, len);
    if (off + len > F.fw.received) F.fw.received = (uint32_t)(off + len);
    return PM_FW_ERROR_NONE;
}
static pm_fw_error_t fw_finish(void *ctx)
{
    (void)ctx;
    F.fw_finish++;
    F.fw.state = F.fw_finish_result == PM_FW_ERROR_NONE ? PM_FW_STATE_READY : PM_FW_STATE_FAILED;
    F.fw.last_error = F.fw_finish_result;
    return F.fw_finish_result;
}
static void fw_abort(void *ctx, pm_fw_error_t reason)
{
    (void)ctx;
    F.fw_abort++;
    F.fw_abort_reason = reason;
    F.fw.state = PM_FW_STATE_FAILED;
    F.fw.last_error = reason;
}
static void fw_status(void *ctx, pm_fw_status_t *out) { (void)ctx; *out = F.fw; }
static void fw_reboot(void *ctx) { (void)ctx; F.fw_reboot++; }

static const pm_fw_sink_t SINK = {NULL, fw_begin, fw_write, fw_finish, fw_abort, fw_status, fw_reboot};

static const pm_rdm_responder_identity_t IDENT = {
    .model_id = PM_MODEL_NODE,
    .sw_version_id = PM_FW_VERSION_ID(0, 1, 0),
    .sw_version_label = "0.1.0",
    .boot_sw_version_id = 0x00050506u,
    .boot_sw_version_label = "ESP-IDF v5.5.6",
    .model_description = "Pixelmapper Node",
    .manufacturer_label = "Prototype",
    .hw_revision = 0,
};

static void setup_with_sink(const pm_fw_sink_t *sink)
{
    memset(&F, 0, sizeof(F));
    pm_settings_defaults(&F.s);
    F.now = 1000;
    F.net.ip = 0x0A000005u;
    F.net.mask = 0xFFFFFF00u;
    F.net.dhcp_active = true;
    const uint8_t mac[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    memcpy(F.net.mac, mac, 6);
    NODE = pm_uid_make(PM_ESTA_MANUFACTURER, 0x80123456u);
    CTRL = pm_uid_make(PM_ESTA_MANUFACTURER, 0xA0000001u);
    pm_rdm_responder_identity_t id = IDENT;
    id.uid = NODE;
    pm_rdm_responder_ops_t ops = {
        .ctx = NULL, .now_ms = f_now, .settings_get = f_settings_get, .settings_apply = f_settings_apply,
        .net_info = f_net_info, .net_apply = f_net_apply, .net_renew_dhcp = f_renew, .identify = f_identify,
        .reset = f_reset, .factory_defaults = f_factory, .boot_catch_start = f_bc_start,
        .boot_catch_abort = f_bc_abort, .boot_catch_status = f_bc_status, .fw_sink = sink,
    };
    CHECK(pm_rdm_responder_size() <= sizeof(g_mem));
    R = pm_rdm_responder_init(g_mem, &id, &ops);
    CHECK(R != NULL);
}

static void setup(void) { setup_with_sink(&SINK); }

/* ------------------------------------------------------------------------------------------------ request helpers */

#define NO_RESPONSE (-100)

/* Send a request; returns the response type, NO_RESPONSE, or the negative handle() result. Checks the frame. */
static int xfer(const pm_uid_t *dest, uint8_t cc, uint16_t sub, uint16_t pid, const uint8_t *pd, uint8_t pdl)
{
    pm_rdm_msg_t m;
    memset(&m, 0, sizeof(m));
    m.dest = *dest;
    m.src = CTRL;
    m.tn = ++g_tn;
    m.port_or_resp = 1;
    m.sub_device = sub;
    m.cc = cc;
    m.pid = pid;
    m.pdl = pdl;
    m.pd = pd;
    uint8_t buf[PM_RDM_MAX_MSG_LEN];
    int n = pm_rdm_encode(&m, buf, sizeof(buf));
    CHECK(n > 0);
    g_len = pm_rdm_responder_handle(R, buf, (size_t)n, g_out, sizeof(g_out));
    memset(&g_resp, 0, sizeof(g_resp));
    if (g_len < 0) return g_len;
    if (g_len == 0) return NO_RESPONSE;
    CHECK_EQ(pm_rdm_decode(g_out, (size_t)g_len, &g_resp), PM_OK);
    CHECK(pm_uid_equal(&g_resp.dest, &CTRL));
    CHECK(pm_uid_equal(&g_resp.src, &NODE));
    CHECK_EQ(g_resp.tn, m.tn);
    CHECK_EQ(g_resp.cc, cc + 1);
    CHECK_EQ(g_resp.sub_device, sub);
    if (pid != PM_RDM_PID_QUEUED_MESSAGE) CHECK_EQ(g_resp.pid, pid);
    CHECK(g_resp.port_or_resp <= PM_RDM_RESP_ACK_OVERFLOW);
    CHECK_EQ(g_resp.message_count, pm_rdm_responder_message_count(R));
    if (g_resp.port_or_resp == PM_RDM_RESP_NACK_REASON) CHECK_EQ(g_resp.pdl, 2);
    return g_resp.port_or_resp;
}

static int get(uint16_t pid, const uint8_t *pd, uint8_t pdl) { return xfer(&NODE, PM_RDM_CC_GET, 0, pid, pd, pdl); }
static int set(uint16_t pid, const uint8_t *pd, uint8_t pdl) { return xfer(&NODE, PM_RDM_CC_SET, 0, pid, pd, pdl); }
static int get0(uint16_t pid) { return get(pid, NULL, 0); }
static int get_u8arg(uint16_t pid, uint8_t v) { return get(pid, &v, 1); }
static int set_u8(uint16_t pid, uint8_t v) { return set(pid, &v, 1); }
static int set_u16(uint16_t pid, uint16_t v)
{
    uint8_t b[2];
    pm_put_be16(b, v);
    return set(pid, b, 2);
}
static int get_u16arg(uint16_t pid, uint16_t v)
{
    uint8_t b[2];
    pm_put_be16(b, v);
    return get(pid, b, 2);
}
static int get_u32arg(uint16_t pid, uint32_t v)
{
    uint8_t b[4];
    pm_put_be32(b, v);
    return get(pid, b, 4);
}

static uint16_t nack_reason(void) { return g_resp.pdl == 2 ? pm_get_be16(g_resp.pd) : 0xFFFF; }

#define CHECK_ACK(call) CHECK_EQ((call), PM_RDM_RESP_ACK)
#define CHECK_NACK(call, reason)                       \
    do {                                               \
        CHECK_EQ((call), PM_RDM_RESP_NACK_REASON);     \
        CHECK_EQ(nack_reason(), (reason));             \
    } while (0)

static uint32_t resp_value(void)
{
    if (g_resp.pdl == 1) return g_resp.pd[0];
    if (g_resp.pdl == 2) return pm_get_be16(g_resp.pd);
    return 0xFFFFFFFFu;
}

static bool resp_is_text(const char *s)
{
    size_t n = strlen(s);
    return g_resp.pdl == n && memcmp(g_resp.pd, s, n) == 0;
}

/* ------------------------------------------------------------------------------------------------ description list */

typedef struct {
    uint16_t pid;
    uint8_t pdl, type, cc, unit, prefix;
    int32_t min, max, def;
    const char *text;
} tdesc_t;

#define TDESC(p, pdl_, type_, cc_, unit_, prefix_, min_, max_, def_, text_) \
    {p, pdl_, type_, cc_, unit_, prefix_, min_, max_, def_, text_},
static const tdesc_t TDESCS[] = {PM_NODE_PID_DESC_LIST(TDESC)};
#undef TDESC
#define NDESCS (sizeof(TDESCS) / sizeof(TDESCS[0]))

static const uint16_t STD_REQUIRED[] = {PM_NODE_STD_PIDS_REQUIRED};
static const uint16_t STD_LISTED[] = {PM_NODE_STD_PIDS_LISTED};
#define NREQ (sizeof(STD_REQUIRED) / sizeof(STD_REQUIRED[0]))
#define NLISTED (sizeof(STD_LISTED) / sizeof(STD_LISTED[0]))

/* Standard PIDs: allowed CCs and request PDLs (manufacturer PIDs come from TDESCS) */
enum { TG = 1, TS = 2 };
typedef struct {
    uint16_t pid;
    uint8_t cc, get_pdl, set_min, set_max;
} tpid_t;
static const tpid_t STD_TP[] = {
    {PM_RDM_PID_QUEUED_MESSAGE, TG, 1, 0, 0},
    {PM_RDM_PID_STATUS_MESSAGES, TG, 1, 0, 0},
    {PM_RDM_PID_STATUS_ID_DESCRIPTION, TG, 2, 0, 0},
    {PM_RDM_PID_CLEAR_STATUS_ID, TS, 0, 0, 0},
    {PM_RDM_PID_SUPPORTED_PARAMETERS, TG, 0, 0, 0},
    {PM_RDM_PID_PARAMETER_DESCRIPTION, TG, 2, 0, 0},
    {PM_RDM_PID_DEVICE_INFO, TG, 0, 0, 0},
    {PM_RDM_PID_PRODUCT_DETAIL_ID_LIST, TG, 0, 0, 0},
    {PM_RDM_PID_DEVICE_MODEL_DESCRIPTION, TG, 0, 0, 0},
    {PM_RDM_PID_MANUFACTURER_LABEL, TG, 0, 0, 0},
    {PM_RDM_PID_DEVICE_LABEL, TG | TS, 0, 0, 32},
    {PM_RDM_PID_FACTORY_DEFAULTS, TG | TS, 0, 0, 0},
    {PM_RDM_PID_SOFTWARE_VERSION_LABEL, TG, 0, 0, 0},
    {PM_RDM_PID_BOOT_SOFTWARE_VERSION_ID, TG, 0, 0, 0},
    {PM_RDM_PID_BOOT_SOFTWARE_VERSION_LABEL, TG, 0, 0, 0},
    {PM_RDM_PID_DISPLAY_LEVEL, TG | TS, 0, 1, 1},
    {PM_RDM_PID_IDENTIFY_DEVICE, TG | TS, 0, 1, 1},
    {PM_RDM_PID_RESET_DEVICE, TS, 0, 1, 1},
    {PM_RDM_PID_LIST_INTERFACES, TG, 0, 0, 0},
    {PM_RDM_PID_INTERFACE_LABEL, TG, 4, 0, 0},
    {PM_RDM_PID_INTERFACE_HARDWARE_ADDRESS_TYPE1, TG, 4, 0, 0},
    {PM_RDM_PID_IPV4_DHCP_MODE, TG | TS, 4, 5, 5},
    {PM_RDM_PID_IPV4_CURRENT_ADDRESS, TG, 4, 0, 0},
    {PM_RDM_PID_IPV4_STATIC_ADDRESS, TG | TS, 4, 9, 9},
    {PM_RDM_PID_INTERFACE_RENEW_DHCP, TS, 0, 4, 4},
    {PM_RDM_PID_INTERFACE_APPLY_CONFIGURATION, TS, 0, 4, 4},
    {PM_RDM_PID_IPV4_DEFAULT_ROUTE, TG | TS, 0, 8, 8},
};
#define NSTD_TP (sizeof(STD_TP) / sizeof(STD_TP[0]))

static tpid_t tp_of_desc(const tdesc_t *d)
{
    tpid_t t = {d->pid, d->cc, 0, d->pdl, d->pdl};
    if (d->pid == PM_PID_FW_WRITE) t.set_min = 4;
    return t;
}

/* ------------------------------------------------------------------------------------------- framing, addressing */

static void test_framing(void)
{
    g_context = "framing";
    setup();
    uint8_t buf[PM_RDM_MAX_MSG_LEN];
    pm_rdm_msg_t m;
    memset(&m, 0, sizeof(m));
    m.dest = NODE;
    m.src = CTRL;
    m.cc = PM_RDM_CC_GET;
    m.pid = PM_RDM_PID_DEVICE_INFO;
    int n = pm_rdm_encode(&m, buf, sizeof(buf));
    CHECK(pm_rdm_responder_handle(R, buf, (size_t)n, g_out, sizeof(g_out)) > 0);
    buf[n - 1] ^= 1; /* checksum */
    CHECK_EQ(pm_rdm_responder_handle(R, buf, (size_t)n, g_out, sizeof(g_out)), PM_ERR_BAD_CHECKSUM);
    buf[n - 1] ^= 1;
    CHECK(pm_rdm_responder_handle(R, buf, (size_t)n - 1, g_out, sizeof(g_out)) < 0); /* truncated */
    CHECK(pm_rdm_responder_handle(R, buf, 10, g_out, sizeof(g_out)) < 0);
    buf[1] = 0x02; /* sub-start code */
    CHECK(pm_rdm_responder_handle(R, buf, (size_t)n, g_out, sizeof(g_out)) < 0);
    buf[1] = PM_RDM_SUB_START_CODE;
    buf[2]++; /* message length != 24 + PDL */
    CHECK(pm_rdm_responder_handle(R, buf, (size_t)n, g_out, sizeof(g_out)) < 0);
    buf[2]--;
    CHECK_EQ(pm_rdm_responder_handle(R, buf, (size_t)n, g_out, 20), PM_ERR_NO_SPACE);
    CHECK_EQ(pm_rdm_responder_handle(NULL, buf, (size_t)n, g_out, sizeof(g_out)), PM_ERR_INVALID_ARG);
    CHECK_EQ(pm_rdm_responder_handle(R, buf, (size_t)n, NULL, 0), PM_ERR_INVALID_ARG);

    /* PDL 231 is accepted (answered) */
    uint8_t big[PM_RDM_MAX_PDL];
    memset(big, 0x41, sizeof(big));
    CHECK_NACK(get(PM_RDM_PID_DEVICE_INFO, big, PM_RDM_MAX_PDL), PM_NR_FORMAT_ERROR);
    CHECK_NACK(get(0x7FDF, big, PM_RDM_MAX_PDL), PM_NR_UNKNOWN_PID);
    CHECK_NACK(set(PM_RDM_PID_DEVICE_LABEL, big, PM_RDM_MAX_PDL), PM_NR_FORMAT_ERROR);
    CHECK_NACK(set(PM_PID_FW_WRITE, big, PM_RDM_MAX_PDL), PM_NR_FORMAT_ERROR);

    /* discovery and response CCs are never answered (the node is not on a line) */
    uint8_t range[12];
    memset(range, 0, 6);
    memset(range + 6, 0xFF, 6);
    CHECK_EQ(xfer(&NODE, PM_RDM_CC_DISCOVERY, 0, PM_RDM_PID_DISC_UNIQUE_BRANCH, range, 12), NO_RESPONSE);
    CHECK_EQ(xfer(&NODE, PM_RDM_CC_DISCOVERY, 0, PM_RDM_PID_DISC_MUTE, NULL, 0), NO_RESPONSE);
    CHECK_EQ(xfer(&NODE, PM_RDM_CC_DISCOVERY, 0, PM_RDM_PID_DISC_UN_MUTE, NULL, 0), NO_RESPONSE);
    CHECK_EQ(xfer(&NODE, PM_RDM_CC_GET_RESPONSE, 0, PM_RDM_PID_DEVICE_INFO, NULL, 0), NO_RESPONSE);
    CHECK_EQ(xfer(&NODE, PM_RDM_CC_SET_RESPONSE, 0, PM_RDM_PID_DEVICE_INFO, NULL, 0), NO_RESPONSE);
}

static void test_addressing(void)
{
    g_context = "addressing";
    setup();
    pm_uid_t other = pm_uid_make(PM_ESTA_MANUFACTURER, 0x80000001u);
    pm_uid_t bcast = pm_uid_make(0xFFFFu, 0xFFFFFFFFu);
    pm_uid_t vcast = pm_uid_make(PM_ESTA_MANUFACTURER, 0xFFFFFFFFu);
    pm_uid_t foreign = pm_uid_make(0x1234u, 0xFFFFFFFFu);
    uint8_t one = 1;

    CHECK_EQ(xfer(&other, PM_RDM_CC_GET, 0, PM_RDM_PID_DEVICE_INFO, NULL, 0), NO_RESPONSE);
    CHECK_EQ(xfer(&other, PM_RDM_CC_SET, 0, PM_RDM_PID_IDENTIFY_DEVICE, &one, 1), NO_RESPONSE);
    CHECK_EQ(F.identify_on, 0);

    /* foreign vendorcast: nothing happens */
    CHECK_EQ(xfer(&foreign, PM_RDM_CC_SET, 0, PM_RDM_PID_IDENTIFY_DEVICE, &one, 1), NO_RESPONSE);
    CHECK_EQ(F.identify_on, 0);
    CHECK_EQ(xfer(&foreign, PM_RDM_CC_SET, 0, PM_PID_PANEL_LOCK, &one, 1), NO_RESPONSE);
    CHECK_EQ(F.apply_count, 0);

    /* broadcast SET: executed, not answered */
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_SET, 0, PM_RDM_PID_IDENTIFY_DEVICE, &one, 1), NO_RESPONSE);
    CHECK_EQ(F.identify_on, 1);
    CHECK_ACK(get0(PM_RDM_PID_IDENTIFY_DEVICE));
    CHECK_EQ(resp_value(), 1);
    /* broadcast SET to all sub-devices too */
    uint8_t zero = 0;
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_SET, 0xFFFF, PM_RDM_PID_IDENTIFY_DEVICE, &zero, 1), NO_RESPONSE);
    CHECK_EQ(F.identify_off, 1);
    /* broadcast SET with a bad value: no answer, no effect */
    uint8_t five = 5;
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_SET, 0, PM_RDM_PID_IDENTIFY_DEVICE, &five, 1), NO_RESPONSE);
    CHECK_EQ(F.identify_on + F.identify_off, 2);

    /* own vendorcast SET: executed, not answered */
    const char *lbl = "Vendorcast";
    CHECK_EQ(xfer(&vcast, PM_RDM_CC_SET, 0, PM_RDM_PID_DEVICE_LABEL, (const uint8_t *)lbl, (uint8_t)strlen(lbl)),
             NO_RESPONSE);
    CHECK(strcmp(F.s.short_name, lbl) == 0);

    /* GET to broadcast / vendorcast: ignored */
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_GET, 0, PM_RDM_PID_DEVICE_INFO, NULL, 0), NO_RESPONSE);
    CHECK_EQ(xfer(&vcast, PM_RDM_CC_GET, 0, PM_RDM_PID_DEVICE_INFO, NULL, 0), NO_RESPONSE);
    /* ... and has no side effect: a broadcast GET QUEUED_MESSAGE / STATUS_MESSAGES must not consume anything */
    pm_rdm_responder_queue_pid(R, PM_RDM_PID_DEVICE_INFO);
    pm_rdm_responder_status(R, PM_STATUS_PANEL_UNLOCKED_LOCAL, PM_RDM_STATUS_ADVISORY, 0, 0);
    uint8_t adv = PM_RDM_STATUS_ADVISORY;
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_GET, 0, PM_RDM_PID_QUEUED_MESSAGE, &adv, 1), NO_RESPONSE);
    CHECK_EQ(xfer(&vcast, PM_RDM_CC_GET, 0, PM_RDM_PID_QUEUED_MESSAGE, &adv, 1), NO_RESPONSE);
    CHECK_EQ(xfer(&vcast, PM_RDM_CC_GET, 0, PM_RDM_PID_STATUS_MESSAGES, &adv, 1), NO_RESPONSE);
    CHECK_EQ(pm_rdm_responder_message_count(R), 2);

    /* manufacturer PIDs to FFFF:FFFFFFFF are ignored, to the own vendorcast executed */
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_SET, 0, PM_PID_PANEL_LOCK, &one, 1), NO_RESPONSE);
    CHECK_EQ(F.s.panel_lock, false);
    CHECK_EQ(xfer(&vcast, PM_RDM_CC_SET, 0, PM_PID_PANEL_LOCK, &one, 1), NO_RESPONSE);
    CHECK_EQ(F.s.panel_lock, true);
    /* standard PID to broadcast (E1.37-2): executed */
    uint8_t ifc[4];
    pm_put_be32(ifc, PM_RDM_RESP_INTERFACE_ID);
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_SET, 0, PM_RDM_PID_INTERFACE_APPLY_CONFIGURATION, ifc, 4), NO_RESPONSE);
    CHECK_EQ(F.net_apply, 1);
}

static void test_nack_order(void)
{
    g_context = "nack order";
    setup();
    uint8_t v = 1;
    /* sub-device first */
    CHECK_NACK(xfer(&NODE, PM_RDM_CC_GET, 1, 0x7FDF, NULL, 0), PM_NR_SUB_DEVICE_OUT_OF_RANGE);
    CHECK_NACK(xfer(&NODE, PM_RDM_CC_SET, 0x0200, PM_RDM_PID_IDENTIFY_DEVICE, &v, 1), PM_NR_SUB_DEVICE_OUT_OF_RANGE);
    CHECK_NACK(xfer(&NODE, PM_RDM_CC_GET, 0xFFFF, PM_RDM_PID_DEVICE_INFO, NULL, 0), PM_NR_SUB_DEVICE_OUT_OF_RANGE);
    CHECK_NACK(xfer(&NODE, PM_RDM_CC_GET, 0xFFFF, 0x7FDF, NULL, 0), PM_NR_SUB_DEVICE_OUT_OF_RANGE);
    /* SET to all sub-devices = root */
    CHECK_ACK(xfer(&NODE, PM_RDM_CC_SET, 0xFFFF, PM_RDM_PID_IDENTIFY_DEVICE, &v, 1));
    CHECK_EQ(g_resp.sub_device, 0xFFFF);
    CHECK_EQ(F.identify_on, 1);
    CHECK_NACK(xfer(&NODE, PM_RDM_CC_SET, 0xFFFF, 0x7FDF, NULL, 0), PM_NR_UNKNOWN_PID);
    /* unknown PIDs */
    const uint16_t unknown[] = {0x0000, PM_RDM_PID_DMX_START_ADDRESS, PM_RDM_PID_DMX_PERSONALITY, 0x0704, 0x0708,
                                PM_PID_ENTER_BOOTLOADER, PM_PID_BOOT_INFO, 0x8105, 0x8125, 0x8143, 0x81FF, 0xFFDF};
    for (size_t i = 0; i < sizeof(unknown) / sizeof(unknown[0]); i++) {
        CHECK_NACK(get0(unknown[i]), PM_NR_UNKNOWN_PID);
        CHECK_NACK(set(unknown[i], NULL, 0), PM_NR_UNKNOWN_PID);
    }
    /* CC before PDL, PDL before value */
    uint8_t five[5] = {5, 5, 5, 5, 5};
    CHECK_NACK(set(PM_RDM_PID_DEVICE_INFO, five, 5), PM_NR_UNSUPPORTED_COMMAND_CLASS);
    CHECK_NACK(get(PM_RDM_PID_RESET_DEVICE, five, 5), PM_NR_UNSUPPORTED_COMMAND_CLASS);
    CHECK_NACK(set(PM_RDM_PID_IDENTIFY_DEVICE, five, 2), PM_NR_FORMAT_ERROR);
    CHECK_NACK(set(PM_PID_PORT_DMX_RATE(1), five, 2), PM_NR_FORMAT_ERROR);
}

/* Every PID: wrong CC, wrong PDL (short and long), sub-devices; valid PDL gets an answer. */
static void check_pid_paths(const tpid_t *t)
{
    static uint8_t pd[PM_RDM_MAX_PDL];
    memset(pd, 0, sizeof(pd));
    if (t->cc & TG) {
        CHECK(get(t->pid, pd, t->get_pdl) == PM_RDM_RESP_ACK || g_resp.port_or_resp == PM_RDM_RESP_NACK_REASON);
        if (t->get_pdl == 0) CHECK_EQ(g_resp.port_or_resp, PM_RDM_RESP_ACK);
        if (g_resp.port_or_resp == PM_RDM_RESP_NACK_REASON) CHECK_EQ(nack_reason(), PM_NR_DATA_OUT_OF_RANGE);
        CHECK_NACK(get(t->pid, pd, (uint8_t)(t->get_pdl + 1u)), PM_NR_FORMAT_ERROR);
        if (t->get_pdl > 0) CHECK_NACK(get(t->pid, pd, (uint8_t)(t->get_pdl - 1u)), PM_NR_FORMAT_ERROR);
        CHECK_NACK(xfer(&NODE, PM_RDM_CC_GET, 0xFFFF, t->pid, pd, t->get_pdl), PM_NR_SUB_DEVICE_OUT_OF_RANGE);
        CHECK_NACK(xfer(&NODE, PM_RDM_CC_GET, 1, t->pid, pd, t->get_pdl), PM_NR_SUB_DEVICE_OUT_OF_RANGE);
    } else {
        CHECK_NACK(get0(t->pid), PM_NR_UNSUPPORTED_COMMAND_CLASS);
        CHECK_NACK(get(t->pid, pd, 3), PM_NR_UNSUPPORTED_COMMAND_CLASS);
    }
    if (t->cc & TS) {
        if (t->set_max < PM_RDM_MAX_PDL) {
            CHECK_NACK(set(t->pid, pd, (uint8_t)(t->set_max + 1u)), PM_NR_FORMAT_ERROR);
        }
        if (t->set_min > 0) CHECK_NACK(set(t->pid, pd, (uint8_t)(t->set_min - 1u)), PM_NR_FORMAT_ERROR);
        CHECK_NACK(xfer(&NODE, PM_RDM_CC_SET, 2, t->pid, pd, t->set_min), PM_NR_SUB_DEVICE_OUT_OF_RANGE);
    } else {
        CHECK_NACK(set(t->pid, pd, 0), PM_NR_UNSUPPORTED_COMMAND_CLASS);
        CHECK_NACK(set(t->pid, pd, 1), PM_NR_UNSUPPORTED_COMMAND_CLASS);
    }
}

static void test_all_pid_paths(void)
{
    g_context = "pid paths";
    setup();
    for (size_t i = 0; i < NSTD_TP; i++) check_pid_paths(&STD_TP[i]);
    for (size_t i = 0; i < NDESCS; i++) {
        tpid_t t = tp_of_desc(&TDESCS[i]);
        check_pid_paths(&t);
    }
    /* the test table covers exactly the required + listed standard PIDs */
    CHECK_EQ(NSTD_TP, NREQ + NLISTED);
    for (size_t i = 0; i < NSTD_TP; i++) {
        bool found = false;
        for (size_t k = 0; k < NREQ; k++) found |= STD_REQUIRED[k] == STD_TP[i].pid;
        for (size_t k = 0; k < NLISTED; k++) found |= STD_LISTED[k] == STD_TP[i].pid;
        CHECK(found);
    }
}

/* ------------------------------------------------------------------------------------------------ standard PIDs */

static void test_info_pids(void)
{
    g_context = "info pids";
    setup();
    CHECK_ACK(get0(PM_RDM_PID_DEVICE_INFO));
    CHECK_EQ(g_resp.pdl, 19);
    const uint8_t *p = g_resp.pd;
    CHECK_EQ(pm_get_be16(p), 0x0100);
    CHECK_EQ(pm_get_be16(p + 2), PM_MODEL_NODE);
    CHECK_EQ(pm_get_be16(p + 4), PM_NODE_PRODUCT_CATEGORY);
    CHECK_EQ(pm_get_be32(p + 6), IDENT.sw_version_id);
    CHECK_EQ(pm_get_be16(p + 10), 0);
    CHECK_EQ(p[12], 0);
    CHECK_EQ(p[13], 0);
    CHECK_EQ(pm_get_be16(p + 14), 0xFFFF);
    CHECK_EQ(pm_get_be16(p + 16), 0);
    CHECK_EQ(p[18], 0);

    CHECK_ACK(get0(PM_RDM_PID_SOFTWARE_VERSION_LABEL));
    CHECK(resp_is_text("0.1.0"));
    CHECK_ACK(get0(PM_RDM_PID_BOOT_SOFTWARE_VERSION_ID));
    CHECK_EQ(g_resp.pdl, 4);
    CHECK_EQ(pm_get_be32(g_resp.pd), IDENT.boot_sw_version_id);
    CHECK_ACK(get0(PM_RDM_PID_BOOT_SOFTWARE_VERSION_LABEL));
    CHECK(resp_is_text("ESP-IDF v5.5.6"));
    CHECK_ACK(get0(PM_RDM_PID_DEVICE_MODEL_DESCRIPTION));
    CHECK(resp_is_text("Pixelmapper Node"));
    CHECK_ACK(get0(PM_RDM_PID_MANUFACTURER_LABEL));
    CHECK(resp_is_text("Prototype"));
    CHECK_ACK(get0(PM_RDM_PID_PRODUCT_DETAIL_ID_LIST));
    CHECK_EQ(g_resp.pdl, 2);
    CHECK_EQ(pm_get_be16(g_resp.pd), PM_NODE_PRODUCT_DETAIL);

    /* SUPPORTED_PARAMETERS: listed standard + all described manufacturer PIDs, no required ones, no duplicates */
    CHECK_ACK(get0(PM_RDM_PID_SUPPORTED_PARAMETERS));
    CHECK_EQ(g_resp.pdl % 2, 0);
    size_t n = g_resp.pdl / 2u;
    CHECK_EQ(n, NLISTED + NDESCS);
    uint16_t list[PM_RDM_MAX_PDL / 2];
    for (size_t i = 0; i < n; i++) list[i] = pm_get_be16(g_resp.pd + 2 * i);
    for (size_t i = 0; i < n; i++) {
        for (size_t k = i + 1; k < n; k++) CHECK(list[i] != list[k]);
        for (size_t k = 0; k < NREQ; k++) CHECK(list[i] != STD_REQUIRED[k]);
        CHECK(list[i] > PM_RDM_PID_DISC_UN_MUTE);
    }
    for (size_t k = 0; k < NLISTED; k++) {
        bool found = false;
        for (size_t i = 0; i < n; i++) found |= list[i] == STD_LISTED[k];
        CHECK(found);
    }
    for (size_t k = 0; k < NDESCS; k++) {
        bool found = false;
        for (size_t i = 0; i < n; i++) found |= list[i] == TDESCS[k].pid;
        CHECK(found);
    }

    /* PARAMETER_DESCRIPTION for every listed manufacturer PID */
    for (size_t k = 0; k < NDESCS; k++) {
        const tdesc_t *d = &TDESCS[k];
        CHECK_ACK(get_u16arg(PM_RDM_PID_PARAMETER_DESCRIPTION, d->pid));
        CHECK(g_resp.pdl >= 20 && g_resp.pdl <= 52);
        p = g_resp.pd;
        CHECK_EQ(pm_get_be16(p), d->pid);
        CHECK_EQ(p[2], d->pdl);
        CHECK_EQ(p[3], d->type);
        CHECK(p[4] >= 1 && p[4] <= 3);
        CHECK_EQ(p[4], d->cc);
        CHECK_EQ(p[5], 0);
        CHECK_EQ(p[6], d->unit);
        CHECK_EQ(p[7], d->prefix);
        CHECK_EQ(pm_get_be32(p + 8), (uint32_t)d->min);
        CHECK_EQ(pm_get_be32(p + 12), (uint32_t)d->max);
        CHECK_EQ(pm_get_be32(p + 16), (uint32_t)d->def);
        size_t tl = g_resp.pdl - 20u;
        CHECK(tl > 0 && tl <= 32 && tl == strlen(d->text) && memcmp(p + 20, d->text, tl) == 0);
        for (size_t i = 0; i < tl; i++) CHECK(isprint(p[20 + i]));
    }
    CHECK_NACK(get_u16arg(PM_RDM_PID_PARAMETER_DESCRIPTION, PM_RDM_PID_DEVICE_INFO), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get_u16arg(PM_RDM_PID_PARAMETER_DESCRIPTION, PM_RDM_PID_IDENTIFY_DEVICE), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get_u16arg(PM_RDM_PID_PARAMETER_DESCRIPTION, PM_PID_ENTER_BOOTLOADER), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get_u16arg(PM_RDM_PID_PARAMETER_DESCRIPTION, 0x81FF), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get0(PM_RDM_PID_PARAMETER_DESCRIPTION), PM_NR_FORMAT_ERROR);
    CHECK_NACK(get_u8arg(PM_RDM_PID_PARAMETER_DESCRIPTION, 0x81), PM_NR_FORMAT_ERROR);
    CHECK_NACK(get_u32arg(PM_RDM_PID_PARAMETER_DESCRIPTION, 0x81000000u), PM_NR_FORMAT_ERROR);
}

static void test_label_identify_reset(void)
{
    g_context = "label/identify/reset/display/factory";
    setup();
    CHECK_ACK(get0(PM_RDM_PID_DEVICE_LABEL));
    CHECK(resp_is_text("PM Node"));
    CHECK_ACK(set(PM_RDM_PID_DEVICE_LABEL, (const uint8_t *)"Hello", 5));
    CHECK(strcmp(F.s.short_name, "Hello") == 0);
    CHECK_EQ(F.last_mask, PM_CHG_NAMES);
    CHECK_ACK(get0(PM_RDM_PID_DEVICE_LABEL));
    CHECK(resp_is_text("Hello"));
    const char *longl = "ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"; /* 32 */
    CHECK_ACK(set(PM_RDM_PID_DEVICE_LABEL, (const uint8_t *)longl, 32));
    CHECK(strcmp(F.s.short_name, "ABCDEFGHIJKLMNOPQ") == 0);
    CHECK_ACK(get0(PM_RDM_PID_DEVICE_LABEL));
    CHECK_EQ(g_resp.pdl, 17);
    CHECK_ACK(set(PM_RDM_PID_DEVICE_LABEL, (const uint8_t *)"AB\0CD", 5));
    CHECK(strcmp(F.s.short_name, "AB") == 0);
    CHECK_ACK(set(PM_RDM_PID_DEVICE_LABEL, NULL, 0));
    CHECK(strcmp(F.s.short_name, "") == 0);
    CHECK_ACK(get0(PM_RDM_PID_DEVICE_LABEL));
    CHECK_EQ(g_resp.pdl, 0);
    uint8_t l33[33];
    memset(l33, 'x', sizeof(l33));
    CHECK_NACK(set(PM_RDM_PID_DEVICE_LABEL, l33, 33), PM_NR_FORMAT_ERROR);
    CHECK_EQ(pm_rdm_responder_message_count(R), 0); /* names are visible in ArtPollReply */

    CHECK_ACK(get0(PM_RDM_PID_IDENTIFY_DEVICE));
    CHECK_EQ(resp_value(), 0);
    CHECK_ACK(set_u8(PM_RDM_PID_IDENTIFY_DEVICE, 1));
    CHECK_EQ(F.identify_on, 1);
    CHECK_ACK(get0(PM_RDM_PID_IDENTIFY_DEVICE));
    CHECK_EQ(resp_value(), 1);
    CHECK_NACK(set_u8(PM_RDM_PID_IDENTIFY_DEVICE, 2), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(set_u8(PM_RDM_PID_IDENTIFY_DEVICE, 0xFF), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_ACK(set_u8(PM_RDM_PID_IDENTIFY_DEVICE, 0));
    CHECK_EQ(F.identify_off, 1);
    CHECK_ACK(get0(PM_RDM_PID_IDENTIFY_DEVICE));
    CHECK_EQ(resp_value(), 0);

    /* RESET_DEVICE runs after the response, from poll() */
    CHECK_NACK(set_u8(PM_RDM_PID_RESET_DEVICE, 0x00), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(set_u8(PM_RDM_PID_RESET_DEVICE, 0x02), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(set_u8(PM_RDM_PID_RESET_DEVICE, 0xFE), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_ACK(set_u8(PM_RDM_PID_RESET_DEVICE, 0x01));
    CHECK_EQ(F.reset_count, 0);
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.reset_count, 0);
    F.now += PM_RDM_RESP_RESET_DELAY_MS;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.reset_count, 1);
    CHECK_EQ(F.reset_kind, 0x01);
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.reset_count, 1);
    CHECK_ACK(set_u8(PM_RDM_PID_RESET_DEVICE, 0xFF));
    F.now += PM_RDM_RESP_RESET_DELAY_MS;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.reset_count, 2);
    CHECK_EQ(F.reset_kind, 0xFF);

    /* DISPLAY_LEVEL */
    CHECK_ACK(get0(PM_RDM_PID_DISPLAY_LEVEL));
    CHECK_EQ(resp_value(), 255);
    CHECK_ACK(set_u8(PM_RDM_PID_DISPLAY_LEVEL, 0));
    CHECK_EQ(F.s.display_level, 0);
    CHECK_EQ(F.last_mask, PM_CHG_DISPLAY);
    CHECK_ACK(set_u8(PM_RDM_PID_DISPLAY_LEVEL, 10));
    CHECK_ACK(get0(PM_RDM_PID_DISPLAY_LEVEL));
    CHECK_EQ(resp_value(), 10);

    /* FACTORY_DEFAULTS */
    setup();
    CHECK_ACK(get0(PM_RDM_PID_FACTORY_DEFAULTS));
    CHECK_EQ(resp_value(), 1);
    F.s.hw_revision_override = 2; /* not a user setting */
    CHECK_ACK(get0(PM_RDM_PID_FACTORY_DEFAULTS));
    CHECK_EQ(resp_value(), 1);
    CHECK_ACK(set_u8(PM_PID_PORT_DMX_RATE(3), 30));
    CHECK_ACK(get0(PM_RDM_PID_FACTORY_DEFAULTS));
    CHECK_EQ(resp_value(), 0);
    CHECK_ACK(set(PM_RDM_PID_FACTORY_DEFAULTS, NULL, 0));
    CHECK_EQ(F.factory, 1);

    /* settings_apply failure */
    F.apply_result = PM_ERR_IO;
    CHECK_NACK(set_u8(PM_RDM_PID_DISPLAY_LEVEL, 1), PM_NR_HARDWARE_FAULT);
}

/* ------------------------------------------------------------------------------------------------ E1.37-2 */

static int ifc_req(uint8_t cc, uint16_t pid, uint32_t id, const uint8_t *extra, uint8_t n)
{
    uint8_t b[16];
    pm_put_be32(b, id);
    if (n) memcpy(b + 4, extra, n);
    return xfer(&NODE, cc, 0, pid, b, (uint8_t)(4u + n));
}

static void test_e137_2(void)
{
    g_context = "e1.37-2";
    setup();
    const uint32_t ID = PM_RDM_RESP_INTERFACE_ID;
    CHECK_ACK(get0(PM_RDM_PID_LIST_INTERFACES));
    CHECK_EQ(g_resp.pdl, 6);
    CHECK_EQ(pm_get_be32(g_resp.pd), 1);
    CHECK_EQ(pm_get_be16(g_resp.pd + 4), 1);

    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_INTERFACE_LABEL, ID, NULL, 0));
    CHECK(g_resp.pdl > 4 && g_resp.pdl <= 36 && pm_get_be32(g_resp.pd) == ID);
    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_INTERFACE_HARDWARE_ADDRESS_TYPE1, ID, NULL, 0));
    CHECK_EQ(g_resp.pdl, 10);
    CHECK(memcmp(g_resp.pd + 4, F.net.mac, 6) == 0);

    /* wrong interface ID everywhere */
    const uint16_t get_pids[] = {PM_RDM_PID_INTERFACE_LABEL, PM_RDM_PID_INTERFACE_HARDWARE_ADDRESS_TYPE1,
                                 PM_RDM_PID_IPV4_DHCP_MODE, PM_RDM_PID_IPV4_CURRENT_ADDRESS,
                                 PM_RDM_PID_IPV4_STATIC_ADDRESS};
    for (size_t i = 0; i < sizeof(get_pids) / sizeof(get_pids[0]); i++) {
        CHECK_NACK(ifc_req(PM_RDM_CC_GET, get_pids[i], 0, NULL, 0), PM_NR_DATA_OUT_OF_RANGE);
        CHECK_NACK(ifc_req(PM_RDM_CC_GET, get_pids[i], 2, NULL, 0), PM_NR_DATA_OUT_OF_RANGE);
    }
    uint8_t z[5] = {0};
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_DHCP_MODE, 2, z, 1), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_STATIC_ADDRESS, 2, z, 5), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_INTERFACE_RENEW_DHCP, 0, NULL, 0), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_INTERFACE_APPLY_CONFIGURATION, 2, NULL, 0),
               PM_NR_DATA_OUT_OF_RANGE);
    CHECK_EQ(F.apply_count + F.net_apply + F.renew, 0);

    /* DHCP mode: stored at once, active only after APPLY_CONFIGURATION */
    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_IPV4_DHCP_MODE, ID, NULL, 0));
    CHECK_EQ(g_resp.pdl, 5);
    CHECK_EQ(g_resp.pd[4], 1);
    uint8_t off = 0, two = 2;
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_DHCP_MODE, ID, &two, 1), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_DHCP_MODE, ID, &off, 1));
    CHECK_EQ(F.s.ip.dhcp, false);
    CHECK_EQ(F.last_mask, PM_CHG_IP);
    CHECK_EQ(F.net_apply, 0);
    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_IPV4_DHCP_MODE, ID, NULL, 0));
    CHECK_EQ(g_resp.pd[4], 0);

    /* current address */
    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_IPV4_CURRENT_ADDRESS, ID, NULL, 0));
    CHECK_EQ(g_resp.pdl, 10);
    CHECK_EQ(pm_get_be32(g_resp.pd + 4), 0x0A000005u);
    CHECK_EQ(g_resp.pd[8], 24);
    CHECK_EQ(g_resp.pd[9], 1);
    F.net.dhcp_active = false;
    F.net.mask = 0xFF000000u;
    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_IPV4_CURRENT_ADDRESS, ID, NULL, 0));
    CHECK_EQ(g_resp.pd[8], 8);
    CHECK_EQ(g_resp.pd[9], 0);

    /* static address */
    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_IPV4_STATIC_ADDRESS, ID, NULL, 0));
    CHECK_EQ(g_resp.pdl, 9);
    /* static_ip 0 = Art-Net fallback 2.B.C.D (B = MAC[3] + OemHi + OemLo): 2.(0x33 + 0xFF).0x44.0x55 */
    CHECK_EQ(pm_get_be32(g_resp.pd + 4), 0x02324455u);
    CHECK_EQ(pm_get_be32(g_resp.pd + 4), pm_ip_artnet_fallback(F.net.mac, PM_ARTNET_OEM_NODE));
    CHECK_EQ(g_resp.pd[8], 8);
    uint8_t st[5];
    pm_put_be32(st, 0xC0A8010Au);
    st[4] = 16;
    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_STATIC_ADDRESS, ID, st, 5));
    CHECK_EQ(F.s.ip.static_ip, 0xC0A8010Au);
    CHECK_EQ(F.s.ip.static_mask, 0xFFFF0000u);
    CHECK_ACK(ifc_req(PM_RDM_CC_GET, PM_RDM_PID_IPV4_STATIC_ADDRESS, ID, NULL, 0));
    CHECK_EQ(pm_get_be32(g_resp.pd + 4), 0xC0A8010Au);
    CHECK_EQ(g_resp.pd[8], 16);
    st[4] = 33;
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_STATIC_ADDRESS, ID, st, 5), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_EQ(F.s.ip.static_mask, 0xFFFF0000u);
    st[4] = 32;
    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_STATIC_ADDRESS, ID, st, 5));
    CHECK_EQ(F.s.ip.static_mask, 0xFFFFFFFFu);
    st[4] = 0;
    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_STATIC_ADDRESS, ID, st, 5));
    CHECK_EQ(F.s.ip.static_mask, 0);

    /* renew: only with DHCP active (E1.37-2 NR_ACTION_NOT_SUPPORTED) */
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_INTERFACE_RENEW_DHCP, ID, NULL, 0),
               PM_RDM_RESP_NR_ACTION_NOT_SUPPORTED);
    CHECK_EQ(F.renew, 0);
    F.net.dhcp_active = true;
    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_INTERFACE_RENEW_DHCP, ID, NULL, 0));
    CHECK_EQ(F.renew, 1);

    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_INTERFACE_APPLY_CONFIGURATION, ID, NULL, 0));
    CHECK_EQ(F.net_apply, 1);

    /* default route */
    CHECK_ACK(get0(PM_RDM_PID_IPV4_DEFAULT_ROUTE));
    CHECK_EQ(g_resp.pdl, 8);
    CHECK_EQ(pm_get_be32(g_resp.pd), 0);
    CHECK_EQ(pm_get_be32(g_resp.pd + 4), 0);
    uint8_t gw[4];
    pm_put_be32(gw, 0x0A000001u);
    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_DEFAULT_ROUTE, ID, gw, 4));
    CHECK_EQ(F.s.ip.gateway, 0x0A000001u);
    CHECK_ACK(get0(PM_RDM_PID_IPV4_DEFAULT_ROUTE));
    CHECK_EQ(pm_get_be32(g_resp.pd), ID);
    CHECK_EQ(pm_get_be32(g_resp.pd + 4), 0x0A000001u);
    CHECK_NACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_DEFAULT_ROUTE, 2, gw, 4), PM_NR_DATA_OUT_OF_RANGE);
    memset(gw, 0, 4);
    CHECK_ACK(ifc_req(PM_RDM_CC_SET, PM_RDM_PID_IPV4_DEFAULT_ROUTE, 0, gw, 4));
    CHECK_EQ(F.s.ip.gateway, 0);
    CHECK_EQ(pm_rdm_responder_message_count(R), 0); /* IP is visible in ArtPollReply */
}

/* ------------------------------------------------------------------------------------------------ manufacturer */

/* Value of the setting behind a numeric node PID, read directly from the fake settings. */
static long setting_of(uint16_t pid)
{
    for (unsigned p = 1; p <= PM_NUM_PORTS; p++) {
        const pm_port_settings_t *ps = &F.s.port[p - 1];
        if (pid == PM_PID_PORT_ENABLED(p)) return ps->enabled;
        if (pid == PM_PID_PORT_DMX_BREAK(p)) return ps->dmx_break_us;
        if (pid == PM_PID_PORT_DMX_MAB(p)) return ps->dmx_mab_us;
        if (pid == PM_PID_PORT_DMX_RATE(p)) return ps->dmx_rate_hz;
        if (pid == PM_PID_PORT_DMX_MBS(p)) return ps->dmx_mbs_us;
        if (pid == PM_PID_PORT_RDM_BG_DISCOVERY(p)) return ps->rdm_bg_discovery;
    }
    if (pid == PM_PID_RDM_BG_DISCOVERY_INTERVAL) return F.s.rdm_bg_interval_s;
    if (pid == PM_PID_DISPLAY_DIM_TIMEOUT) return F.s.display_dim_timeout_s;
    if (pid == PM_PID_DISPLAY_ADDRESS_FORMAT) return F.s.address_format;
    if (pid == PM_PID_PANEL_LOCK) return F.s.panel_lock;
    return -1;
}

static pm_change_mask_t mask_of(uint16_t pid)
{
    for (unsigned p = 1; p <= PM_NUM_PORTS; p++) {
        if (pid == PM_PID_PORT_ENABLED(p)) return PM_CHG_PORT(PM_CHG_P_ENABLED, p - 1);
        if (pid == PM_PID_PORT_DMX_BREAK(p) || pid == PM_PID_PORT_DMX_MAB(p) || pid == PM_PID_PORT_DMX_RATE(p) ||
            pid == PM_PID_PORT_DMX_MBS(p)) {
            return PM_CHG_PORT(PM_CHG_P_TIMING, p - 1);
        }
        if (pid == PM_PID_PORT_RDM_BG_DISCOVERY(p)) return PM_CHG_PORT(PM_CHG_P_RDM, p - 1);
    }
    if (pid == PM_PID_RDM_BG_DISCOVERY_INTERVAL) return PM_CHG_RDM_INTERVAL;
    if (pid == PM_PID_DISPLAY_DIM_TIMEOUT || pid == PM_PID_DISPLAY_ADDRESS_FORMAT) return PM_CHG_DISPLAY;
    if (pid == PM_PID_PANEL_LOCK) return PM_CHG_PANEL_LOCK;
    return 0;
}

static int set_val(const tdesc_t *d, uint32_t v)
{
    return d->pdl == 2 ? set_u16(d->pid, (uint16_t)v) : set_u8(d->pid, (uint8_t)v);
}

static void test_numeric_pids(void)
{
    g_context = "numeric pids";
    setup();
    int tested = 0;
    for (size_t i = 0; i < NDESCS; i++) {
        const tdesc_t *d = &TDESCS[i];
        if (d->type == 0x00 || d->cc != 3) continue; /* structured or GET-only */
        tested++;
        CHECK(setting_of(d->pid) >= 0);
        CHECK_ACK(get0(d->pid));
        CHECK_EQ(g_resp.pdl, d->pdl);
        CHECK_EQ(resp_value(), setting_of(d->pid));
        CHECK_EQ(resp_value(), d->def); /* defaults of pm_settings match pids.toml */
        if (d->min > 0) {
            CHECK_NACK(set_val(d, (uint32_t)(d->min - 1)), PM_NR_DATA_OUT_OF_RANGE);
            CHECK_EQ(setting_of(d->pid), d->def);
        }
        CHECK_ACK(set_val(d, (uint32_t)d->min));
        CHECK_EQ(setting_of(d->pid), d->min);
        CHECK_EQ(F.last_mask, mask_of(d->pid));
        CHECK_ACK(get0(d->pid));
        CHECK_EQ(resp_value(), d->min);
        CHECK_ACK(set_val(d, (uint32_t)d->max));
        CHECK_EQ(setting_of(d->pid), d->max);
        CHECK_ACK(get0(d->pid));
        CHECK_EQ(resp_value(), d->max);
        if (d->max + 1 < (1L << (8 * d->pdl))) {
            CHECK_NACK(set_val(d, (uint32_t)(d->max + 1)), PM_NR_DATA_OUT_OF_RANGE);
            CHECK_EQ(setting_of(d->pid), d->max);
        }
        CHECK_NACK(set_val(d, d->pdl == 2 ? 0xFFFFu : 0xFFu), PM_NR_DATA_OUT_OF_RANGE);
        CHECK_ACK(set_val(d, (uint32_t)d->def));
    }
    CHECK_EQ(tested, 4 * 6 + 4);

    /* NODE_HW_REVISION: detected value, prototype override */
    CHECK_ACK(get0(PM_PID_NODE_HW_REVISION));
    CHECK_EQ(g_resp.pdl, 1);
    CHECK_EQ(resp_value(), IDENT.hw_revision);
    F.s.hw_revision_override = 3;
    CHECK_ACK(get0(PM_PID_NODE_HW_REVISION));
    CHECK_EQ(resp_value(), 3);
    CHECK_NACK(set_u8(PM_PID_NODE_HW_REVISION, 1), PM_NR_UNSUPPORTED_COMMAND_CLASS);
}

static void test_boot_catch(void)
{
    g_context = "boot catch";
    setup();
    pm_uid_t tube = pm_uid_make(PM_ESTA_MANUFACTURER, 0x12345678u);
    uint8_t pd[7];
    pd[0] = 1;
    memcpy(pd + 1, tube.b, 6);
    CHECK_ACK(set(PM_PID_BOOT_CATCH_START, pd, 7));
    CHECK_EQ(F.bc_start, 1);
    CHECK_EQ(F.bc_port, 0);
    CHECK(pm_uid_equal(&F.bc_target, &tube));
    pd[0] = 4;
    CHECK_ACK(set(PM_PID_BOOT_CATCH_START, pd, 7));
    CHECK_EQ(F.bc_port, 3);
    pd[0] = 0;
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    pd[0] = 5;
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    F.s.port[1].enabled = false;
    pd[0] = 2;
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    F.s.port[2].direction = PM_PORT_DIR_INPUT;
    pd[0] = 3;
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    F.s.port[3].rdm_enabled = false;
    pd[0] = 4;
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    pd[0] = 1;
    pm_uid_t bc = pm_uid_make(0xFFFFu, 0xFFFFFFFFu), vc = pm_uid_make(PM_ESTA_MANUFACTURER, 0xFFFFFFFFu);
    memcpy(pd + 1, bc.b, 6);
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    memcpy(pd + 1, vc.b, 6);
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    memcpy(pd + 1, tube.b, 6);
    F.bc_result = PM_ERR_STATE;
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 7), PM_NR_DATA_OUT_OF_RANGE);
    F.bc_result = PM_OK;
    CHECK_EQ(F.bc_start, 2);
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 6), PM_NR_FORMAT_ERROR);
    CHECK_NACK(set(PM_PID_BOOT_CATCH_START, pd, 8), PM_NR_FORMAT_ERROR);

    /* status */
    CHECK_ACK(get0(PM_PID_BOOT_CATCH_STATUS));
    CHECK_EQ(g_resp.pdl, PM_PDL_BOOT_CATCH_STATUS_GET_RESPONSE);
    CHECK_EQ(g_resp.pd[0], PM_BOOT_CATCH_STATE_IDLE);
    CHECK_EQ(g_resp.pd[1], 0);
    F.bc.state = PM_BOOT_CATCH_STATE_RUNNING;
    F.bc.target = tube;
    F.bc.elapsed_ms = 1234;
    F.bc.attempts = 56;
    F.bc_status_port = 2;
    CHECK_ACK(get0(PM_PID_BOOT_CATCH_STATUS));
    CHECK_EQ(g_resp.pd[0], PM_BOOT_CATCH_STATE_RUNNING);
    CHECK_EQ(g_resp.pd[1], 3);
    CHECK(memcmp(g_resp.pd + 2, tube.b, 6) == 0);
    CHECK_EQ(pm_get_be32(g_resp.pd + 8), 1234);
    CHECK_EQ(pm_get_be16(g_resp.pd + 12), 56);

    CHECK_ACK(set(PM_PID_BOOT_CATCH_ABORT, NULL, 0));
    CHECK_EQ(F.bc_abort, 1);

    /* events -> queued BOOT_CATCH_STATUS + status messages */
    CHECK_EQ(pm_rdm_responder_message_count(R), 0);
    pm_rdm_responder_boot_catch_event(R, 2, PM_BOOT_CATCH_STATE_RUNNING);
    CHECK_EQ(pm_rdm_responder_message_count(R), 1);
    F.bc.state = PM_BOOT_CATCH_STATE_CAUGHT;
    pm_rdm_responder_boot_catch_event(R, 2, PM_BOOT_CATCH_STATE_CAUGHT);
    CHECK_EQ(pm_rdm_responder_message_count(R), 2); /* PID deduplicated + 1 status */
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_PID_BOOT_CATCH_STATUS);
    CHECK_EQ(g_resp.cc, PM_RDM_CC_GET_RESPONSE);
    CHECK_EQ(g_resp.pd[0], PM_BOOT_CATCH_STATE_CAUGHT);
    CHECK_EQ(g_resp.message_count, 1);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 9);
    CHECK_EQ(g_resp.pd[2], PM_RDM_STATUS_ADVISORY);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), PM_STATUS_BOOT_CATCH_CAUGHT);
    CHECK_EQ(pm_get_be16(g_resp.pd + 5), 3);
    pm_rdm_responder_boot_catch_event(R, 0, PM_BOOT_CATCH_STATE_TIMEOUT);
    CHECK_EQ(pm_rdm_responder_message_count(R), 2);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_WARNING));
    CHECK_EQ(g_resp.pdl, 9);
    CHECK_EQ(g_resp.pd[2], PM_RDM_STATUS_WARNING);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), PM_STATUS_BOOT_CATCH_TIMEOUT);
    CHECK_EQ(pm_get_be16(g_resp.pd + 5), 1);
    pm_rdm_responder_boot_catch_event(R, 0, PM_BOOT_CATCH_STATE_ABORTED);
    CHECK_EQ(pm_rdm_responder_message_count(R), 1); /* BOOT_CATCH_STATUS only */
}

static void test_local_rdm_set(void)
{
    g_context = "local rdm set";
    setup();
    CHECK_ACK(get0(PM_PID_LOCAL_RDM_SET));
    CHECK_EQ(g_resp.pdl, PM_PDL_LOCAL_RDM_SET_GET_RESPONSE);
    CHECK_EQ(pm_get_be16(g_resp.pd), 0);
    pm_uid_t dev = pm_uid_make(PM_ESTA_MANUFACTURER, 0x00000042u);
    pm_rdm_responder_local_set(R, 1, &dev, 0, PM_RDM_PID_DMX_START_ADDRESS);
    CHECK_EQ(pm_rdm_responder_message_count(R), 2);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_PID_LOCAL_RDM_SET);
    CHECK_EQ(pm_get_be16(g_resp.pd), 1);
    CHECK_EQ(g_resp.pd[2], 2);
    CHECK(memcmp(g_resp.pd + 3, dev.b, 6) == 0);
    CHECK_EQ(pm_get_be16(g_resp.pd + 9), 0);
    CHECK_EQ(pm_get_be16(g_resp.pd + 11), PM_RDM_PID_DMX_START_ADDRESS);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 9);
    CHECK_EQ(pm_get_be16(g_resp.pd), 0);
    CHECK_EQ(g_resp.pd[2], PM_RDM_STATUS_ADVISORY);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), PM_STATUS_LOCAL_RDM_SET);
    CHECK_EQ(pm_get_be16(g_resp.pd + 5), 2);
    CHECK_EQ(pm_get_be16(g_resp.pd + 7), PM_RDM_PID_DMX_START_ADDRESS);
    pm_rdm_responder_local_set(R, 3, &dev, 0x0005, 0x8001);
    CHECK_ACK(get0(PM_PID_LOCAL_RDM_SET));
    CHECK_EQ(pm_get_be16(g_resp.pd), 2);
    CHECK_EQ(g_resp.pd[2], 4);
    CHECK_EQ(pm_get_be16(g_resp.pd + 9), 5);
    CHECK_EQ(pm_get_be16(g_resp.pd + 11), 0x8001);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(pm_get_be16(g_resp.pd + 7), 0x8001); /* PID as int16 */
}

/* ------------------------------------------------------------------------------------------------ queued/status */

static void test_queued_messages(void)
{
    g_context = "queued messages";
    setup();
    CHECK_EQ(pm_rdm_responder_message_count(R), 0);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 0);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_GET_LAST_MESSAGE));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 0);
    CHECK_NACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_NONE), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, 5), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY_CLEARED), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get0(PM_RDM_PID_QUEUED_MESSAGE), PM_NR_FORMAT_ERROR);
    CHECK_NACK(get_u16arg(PM_RDM_PID_QUEUED_MESSAGE, 0x0202), PM_NR_FORMAT_ERROR);

    /* dedupe + last writer wins: content = value at fetch time */
    CHECK_ACK(set_u8(PM_PID_PORT_DMX_RATE(1), 10));
    CHECK_EQ(g_resp.message_count, 1);
    CHECK_ACK(set_u8(PM_PID_PORT_DMX_RATE(1), 20));
    CHECK_EQ(g_resp.message_count, 1);
    CHECK_ACK(set_u16(PM_PID_PORT_DMX_BREAK(2), 200));
    CHECK_EQ(g_resp.message_count, 2);
    CHECK_ACK(set_u8(PM_PID_PORT_DMX_RATE(1), 30)); /* moves to the end */
    CHECK_EQ(g_resp.message_count, 2);
    CHECK_ACK(set_u8(PM_PID_PORT_DMX_RATE(1), 30)); /* unchanged value: nothing new */
    CHECK_EQ(g_resp.message_count, 2);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ERROR)); /* any type fetches PIDs */
    CHECK_EQ(g_resp.pid, PM_PID_PORT_DMX_BREAK(2));
    CHECK_EQ(g_resp.cc, PM_RDM_CC_GET_RESPONSE);
    CHECK_EQ(g_resp.port_or_resp, PM_RDM_RESP_ACK);
    CHECK_EQ(resp_value(), 200);
    CHECK_EQ(g_resp.message_count, 1);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_GET_LAST_MESSAGE));
    CHECK_EQ(g_resp.pid, PM_PID_PORT_DMX_BREAK(2));
    CHECK_EQ(resp_value(), 200);
    CHECK_EQ(g_resp.message_count, 1);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_PID_PORT_DMX_RATE(1));
    CHECK_EQ(resp_value(), 30);
    CHECK_EQ(g_resp.message_count, 0);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 0);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_GET_LAST_MESSAGE));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 0);

    /* changes visible in ArtPollReply are not queued, whatever the source */
    pm_settings_t *s = &F.s;
    strcpy(s->long_name, "x");
    s->port[0].direction = PM_PORT_DIR_INPUT;
    s->port[1].address = 77;
    s->port[2].merge = PM_MERGE_LTP;
    s->port[3].rdm_enabled = false;
    s->port[0].failsafe = PM_FAILSAFE_ZERO;
    s->port[1].output_style = PM_OUTPUT_DELTA;
    s->ip.static_ip = 0x02000001u;
    pm_rdm_responder_settings_changed(R, PM_CHG_NAMES | PM_CHG_IP, PM_SRC_ARTNET);
    CHECK_EQ(pm_rdm_responder_message_count(R), 0);

    /* RDM-only values are queued for every source */
    s->port[0].enabled = false;
    s->port[1].dmx_mab_us = 20;
    s->port[2].rdm_bg_discovery = false;
    s->rdm_bg_interval_s = 9;
    s->display_level = 7;
    s->display_dim_timeout_s = 0;
    s->address_format = PM_ADDRESS_FORMAT_PORT_ADDRESS;
    s->hw_revision_override = 1;
    pm_rdm_responder_settings_changed(R, 0, PM_SRC_LOCAL_UI);
    const uint16_t expect[] = {PM_PID_PORT_ENABLED(1), PM_PID_PORT_DMX_MAB(2), PM_PID_PORT_RDM_BG_DISCOVERY(3),
                               PM_PID_RDM_BG_DISCOVERY_INTERVAL, PM_RDM_PID_DISPLAY_LEVEL,
                               PM_PID_DISPLAY_DIM_TIMEOUT, PM_PID_DISPLAY_ADDRESS_FORMAT, PM_PID_NODE_HW_REVISION};
    const size_t ne = sizeof(expect) / sizeof(expect[0]);
    CHECK_EQ(pm_rdm_responder_message_count(R), ne);
    for (size_t i = 0; i < ne; i++) {
        CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
        CHECK_EQ(g_resp.pid, expect[i]);
        CHECK_EQ(g_resp.message_count, ne - i - 1);
    }
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_GET_LAST_MESSAGE));
    CHECK_EQ(g_resp.pid, PM_PID_NODE_HW_REVISION);
    CHECK_EQ(resp_value(), 1);

    /* overflow: the oldest entry is dropped */
    setup();
    for (unsigned p = 1; p <= 4; p++) {
        F.s.port[p - 1].dmx_rate_hz = 1;
        pm_rdm_responder_settings_changed(R, 0, PM_SRC_ARTNET);
        F.s.port[p - 1].dmx_mab_us = 80;
        pm_rdm_responder_settings_changed(R, 0, PM_SRC_ARTNET);
        F.s.port[p - 1].dmx_break_us = 300;
        pm_rdm_responder_settings_changed(R, 0, PM_SRC_ARTNET);
        F.s.port[p - 1].enabled = false;
        pm_rdm_responder_settings_changed(R, 0, PM_SRC_ARTNET);
    }
    CHECK_EQ(pm_rdm_responder_message_count(R), PM_RDM_RESP_QUEUE_LEN);
    F.s.panel_lock = true;
    pm_rdm_responder_settings_changed(R, 0, PM_SRC_ARTNET);
    CHECK_EQ(pm_rdm_responder_message_count(R), PM_RDM_RESP_QUEUE_LEN);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_PID_PORT_DMX_MAB(1)); /* PORT1_DMX_RATE was dropped */

    /* explicit queue_pid */
    setup();
    pm_rdm_responder_queue_pid(R, PM_RDM_PID_DEVICE_INFO);
    pm_rdm_responder_queue_pid(R, PM_RDM_PID_DEVICE_INFO);
    CHECK_EQ(pm_rdm_responder_message_count(R), 1);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_DEVICE_INFO);
    CHECK_EQ(g_resp.pdl, 19);
    /* a queued PID without GET is answered with a NACK under that PID */
    pm_rdm_responder_queue_pid(R, PM_RDM_PID_RESET_DEVICE);
    CHECK_NACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY), PM_NR_UNSUPPORTED_COMMAND_CLASS);
    CHECK_EQ(g_resp.pid, PM_RDM_PID_RESET_DEVICE);

    /* panel unlock at the device */
    setup();
    F.s.panel_lock = true;
    pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, PM_SRC_RDM);
    F.s.panel_lock = false;
    pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, PM_SRC_RDM);
    CHECK_EQ(pm_rdm_responder_message_count(R), 1); /* no status: not local */
    F.s.panel_lock = true;
    pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, PM_SRC_LOCAL_UI); /* locking locally: no status */
    CHECK_EQ(pm_rdm_responder_message_count(R), 1);
    F.s.panel_lock = false;
    pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, PM_SRC_LOCAL_UI);
    CHECK_EQ(pm_rdm_responder_message_count(R), 2);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_PID_PANEL_LOCK);
    CHECK_EQ(resp_value(), 0);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 9);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), PM_STATUS_PANEL_UNLOCKED_LOCAL);
    CHECK_EQ(g_resp.message_count, 0);
}

/* Phase 2: identify set from outside (ArtAddress Locate, front panel) + panel unlock status per source */
static void test_set_identify(void)
{
    g_context = "set_identify / panel unlock";
    setup();
    CHECK(!pm_rdm_responder_identify(R));
    pm_rdm_responder_set_identify(R, true);
    CHECK(pm_rdm_responder_identify(R));
    CHECK_EQ(F.identify_on + F.identify_off, 0); /* no ops->identify: no loop */
    CHECK_EQ(pm_rdm_responder_message_count(R), 1);
    CHECK_ACK(get0(PM_RDM_PID_IDENTIFY_DEVICE));
    CHECK_EQ(g_resp.pdl, 1);
    CHECK_EQ(resp_value(), 1);
    pm_rdm_responder_set_identify(R, true); /* no change: nothing queued */
    CHECK_EQ(pm_rdm_responder_message_count(R), 1);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_IDENTIFY_DEVICE);
    CHECK_EQ(g_resp.cc, PM_RDM_CC_GET_RESPONSE);
    CHECK_EQ(resp_value(), 1);
    CHECK_EQ(g_resp.message_count, 0);
    pm_rdm_responder_set_identify(R, false);
    CHECK(!pm_rdm_responder_identify(R));
    CHECK_EQ(pm_rdm_responder_message_count(R), 1);
    CHECK_ACK(get0(PM_RDM_PID_IDENTIFY_DEVICE));
    CHECK_EQ(resp_value(), 0);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_IDENTIFY_DEVICE);
    CHECK_EQ(resp_value(), 0);
    CHECK_EQ(F.identify_on + F.identify_off, 0);
    /* RDM SET still calls ops->identify, and the state is shared */
    CHECK_ACK(set_u8(PM_RDM_PID_IDENTIFY_DEVICE, 1));
    CHECK_EQ(F.identify_on, 1);
    CHECK(pm_rdm_responder_identify(R));
    pm_rdm_responder_set_identify(R, false);
    CHECK_EQ(F.identify_off, 0);
    CHECK_ACK(get0(PM_RDM_PID_IDENTIFY_DEVICE));
    CHECK_EQ(resp_value(), 0);
    pm_rdm_responder_set_identify(NULL, true); /* NULL-safe */
    CHECK(!pm_rdm_responder_identify(NULL));

    /* PANEL_UNLOCKED_LOCAL only for 1 -> 0 from the local panel */
    setup();
    CHECK_ACK(set_u8(PM_PID_PANEL_LOCK, 1)); /* RDM lock + unlock: queued PANEL_LOCK only */
    CHECK_ACK(set_u8(PM_PID_PANEL_LOCK, 0));
    CHECK_EQ(pm_rdm_responder_message_count(R), 1);
    const pm_change_source_t srcs[] = {PM_SRC_ARTNET, PM_SRC_IPPROG, PM_SRC_SYSTEM};
    for (size_t i = 0; i < sizeof(srcs) / sizeof(srcs[0]); i++) {
        F.s.panel_lock = true;
        pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, srcs[i]);
        F.s.panel_lock = false;
        pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, srcs[i]);
    }
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pdl, 0); /* no status from any non-local source */
    pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, PM_SRC_LOCAL_UI); /* no change: nothing */
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pdl, 0);
    F.s.panel_lock = true;
    pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, PM_SRC_RDM);
    F.s.panel_lock = false;
    pm_rdm_responder_settings_changed(R, PM_CHG_PANEL_LOCK, PM_SRC_LOCAL_UI);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pdl, 9);
    CHECK_EQ(pm_get_be16(g_resp.pd + 0), 0); /* sub-device */
    CHECK_EQ(g_resp.pd[2], PM_RDM_STATUS_ADVISORY);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), PM_STATUS_PANEL_UNLOCKED_LOCAL);
    g_context = NULL;
}

static void test_status_messages(void)
{
    g_context = "status messages";
    setup();
    pm_rdm_responder_status(R, 0x8001, PM_RDM_STATUS_ADVISORY, 1, 0);
    pm_rdm_responder_status(R, 0x8002, PM_RDM_STATUS_WARNING, 2, 0);
    pm_rdm_responder_status(R, 0x0021, PM_RDM_STATUS_ERROR, -5, 7);
    pm_rdm_responder_status(R, 0x0021, PM_RDM_STATUS_ADVISORY_CLEARED, 3, 0);
    pm_rdm_responder_status(R, 0x0001, PM_RDM_STATUS_GET_LAST_MESSAGE, 0, 0); /* invalid types ignored */
    pm_rdm_responder_status(R, 0x0001, PM_RDM_STATUS_NONE, 0, 0);
    pm_rdm_responder_status(R, 0x0001, 0x05, 0, 0);
    pm_rdm_responder_status(R, 0x0001, 0x22, 0, 0);
    CHECK_EQ(pm_rdm_responder_message_count(R), 4);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_NONE));
    CHECK_EQ(g_resp.pdl, 0);
    CHECK_EQ(g_resp.message_count, 4);
    CHECK_NACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, 5), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get0(PM_RDM_PID_STATUS_MESSAGES), PM_NR_FORMAT_ERROR);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_WARNING));
    CHECK_EQ(g_resp.pdl, 18);
    CHECK_EQ(g_resp.message_count, 2);
    CHECK_EQ(g_resp.pd[2], PM_RDM_STATUS_WARNING);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), 0x8002);
    CHECK_EQ(g_resp.pd[9 + 2], PM_RDM_STATUS_ERROR);
    CHECK_EQ((int16_t)pm_get_be16(g_resp.pd + 9 + 5), -5);
    CHECK_EQ(pm_get_be16(g_resp.pd + 9 + 7), 7);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_GET_LAST_MESSAGE));
    CHECK_EQ(g_resp.pdl, 18);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), 0x8002);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_ERROR));
    CHECK_EQ(g_resp.pdl, 0);
    /* the rest through QUEUED_MESSAGE */
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 18);
    CHECK_EQ(pm_get_be16(g_resp.pd + 3), 0x8001);
    CHECK_EQ(g_resp.pd[9 + 2], PM_RDM_STATUS_ADVISORY_CLEARED);
    CHECK_EQ(g_resp.message_count, 0);
    CHECK_ACK(get_u8arg(PM_RDM_PID_QUEUED_MESSAGE, PM_RDM_STATUS_GET_LAST_MESSAGE));
    CHECK_EQ(g_resp.pid, PM_RDM_PID_STATUS_MESSAGES);
    CHECK_EQ(g_resp.pdl, 18);

    /* full queue drops the oldest */
    for (int i = 0; i <= (int)PM_RDM_STATUS_QUEUE_LEN; i++) {
        pm_rdm_responder_status(R, 0x8001, PM_RDM_STATUS_ADVISORY, (int16_t)i, 0);
    }
    CHECK_EQ(pm_rdm_responder_message_count(R), PM_RDM_STATUS_QUEUE_LEN);
    CHECK_ACK(get_u8arg(PM_RDM_PID_STATUS_MESSAGES, PM_RDM_STATUS_ADVISORY));
    CHECK_EQ(g_resp.pdl, 9 * PM_RDM_STATUS_QUEUE_LEN);
    CHECK_EQ(pm_get_be16(g_resp.pd + 5), 1);

    /* CLEAR_STATUS_ID */
    pm_rdm_responder_status(R, 0x8001, PM_RDM_STATUS_ADVISORY, 1, 0);
    pm_rdm_responder_status(R, 0x8002, PM_RDM_STATUS_WARNING, 1, 0);
    CHECK_ACK(set(PM_RDM_PID_CLEAR_STATUS_ID, NULL, 0));
    CHECK_EQ(g_resp.message_count, 0);
    pm_rdm_responder_status(R, 0x8002, PM_RDM_STATUS_WARNING, 1, 0);
    pm_uid_t vcast = pm_uid_make(PM_ESTA_MANUFACTURER, 0xFFFFFFFFu);
    CHECK_EQ(xfer(&vcast, PM_RDM_CC_SET, 0xFFFF, PM_RDM_PID_CLEAR_STATUS_ID, NULL, 0), NO_RESPONSE);
    CHECK_EQ(pm_rdm_responder_message_count(R), 0);

    /* STATUS_ID_DESCRIPTION */
#define CHECK_STATUS_DESC(id_, text_)                         \
    CHECK_ACK(get_u16arg(PM_RDM_PID_STATUS_ID_DESCRIPTION, id_)); \
    CHECK(resp_is_text(text_));
    PM_STATUS_ID_DESC_LIST(CHECK_STATUS_DESC)
#undef CHECK_STATUS_DESC
    CHECK_NACK(get_u16arg(PM_RDM_PID_STATUS_ID_DESCRIPTION, 0x0001), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(get_u16arg(PM_RDM_PID_STATUS_ID_DESCRIPTION, 0x8005), PM_NR_DATA_OUT_OF_RANGE);
}

/* ------------------------------------------------------------------------------------------------ FW_* */

static int fw_write_req(uint32_t off, const uint8_t *data, size_t len)
{
    uint8_t b[PM_RDM_MAX_PDL];
    pm_put_be32(b, off);
    memcpy(b + 4, data, len);
    return set(PM_PID_FW_WRITE, b, (uint8_t)(4u + len));
}

static int fw_begin_req(uint32_t image_len)
{
    pm_image_desc_t d;
    memset(&d, 0, sizeof(d));
    d.magic = PM_NODE_IMAGE_MAGIC;
    d.format = PM_NODE_IMAGE_FORMAT;
    d.size = PM_IMAGE_DESC_SIZE;
    d.esta_manufacturer = PM_ESTA_MANUFACTURER;
    d.device_model_id = PM_MODEL_NODE;
    d.image_length = image_len;
    uint8_t b[PM_IMAGE_DESC_SIZE];
    pm_image_desc_encode(&d, true, b);
    return set(PM_PID_FW_BEGIN, b, PM_IMAGE_DESC_SIZE);
}

static void test_fw(void)
{
    g_context = "fw";
    setup();
    uint8_t img[1000];
    for (size_t i = 0; i < sizeof(img); i++) img[i] = (uint8_t)(i * 7u + 3u);

    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(g_resp.pdl, PM_PDL_FW_INFO_GET_RESPONSE);
    const uint8_t *p = g_resp.pd;
    CHECK_EQ(pm_get_be16(p), PM_ESTA_MANUFACTURER);
    CHECK_EQ(pm_get_be16(p + 2), PM_MODEL_NODE);
    CHECK_EQ(p[4], 0);
    CHECK_EQ(pm_get_be32(p + 5), IDENT.sw_version_id);
    CHECK_EQ(p[9], PM_FW_STATE_IDLE);
    CHECK_EQ(p[10], PM_FW_ERROR_NONE);
    CHECK_EQ(p[11], PM_NODE_FW_MAX_BLOCK);
    CHECK_EQ(pm_get_be32(p + 12), 0);
    CHECK_EQ(pm_get_be32(p + 16), 0);

    /* no session */
    CHECK_NACK(fw_write_req(0, img, 10), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(set(PM_PID_FW_FINISH, NULL, 0), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_EQ(F.fw_writes + F.fw_finish, 0);

    /* FW_BEGIN */
    uint8_t d65[65] = {0};
    CHECK_NACK(set(PM_PID_FW_BEGIN, d65, 63), PM_NR_FORMAT_ERROR);
    CHECK_NACK(set(PM_PID_FW_BEGIN, d65, 65), PM_NR_FORMAT_ERROR);
    CHECK_EQ(F.fw_begin, 0);
    F.fw_begin_result = PM_FW_ERROR_DESC_MODEL;
    CHECK_NACK(fw_begin_req(sizeof(img)), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(g_resp.pd[9], PM_FW_STATE_FAILED);
    CHECK_EQ(g_resp.pd[10], PM_FW_ERROR_DESC_MODEL);
    CHECK_NACK(fw_write_req(0, img, 10), PM_NR_DATA_OUT_OF_RANGE);
    F.fw_begin_result = PM_FW_ERROR_FLASH;
    CHECK_NACK(fw_begin_req(sizeof(img)), PM_NR_HARDWARE_FAULT);
    F.fw_begin_result = PM_FW_ERROR_NONE;
    CHECK_ACK(fw_begin_req(sizeof(img)));
    CHECK_EQ(F.fw_desc.image_length, sizeof(img));
    CHECK_EQ(F.fw_desc.magic, PM_NODE_IMAGE_MAGIC);
    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(g_resp.pd[9], PM_FW_STATE_RECEIVING);
    CHECK_EQ(pm_get_be32(g_resp.pd + 16), sizeof(img));

    /* FW_WRITE */
    CHECK_ACK(fw_write_req(0, img, PM_NODE_FW_MAX_BLOCK));
    CHECK_ACK(fw_write_req(224, img + 224, 224));
    CHECK_EQ(F.fw_writes, 2);
    CHECK_ACK(fw_write_req(0, img, 224)); /* retransmission: not written again */
    CHECK_ACK(fw_write_req(224, img + 224, 224));
    CHECK_EQ(F.fw_writes, 2);
    CHECK_NACK(fw_write_req(449, img + 449, 10), PM_NR_DATA_OUT_OF_RANGE); /* gap */
    CHECK_NACK(fw_write_req(0xFFFFFFF0u, img, 10), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_EQ(F.fw_writes, 2);
    CHECK_ACK(fw_write_req(400, img + 400, 100)); /* overlap: passed on, sink accepts */
    CHECK_EQ(F.fw_writes, 3);
    CHECK_ACK(fw_write_req(500, img, 0)); /* empty write at the end */
    CHECK_EQ(F.fw_writes, 3);
    uint8_t short3[3] = {0};
    CHECK_NACK(set(PM_PID_FW_WRITE, short3, 3), PM_NR_FORMAT_ERROR);
    CHECK_NACK(set(PM_PID_FW_WRITE, NULL, 0), PM_NR_FORMAT_ERROR);
    uint8_t big[PM_RDM_MAX_PDL] = {0};
    CHECK_NACK(set(PM_PID_FW_WRITE, big, PM_PDL_FW_WRITE_SET_REQUEST + 1), PM_NR_FORMAT_ERROR);
    for (uint32_t off = 500; off < sizeof(img); off += PM_NODE_FW_MAX_BLOCK) {
        size_t n = sizeof(img) - off < PM_NODE_FW_MAX_BLOCK ? sizeof(img) - off : PM_NODE_FW_MAX_BLOCK;
        CHECK_ACK(fw_write_req(off, img + off, n));
    }
    CHECK(memcmp(F.img, img, sizeof(img)) == 0);
    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(pm_get_be32(g_resp.pd + 12), sizeof(img));

    /* FW_FINISH -> reboot ~500 ms later */
    CHECK_ACK(set(PM_PID_FW_FINISH, NULL, 0));
    CHECK_EQ(F.fw_finish, 1);
    CHECK_EQ(F.fw_reboot, 0);
    F.now += PM_RDM_RESP_REBOOT_DELAY_MS - 1;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_reboot, 0);
    F.now += 1;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_reboot, 1);
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_reboot, 1);
    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(g_resp.pd[9], PM_FW_STATE_READY);
    CHECK_NACK(fw_write_req(0, img, 10), PM_NR_DATA_OUT_OF_RANGE); /* session over */

    /* FW_FINISH failures */
    CHECK_ACK(fw_begin_req(sizeof(img)));
    F.fw_finish_result = PM_FW_ERROR_CRC;
    CHECK_NACK(set(PM_PID_FW_FINISH, NULL, 0), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_NACK(set(PM_PID_FW_FINISH, NULL, 0), PM_NR_DATA_OUT_OF_RANGE); /* no session any more */
    CHECK_EQ(F.fw_finish, 2);
    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(g_resp.pd[10], PM_FW_ERROR_CRC);
    CHECK_ACK(fw_begin_req(sizeof(img)));
    F.fw_finish_result = PM_FW_ERROR_FLASH;
    CHECK_NACK(set(PM_PID_FW_FINISH, NULL, 0), PM_NR_HARDWARE_FAULT);
    F.now += 1000;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_reboot, 1);
    F.fw_finish_result = PM_FW_ERROR_NONE;

    /* session timeout 30 s without FW_WRITE */
    CHECK_ACK(fw_begin_req(sizeof(img)));
    CHECK_ACK(fw_begin_req(sizeof(img))); /* restart is allowed */
    F.now += PM_RDM_RESP_FW_TIMEOUT_MS - 1;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_abort, 0);
    CHECK_ACK(fw_write_req(0, img, 100)); /* restarts the timer */
    F.now += PM_RDM_RESP_FW_TIMEOUT_MS - 1;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_abort, 0);
    CHECK_ACK(fw_write_req(0, img, 50)); /* a retransmission also counts as activity */
    F.now += PM_RDM_RESP_FW_TIMEOUT_MS;
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_abort, 1);
    CHECK_EQ(F.fw_abort_reason, PM_FW_ERROR_TIMEOUT);
    pm_rdm_responder_poll(R);
    CHECK_EQ(F.fw_abort, 1);
    CHECK_NACK(fw_write_req(100, img, 10), PM_NR_DATA_OUT_OF_RANGE);
    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(g_resp.pd[10], PM_FW_ERROR_TIMEOUT);

    /* own vendorcast executes FW_BEGIN (no answer), broadcast ignores it */
    pm_uid_t vcast = pm_uid_make(PM_ESTA_MANUFACTURER, 0xFFFFFFFFu), bcast = pm_uid_make(0xFFFFu, 0xFFFFFFFFu);
    uint8_t desc[PM_IMAGE_DESC_SIZE] = {0};
    int before = F.fw_begin;
    CHECK_EQ(xfer(&bcast, PM_RDM_CC_SET, 0, PM_PID_FW_BEGIN, desc, sizeof(desc)), NO_RESPONSE);
    CHECK_EQ(F.fw_begin, before);
    CHECK_EQ(xfer(&vcast, PM_RDM_CC_SET, 0, PM_PID_FW_BEGIN, desc, sizeof(desc)), NO_RESPONSE);
    CHECK_EQ(F.fw_begin, before + 1);

    /* no sink */
    setup_with_sink(NULL);
    CHECK_NACK(fw_begin_req(10), PM_NR_HARDWARE_FAULT);
    CHECK_NACK(fw_write_req(0, img, 1), PM_NR_HARDWARE_FAULT);
    CHECK_NACK(set(PM_PID_FW_FINISH, NULL, 0), PM_NR_HARDWARE_FAULT);
    CHECK_ACK(get0(PM_PID_FW_INFO));
    CHECK_EQ(g_resp.pd[9], PM_FW_STATE_IDLE);
}

void test_rdm_responder(void)
{
    test_framing();
    test_addressing();
    test_nack_order();
    test_all_pid_paths();
    test_info_pids();
    test_label_identify_reset();
    test_e137_2();
    test_numeric_pids();
    test_boot_catch();
    test_local_rdm_set();
    test_queued_messages();
    test_set_identify();
    test_status_messages();
    test_fw();
    g_context = NULL;
}
