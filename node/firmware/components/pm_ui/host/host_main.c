/*
 * host_main.c - host renderer of the front panel: runs ui_core against a fake model with virtual time, plays the
 * scenes from scenes.c and writes every snapshot as PNG (PPM -> `sips`). No dependencies besides libc.
 *
 *   make -C node/firmware/components/pm_ui/host run          (all scenes -> out/)
 *   build/ui_host out <scene-name>                           (one scene)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host.h"
#include "pm_rdm.h"
#include "ui_core.h"
#include "ui_gfx.h"

#define SCALE 4

fake_t F;
int g_host_fail;

static uint32_t s_now;
static uint16_t s_fb[UI_H][UI_W];
static uint16_t s_strip[UI_W * UI_STRIP_H];
static const char *s_outdir = "out";
static FILE *s_index;
static uint32_t s_sent_since_snap;

/* contact sheet per scene (build/sheets/, for quick review; not committed) */
#define SHEET_MAX 12
static uint16_t s_sheet[SHEET_MAX][UI_H][UI_W];
static int s_sheet_n;

static void write_sheet(const char *scene)
{
    if (!s_sheet_n) return;
    const int sc = 2, gap = 6, cols = 3;
    int rows = (s_sheet_n + cols - 1) / cols;
    int W = cols * (UI_W * sc + gap) + gap, H = rows * (UI_H * sc + gap) + gap;
    char ppm[256], cmd[700];
    snprintf(ppm, sizeof(ppm), "build/sheets/%s.ppm", scene);
    if (system("mkdir -p build/sheets") != 0) return;
    FILE *f = fopen(ppm, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint8_t rgb[3] = {60, 60, 70};
            int cx = (x - gap) / (UI_W * sc + gap), cy = (y - gap) / (UI_H * sc + gap);
            int ox = (x - gap) % (UI_W * sc + gap), oy = (y - gap) % (UI_H * sc + gap);
            int i = cy * cols + cx;
            if (x >= gap && y >= gap && ox < UI_W * sc && oy < UI_H * sc && i < s_sheet_n) {
                uint16_t c = s_sheet[i][oy / sc][ox / sc];
                rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
                rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
                rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31);
            }
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    snprintf(cmd, sizeof(cmd), "sips -s format png '%s' --out 'build/sheets/%s.png' >/dev/null 2>&1 && rm '%s'", ppm,
             scene, ppm);
    if (system(cmd) != 0) fprintf(stderr, "sips failed\n");
    s_sheet_n = 0;
}

/* input levels */
static int s_pending_det;
static bool s_push, s_back;

/* event queue (like the FreeRTOS queue on the target) */
#define EVQ 64
static struct {
    pm_ui_event_t ev;
    uint8_t data[PM_RDM_MAX_MSG_LEN];
    uint32_t due;
} s_evq[EVQ];
static int s_evn;

/* ------------------------------------------------------------------------------------------------ platform */

static uint32_t p_now(void *ctx)
{
    (void)ctx;
    return s_now;
}

static void p_settings_get(void *ctx, pm_settings_t *out)
{
    (void)ctx;
    *out = F.settings;
}

static void queue_event(const pm_ui_event_t *ev, uint32_t delay)
{
    if (s_evn >= EVQ) return;
    s_evq[s_evn].ev = *ev;
    if (ev->data && ev->len) {
        memcpy(s_evq[s_evn].data, ev->data, ev->len);
        s_evq[s_evn].ev.data = s_evq[s_evn].data;
    }
    s_evq[s_evn].due = s_now + delay;
    s_evn++;
}

static void p_settings_set(void *ctx, const pm_settings_t *s)
{
    (void)ctx;
    F.settings = *s;
    pm_settings_sanitize(&F.settings);
    pm_ui_event_t ev = {.type = PM_UI_EV_SETTINGS_CHANGED, .value = PM_SRC_LOCAL_UI};
    queue_event(&ev, 0);
}

static void p_backlight(void *ctx, uint8_t level, bool on)
{
    (void)ctx;
    F.backlight = level;
    F.panel_on = on;
}

static void p_restart(void *ctx)
{
    (void)ctx;
    F.restarted = true;
}

static void p_factory(void *ctx, bool keep_network)
{
    (void)ctx;
    pm_settings_t s;
    pm_settings_defaults(&s);
    if (keep_network) s.ip = F.settings.ip;
    s.hw_revision_override = F.settings.hw_revision_override;
    p_settings_set(ctx, &s);
}

static void p_record_scene(void *ctx, pm_port_idx_t port)
{
    pm_settings_t s = F.settings;
    s.port[port].scene_len = 512;
    memset(s.port[port].scene, 0x80, 512);
    p_settings_set(ctx, &s);
}

static void p_tod_flush(void *ctx, pm_port_idx_t port)
{
    (void)ctx;
    pm_ui_event_t ev = {.type = PM_UI_EV_TOD_CHANGED, .port = port};
    queue_event(&ev, 500);
}

/* ------------------------------------------------------------------------------------------------ model */

static void m_identify(void *ctx, bool on)
{
    (void)ctx;
    F.identify = on;
    F.identify_calls++;
}

static void m_net(void *ctx, pm_ui_net_info_t *out)
{
    (void)ctx;
    out->ip = F.link ? F.ip : 0;
    out->mask = F.link ? F.mask : 0;
    out->gateway = F.link ? F.gateway : 0;
    out->mode = F.link ? F.net_mode : 0;
    out->link_up = F.link;
}

static void m_stats(void *ctx, pm_port_idx_t p, pm_port_stats_t *out)
{
    (void)ctx;
    *out = F.stats[p];
    out->tod_count = F.n_dev[p];
}

static void m_live(void *ctx, pm_port_idx_t p, pm_ui_port_live_t *out)
{
    (void)ctx;
    out->rx_active = F.rx_fps[p] > 0;
    out->rx_fps = F.rx_fps[p];
    out->merge_sources = (uint8_t)(F.merging[p] ? 2 : F.rx_fps[p] ? 1 : 0);
    out->data_loss = F.loss[p];
    out->merging = F.merging[p];
    out->synced = F.synced[p];
}

static uint16_t m_tod(void *ctx, pm_port_idx_t p, pm_uid_t *out, uint16_t cap)
{
    (void)ctx;
    for (uint16_t i = 0; i < F.n_dev[p] && i < cap; i++) out[i] = F.dev[p][i].uid;
    return F.n_dev[p];
}

/** Fake RDM responders: answer GET DEVICE_INFO / DEVICE_LABEL / PERSONALITY_DESCRIPTION and the four SETs. */
static esp_err_t m_rdm(void *ctx, pm_port_idx_t p, uint32_t token, const uint8_t *d, uint16_t len)
{
    (void)ctx;
    if (len < 26) return ESP_FAIL;
    fake_dev_t *dev = NULL;
    for (int i = 0; i < F.n_dev[p]; i++) {
        if (memcmp(F.dev[p][i].uid.b, &d[3], 6) == 0) dev = &F.dev[p][i];
    }
    if (!dev || dev->dead) return ESP_OK; /* no answer: the UI times out */
    uint8_t cc = d[20], pdl = d[23];
    uint16_t pid = (uint16_t)((d[21] << 8) | d[22]);
    const uint8_t *pd = &d[24];
    uint8_t r[PM_RDM_MAX_MSG_LEN];
    uint8_t rpd[64];
    uint8_t rpdl = 0;
    uint8_t resp = PM_RDM_RESP_ACK;
    if (cc == PM_RDM_CC_GET && pid == PM_RDM_PID_DEVICE_INFO) {
        memset(rpd, 0, 19);
        rpd[0] = 1; rpd[1] = 0;
        rpd[2] = 0; rpd[3] = 1; /* model 0x0001 tube */
        rpd[4] = 0x01; rpd[5] = 0x01;
        rpd[6] = 0; rpd[7] = 1; rpd[8] = 2; rpd[9] = 0;
        rpd[10] = (uint8_t)(dev->footprint >> 8); rpd[11] = (uint8_t)dev->footprint;
        rpd[12] = dev->pers; rpd[13] = dev->pers_count;
        rpd[14] = (uint8_t)(dev->start >> 8); rpd[15] = (uint8_t)dev->start;
        rpdl = 19;
    } else if (cc == PM_RDM_CC_GET && pid == PM_RDM_PID_DEVICE_LABEL) {
        rpdl = (uint8_t)strlen(dev->label);
        memcpy(rpd, dev->label, rpdl);
    } else if (cc == PM_RDM_CC_GET && pid == PM_RDM_PID_DMX_PERSONALITY_DESCRIPTION && pdl == 1) {
        static const char *const names[] = {"RGB", "RGBW 8bit", "RGB 16bit", "Pixel 12"};
        static const uint16_t fp[] = {3, 4, 6, 12};
        uint8_t n = pd[0];
        if (n < 1 || n > 4) {
            resp = PM_RDM_RESP_NACK_REASON;
        } else {
            rpd[0] = n;
            rpd[1] = 0;
            rpd[2] = (uint8_t)fp[n - 1];
            rpdl = (uint8_t)(3 + strlen(names[n - 1]));
            memcpy(&rpd[3], names[n - 1], strlen(names[n - 1]));
        }
    } else if (cc == PM_RDM_CC_SET && pid == PM_RDM_PID_IDENTIFY_DEVICE) {
        dev->identify = pd[0];
    } else if (cc == PM_RDM_CC_SET && pid == PM_RDM_PID_DMX_START_ADDRESS) {
        dev->start = (uint16_t)((pd[0] << 8) | pd[1]);
    } else if (cc == PM_RDM_CC_SET && pid == PM_RDM_PID_DMX_PERSONALITY) {
        static const uint16_t fp[] = {3, 4, 6, 12};
        dev->pers = pd[0];
        if (pd[0] >= 1 && pd[0] <= 4) dev->footprint = fp[pd[0] - 1];
    } else if (cc == PM_RDM_CC_SET && pid == PM_RDM_PID_DEVICE_LABEL) {
        memset(dev->label, 0, sizeof(dev->label));
        memcpy(dev->label, pd, pdl > 32 ? 32 : pdl);
    } else {
        resp = PM_RDM_RESP_NACK_REASON;
        rpd[0] = 0;
        rpd[1] = 0;
        rpdl = 2;
    }
    uint8_t mlen = (uint8_t)(24 + rpdl);
    r[0] = PM_RDM_START_CODE;
    r[1] = PM_RDM_SUB_START_CODE;
    r[2] = mlen;
    memcpy(&r[3], &d[9], 6);
    memcpy(&r[9], &d[3], 6);
    r[15] = d[15];
    r[16] = resp;
    r[17] = 0;
    r[18] = 0;
    r[19] = 0;
    r[20] = (uint8_t)(cc + 1);
    r[21] = d[21];
    r[22] = d[22];
    r[23] = rpdl;
    memcpy(&r[24], rpd, rpdl);
    uint16_t sum = 0;
    for (int i = 0; i < mlen; i++) sum = (uint16_t)(sum + r[i]);
    r[mlen] = (uint8_t)(sum >> 8);
    r[mlen + 1] = (uint8_t)sum;
    pm_ui_event_t ev = {.type = PM_UI_EV_RDM_RESPONSE, .port = p, .token = token, .len = (uint16_t)(mlen + 2), .data = r};
    queue_event(&ev, 30);
    F.stats[p].rdm_requests++;
    return ESP_OK;
}

static const pm_ui_model_t s_model = {
    .port_stats = m_stats, .tod_get = m_tod, .rdm_request = m_rdm, .net_info = m_net, .port_live = m_live,
    .identify = m_identify,
};

static const ui_platform_t s_plat = {
    .now_ms = p_now, .settings_get = p_settings_get, .settings_set = p_settings_set, .backlight = p_backlight,
    .restart = p_restart, .factory_reset = p_factory, .record_scene = p_record_scene, .tod_flush = p_tod_flush,
};

/* ------------------------------------------------------------------------------------------------ display */

static void blit(void *ctx, int x, int y, int w, int h, const uint16_t *px)
{
    (void)ctx;
    for (int r = 0; r < h; r++) memcpy(&s_fb[y + r][x], &px[r * w], sizeof(uint16_t) * (size_t)w);
    s_sent_since_snap += (uint32_t)(w * h);
}

void snap(const char *name)
{
    char ppm[512], png[512], cmd[1200];
    snprintf(ppm, sizeof(ppm), "%s/%s.ppm", s_outdir, name);
    snprintf(png, sizeof(png), "%s/%s.png", s_outdir, name);
    FILE *f = fopen(ppm, "wb");
    if (!f) {
        perror(ppm);
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", UI_W * SCALE, UI_H * SCALE);
    for (int y = 0; y < UI_H * SCALE; y++) {
        for (int x = 0; x < UI_W * SCALE; x++) {
            uint16_t c = s_fb[y / SCALE][x / SCALE];
            uint8_t rgb[3] = {(uint8_t)(((c >> 11) & 0x1F) * 255 / 31), (uint8_t)(((c >> 5) & 0x3F) * 255 / 63),
                              (uint8_t)((c & 0x1F) * 255 / 31)};
            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    snprintf(cmd, sizeof(cmd), "sips -s format png '%s' --out '%s' >/dev/null 2>&1 && rm '%s'", ppm, png, ppm);
    if (system(cmd) != 0) fprintf(stderr, "sips failed, kept %s\n", ppm);
    printf("  %-28s (%u px sent since last snapshot)\n", name, (unsigned)s_sent_since_snap);
    if (s_index) fprintf(s_index, "| `%s.png` | ![](%s.png) |\n", name, name);
    s_sent_since_snap = 0;
    if (s_sheet_n < SHEET_MAX) memcpy(s_sheet[s_sheet_n++], s_fb, sizeof(s_fb));
}

/* ------------------------------------------------------------------------------------------------ sim */

static void deliver_events(void)
{
    for (int i = 0; i < s_evn;) {
        if (s_evq[i].due <= s_now) {
            pm_ui_event_t ev = s_evq[i].ev;
            uint8_t data[PM_RDM_MAX_MSG_LEN];
            if (ev.data && ev.len) {
                memcpy(data, s_evq[i].data, ev.len);
                ev.data = data;
            }
            memmove(&s_evq[i], &s_evq[i + 1], sizeof(s_evq[0]) * (size_t)(s_evn - i - 1));
            s_evn--;
            ui_core_event(&ev);
        } else {
            i++;
        }
    }
}

void sim_run(uint32_t ms)
{
    for (uint32_t t = 0; t < ms; t += 5) {
        s_now += 5;
        ui_core_input(s_pending_det, s_push, s_back);
        s_pending_det = 0;
        if (s_now % 20 == 0) {
            deliver_events();
            ui_core_tick();
        }
    }
}

uint32_t sim_now(void) { return s_now; }

void sim_align(uint32_t period)
{
    while (s_now % period >= 20) sim_run(5);
}

void key_rot(int det, uint32_t gap)
{
    int dir = det > 0 ? 1 : -1;
    for (int i = 0; i < (det > 0 ? det : -det); i++) {
        s_pending_det = dir;
        sim_run(gap < 5 ? 5 : gap);
    }
    sim_run(100);
}

void key_push(void)
{
    s_push = true;
    sim_run(80);
    s_push = false;
    sim_run(120);
}

void key_back(void)
{
    s_back = true;
    sim_run(80);
    s_back = false;
    sim_run(120);
}

void key_back_long(void)
{
    s_back = true;
    sim_run(1200);
    s_back = false;
    sim_run(100);
}

void key_hold_push(uint32_t ms, bool keep)
{
    s_push = true;
    sim_run(ms);
    if (!keep) {
        s_push = false;
        sim_run(100);
    }
}

void key_combo(uint32_t ms, bool keep)
{
    s_back = true;
    sim_run(40);
    s_push = true;
    sim_run(ms);
    if (!keep) {
        s_push = s_back = false;
        sim_run(100);
    }
}

void key_release_all(void)
{
    s_push = s_back = false;
    sim_run(100);
}

void keys(const char *s)
{
    while (*s) {
        char c = *s++;
        char *end;
        long n = strtol(s, &end, 10);
        bool has_n = end != s;
        s = end;
        switch (c) {
        case 'R': key_rot(has_n ? (int)n : 1, 150); break;
        case 'F': key_rot(has_n ? (int)n : 1, 15); break;
        case 'P': key_push(); break;
        case 'B': key_back(); break;
        case 'L': key_back_long(); break;
        case 'W': sim_run(has_n ? (uint32_t)n : 100); break;
        default: break;
        }
    }
}

void remote_settings(const pm_settings_t *s, pm_change_source_t src)
{
    F.settings = *s;
    pm_ui_event_t ev = {.type = PM_UI_EV_SETTINGS_CHANGED, .value = src};
    queue_event(&ev, 0);
}

void post_event(pm_ui_event_t ev) { queue_event(&ev, 0); }

static void fake_defaults(void)
{
    memset(&F, 0, sizeof(F));
    pm_settings_defaults(&F.settings);
    snprintf(F.settings.short_name, sizeof(F.settings.short_name), "XBAR-NODE-01");
    F.settings.port[1].address = PM_PA_MAKE(0, 0, 1);
    F.settings.port[0].address = PM_PA_MAKE(0, 0, 0);
    F.settings.port[2].direction = PM_PORT_DIR_INPUT;
    F.settings.port[2].address = PM_PA_MAKE(0, 1, 0);
    F.settings.port[3].enabled = false;
    F.settings.port[3].address = PM_PA_MAKE(0, 0, 3);
    F.link = true;
    F.ip = 0xC0A864D9u; /* 192.168.100.217 */
    F.mask = 0xFFFFFF00u;
    F.gateway = 0xC0A86401u; /* 192.168.100.1 */
    F.net_mode = 1; /* DHCP */
    F.stats[0].fps = 40; /* the engine sends at the port rate; home shows the received Art-Net rate */
    F.stats[1].fps = 40;
    F.rx_fps[0] = 44;
    F.rx_fps[1] = 44;
    F.merging[1] = true;
    F.stats[2].fps = 30;
    F.stats[2].input_signal = true;
    static const char *const labels[] = {"T-01", "T-02", "T-03", "T-04", "T-05", "T-06", "T-07", "T-08",
                                         "Front left", "Front right", "Back wash", "Spare tube"};
    F.n_dev[0] = 12;
    for (int i = 0; i < 12; i++) {
        fake_dev_t *d = &F.dev[0][i];
        d->uid = pm_uid_make(0x7FF0, 0x00000101u + (uint32_t)i * 7u);
        d->footprint = 3;
        d->start = (uint16_t)(1 + i * 3);
        d->pers = 1;
        d->pers_count = 4;
        snprintf(d->label, sizeof(d->label), "%s", labels[i]);
    }
    F.dev[0][3].start = 9; /* overlaps T-03 (7..9) */
    F.n_dev[1] = 2;
    F.dev[1][0] = (fake_dev_t){.uid = pm_uid_make(0x7FF0, 0x0BADC0DE), .start = 1, .footprint = 3, .pers = 1,
                               .pers_count = 4, .label = "Bar A"};
    F.dev[1][1] = (fake_dev_t){.uid = pm_uid_make(0x0102, 0x0000002A), .start = 100, .footprint = 12, .pers = 4,
                               .pers_count = 4, .label = "", .dead = true};
}

static void reset_with(const pm_ui_model_t *model);

void sim_reset(void) { reset_with(&s_model); }

void sim_reset_model(const pm_ui_model_t *model) { reset_with(model); }

static void reset_with(const pm_ui_model_t *model)
{
    fake_defaults();
    s_now = 1000;
    s_evn = 0;
    s_push = s_back = false;
    s_pending_det = 0;
    memset(s_fb, 0, sizeof(s_fb));
    ui_gfx_init(s_strip, false, blit, NULL);
    ui_node_info_t info = {.hw_revision = 0};
    info.uid = pm_uid_make(0x7FF0, 0x8A1B2C3D);
    uint8_t mac[6] = {0x30, 0xED, 0xA0, 0xA1, 0xB2, 0xC3};
    memcpy(info.mac, mac, 6);
    snprintf(info.fw_version, sizeof(info.fw_version), "0.1.0");
    ui_core_init(&s_plat, model, &info);
    sim_run(200);
    s_sent_since_snap = 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) s_outdir = argv[1];
    const char *only = argc > 2 ? argv[2] : NULL;
    char idx[512];
    snprintf(idx, sizeof(idx), "%s/README.md", s_outdir);
    if (!only) {
        s_index = fopen(idx, "w");
        if (s_index) {
            fprintf(s_index, "# Front panel screens (host renderer)\n\nGenerated by `make -C "
                             "node/firmware/components/pm_ui/host run` from `host/scenes.c` - do not edit.\n"
                             "4x scaled, 160 x 128 px landscape.\n\n");
        }
    }
    for (int i = 0; i < g_num_scenes; i++) {
        if (only && strcmp(only, g_scenes[i].name) != 0) continue;
        printf("%s: %s\n", g_scenes[i].name, g_scenes[i].doc);
        if (s_index) fprintf(s_index, "\n## %s\n\n%s\n\n| file | screen |\n|---|---|\n", g_scenes[i].name, g_scenes[i].doc);
        sim_reset();
        g_scenes[i].run();
        write_sheet(g_scenes[i].name);
    }
    if (s_index) fclose(s_index);
    if (g_host_fail) printf("%d scene check(s) failed\n", g_host_fail);
    return g_host_fail ? 1 : 0;
}
