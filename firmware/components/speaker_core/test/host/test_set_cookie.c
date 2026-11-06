#include <stdint.h>
#include <stdlib.h>

#include "sc_platform.h"
#include "set_cookie.h"
#include "test.h"

/* NextAuth session values are JWE strings (base64url segments joined by '.'), ~3.9 KB per chunk. */
static char *fake_jwe(size_t len, uint32_t seed)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    char *s = malloc(len + 1);
    const char *head = "eyJhbGciOiJkaXIiLCJlbmMiOiJBMjU2R0NNIn0..";
    size_t h = strlen(head);
    memcpy(s, head, h);
    for (size_t i = h; i < len; i++) s[i] = (i % 977 == 0) ? '.' : T[sc_test_rand(&seed) & 63];
    s[len] = 0;
    return s;
}

static char *set_cookie_line(const char *name, const char *value)
{
    size_t n = strlen(name) + strlen(value) + 200;
    char *s = malloc(n);
    snprintf(s, n, "%s=%s; Domain=.chatgpt.com; Path=/; Expires=Sun, 22 Nov 2026 10:00:00 GMT; HttpOnly; Secure; SameSite=Lax",
             name, value);
    return s;
}

static void parses_rotated_chunks_from_separate_headers(void)
{
    char *v0 = fake_jwe(3936, 1), *v1 = fake_jwe(1210, 2);
    char *h0 = set_cookie_line("__Secure-next-auth.session-token.0", v0);
    char *h1 = set_cookie_line("__Secure-next-auth.session-token.1", v1);
    sc_session_cookies_t acc;
    sc_cookies_init(&acc);
    CHECK_EQ_INT(sc_set_cookie_feed(&acc, "__cf_bm=abc.def-123; path=/; expires=Sat, 24-Oct-26 10:30:00 GMT; domain=.chatgpt.com; HttpOnly; Secure; SameSite=None"), 0);
    CHECK_EQ_INT(sc_set_cookie_feed(&acc, h1), 1); /* order of headers doesn't matter */
    CHECK_EQ_INT(sc_set_cookie_feed(&acc, h0), 1);
    CHECK_EQ_INT(sc_set_cookie_feed(&acc, "__Secure-next-auth.callback-url=https%3A%2F%2Fchatgpt.com; Path=/; Secure"), 0);
    CHECK_EQ_INT(acc.count, 2);
    CHECK_STR(sc_cookies_get(&acc, 0), v0);
    CHECK_STR(sc_cookies_get(&acc, 1), v1);
    CHECK_EQ_INT(strlen(sc_cookies_get(&acc, 0)), 3936);
    sc_cookies_free(&acc);
    free(v0); free(v1); free(h0); free(h1);
}

static void parses_comma_joined_header_and_ignores_deletions(void)
{
    const char *joined =
        "__Secure-next-auth.session-token=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT; Max-Age=0, "
        "__Secure-next-auth.session-token.0=AAA.bbb-ccc_ddd; Path=/; Expires=Sun, 22 Nov 2026 10:00:00 GMT; Secure, "
        "x__Secure-next-auth.session-token.1=evil; Path=/, "
        "__Secure-next-auth.session-token.1=EEE.fff; Path=/; Secure";
    sc_session_cookies_t acc;
    sc_cookies_init(&acc);
    CHECK_EQ_INT(sc_set_cookie_feed(&acc, joined), 2);
    CHECK_EQ_INT(acc.count, 2);
    CHECK_STR(sc_cookies_get(&acc, 0), "AAA.bbb-ccc_ddd");
    CHECK_STR(sc_cookies_get(&acc, 1), "EEE.fff");
    CHECK(sc_cookies_get(&acc, SC_COOKIE_UNCHUNKED) == NULL);
    sc_cookies_free(&acc);
}

static void rotation_replaces_whole_set(void)
{
    sc_session_cookies_t cur, in;
    sc_cookies_init(&cur);
    sc_cookies_init(&in);
    CHECK_EQ_INT(sc_cookies_from_setup(&cur, "old0", "old1"), SC_OK);
    /* No session cookies in response: unchanged */
    CHECK_EQ_INT(sc_cookies_apply_rotation(&cur, &in), 0);
    CHECK_STR(sc_cookies_get(&cur, 0), "old0");
    /* Same values: unchanged */
    sc_set_cookie_feed(&in, "__Secure-next-auth.session-token.0=old0; Path=/");
    sc_set_cookie_feed(&in, "__Secure-next-auth.session-token.1=old1; Path=/");
    CHECK_EQ_INT(sc_cookies_apply_rotation(&cur, &in), 0);
    sc_cookies_free(&in);
    /* Shrinks to a single unchunked cookie */
    sc_set_cookie_feed(&in, "__Secure-next-auth.session-token=single; Path=/");
    CHECK_EQ_INT(sc_cookies_apply_rotation(&cur, &in), 1);
    CHECK_EQ_INT(cur.count, 1);
    CHECK_STR(sc_cookies_get(&cur, SC_COOKIE_UNCHUNKED), "single");
    sc_cookies_free(&in);
    /* Chunks win over a plain cookie in the same response */
    sc_set_cookie_feed(&in, "__Secure-next-auth.session-token=plain; Path=/");
    sc_set_cookie_feed(&in, "__Secure-next-auth.session-token.0=n0; Path=/");
    sc_set_cookie_feed(&in, "__Secure-next-auth.session-token.1=n1; Path=/");
    sc_set_cookie_feed(&in, "__Secure-next-auth.session-token.2=n2; Path=/");
    CHECK_EQ_INT(sc_cookies_apply_rotation(&cur, &in), 1);
    CHECK_EQ_INT(cur.count, 3);
    CHECK(sc_cookies_get(&cur, SC_COOKIE_UNCHUNKED) == NULL);
    CHECK_STR(sc_cookies_get(&cur, 2), "n2");
    sc_cookies_free(&in);
    sc_cookies_free(&cur);
}

static void builds_cookie_header(void)
{
    sc_session_cookies_t c;
    sc_cookies_init(&c);
    CHECK_EQ_INT(sc_cookies_set(&c, 1, "B1", 2), SC_OK);
    CHECK_EQ_INT(sc_cookies_set(&c, 0, "A0", 2), SC_OK);
    CHECK_EQ_INT(sc_cookies_set(&c, 10, "K10", 3), SC_OK);
    const char *did = "4f9b2c1e-8d7a-4e3b-9c2d-1a2b3c4d5e6f";
    size_t need = sc_cookies_header_len(&c, did);
    char *buf = malloc(need + 1);
    CHECK_EQ_INT(sc_cookies_build_header(&c, did, buf, need + 1), (int)need);
    CHECK_STR(buf, "__Secure-next-auth.session-token.0=A0; __Secure-next-auth.session-token.1=B1; "
                   "__Secure-next-auth.session-token.10=K10; oai-did=4f9b2c1e-8d7a-4e3b-9c2d-1a2b3c4d5e6f");
    CHECK_EQ_INT(sc_cookies_build_header(&c, did, buf, need), SC_ERR_TOO_BIG);
    free(buf);
    char small[128];
    sc_cookies_free(&c);
    sc_cookies_set(&c, SC_COOKIE_UNCHUNKED, "tok", 3);
    sc_cookies_build_header(&c, NULL, small, sizeof small);
    CHECK_STR(small, "__Secure-next-auth.session-token=tok");
    sc_cookies_free(&c);
}

static void long_realistic_header_roundtrip(void)
{
    char *v0 = fake_jwe(3936, 11), *v1 = fake_jwe(2875, 12);
    sc_session_cookies_t c;
    sc_cookies_init(&c);
    CHECK_EQ_INT(sc_cookies_from_setup(&c, v0, v1), SC_OK);
    size_t need = sc_cookies_header_len(&c, "did");
    CHECK(need > 6800);
    char *buf = malloc(need + 1);
    CHECK(sc_cookies_build_header(&c, "did", buf, need + 1) > 0);
    CHECK(strstr(buf, v1) != NULL);
    free(buf);
    sc_cookies_free(&c);
    free(v0); free(v1);
}

static void setup_input_forms(void)
{
    sc_session_cookies_t c;
    sc_cookies_init(&c);
    CHECK_EQ_INT(sc_cookies_from_setup(&c, "  abc.def\n", ""), SC_OK);
    CHECK_STR(sc_cookies_get(&c, SC_COOKIE_UNCHUNKED), "abc.def");
    CHECK_EQ_INT(sc_cookies_from_setup(&c, "{\"__Secure-next-auth.session-token.0\":\"X0\",\"__Secure-next-auth.session-token.1\":\"X1\",\"other\":\"z\"}", NULL), SC_OK);
    CHECK_EQ_INT(c.count, 2);
    CHECK_STR(sc_cookies_get(&c, 1), "X1");
    /* invalid characters are rejected and leave the old value */
    CHECK_EQ_INT(sc_cookies_from_setup(&c, "bad;value", "x"), SC_ERR_ARG);
    CHECK_STR(sc_cookies_get(&c, 0), "X0");
    CHECK_EQ_INT(sc_cookies_from_setup(&c, "", ""), SC_ERR_ARG);
    CHECK_EQ_INT(sc_cookies_from_setup(&c, "{broken", NULL), SC_ERR_PARSE);
    sc_cookies_free(&c);
}

void suite_set_cookie(void)
{
    RUN(parses_rotated_chunks_from_separate_headers);
    RUN(parses_comma_joined_header_and_ignores_deletions);
    RUN(rotation_replaces_whole_set);
    RUN(builds_cookie_header);
    RUN(long_realistic_header_roundtrip);
    RUN(setup_input_forms);
}
