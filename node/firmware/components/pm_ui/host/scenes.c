/*
 * scenes.c - scenes of the host renderer: fake model + key script + snapshots. One scene per mockup of
 * docs/ui-konzept.md (section numbers in the names), plus states the concept only describes in prose.
 *
 * Key script (keys()): R<n> turn n detents (slow), F<n> turn fast (acceleration), P push, B BACK, L BACK long,
 * W<ms> wait. Home cursor: first detent shows it on port 1; targets name, IP line, port 1..4.
 */
#include <stdio.h>
#include <string.h>

#include "host.h"

static pm_settings_t S(void) { return F.settings; }

/* 6.1 */
static void sc_home(void)
{
    snap("6.1-home");
    F.settings.address_format = 1;
    sim_run(100);
    remote_settings(&F.settings, PM_SRC_LOCAL_UI);
    sim_run(100);
    snap("6.1-home-15bit");
}

static void sc_home_cursor(void)
{
    F.loss[1] = true;
    keys("R1 R1");
    sim_align(1000);
    snap("6.1-home-cursor-dataloss");
    sim_run(500);
    snap("6.1-home-cursor-dataloss-blink");
}

static void sc_home_net(void)
{
    F.link = false;
    F.loss[0] = F.loss[1] = true;
    sim_run(100);
    sim_align(1000);
    snap("home-no-link");
    F.link = true;
    F.net_mode = 2;
    F.ip = 0x02A1B2C3u;
    F.mask = 0xFF000000u;
    F.loss[0] = F.loss[1] = false;
    sim_run(100);
    snap("home-fallback-ip");
    F.net_mode = 0;
    sim_align(1000);
    snap("home-dhcp-pending");
}

static void sc_home_states(void)
{
    pm_ui_event_t e = {.type = PM_UI_EV_ERROR, .port = 0, .value = 263};
    post_event(e);
    F.stats[2].input_signal = false;
    F.stats[2].fps = 0;
    F.settings.port[3].enabled = true;
    F.stats[3].fps = 0;
    remote_settings(&F.settings, PM_SRC_LOCAL_UI);
    sim_run(3200);
    snap("home-error-idle-ports");
}

/* 6.3 */
static void sc_port(void)
{
    keys("R1 R1 P");
    snap("6.3-port-detail");
    keys("R6");
    snap("6.3-port-detail-scrolled");
    keys("P");
    snap("6.3-port-advanced");
    keys("L R1 P");
    F.loss[1] = true;
    F.stats[1].fps = 0;
    sim_run(200);
    snap("6.3-port-detail-dataloss");
}

static void sc_port_input(void)
{
    keys("R1 R2 P");
    snap("port-input-detail");
    keys("L R1 R3 P");
    snap("port-off-detail");
}

/* 6.4 */
static void sc_addr(void)
{
    F.stats[0].fps = 0; /* no data on port 1: no confirmation */
    keys("R1 P P");
    snap("6.4-address-quick-edit");
    keys("R3");
    snap("6.4-address-quick-edit-turned");
    keys("F20");
    snap("6.4-address-carry-accel");
    keys("B");
    snap("6.4-address-field-select");
    keys("R-1 P R2");
    snap("6.4-address-field-edit");
    keys("P R2");
    snap("6.4-address-ok-focus");
    keys("P");
    snap("6.4-address-applied");
}

static void sc_addr_confirm(void)
{
    keys("R1 R1 P P R2 P");
    snap("6.4-address-confirm-data");
    keys("R1 P");
    snap("6.4-address-confirm-done");
}

static void sc_addr_15(void)
{
    F.settings.address_format = 1;
    F.stats[0].fps = 0;
    remote_settings(&F.settings, PM_SRC_LOCAL_UI);
    keys("W100 R1 P P F12");
    snap("6.4-address-15bit");
}

/* 6.5 */
static void sc_network(void)
{
    keys("R1 R-1 P");
    snap("6.5-network-dhcp");
    keys("P R1");
    snap("6.5-network-mode-edit");
    keys("P");
    snap("6.5-network-static-staged");
    keys("R1 P");
    snap("6.5-ip-quick-edit");
    keys("R-5");
    snap("6.5-ip-quick-edit-turned");
    keys("P");
    snap("6.5-network-staged-ip");
    keys("R1 P R-14 P");
    snap("6.5-network-staged-mask");
    keys("R2 P");
    snap("6.8-confirm-network");
    keys("R1 P");
    snap("6.5-network-applied");
}

static void sc_ip_invalid(void)
{
    F.settings.ip.static_mask = 0xFFFFFF00u;
    F.settings.ip.dhcp = false;
    F.settings.ip.static_ip = 0x0A000032u;
    F.net_mode = 3;
    remote_settings(&F.settings, PM_SRC_LOCAL_UI);
    keys("W100 R1 R-1 P R1 P");
    keys("F40");
    keys("P");
    snap("6.5-ip-invalid");
    keys("B");
    snap("6.5-ip-field-select");
}

static void sc_net_leave(void)
{
    keys("R1 R-1 P P R1 P B");
    snap("6.5-network-leave-staged");
}

/* 6.6 */
static void sc_name(void)
{
    keys("P R2 P");
    snap("node-menu");
    keys("R1 P");
    snap("6.6-short-name");
    keys("R5");
    snap("6.6-short-name-ring");
    keys("P");
    snap("6.6-short-name-added");
    keys("R-4");
    snap("6.6-short-name-backspace");
    key_hold_push(600, true);
    snap("6.6-short-name-clear-hold");
    key_release_all();
    keys("B");
    snap("6.6-discard-dialog");
}

static void sc_long_name(void)
{
    snprintf(F.settings.long_name, sizeof(F.settings.long_name), "Pixelmapper Node - Lab rack left side");
    remote_settings(&F.settings, PM_SRC_LOCAL_UI);
    keys("W100 P R2 P R2 P");
    snap("6.6-long-name-scroll");
}

/* 6.7 */
static void sc_devices(void)
{
    keys("R1 P R2 P W800");
    snap("6.7-devices-port");
    keys("R7 W800");
    snap("6.7-devices-scrolled");
    keys("L R1 P R2 P W800 R3 P W600");
    snap("6.7-device-page");
    keys("R1 P F8");
    snap("6.7-device-address-edit");
    keys("P W300");
    snap("6.7-device-address-set");
    keys("R1 P W600 R2");
    snap("6.7-device-personality-edit");
    keys("P");
    snap("6.7-device-personality-confirm");
    keys("R1 P W300");
    snap("6.7-device-personality-set");
    keys("R4 P");
    snap("6.7-device-info");
}

static void sc_devices_all(void)
{
    keys("P R1 P W1500");
    snap("6.7-devices-all");
    keys("R14 W1000");
    snap("6.7-devices-all-port2");
    keys("P W1500");
    snap("6.7-device-not-responding");
}

static void sc_device_identify(void)
{
    keys("R1 P R2 P W800 R1 P W600 P R1 P W300");
    snap("6.7-device-identify-on");
}

/* 6.8 */
static void sc_factory(void)
{
    keys("P R5 P");
    snap("system-menu");
    keys("R3 P");
    snap("6.8-factory-choice");
    keys("R1 P");
    snap("6.8-factory-confirm");
    keys("R1");
    key_hold_push(1500, true);
    snap("6.8-factory-hold");
    key_release_all();
}

static void sc_restart(void)
{
    keys("P R5 P R2 P");
    snap("6.8-restart-confirm");
}

/* 6.9 */
static void sc_remote(void)
{
    pm_settings_t s = S();
    s.port[1].address = PM_PA_MAKE(0, 1, 4);
    remote_settings(&s, PM_SRC_ARTNET);
    sim_run(100);
    snap("6.9-remote-change-home");
    s.port[0].merge = PM_MERGE_LTP;
    s.port[2].address = 7;
    remote_settings(&s, PM_SRC_RDM);
    sim_run(100);
    snap("6.9-remote-multi-change");
}

static void sc_conflict(void)
{
    F.stats[0].fps = 0;
    keys("R1 P P R4");
    pm_settings_t s = S();
    s.port[0].address = PM_PA_MAKE(0, 1, 4);
    remote_settings(&s, PM_SRC_ARTNET);
    sim_run(100);
    snap("6.9-conflict-address-editor");
    keys("P");
    snap("6.9-conflict-local-wins");
}

static void sc_conflict_inline(void)
{
    keys("R1 P R3 P R1");
    snap("inline-edit-merge");
    pm_settings_t s = S();
    s.port[0].merge = PM_MERGE_LTP;
    s.port[0].rdm_enabled = true;
    remote_settings(&s, PM_SRC_ARTNET);
    sim_run(100);
    snap("6.9-conflict-inline");
    sim_run(4500);
    snap("6.9-conflict-inline-after");
}

static void sc_remote_page(void)
{
    keys("R1 P R4 P");
    pm_settings_t s = S();
    s.port[0].direction = PM_PORT_DIR_INPUT;
    remote_settings(&s, PM_SRC_ARTNET);
    sim_run(100);
    snap("remote-page-invalidated");
}

/* 6.10 */
static void sc_update(void)
{
    pm_ui_event_t e = {.type = PM_UI_EV_FW_PROGRESS, .value = 71};
    post_event(e);
    sim_run(100);
    snap("6.10-node-firmware-update");
    pm_ui_event_t t = {.type = PM_UI_EV_FW_PROGRESS, .value = -1};
    post_event(t);
    sim_run(3200); /* let the "failed" toast expire */
    pm_ui_event_t tu = {.type = PM_UI_EV_TUBE_UPDATE, .port = 0, .value = 41};
    post_event(tu);
    sim_run(100);
    snap("6.10-tube-update-home");
    keys("R1 P");
    snap("6.10-tube-update-port");
}

/* 6.11 */
static void sc_lock(void)
{
    pm_settings_t s = S();
    s.panel_lock = true;
    remote_settings(&s, PM_SRC_RDM);
    sim_run(3200);
    snap("lock-remote-home");
    keys("R1 P P");
    snap("6.11-lock-overlay-remote");
    key_combo(1500, true);
    snap("6.11-unlock-combo-progress");
    sim_run(1700);
    key_release_all();
    snap("6.11-unlocked");
    key_combo(3200, false);
    sim_run(3000);
    snap("lock-local");
}

/* 6.12 */
static void sc_identify(void)
{
    pm_ui_event_t e = {.type = PM_UI_EV_IDENTIFY, .value = 1};
    post_event(e);
    sim_run(50);
    sim_align(500);
    sim_run(40);
    snap("6.12-identify-bar-on");
    sim_run(250);
    snap("6.12-identify-bar-off");
    keys("P");
    sim_align(500);
    sim_run(40);
    snap("6.12-identify-in-menu");
}

static void sc_menus(void)
{
    keys("P");
    snap("4.2-main-menu");
    keys("R3 P");
    snap("4.2-display-menu");
    keys("P R1");
    snap("4.2-display-brightness-edit");
    keys("B B R-1 P");
    snap("4.2-node-menu");
    keys("B R3 P");
    snap("4.2-system-menu");
    keys("P");
    snap("4.2-system-info");
    keys("B R1 P");
    snap("4.2-events");
    keys("L R1 R-1 P R1 R1 R1 P");
    snap("4.2-network-info");
}

static void sc_mute(void)
{
    pm_ui_event_t e = {.type = PM_UI_EV_LED_MODE, .value = 0x03};
    post_event(e);
    sim_run(100);
    printf("    LED Mute: backlight %u, panel %s\n", F.backlight, F.panel_on ? "on" : "off");
    keys("R1");
    printf("    after input: backlight %u, panel %s\n", F.backlight, F.panel_on ? "on" : "off");
    sim_run(61000);
    printf("    after 61 s idle: backlight %u (dimmed)\n", F.backlight);
    snap("display-dimmed-content");
}

static void sc_regress(void)
{
    /* brightness at 5 % must not swallow keys (live preview == dim level) */
    keys("P R3 P P F-30 W100");
    snap("regress-brightness-5pct");
    keys("B");
    snap("regress-brightness-cancelled");
    /* locking with the combo closes an open editor */
    keys("L R1 P P R2");
    key_combo(3200, false);
    sim_run(200);
    snap("regress-lock-closes-editor");
    printf("    port 1 address after lock: %u (must stay 0)\n", (unsigned)F.settings.port[0].address);
    /* BACK long on an identifying device page switches identify off */
    key_combo(3200, false);
    keys("L R1 P R2 P W800 R1 P W600 P R1 P W300 L W300");
    printf("    device identify after BACK long: %d (must be 0)\n", F.dev[0][0].identify);
}

const scene_t g_scenes[] = {
    {"home", "Home: short name, IP + mode, 4 ports (6.1), also in 15-bit address format.", sc_home},
    {"home_cursor", "Home cursor after the first detents, port 2 lost its data (yellow, blinking).", sc_home_cursor},
    {"home_net", "Home without link, with the Art-Net fallback address, while DHCP is running.", sc_home_net},
    {"home_states", "Port 1 error (red), input without signal, enabled port without data.", sc_home_states},
    {"port", "Port detail with status line, scrolled, advanced page, data loss (6.3).", sc_port},
    {"port_input", "Port detail of an input and of a disabled port.", sc_port_input},
    {"addr", "Port-Address editor: quick edit on Uni, carry, field selection, OK (6.4).", sc_addr},
    {"addr_confirm", "Address change on a port that carries data asks first (6.4, 5.3).", sc_addr_confirm},
    {"addr_15", "Address editor in 15-bit format (N:S:U shown below).", sc_addr_15},
    {"network", "Network page: DHCP, staged static values, IP editor, mask, apply with confirmation (6.5, 6.8).",
     sc_network},
    {"ip_invalid", "IP editor rejects a broadcast address for the mask.", sc_ip_invalid},
    {"net_leave", "Leaving the network page with staged values.", sc_net_leave},
    {"name", "Text editor for the short name: ring, backspace, hold to clear, discard dialog (6.6).", sc_name},
    {"long_name", "Long name scrolls with an ellipsis.", sc_long_name},
    {"devices", "RDM device list of a port, device page, address and personality via RDM (6.7).", sc_devices},
    {"devices_all", "Device list of all ports; a device that does not answer.", sc_devices_all},
    {"device_identify", "RDM identify of a device from the panel.", sc_device_identify},
    {"factory", "Factory reset: choice, confirmation, 3 s hold (6.8).", sc_factory},
    {"restart", "Restart confirmation.", sc_restart},
    {"remote", "Remote change while on home: toast + orange value (6.9).", sc_remote},
    {"conflict", "Pixelmapper changes the address that is being edited: hint, local confirm wins (6.9).",
     sc_conflict},
    {"conflict_inline", "Same for an inline enum edit (Merge).", sc_conflict_inline},
    {"remote_page", "Remote change hides the field that is being edited.", sc_remote_page},
    {"update", "Node firmware update (full screen) and tube update on a port (6.10).", sc_update},
    {"lock", "Panel locked by Pixelmapper, edit attempt, unlock with BACK + push 3 s (6.11).", sc_lock},
    {"identify", "Identify / Art-Net Locate: the status bar blinks (6.12).", sc_identify},
    {"menus", "Main menu, display, node, system, info and event pages (4.2).", sc_menus},
    {"regress", "Review regressions: 5 % brightness, lock closes editors, identify off on BACK long.", sc_regress},
    {"mute", "Art-Net LED Mute switches the display off; dimming after the timeout.", sc_mute},
};
const int g_num_scenes = (int)(sizeof(g_scenes) / sizeof(g_scenes[0]));
