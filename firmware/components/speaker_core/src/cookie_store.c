#include "cookie_store.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc_util.h"

#define SLOT_MAGIC 0x31434B53u /* "SKC1" little-endian */
#define PTR_MAGIC 0x31544B53u
#define HDR_LEN 16
#define PTR_LEN 16

static const char *slot_key(int slot)
{
    return slot ? SC_CK_KEY_B : SC_CK_KEY_A;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Reads and validates one slot. On success returns malloc'd blob (header+payload). */
static int read_slot(const sc_kv_t *kv, int slot, uint8_t **blob, size_t *blob_len, uint32_t *gen)
{
    size_t len = 0;
    int rc = kv->get(kv->ctx, slot_key(slot), NULL, 0, &len);
    if (rc != SC_OK) return rc;
    if (len < HDR_LEN || len > HDR_LEN + SC_CK_MAX_PAYLOAD) return SC_ERR_CORRUPT;
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) return SC_ERR_NOMEM;
    size_t got = 0;
    rc = kv->get(kv->ctx, slot_key(slot), buf, len, &got);
    if (rc != SC_OK || got != len) {
        free(buf);
        return rc != SC_OK ? rc : SC_ERR_CORRUPT;
    }
    uint32_t plen = get32(buf + 8);
    if (get32(buf) != SLOT_MAGIC || plen != len - HDR_LEN ||
        sc_crc32(sc_crc32(0, buf + 4, 8), buf + HDR_LEN, plen) != get32(buf + 12)) {
        free(buf);
        return SC_ERR_CORRUPT;
    }
    *gen = get32(buf + 4);
    *blob = buf;
    *blob_len = len;
    return SC_OK;
}

static int read_pointer(const sc_kv_t *kv, int *slot, uint32_t *gen)
{
    uint8_t p[PTR_LEN];
    size_t got = 0;
    if (kv->get(kv->ctx, SC_CK_KEY_ACTIVE, p, sizeof p, &got) != SC_OK || got != PTR_LEN) return SC_ERR_NOT_FOUND;
    if (get32(p) != PTR_MAGIC || get32(p + 12) != sc_crc32(0, p, 12) || get32(p + 4) > 1) return SC_ERR_CORRUPT;
    *slot = (int)get32(p + 4);
    *gen = get32(p + 8);
    return SC_OK;
}

/* Picks the newest valid slot. Returns slot index or -1; fills blob if requested. */
static int pick(const sc_kv_t *kv, uint8_t **blob, size_t *blob_len, uint32_t *gen_out)
{
    uint8_t *b[2] = {NULL, NULL};
    size_t l[2] = {0, 0};
    uint32_t g[2] = {0, 0};
    bool ok[2];
    for (int s = 0; s < 2; s++) ok[s] = read_slot(kv, s, &b[s], &l[s], &g[s]) == SC_OK;
    int best = -1;
    if (ok[0] && ok[1]) {
        if (g[0] != g[1]) {
            best = g[1] > g[0] ? 1 : 0;
        } else {
            int ps;
            uint32_t pg;
            best = (read_pointer(kv, &ps, &pg) == SC_OK) ? ps : 0;
        }
    } else if (ok[0]) {
        best = 0;
    } else if (ok[1]) {
        best = 1;
    }
    for (int s = 0; s < 2; s++) {
        if (s == best && blob) {
            *blob = b[s];
            *blob_len = l[s];
        } else {
            free(b[s]);
        }
    }
    if (best >= 0 && gen_out) *gen_out = g[best];
    return best;
}

int sc_cookie_store_init(sc_cookie_store_t *st, const sc_kv_t *kv)
{
    if (!st || !kv || !kv->get || !kv->set) return SC_ERR_ARG;
    st->kv = kv;
    st->generation = 0;
    st->active = pick(kv, NULL, NULL, &st->generation);
    if (st->active < 0) st->generation = 0;
    return SC_OK;
}

int sc_cookie_store_load(sc_cookie_store_t *st, uint8_t **out, size_t *out_len)
{
    if (!st || !st->kv || !out || !out_len) return SC_ERR_ARG;
    uint8_t *blob = NULL;
    size_t len = 0;
    uint32_t gen = 0;
    int slot = pick(st->kv, &blob, &len, &gen);
    st->active = slot;
    st->generation = slot >= 0 ? gen : 0;
    if (slot < 0) return SC_ERR_NOT_FOUND;
    size_t plen = len - HDR_LEN;
    uint8_t *payload = (uint8_t *)malloc(plen + 1);
    if (!payload) {
        free(blob);
        return SC_ERR_NOMEM;
    }
    memcpy(payload, blob + HDR_LEN, plen);
    payload[plen] = 0; /* convenience for text payloads */
    free(blob);
    *out = payload;
    *out_len = plen;
    return SC_OK;
}

int sc_cookie_store_save(sc_cookie_store_t *st, const uint8_t *data, size_t len)
{
    if (!st || !st->kv || (!data && len)) return SC_ERR_ARG;
    if (len > SC_CK_MAX_PAYLOAD) return SC_ERR_TOO_BIG;
    /* Re-scan: the in-memory view may be stale if a previous save failed midway. */
    uint32_t gen = 0;
    int newest = pick(st->kv, NULL, NULL, &gen);
    int target = newest == 0 ? 1 : 0;
    uint32_t next_gen = (newest >= 0 ? gen : 0) + 1;

    uint8_t *blob = (uint8_t *)malloc(HDR_LEN + len);
    if (!blob) return SC_ERR_NOMEM;
    put32(blob, SLOT_MAGIC);
    put32(blob + 4, next_gen);
    put32(blob + 8, (uint32_t)len);
    if (len) memcpy(blob + HDR_LEN, data, len);
    put32(blob + 12, sc_crc32(sc_crc32(0, blob + 4, 8), blob + HDR_LEN, len));
    int rc = st->kv->set(st->kv->ctx, slot_key(target), blob, HDR_LEN + len);
    free(blob);
    if (rc != SC_OK) return SC_ERR_IO;

    uint8_t p[PTR_LEN];
    put32(p, PTR_MAGIC);
    put32(p + 4, (uint32_t)target);
    put32(p + 8, next_gen);
    put32(p + 12, sc_crc32(0, p, 12));
    /* The slot is already durable; a failed pointer write is harmless (load uses generations). */
    (void)st->kv->set(st->kv->ctx, SC_CK_KEY_ACTIVE, p, sizeof p);
    st->active = target;
    st->generation = next_gen;
    return SC_OK;
}

int sc_cookie_store_clear(sc_cookie_store_t *st)
{
    if (!st || !st->kv) return SC_ERR_ARG;
    int rc = SC_OK;
    const char *keys[] = {SC_CK_KEY_ACTIVE, SC_CK_KEY_A, SC_CK_KEY_B};
    for (int i = 0; i < 3; i++) {
        int r = st->kv->erase ? st->kv->erase(st->kv->ctx, keys[i]) : SC_ERR_IO;
        if (r != SC_OK && r != SC_ERR_NOT_FOUND) rc = SC_ERR_IO;
    }
    st->active = -1;
    st->generation = 0;
    return rc;
}

/* ---- session record (text) ---- */

int sc_cookie_record_encode(const sc_session_cookies_t *c, const char *device_id, char **out, size_t *out_len)
{
    if (!c || !out || !out_len) return SC_ERR_ARG;
    size_t cap = 64 + (device_id ? strlen(device_id) : 0);
    for (size_t i = 0; i < c->count; i++) cap += sizeof(SC_SESSION_COOKIE_NAME) + 8 + strlen(c->chunks[i].value);
    char *buf = (char *)malloc(cap);
    if (!buf) return SC_ERR_NOMEM;
    size_t n = 0;
    if (device_id && *device_id) n += (size_t)snprintf(buf + n, cap - n, "oai-did=%s\n", device_id);
    for (size_t i = 0; i < c->count; i++) {
        const sc_cookie_chunk_t *ch = &c->chunks[i];
        if (ch->index < 0)
            n += (size_t)snprintf(buf + n, cap - n, SC_SESSION_COOKIE_NAME "=%s\n", ch->value);
        else
            n += (size_t)snprintf(buf + n, cap - n, SC_SESSION_COOKIE_NAME ".%d=%s\n", ch->index, ch->value);
    }
    *out = buf;
    *out_len = n;
    return SC_OK;
}

int sc_cookie_record_decode(const char *data, size_t len, sc_session_cookies_t *c, char *device_id)
{
    if (!data || !c) return SC_ERR_ARG;
    sc_session_cookies_t next;
    sc_cookies_init(&next);
    if (device_id) device_id[0] = 0;
    const char *p = data, *end = data + len;
    const size_t nl = sizeof(SC_SESSION_COOKIE_NAME) - 1;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        const char *eq = memchr(p, '=', (size_t)(eol - p));
        if (eq) {
            size_t klen = (size_t)(eq - p), vlen = (size_t)(eol - eq - 1);
            if (klen == 7 && memcmp(p, "oai-did", 7) == 0) {
                if (device_id && vlen < SC_DEVICE_ID_MAX) {
                    memcpy(device_id, eq + 1, vlen);
                    device_id[vlen] = 0;
                }
            } else if (klen >= nl && memcmp(p, SC_SESSION_COOKIE_NAME, nl) == 0) {
                int index = SC_COOKIE_UNCHUNKED;
                if (klen > nl) {
                    if (p[nl] != '.' || klen == nl + 1 || klen > nl + 3) goto bad;
                    index = 0;
                    for (const char *d = p + nl + 1; d < eq; d++) {
                        if (*d < '0' || *d > '9') goto bad;
                        index = index * 10 + (*d - '0');
                    }
                }
                if (sc_cookies_set(&next, index, eq + 1, vlen) != SC_OK) goto bad;
            }
        }
        p = eol + 1;
    }
    if (next.count == 0) goto bad;
    sc_cookies_free(c);
    *c = next;
    return SC_OK;
bad:
    sc_cookies_free(&next);
    return SC_ERR_PARSE;
}

int sc_cookie_store_save_session(sc_cookie_store_t *st, const sc_session_cookies_t *c, const char *device_id)
{
    char *buf;
    size_t len;
    int rc = sc_cookie_record_encode(c, device_id, &buf, &len);
    if (rc != SC_OK) return rc;
    rc = sc_cookie_store_save(st, (const uint8_t *)buf, len);
    free(buf);
    return rc;
}

int sc_cookie_store_load_session(sc_cookie_store_t *st, sc_session_cookies_t *c, char *device_id)
{
    uint8_t *buf;
    size_t len;
    int rc = sc_cookie_store_load(st, &buf, &len);
    if (rc != SC_OK) return rc;
    rc = sc_cookie_record_decode((const char *)buf, len, c, device_id);
    free(buf);
    return rc;
}
