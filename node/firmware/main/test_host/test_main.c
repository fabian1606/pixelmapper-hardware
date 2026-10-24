/* Host test runner for the pure-C logic of pm_net (IP), pm_config (NVS blob/migration), pm_ota (session). */
#include <stdio.h>

#include "test.h"

int g_failures;
int g_checks;
const char *g_context;

void test_net_ip(void);
void test_config_store(void);
void test_ota_session(void);

static const test_case_t TESTS[] = {
    {"pm_net ip (fallback formula, address plan, state machine)", test_net_ip},
    {"pm_config store (encode/decode, migration)", test_config_store},
    {"pm_ota session (desc check, sequence, crc)", test_ota_session},
};

int main(void)
{
    for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        int before = g_failures;
        g_context = NULL;
        TESTS[i].fn();
        printf("%-60s %s\n", TESTS[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
