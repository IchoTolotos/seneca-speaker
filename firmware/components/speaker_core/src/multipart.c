#include "multipart.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sc_multipart_make_boundary(char out[SC_MULTIPART_BOUNDARY_MAX], sc_random_fn rnd, void *rnd_ctx)
{
    snprintf(out, SC_MULTIPART_BOUNDARY_MAX, "----SpeakerCoreBoundary%08x%08x", (unsigned)rnd(rnd_ctx),
             (unsigned)rnd(rnd_ctx));
}

int sc_multipart_content_type(const char *boundary, char *out, size_t cap)
{
    if (!boundary || !out) return SC_ERR_ARG;
    int n = snprintf(out, cap, "multipart/form-data; boundary=%s", boundary);
    if (n < 0 || (size_t)n >= cap) return SC_ERR_TOO_BIG;
    return n;
}

int sc_multipart_build(const char *boundary, const char *sdp, const char *session_json, char **out, size_t *out_len)
{
    if (!boundary || !*boundary || !sdp || !session_json || !out || !out_len) return SC_ERR_ARG;
    if (strstr(sdp, boundary) || strstr(session_json, boundary)) return SC_ERR_ARG;
    static const char *fmt = "--%s\r\n"
                             "Content-Disposition: form-data; name=\"sdp\"\r\n"
                             "\r\n"
                             "%s\r\n"
                             "--%s\r\n"
                             "Content-Disposition: form-data; name=\"session\"\r\n"
                             "\r\n"
                             "%s\r\n"
                             "--%s--\r\n";
    size_t cap = strlen(fmt) + 3 * strlen(boundary) + strlen(sdp) + strlen(session_json) + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) return SC_ERR_NOMEM;
    int n = snprintf(buf, cap, fmt, boundary, sdp, boundary, session_json, boundary);
    if (n < 0 || (size_t)n >= cap) {
        free(buf);
        return SC_ERR_TOO_BIG;
    }
    *out = buf;
    *out_len = (size_t)n;
    return SC_OK;
}
