/*
 * Minimal JWT payload inspection. Does NOT verify signatures: the device only
 * needs `exp` to schedule refreshes and `client_id` / email for status display.
 */
#ifndef SC_JWT_H
#define SC_JWT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Reads the numeric `exp` claim (unix seconds). Returns SC_OK or SC_ERR_PARSE / SC_ERR_NOT_FOUND. */
int sc_jwt_get_exp(const char *jwt, int64_t *exp_out);

/**
 * Copies a top-level string claim (e.g. "client_id") into out (NUL-terminated).
 * Returns SC_OK, SC_ERR_NOT_FOUND, SC_ERR_TOO_BIG or SC_ERR_PARSE.
 */
int sc_jwt_get_string_claim(const char *jwt, const char *claim, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
