/* Small helpers shared by speaker_core modules: CRC32, base64url, UUIDv4. */
#ifndef SC_UTIL_H
#define SC_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "sc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/** IEEE 802.3 CRC32 (same as zlib crc32). Pass crc=0 to start; chain for more data. */
uint32_t sc_crc32(uint32_t crc, const void *data, size_t len);

/**
 * Decodes base64url (also accepts standard base64 '+' '/', and optional '=' padding).
 * Writes at most cap bytes; returns the decoded length or a negative SC_ERR_*.
 * A buffer of (len * 3 / 4 + 3) bytes is always enough.
 */
int sc_b64url_decode(const char *in, size_t len, uint8_t *out, size_t cap);

#define SC_UUID_STR_LEN 37 /* 36 chars + NUL */
/** Random RFC 4122 v4 UUID, lowercase, e.g. "1b4e28ba-2fa1-4d3b-a3f5-ef19b5a7633b". */
void sc_uuid_v4(char out[SC_UUID_STR_LEN], sc_random_fn rnd, void *rnd_ctx);

#ifdef __cplusplus
}
#endif
#endif
