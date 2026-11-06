#include "set_cookie.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "sc_platform.h"

#define NAME SC_SESSION_COOKIE_NAME
#define NAME_LEN (sizeof(NAME) - 1)

void sc_cookies_init(sc_session_cookies_t *c)
{
    memset(c, 0, sizeof(*c));
}

void sc_cookies_free(sc_session_cookies_t *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->count; i++) free(c->chunks[i].value);
    memset(c, 0, sizeof(*c));
}

static bool valid_value(const char *v, size_t len)
{
    if (len == 0 || len > SC_COOKIE_MAX_VALUE) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)v[i];
        if (ch <= 0x20 || ch == ';' || ch == ',' || ch == '"' || ch == '\\' || ch >= 0x7f) return false;
    }
    return true;
}

int sc_cookies_set(sc_session_cookies_t *c, int index, const char *value, size_t len)
{
    if (!c || !value || index < SC_COOKIE_UNCHUNKED || index > 99) return SC_ERR_ARG;
    if (!valid_value(value, len)) return SC_ERR_ARG;
    char *copy = (char *)malloc(len + 1);
    if (!copy) return SC_ERR_NOMEM;
    memcpy(copy, value, len);
    copy[len] = 0;
    size_t pos = 0;
    while (pos < c->count && c->chunks[pos].index < index) pos++;
    if (pos < c->count && c->chunks[pos].index == index) {
        free(c->chunks[pos].value);
        c->chunks[pos].value = copy;
        return SC_OK;
    }
    if (c->count >= SC_COOKIE_MAX_CHUNKS) {
        free(copy);
        return SC_ERR_TOO_BIG;
    }
    memmove(&c->chunks[pos + 1], &c->chunks[pos], (c->count - pos) * sizeof(c->chunks[0]));
    c->chunks[pos].index = index;
    c->chunks[pos].value = copy;
    c->count++;
    return SC_OK;
}

const char *sc_cookies_get(const sc_session_cookies_t *c, int index)
{
    for (size_t i = 0; c && i < c->count; i++)
        if (c->chunks[i].index == index) return c->chunks[i].value;
    return NULL;
}

bool sc_cookies_equal(const sc_session_cookies_t *a, const sc_session_cookies_t *b)
{
    if (a->count != b->count) return false;
    for (size_t i = 0; i < a->count; i++) {
        if (a->chunks[i].index != b->chunks[i].index) return false;
        if (strcmp(a->chunks[i].value, b->chunks[i].value) != 0) return false;
    }
    return true;
}

int sc_cookies_copy(sc_session_cookies_t *dst, const sc_session_cookies_t *src)
{
    sc_session_cookies_t tmp;
    sc_cookies_init(&tmp);
    for (size_t i = 0; i < src->count; i++) {
        int rc = sc_cookies_set(&tmp, src->chunks[i].index, src->chunks[i].value, strlen(src->chunks[i].value));
        if (rc != SC_OK) {
            sc_cookies_free(&tmp);
            return rc;
        }
    }
    sc_cookies_free(dst);
    *dst = tmp;
    return SC_OK;
}

int sc_set_cookie_feed(sc_session_cookies_t *acc, const char *hv)
{
    if (!acc || !hv) return SC_ERR_ARG;
    int found = 0;
    const char *p = hv;
    while ((p = strstr(p, NAME)) != NULL) {
        /* Must start the header or follow a ',' (joined headers), with optional spaces. */
        const char *q = p;
        while (q > hv && (q[-1] == ' ' || q[-1] == '\t')) q--;
        bool at_start = (q == hv) || (q[-1] == ',');
        const char *s = p + NAME_LEN;
        p = s;
        if (!at_start) continue;
        int index = SC_COOKIE_UNCHUNKED;
        if (*s == '.') {
            s++;
            if (!isdigit((unsigned char)*s)) continue;
            index = 0;
            while (isdigit((unsigned char)*s)) {
                index = index * 10 + (*s - '0');
                if (index > 99) break;
                s++;
            }
            if (index > 99) continue;
        }
        if (*s != '=') continue;
        s++;
        const char *e = s;
        while (*e && *e != ';' && *e != ',') e++;
        /* trim trailing whitespace */
        const char *t = e;
        while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
        p = e;
        if (t == s) continue; /* deletion (empty value) */
        int rc = sc_cookies_set(acc, index, s, (size_t)(t - s));
        if (rc == SC_ERR_NOMEM) return rc;
        if (rc == SC_OK) found++;
    }
    return found;
}

int sc_cookies_apply_rotation(sc_session_cookies_t *current, const sc_session_cookies_t *incoming)
{
    if (!current || !incoming) return SC_ERR_ARG;
    sc_session_cookies_t next;
    sc_cookies_init(&next);
    bool any_chunk = false;
    for (size_t i = 0; i < incoming->count; i++)
        if (incoming->chunks[i].index >= 0) any_chunk = true;
    for (size_t i = 0; i < incoming->count; i++) {
        const sc_cookie_chunk_t *ch = &incoming->chunks[i];
        if (any_chunk && ch->index < 0) continue;
        int rc = sc_cookies_set(&next, ch->index, ch->value, strlen(ch->value));
        if (rc != SC_OK) {
            sc_cookies_free(&next);
            return rc;
        }
    }
    if (next.count == 0) return 0;
    if (sc_cookies_equal(current, &next)) {
        sc_cookies_free(&next);
        return 0;
    }
    sc_cookies_free(current);
    *current = next;
    return 1;
}

static size_t chunk_name_len(int index)
{
    if (index < 0) return NAME_LEN;
    return NAME_LEN + 1 + (index >= 10 ? 2 : 1);
}

size_t sc_cookies_header_len(const sc_session_cookies_t *c, const char *device_id)
{
    size_t n = 0;
    for (size_t i = 0; i < c->count; i++) {
        if (n) n += 2;
        n += chunk_name_len(c->chunks[i].index) + 1 + strlen(c->chunks[i].value);
    }
    if (device_id && *device_id) {
        if (n) n += 2;
        n += strlen("oai-did=") + strlen(device_id);
    }
    return n;
}

int sc_cookies_build_header(const sc_session_cookies_t *c, const char *device_id, char *out, size_t cap)
{
    if (!c || !out) return SC_ERR_ARG;
    size_t need = sc_cookies_header_len(c, device_id);
    if (need + 1 > cap) return SC_ERR_TOO_BIG;
    size_t n = 0;
    for (size_t i = 0; i < c->count; i++) {
        const sc_cookie_chunk_t *ch = &c->chunks[i];
        if (ch->index < 0)
            n += (size_t)snprintf(out + n, cap - n, "%s" NAME "=%s", n ? "; " : "", ch->value);
        else
            n += (size_t)snprintf(out + n, cap - n, "%s" NAME ".%d=%s", n ? "; " : "", ch->index, ch->value);
    }
    if (device_id && *device_id) n += (size_t)snprintf(out + n, cap - n, "%soai-did=%s", n ? "; " : "", device_id);
    return (int)n;
}

static void trim(const char **s, size_t *len)
{
    while (*len && isspace((unsigned char)(*s)[0])) {
        (*s)++;
        (*len)--;
    }
    while (*len && isspace((unsigned char)(*s)[*len - 1])) (*len)--;
}

int sc_cookies_from_setup(sc_session_cookies_t *c, const char *chunk0, const char *chunk1)
{
    if (!c || !chunk0) return SC_ERR_ARG;
    sc_session_cookies_t next;
    sc_cookies_init(&next);
    const char *a = chunk0;
    size_t alen = strlen(a);
    trim(&a, &alen);
    const char *b = chunk1 ? chunk1 : "";
    size_t blen = strlen(b);
    trim(&b, &blen);
    int rc = SC_OK;
    if (blen == 0 && alen && a[0] == '{') {
        cJSON *root = cJSON_ParseWithLength(a, alen);
        if (!cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return SC_ERR_PARSE;
        }
        const cJSON *it;
        cJSON_ArrayForEach(it, root)
        {
            if (!cJSON_IsString(it) || strncmp(it->string, NAME, NAME_LEN) != 0) continue;
            const char *suffix = it->string + NAME_LEN;
            int index = SC_COOKIE_UNCHUNKED;
            if (*suffix == '.') {
                char *end;
                long v = strtol(suffix + 1, &end, 10);
                if (*end || end == suffix + 1 || v < 0 || v > 99) continue;
                index = (int)v;
            } else if (*suffix) {
                continue;
            }
            rc = sc_cookies_set(&next, index, it->valuestring, strlen(it->valuestring));
            if (rc != SC_OK) break;
        }
        cJSON_Delete(root);
        if (rc == SC_OK) {
            /* Prefer chunks when both forms were pasted. */
            sc_session_cookies_t only;
            sc_cookies_init(&only);
            rc = sc_cookies_apply_rotation(&only, &next) < 0 ? SC_ERR_NOMEM : SC_OK;
            sc_cookies_free(&next);
            next = only;
        }
    } else if (blen == 0) {
        rc = sc_cookies_set(&next, SC_COOKIE_UNCHUNKED, a, alen);
    } else {
        rc = sc_cookies_set(&next, 0, a, alen);
        if (rc == SC_OK) rc = sc_cookies_set(&next, 1, b, blen);
    }
    if (rc == SC_OK && next.count == 0) rc = SC_ERR_ARG;
    if (rc != SC_OK) {
        sc_cookies_free(&next);
        return rc;
    }
    sc_cookies_free(c);
    *c = next;
    return SC_OK;
}
