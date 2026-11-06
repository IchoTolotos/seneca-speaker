/*
 * multipart/form-data body for POST https://chatgpt.com/realtime/wm?dcid=0
 * with the two text fields the browser's FormData sends: "sdp" and "session".
 */
#ifndef SC_MULTIPART_H
#define SC_MULTIPART_H

#include <stddef.h>

#include "sc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC_MULTIPART_BOUNDARY_MAX 64

/** Random boundary like "----SpeakerCoreBoundary1a2b3c4d5e6f7a8b". */
void sc_multipart_make_boundary(char out[SC_MULTIPART_BOUNDARY_MAX], sc_random_fn rnd, void *rnd_ctx);

/** Writes "multipart/form-data; boundary=<b>" for the Content-Type header. Returns length or SC_ERR_TOO_BIG. */
int sc_multipart_content_type(const char *boundary, char *out, size_t cap);

/**
 * Builds the body. *out is malloc'd and NUL-terminated (caller frees);
 * *out_len excludes the NUL (use it as Content-Length).
 * Returns SC_ERR_ARG if the boundary occurs inside a field value.
 */
int sc_multipart_build(const char *boundary, const char *sdp, const char *session_json, char **out, size_t *out_len);

#ifdef __cplusplus
}
#endif
#endif
