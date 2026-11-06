#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "cookie_store.h"
#include "test.h"

/* ---- fake NVS with power-loss injection ---- */

#define MAXK 8
typedef struct {
    char key[16];
    uint8_t *data;
    size_t len;
    bool used;
} entry_t;

typedef struct {
    entry_t e[MAXK];
    int sets;            /* number of set() calls so far */
    int crash_at_set;    /* -1 = never; else the set() index that loses power */
    size_t torn_bytes;   /* bytes of the crashing write that reach flash (SIZE_MAX = key untouched) */
    bool dead;           /* powered off: everything fails until reboot */
} fake_kv_t;

static entry_t *find(fake_kv_t *kv, const char *key)
{
    for (int i = 0; i < MAXK; i++)
        if (kv->e[i].used && !strcmp(kv->e[i].key, key)) return &kv->e[i];
    return NULL;
}

static int kv_get(void *ctx, const char *key, void *buf, size_t cap, size_t *out_len)
{
    fake_kv_t *kv = ctx;
    if (kv->dead) return SC_ERR_IO;
    entry_t *e = find(kv, key);
    if (!e) return SC_ERR_NOT_FOUND;
    *out_len = e->len;
    if (!buf) return SC_OK;
    if (cap < e->len) return SC_ERR_TOO_BIG;
    memcpy(buf, e->data, e->len);
    return SC_OK;
}

static void store(fake_kv_t *kv, const char *key, const void *data, size_t len)
{
    entry_t *e = find(kv, key);
    if (!e) {
        for (int i = 0; i < MAXK && !e; i++)
            if (!kv->e[i].used) e = &kv->e[i];
        e->used = true;
        snprintf(e->key, sizeof e->key, "%s", key);
    }
    free(e->data);
    e->data = malloc(len ? len : 1);
    memcpy(e->data, data, len);
    e->len = len;
}

static int kv_set(void *ctx, const char *key, const void *data, size_t len)
{
    fake_kv_t *kv = ctx;
    if (kv->dead) return SC_ERR_IO;
    if (kv->sets++ == kv->crash_at_set) {
        if (kv->torn_bytes != SIZE_MAX) store(kv, key, data, kv->torn_bytes < len ? kv->torn_bytes : len);
        kv->dead = true;
        return SC_ERR_IO;
    }
    store(kv, key, data, len);
    return SC_OK;
}

static int kv_erase(void *ctx, const char *key)
{
    fake_kv_t *kv = ctx;
    entry_t *e = find(kv, key);
    if (!e) return SC_ERR_NOT_FOUND;
    free(e->data);
    memset(e, 0, sizeof *e);
    return SC_OK;
}

static void kv_reset(fake_kv_t *kv)
{
    for (int i = 0; i < MAXK; i++) free(kv->e[i].data);
    memset(kv, 0, sizeof *kv);
    kv->crash_at_set = -1;
}

static void kv_reboot(fake_kv_t *kv)
{
    kv->dead = false;
    kv->crash_at_set = -1;
    kv->sets = 0;
}

static sc_kv_t make_kv(fake_kv_t *f)
{
    sc_kv_t kv = {f, kv_get, kv_set, kv_erase};
    return kv;
}

static char *load_str(const sc_kv_t *kv)
{
    sc_cookie_store_t st;
    sc_cookie_store_init(&st, kv);
    uint8_t *data = NULL;
    size_t len = 0;
    if (sc_cookie_store_load(&st, &data, &len) != SC_OK) return NULL;
    return (char *)data; /* NUL-terminated by load */
}

/* ---- tests ---- */

static void empty_store_reports_not_found(void)
{
    fake_kv_t f;
    memset(&f, 0, sizeof f);
    kv_reset(&f);
    sc_kv_t kv = make_kv(&f);
    sc_cookie_store_t st;
    CHECK_EQ_INT(sc_cookie_store_init(&st, &kv), SC_OK);
    CHECK_EQ_INT(st.active, -1);
    uint8_t *d;
    size_t l;
    CHECK_EQ_INT(sc_cookie_store_load(&st, &d, &l), SC_ERR_NOT_FOUND);
    kv_reset(&f);
}

static void alternates_slots_and_returns_newest(void)
{
    fake_kv_t f;
    memset(&f, 0, sizeof f);
    kv_reset(&f);
    sc_kv_t kv = make_kv(&f);
    sc_cookie_store_t st;
    sc_cookie_store_init(&st, &kv);
    const char *vals[] = {"gen-one", "gen-two", "gen-three"};
    for (int i = 0; i < 3; i++) {
        CHECK_EQ_INT(sc_cookie_store_save(&st, (const uint8_t *)vals[i], strlen(vals[i])), SC_OK);
        CHECK_EQ_INT(st.generation, (uint32_t)(i + 1));
        CHECK_EQ_INT(st.active, i % 2);
        char *s = load_str(&kv);
        CHECK_STR(s, vals[i]);
        free(s);
    }
    /* Older slot still holds gen-two (the fallback copy). */
    entry_t *b = find(&f, SC_CK_KEY_B);
    CHECK(b && b->len == 16 + 7 && memcmp(b->data + 16, "gen-two", 7) == 0);
    CHECK_EQ_INT(sc_cookie_store_clear(&st), SC_OK);
    char *s = load_str(&kv);
    CHECK(s == NULL);
    kv_reset(&f);
}

/*
 * Power-loss simulation: with two generations stored, crash the third save at
 * every set() call and at many tear points, reboot, and require that the store
 * yields either the previous or the new value (never garbage, never nothing),
 * and that the next save works and wins.
 */
static void survives_power_loss_at_every_point(void)
{
    const char *prev = "__Secure-next-auth.session-token.0=PREVIOUS-VALUE-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n";
    const char *next = "__Secure-next-auth.session-token.0=NEW-ROTATED-VALUE-yyyyyyyyyyyyyyyyyyyyyyyyyyy\n";
    size_t tears[] = {SIZE_MAX, 0, 1, 4, 8, 12, 15, 16, 17, 40, 60, 1000};
    int scenarios = 0, saw_prev = 0, saw_next = 0;
    for (int crash_set = 0; crash_set < 2; crash_set++) {
        for (size_t t = 0; t < sizeof tears / sizeof tears[0]; t++) {
            for (int base_gens = 1; base_gens <= 2; base_gens++) { /* newest in A or in B */
                fake_kv_t f;
                memset(&f, 0, sizeof f);
                kv_reset(&f);
                sc_kv_t kv = make_kv(&f);
                sc_cookie_store_t st;
                sc_cookie_store_init(&st, &kv);
                for (int g = 0; g < base_gens - 1; g++) sc_cookie_store_save(&st, (const uint8_t *)"older", 5);
                CHECK_EQ_INT(sc_cookie_store_save(&st, (const uint8_t *)prev, strlen(prev)), SC_OK);

                f.sets = 0;
                f.crash_at_set = crash_set; /* 0 = slot write, 1 = pointer write */
                f.torn_bytes = tears[t];
                int rc = sc_cookie_store_save(&st, (const uint8_t *)next, strlen(next));
                kv_reboot(&f);

                char *s = load_str(&kv);
                CHECK(s != NULL);
                bool is_prev = s && !strcmp(s, prev), is_next = s && !strcmp(s, next);
                CHECK(is_prev || is_next);
                if (crash_set == 0) {
                    CHECK(rc != SC_OK);
                    /* slot write torn: only a complete write may surface the new value */
                    size_t full = 16 + strlen(next);
                    if (tears[t] == SIZE_MAX || tears[t] < full) CHECK(is_prev);
                } else {
                    CHECK(is_next); /* slot was durable before the pointer flip */
                }
                saw_prev += is_prev;
                saw_next += is_next;
                free(s);

                /* After reboot the next rotation must persist and win. */
                sc_cookie_store_init(&st, &kv);
                CHECK_EQ_INT(sc_cookie_store_save(&st, (const uint8_t *)"after-reboot", 12), SC_OK);
                s = load_str(&kv);
                CHECK_STR(s, "after-reboot");
                free(s);
                kv_reset(&f);
                scenarios++;
            }
        }
    }
    CHECK(scenarios == 48);
    CHECK(saw_prev > 0 && saw_next > 0);
}

static void corrupt_newest_falls_back_to_older(void)
{
    fake_kv_t f;
    memset(&f, 0, sizeof f);
    kv_reset(&f);
    sc_kv_t kv = make_kv(&f);
    sc_cookie_store_t st;
    sc_cookie_store_init(&st, &kv);
    sc_cookie_store_save(&st, (const uint8_t *)"first", 5);
    sc_cookie_store_save(&st, (const uint8_t *)"second", 6); /* slot B */
    find(&f, SC_CK_KEY_B)->data[18] ^= 0x01;                   /* bit rot in payload */
    char *s = load_str(&kv);
    CHECK_STR(s, "first");
    free(s);
    /* Next save must not overwrite the only valid slot (A). */
    sc_cookie_store_init(&st, &kv);
    sc_cookie_store_save(&st, (const uint8_t *)"third", 5);
    CHECK_EQ_INT(st.active, 1);
    s = load_str(&kv);
    CHECK_STR(s, "third");
    free(s);
    kv_reset(&f);
}

static void session_record_roundtrip(void)
{
    fake_kv_t f;
    memset(&f, 0, sizeof f);
    kv_reset(&f);
    sc_kv_t kv = make_kv(&f);
    sc_cookie_store_t st;
    sc_cookie_store_init(&st, &kv);
    sc_session_cookies_t c, back;
    sc_cookies_init(&c);
    sc_cookies_init(&back);
    sc_cookies_set(&c, 0, "AAA.bbb", 7);
    sc_cookies_set(&c, 1, "CCC", 3);
    CHECK_EQ_INT(sc_cookie_store_save_session(&st, &c, "11111111-2222-4333-8444-555555555555"), SC_OK);
    char did[SC_DEVICE_ID_MAX];
    CHECK_EQ_INT(sc_cookie_store_load_session(&st, &back, did), SC_OK);
    CHECK(sc_cookies_equal(&c, &back));
    CHECK_STR(did, "11111111-2222-4333-8444-555555555555");
    /* garbage record is rejected */
    CHECK_EQ_INT(sc_cookie_record_decode("hello\nworld\n", 12, &back, did), SC_ERR_PARSE);
    CHECK_EQ_INT(sc_cookie_record_decode("__Secure-next-auth.session-token.x=1\n", 37, &back, did), SC_ERR_PARSE);
    CHECK(sc_cookies_equal(&c, &back)); /* untouched on error */
    sc_cookies_free(&c);
    sc_cookies_free(&back);
    kv_reset(&f);
}

void suite_cookie_store(void)
{
    RUN(empty_store_reports_not_found);
    RUN(alternates_slots_and_returns_newest);
    RUN(survives_power_loss_at_every_point);
    RUN(corrupt_newest_falls_back_to_older);
    RUN(session_record_roundtrip);
}
