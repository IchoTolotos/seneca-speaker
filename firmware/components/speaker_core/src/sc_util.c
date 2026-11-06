#include "sc_util.h"

#include <stdio.h>

uint32_t sc_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

int sc_b64url_decode(const char *in, size_t len, uint8_t *out, size_t cap)
{
    if (!in || !out) return SC_ERR_ARG;
    while (len > 0 && in[len - 1] == '=') len--;
    if (len % 4 == 1) return SC_ERR_PARSE;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64_value(in[i]);
        if (v < 0) return SC_ERR_PARSE;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return SC_ERR_TOO_BIG;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)n;
}

void sc_uuid_v4(char out[SC_UUID_STR_LEN], sc_random_fn rnd, void *rnd_ctx)
{
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = rnd(rnd_ctx);
        b[i] = (uint8_t)r;
        b[i + 1] = (uint8_t)(r >> 8);
        b[i + 2] = (uint8_t)(r >> 16);
        b[i + 3] = (uint8_t)(r >> 24);
    }
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    snprintf(out, SC_UUID_STR_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13],
             b[14], b[15]);
}
