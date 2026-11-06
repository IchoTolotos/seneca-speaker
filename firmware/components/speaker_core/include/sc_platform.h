/*
 * speaker_core platform interfaces.
 *
 * The core never includes ESP-IDF headers. Everything platform-specific
 * (time, randomness, persistent storage, logging) is injected through the
 * small structs below. On ESP-IDF, back them with esp_timer_get_time(),
 * esp_random(), NVS (nvs_get_blob/nvs_set_blob + nvs_commit) and ESP_LOGx.
 * On the host, tests supply deterministic fakes.
 */
#ifndef SC_PLATFORM_H
#define SC_PLATFORM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Generic return codes used across speaker_core. */
#define SC_OK 0
#define SC_ERR_ARG (-1)
#define SC_ERR_NOMEM (-2)
#define SC_ERR_PARSE (-3)
#define SC_ERR_NOT_FOUND (-4)
#define SC_ERR_IO (-5)
#define SC_ERR_CORRUPT (-6)
#define SC_ERR_TOO_BIG (-7)

/** Monotonic milliseconds since boot. */
typedef uint64_t (*sc_clock_ms_fn)(void *ctx);

/** Uniform random 32-bit value (esp_random() on device). */
typedef uint32_t (*sc_random_fn)(void *ctx);

typedef enum { SC_LOG_ERROR = 1, SC_LOG_WARN, SC_LOG_INFO, SC_LOG_DEBUG } sc_log_level_t;
typedef void (*sc_log_fn)(void *ctx, sc_log_level_t level, const char *msg);

/**
 * Key/value persistence (NVS on device). Keys are <= 15 chars (NVS limit).
 *
 * get: if buf is NULL, only report the stored length in *out_len.
 *      Returns SC_OK, SC_ERR_NOT_FOUND, SC_ERR_TOO_BIG (cap too small) or SC_ERR_IO.
 * set: must be durable when it returns SC_OK (call nvs_commit inside).
 *      A power loss during set may leave the key absent, old or torn;
 *      cookie_store tolerates all three.
 * erase: SC_OK or SC_ERR_NOT_FOUND are both fine.
 */
typedef struct sc_kv {
    void *ctx;
    int (*get)(void *ctx, const char *key, void *buf, size_t cap, size_t *out_len);
    int (*set)(void *ctx, const char *key, const void *data, size_t len);
    int (*erase)(void *ctx, const char *key);
} sc_kv_t;

/** Bundle handed to modules that need more than one service. */
typedef struct sc_platform {
    void *ctx;
    sc_clock_ms_fn now_ms;
    sc_random_fn random_u32;
    sc_log_fn log; /* optional */
} sc_platform_t;

#ifdef __cplusplus
}
#endif
#endif /* SC_PLATFORM_H */
