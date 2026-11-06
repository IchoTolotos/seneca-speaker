/*
 * ChatGPT NextAuth session cookie handling.
 *
 * The web session lives in `__Secure-next-auth.session-token` or, when it is
 * too large for one cookie, in numbered chunks `...session-token.0`, `.1`, ...
 * Every GET /api/auth/session may answer with Set-Cookie headers carrying a
 * rotated session; the device must persist the new set (see cookie_store.h).
 *
 * Usage per auth request:
 *   sc_session_cookies_t incoming; sc_cookies_init(&incoming);
 *   for each "Set-Cookie" response header value: sc_set_cookie_feed(&incoming, value);
 *   if (sc_cookies_apply_rotation(&current, &incoming) == 1) persist current;
 *   sc_cookies_free(&incoming);
 */
#ifndef SC_SET_COOKIE_H
#define SC_SET_COOKIE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SC_SESSION_COOKIE_NAME "__Secure-next-auth.session-token"
#define SC_COOKIE_MAX_CHUNKS 8
#define SC_COOKIE_UNCHUNKED (-1) /* index of the plain, unnumbered cookie */
#define SC_COOKIE_MAX_VALUE 8192 /* per chunk; NextAuth chunks are ~4 KB */

typedef struct {
    int index;   /* 0..N for `.N` chunks, SC_COOKIE_UNCHUNKED for the plain name */
    char *value; /* heap, NUL-terminated */
} sc_cookie_chunk_t;

typedef struct {
    sc_cookie_chunk_t chunks[SC_COOKIE_MAX_CHUNKS]; /* kept sorted by index */
    size_t count;
} sc_session_cookies_t;

void sc_cookies_init(sc_session_cookies_t *c);
void sc_cookies_free(sc_session_cookies_t *c); /* frees values, leaves c empty */

/** Inserts or replaces one chunk. Rejects empty values and values with ';', ',', CR, LF, space. */
int sc_cookies_set(sc_session_cookies_t *c, int index, const char *value, size_t len);
const char *sc_cookies_get(const sc_session_cookies_t *c, int index);
bool sc_cookies_equal(const sc_session_cookies_t *a, const sc_session_cookies_t *b);
int sc_cookies_copy(sc_session_cookies_t *dst, const sc_session_cookies_t *src);

/**
 * Parses one Set-Cookie header value (or several joined by ", " as some HTTP
 * stacks do) and records every session-token cookie into acc. Cookies with an
 * empty value (deletions) are ignored. Returns the number of session cookies
 * recorded (0 if none) or a negative SC_ERR_*.
 */
int sc_set_cookie_feed(sc_session_cookies_t *acc, const char *header_value);

/**
 * Replaces current with the rotated set in incoming (same rule as
 * login-with-chatgpt): if incoming holds any `.N` chunks, current becomes
 * exactly those chunks; else if it holds the plain cookie, current becomes
 * that; else current is unchanged. Returns 1 if current changed, 0 if not,
 * negative on error (current untouched on error).
 */
int sc_cookies_apply_rotation(sc_session_cookies_t *current, const sc_session_cookies_t *incoming);

/**
 * Builds the `Cookie:` request header value:
 *   "<name>.0=<v0>; <name>.1=<v1>; oai-did=<device_id>"
 * device_id may be NULL to omit oai-did. Returns the string length, or a
 * negative SC_ERR_TOO_BIG if cap is too small (call sc_cookies_header_len first).
 */
int sc_cookies_build_header(const sc_session_cookies_t *c, const char *device_id, char *out, size_t cap);
size_t sc_cookies_header_len(const sc_session_cookies_t *c, const char *device_id); /* excl. NUL */

/**
 * Accepts what the setup page posts: either chunk values (chunk0 + optional
 * chunk1) or, when chunk1 is NULL/empty and chunk0 starts with '{', the JSON
 * map {"__Secure-next-auth.session-token.0":"..",".1":".."} used by the Phase 0
 * signaling page. A lone chunk0 without chunk1 is stored as the unchunked cookie.
 */
int sc_cookies_from_setup(sc_session_cookies_t *c, const char *chunk0, const char *chunk1);

#ifdef __cplusplus
}
#endif
#endif
