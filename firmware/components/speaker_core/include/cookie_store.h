/*
 * Power-loss-safe persistence of the rotated ChatGPT session cookies.
 *
 * Layout in the injected KV (NVS namespace chosen by the integrator):
 *   "sc_ck_a", "sc_ck_b"  slot blobs: header {magic, generation, length, crc32} + payload
 *   "sc_ck_act"           active pointer {magic, slot, generation, crc32}
 *
 * Save: write the INACTIVE slot completely (gen = newest + 1), then flip the
 * active pointer. Load: validate both slots (magic, length, CRC) and pick the
 * one with the highest generation; the pointer only breaks ties. A crash at
 * any point therefore leaves either the old or the new session readable.
 *
 * Not thread-safe: call from one task (or guard with a mutex).
 */
#ifndef SC_COOKIE_STORE_H
#define SC_COOKIE_STORE_H

#include <stddef.h>
#include <stdint.h>

#include "sc_platform.h"
#include "set_cookie.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC_CK_KEY_A "sc_ck_a"
#define SC_CK_KEY_B "sc_ck_b"
#define SC_CK_KEY_ACTIVE "sc_ck_act"
#define SC_CK_MAX_PAYLOAD (32 * 1024)
#define SC_DEVICE_ID_MAX 64

typedef struct {
    const sc_kv_t *kv;
    int active;          /* 0 = A, 1 = B, -1 = nothing stored */
    uint32_t generation; /* generation of the active slot */
} sc_cookie_store_t;

/** Scans both slots so the next save targets the right one. Never fails on corrupt data. */
int sc_cookie_store_init(sc_cookie_store_t *st, const sc_kv_t *kv);

/** Raw blob API. load returns a malloc'd copy in *out (caller frees) or SC_ERR_NOT_FOUND. */
int sc_cookie_store_load(sc_cookie_store_t *st, uint8_t **out, size_t *out_len);
int sc_cookie_store_save(sc_cookie_store_t *st, const uint8_t *data, size_t len);
/** Erases both slots and the pointer (logout). */
int sc_cookie_store_clear(sc_cookie_store_t *st);

/**
 * Session helpers: (de)serialise cookies + device id ("oai-did") as text lines
 * "name=value\n" and store/load them through the A/B slots.
 * device_id buffer must hold SC_DEVICE_ID_MAX bytes.
 */
int sc_cookie_store_save_session(sc_cookie_store_t *st, const sc_session_cookies_t *c, const char *device_id);
int sc_cookie_store_load_session(sc_cookie_store_t *st, sc_session_cookies_t *c, char *device_id);

int sc_cookie_record_encode(const sc_session_cookies_t *c, const char *device_id, char **out, size_t *out_len);
int sc_cookie_record_decode(const char *data, size_t len, sc_session_cookies_t *c, char *device_id);

#ifdef __cplusplus
}
#endif
#endif
