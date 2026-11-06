#include <stdint.h>
#include <stdlib.h>

#include "sc_util.h"
#include "test.h"

static void crc32_known_vector(void)
{
    CHECK_EQ_INT(sc_crc32(0, "123456789", 9), 0xCBF43926u);
    /* chaining equals one-shot */
    uint32_t c = sc_crc32(0, "1234", 4);
    CHECK_EQ_INT(sc_crc32(c, "56789", 5), 0xCBF43926u);
    CHECK_EQ_INT(sc_crc32(0, "", 0), 0);
}

static void b64url_decodes_with_and_without_padding(void)
{
    uint8_t out[32];
    int n = sc_b64url_decode("aGVsbG8", 7, out, sizeof out);
    CHECK_EQ_INT(n, 5);
    CHECK(memcmp(out, "hello", 5) == 0);
    n = sc_b64url_decode("aGVsbG8=", 8, out, sizeof out);
    CHECK_EQ_INT(n, 5);
    /* url-safe alphabet: 0xfb 0xff -> "-_8" */
    n = sc_b64url_decode("-_8", 3, out, sizeof out);
    CHECK_EQ_INT(n, 2);
    CHECK_EQ_INT(out[0], 0xfb);
    CHECK_EQ_INT(out[1], 0xff);
    CHECK(sc_b64url_decode("a$bc", 4, out, sizeof out) < 0);
    CHECK(sc_b64url_decode("aGVsbG8", 7, out, 3) == SC_ERR_TOO_BIG);
}

static void uuid_v4_format(void)
{
    uint32_t seed = 42;
    char a[SC_UUID_STR_LEN], b[SC_UUID_STR_LEN];
    sc_uuid_v4(a, (sc_random_fn)sc_test_rand, &seed);
    sc_uuid_v4(b, (sc_random_fn)sc_test_rand, &seed);
    CHECK_EQ_INT(strlen(a), 36);
    CHECK(a[8] == '-' && a[13] == '-' && a[18] == '-' && a[23] == '-');
    CHECK(a[14] == '4');
    CHECK(strchr("89ab", a[19]) != NULL);
    CHECK(strcmp(a, b) != 0);
}

void suite_util(void)
{
    RUN(crc32_known_vector);
    RUN(b64url_decodes_with_and_without_padding);
    RUN(uuid_v4_format);
}
