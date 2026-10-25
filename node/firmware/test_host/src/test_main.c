/* Host test runner: builds pm_protocol, pm_artnet, pm_rdm on the Mac and checks them against protocol/vectors. */
#include <stdlib.h>

#include "test.h"
#include "vectors.h"

int g_failures;
int g_checks;
const char *g_context;

void test_protocol(void);
void test_artnet(void);
void test_rdm(void);
void test_node(void);

static const test_case_t TESTS[] = {
    {"protocol (uid, crc, settings, image desc, pids)", test_protocol},
    {"artnet vectors", test_artnet},
    {"rdm vectors", test_rdm},
    {"node glue (pm_node)", test_node},
};

const pm_vec_field_t *vec_field(const pm_vec_t *v, const char *key)
{
    for (size_t i = 0; i < v->nfields; i++) {
        if (v->fields[i].key && strcmp(v->fields[i].key, key) == 0) return &v->fields[i];
    }
    return NULL;
}

int vec_has(const pm_vec_t *v, const char *key) { return vec_field(v, key) != NULL; }

long long vec_num(const pm_vec_t *v, const char *key)
{
    const pm_vec_field_t *f = vec_field(v, key);
    return f && f->is_number ? f->num : 0;
}

const char *vec_str(const pm_vec_t *v, const char *key)
{
    const pm_vec_field_t *f = vec_field(v, key);
    return f && !f->is_number ? f->str : NULL;
}

int vec_hex(const char *hex, uint8_t *buf, size_t cap)
{
    size_t n = 0;
    for (const char *p = hex; p[0] && p[1]; p += 2) {
        unsigned b;
        if (n >= cap || sscanf(p, "%2x", &b) != 1) return -1;
        buf[n++] = (uint8_t)b;
    }
    return (int)n;
}

int main(void)
{
    printf("%zu vectors loaded\n", pm_vectors_count);
    for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
        int before = g_failures;
        g_context = NULL;
        TESTS[i].fn();
        printf("%-50s %s\n", TESTS[i].name, g_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
