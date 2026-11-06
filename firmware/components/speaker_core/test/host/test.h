/* Tiny assert-style test harness for speaker_core host tests. */
#ifndef SC_TEST_H
#define SC_TEST_H

#include <stdio.h>
#include <string.h>

extern int sc_test_failures;
extern int sc_test_checks;

#define CHECK(cond)                                                                   \
    do {                                                                              \
        sc_test_checks++;                                                             \
        if (!(cond)) {                                                                \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);  \
            sc_test_failures++;                                                       \
        }                                                                             \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                              \
    do {                                                                                                \
        long long _a = (long long)(a), _b = (long long)(b);                                             \
        sc_test_checks++;                                                                               \
        if (_a != _b) {                                                                                 \
            fprintf(stderr, "%s:%d: %s == %s failed: %lld != %lld\n", __FILE__, __LINE__, #a, #b, _a, _b); \
            sc_test_failures++;                                                                         \
        }                                                                                               \
    } while (0)

#define CHECK_STR(a, b)                                                                                    \
    do {                                                                                                   \
        const char *_a = (a), *_b = (b);                                                                   \
        sc_test_checks++;                                                                                  \
        if (!_a || !_b || strcmp(_a, _b) != 0) {                                                           \
            fprintf(stderr, "%s:%d: %s == %s failed:\n  got:      %s\n  expected: %s\n", __FILE__, __LINE__, \
                    #a, #b, _a ? _a : "(null)", _b ? _b : "(null)");                                       \
            sc_test_failures++;                                                                            \
        }                                                                                                  \
    } while (0)

#define RUN(fn)                               \
    do {                                      \
        int _before = sc_test_failures;       \
        fn();                                 \
        printf("  %-48s %s\n", #fn, sc_test_failures == _before ? "ok" : "FAIL"); \
    } while (0)

/* Reads a fixture file from SC_FIXTURE_DIR; returns malloc'd NUL-terminated data. */
char *sc_test_read_fixture(const char *name, size_t *len);

/* Deterministic xorshift PRNG usable as sc_random_fn. */
unsigned sc_test_rand(void *ctx);

void suite_util(void);
void suite_jwt(void);
void suite_set_cookie(void);
void suite_cookie_store(void);
void suite_multipart(void);
void suite_session_json(void);
void suite_wm_events(void);
void suite_session_fsm(void);
void suite_voice_control(void);

#endif
