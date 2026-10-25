/* Host tests: Art-Net fallback address formula and the DHCP/fallback/static state machine of pm_net. */
#include "pm_net_ip.h"
#include "test.h"

#define IP(a, b, c, d) (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

static const uint8_t MAC[6] = {0x12, 0x45, 0x78, 0x98, 0x34, 0x76};

static void test_formula(void)
{
    g_context = "fallback formula";
    /* Spec example (Art-Net 4 p. 13-14): MAC 12:45:78:98:34:76, OEM 0x0010 -> 2.168.52.118 */
    CHECK_EQ(pm_net_fallback_ip(MAC, 0x0010), IP(2, 168, 52, 118));
    /* our prototype OEM 0x00FF: 0x98 + 0x00 + 0xFF = 0x197 -> 0x97 (mod 256) */
    CHECK_EQ(pm_net_fallback_ip(MAC, 0x00FF), IP(2, 0x97, 0x34, 0x76));
    const uint8_t m2[6] = {0, 0, 0, 0xFF, 0xFF, 0xFF};
    CHECK_EQ(pm_net_fallback_ip(m2, 0xFFFF), IP(2, (0xFF + 0xFF + 0xFF) & 0xFF, 255, 255));
    CHECK_EQ(pm_net_fallback_ip(m2, 0x0000), IP(2, 255, 255, 255)); /* degenerate, but defined */
}

static void test_static(void)
{
    g_context = "static plan";
    pm_ip_settings_t s = {.dhcp = false, .static_ip = 0, .static_mask = 0, .gateway = 0};
    pm_net_addr_t a;
    pm_net_static_addr(&s, MAC, 0x10, &a);
    CHECK_EQ(a.mode, PM_NET_ADDR_STATIC);
    CHECK_EQ(a.ip, IP(2, 168, 52, 118));
    CHECK_EQ(a.mask, 0xFF000000u);
    s.static_ip = IP(192, 168, 1, 50);
    s.static_mask = 0xFFFFFF00u;
    s.gateway = IP(192, 168, 1, 1);
    pm_net_static_addr(&s, MAC, 0x10, &a);
    CHECK_EQ(a.ip, IP(192, 168, 1, 50));
    CHECK_EQ(a.mask, 0xFFFFFF00u);
    CHECK_EQ(a.gateway, IP(192, 168, 1, 1));
}

static void test_dhcp_fallback(void)
{
    g_context = "dhcp -> fallback -> late lease";
    pm_net_ipsm_t sm;
    pm_net_ipsm_init(&sm, MAC, 0xFF);
    pm_ip_settings_t s = {.dhcp = true, .static_mask = 0xFF000000u};
    unsigned act = pm_net_ipsm_configure(&sm, &s, MAC, 0xFF, 100);
    CHECK(act & PM_NET_ACT_DHCP_START);
    CHECK(!(act & PM_NET_ACT_SET_FALLBACK));
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_NONE);
    /* no link: no timer */
    CHECK_EQ(pm_net_ipsm_tick(&sm, 100000), 0);
    CHECK_EQ(pm_net_ipsm_link(&sm, true, 1000), PM_NET_ACT_NOTIFY);
    CHECK_EQ(pm_net_ipsm_tick(&sm, 5999), 0);
    act = pm_net_ipsm_tick(&sm, 6000);
    CHECK(act & PM_NET_ACT_SET_FALLBACK);
    CHECK(!(act & PM_NET_ACT_DHCP_STOP)); /* client keeps asking */
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_FALLBACK);
    CHECK_EQ(sm.addr.ip, IP(2, 0x97, 0x34, 0x76));
    CHECK_EQ(sm.addr.mask, 0xFF000000u);
    CHECK_EQ(pm_net_ipsm_tick(&sm, 20000), 0); /* only once */
    /* a DHCP server appears later */
    act = pm_net_ipsm_dhcp_bound(&sm, IP(10, 0, 0, 7), 0xFFFFFF00u, IP(10, 0, 0, 1));
    CHECK_EQ(act, PM_NET_ACT_NOTIFY);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_DHCP);
    CHECK_EQ(sm.addr.ip, IP(10, 0, 0, 7));
    CHECK_EQ(pm_net_ipsm_dhcp_bound(&sm, IP(10, 0, 0, 7), 0xFFFFFF00u, IP(10, 0, 0, 1)), 0); /* renewal */
    /* lease lost -> fallback again after 5 s */
    CHECK_EQ(pm_net_ipsm_dhcp_lost(&sm, 50000), PM_NET_ACT_NOTIFY);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_NONE);
    CHECK_EQ(pm_net_ipsm_tick(&sm, 54999), 0);
    CHECK(pm_net_ipsm_tick(&sm, 55000) & PM_NET_ACT_SET_FALLBACK);
    /* reconfigure DHCP while already DHCP: nothing to do */
    CHECK_EQ(pm_net_ipsm_configure(&sm, &s, MAC, 0xFF, 60000), 0);
}

static void test_lease_in_time(void)
{
    g_context = "lease within 5 s";
    pm_net_ipsm_t sm;
    pm_net_ipsm_init(&sm, MAC, 0xFF);
    pm_ip_settings_t s = {.dhcp = true};
    pm_net_ipsm_configure(&sm, &s, MAC, 0xFF, 0);
    pm_net_ipsm_link(&sm, true, 0);
    CHECK_EQ(pm_net_ipsm_dhcp_bound(&sm, IP(192, 168, 0, 20), 0xFFFFFF00u, 0), PM_NET_ACT_NOTIFY);
    CHECK_EQ(pm_net_ipsm_tick(&sm, 10000), 0);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_DHCP);
    /* link flap: IDF drops the address and restarts DHCP -> wait again, fallback after 5 s without lease */
    CHECK_EQ(pm_net_ipsm_link(&sm, false, 11000), PM_NET_ACT_NOTIFY);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_NONE);
    CHECK_EQ(pm_net_ipsm_tick(&sm, 30000), 0); /* no link: no fallback */
    CHECK_EQ(pm_net_ipsm_link(&sm, true, 40000), PM_NET_ACT_NOTIFY);
    CHECK_EQ(pm_net_ipsm_dhcp_bound(&sm, IP(192, 168, 0, 20), 0xFFFFFF00u, 0), PM_NET_ACT_NOTIFY);
    CHECK_EQ(pm_net_ipsm_tick(&sm, 60000), 0);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_DHCP);
    /* timer wrap-around */
    pm_net_ipsm_t w;
    pm_net_ipsm_init(&w, MAC, 0xFF);
    pm_net_ipsm_configure(&w, &s, MAC, 0xFF, 0xFFFFF000u);
    pm_net_ipsm_link(&w, true, 0xFFFFF000u);
    CHECK_EQ(pm_net_ipsm_tick(&w, 0x00000100u), 0);
    CHECK(pm_net_ipsm_tick(&w, 0xFFFFF000u + 5000u) & PM_NET_ACT_SET_FALLBACK);
}

static void test_switching(void)
{
    g_context = "static <-> dhcp (ArtIpProg)";
    pm_net_ipsm_t sm;
    pm_net_ipsm_init(&sm, MAC, 0xFF);
    pm_ip_settings_t st = {.dhcp = false, .static_ip = IP(2, 0, 0, 10), .static_mask = 0xFF000000u};
    unsigned act = pm_net_ipsm_configure(&sm, &st, MAC, 0xFF, 0);
    CHECK_EQ(act, PM_NET_ACT_DHCP_STOP | PM_NET_ACT_SET_STATIC | PM_NET_ACT_NOTIFY);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_STATIC);
    CHECK_EQ(pm_net_ipsm_link(&sm, true, 0), PM_NET_ACT_SET_STATIC | PM_NET_ACT_NOTIFY);
    CHECK_EQ(pm_net_ipsm_tick(&sm, 100000), 0);
    CHECK_EQ(pm_net_ipsm_dhcp_bound(&sm, IP(10, 0, 0, 2), 0xFF000000u, 0), 0); /* stray event ignored */
    CHECK_EQ(pm_net_ipsm_configure(&sm, &st, MAC, 0xFF, 1), 0);                 /* unchanged */
    st.static_ip = IP(2, 0, 0, 11);
    CHECK(pm_net_ipsm_configure(&sm, &st, MAC, 0xFF, 2) & PM_NET_ACT_SET_STATIC);
    CHECK_EQ(sm.addr.ip, IP(2, 0, 0, 11));
    /* ArtIpProg bit 6: DHCP on (link is up -> fallback timer starts now) */
    pm_ip_settings_t dh = {.dhcp = true};
    act = pm_net_ipsm_configure(&sm, &dh, MAC, 0xFF, 1000);
    CHECK_EQ(act, PM_NET_ACT_DHCP_START | PM_NET_ACT_NOTIFY);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_NONE);
    CHECK(pm_net_ipsm_tick(&sm, 6000) & PM_NET_ACT_SET_FALLBACK);
    /* back to static from fallback */
    act = pm_net_ipsm_configure(&sm, &st, MAC, 0xFF, 7000);
    CHECK(act & PM_NET_ACT_DHCP_STOP);
    CHECK_EQ(sm.addr.mode, PM_NET_ADDR_STATIC);
    CHECK_EQ(pm_net_ipsm_dhcp_lost(&sm, 8000), 0);
}

void test_net_ip(void)
{
    test_formula();
    test_static();
    test_dhcp_fallback();
    test_lease_in_time();
    test_switching();
}
