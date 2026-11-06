#include <stdint.h>
#include <stdlib.h>

#include "test.h"

int sc_test_failures = 0;
int sc_test_checks = 0;

char *sc_test_read_fixture(const char *name, size_t *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", SC_FIXTURE_DIR, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "missing fixture %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    while (got && (buf[got - 1] == '\n' || buf[got - 1] == '\r')) buf[--got] = 0;
    if (len) *len = got;
    return buf;
}

unsigned sc_test_rand(void *ctx)
{
    static uint32_t fallback = 0x12345678u;
    uint32_t *s = ctx ? (uint32_t *)ctx : &fallback;
    uint32_t x = *s ? *s : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

static const struct {
    const char *name;
    void (*fn)(void);
} SUITES[] = {
    {"util", suite_util},
    {"jwt", suite_jwt},
    {"set_cookie", suite_set_cookie},
    {"cookie_store", suite_cookie_store},
    {"multipart", suite_multipart},
    {"session_json", suite_session_json},
    {"wm_events", suite_wm_events},
    {"session_fsm", suite_session_fsm},
    {"voice_control", suite_voice_control},
};

int main(int argc, char **argv)
{
    int ran = 0;
    for (size_t i = 0; i < sizeof SUITES / sizeof SUITES[0]; i++) {
        if (argc > 1 && strcmp(argv[1], SUITES[i].name) != 0) continue;
        printf("[%s]\n", SUITES[i].name);
        SUITES[i].fn();
        ran++;
    }
    if (!ran) {
        fprintf(stderr, "unknown suite %s\n", argv[1]);
        return 2;
    }
    printf("%d checks, %d failures\n", sc_test_checks, sc_test_failures);
    return sc_test_failures ? 1 : 0;
}
