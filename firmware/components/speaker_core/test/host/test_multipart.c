#include <stdint.h>
#include <stdlib.h>

#include "multipart.h"
#include "test.h"

static void builds_formdata_body(void)
{
    const char *sdp = "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n";
    const char *session = "{\"voice\":\"juniper\"}";
    char *body;
    size_t len;
    CHECK_EQ_INT(sc_multipart_build("XyZ", sdp, session, &body, &len), SC_OK);
    const char *expected = "--XyZ\r\nContent-Disposition: form-data; name=\"sdp\"\r\n\r\n"
                           "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n\r\n"
                           "--XyZ\r\nContent-Disposition: form-data; name=\"session\"\r\n\r\n"
                           "{\"voice\":\"juniper\"}\r\n--XyZ--\r\n";
    CHECK_STR(body, expected);
    CHECK_EQ_INT(len, strlen(expected));
    free(body);
}

static void boundary_and_content_type(void)
{
    uint32_t seed = 7;
    char b[SC_MULTIPART_BOUNDARY_MAX], ct[128];
    sc_multipart_make_boundary(b, (sc_random_fn)sc_test_rand, &seed);
    CHECK(strncmp(b, "----SpeakerCoreBoundary", 23) == 0);
    CHECK_EQ_INT(strlen(b), 23 + 16);
    int n = sc_multipart_content_type(b, ct, sizeof ct);
    CHECK(n > 0);
    CHECK(strncmp(ct, "multipart/form-data; boundary=----SpeakerCoreBoundary", 53) == 0);
    CHECK_EQ_INT(sc_multipart_content_type(b, ct, 10), SC_ERR_TOO_BIG);
}

static void rejects_boundary_collision(void)
{
    char *body = NULL;
    size_t len;
    CHECK_EQ_INT(sc_multipart_build("abc", "v=0 abc", "{}", &body, &len), SC_ERR_ARG);
    CHECK(body == NULL);
}

void suite_multipart(void)
{
    RUN(builds_formdata_body);
    RUN(boundary_and_content_type);
    RUN(rejects_boundary_collision);
}
