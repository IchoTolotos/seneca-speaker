#include <stdint.h>
#include <stdlib.h>

#include "jwt.h"
#include "sc_platform.h"
#include "test.h"

static void b64url(const char *in, char *out)
{
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t len = strlen(in), o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)(uint8_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)(uint8_t)in[i + 1] << 8;
        if (i + 2 < len) v |= (uint8_t)in[i + 2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = T[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = T[v & 63];
    }
    out[o] = 0;
}

static char *make_jwt(const char *payload)
{
    char *jwt = malloc(4096);
    char h[128], p[3000];
    b64url("{\"alg\":\"RS256\",\"kid\":\"19344e65-bbc9-44d1-a9d0-f957b079bd0e\",\"typ\":\"JWT\"}", h);
    b64url(payload, p);
    snprintf(jwt, 4096, "%s.%s.c2lnbmF0dXJlLW5vdC1jaGVja2Vk", h, p);
    return jwt;
}

static void exp_from_realistic_token(void)
{
    /* Shaped like a ChatGPT web access token (claims trimmed). Payload length forces '-'/'_' chars too. */
    char *jwt = make_jwt("{\"aud\":[\"https://api.openai.com/v1\"],\"client_id\":\"app_X8zY6vW2pQ9tR3dE7nK1jL5gH\","
                         "\"exp\":1762300000,\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"a1b2c3?>?>\","
                         "\"chatgpt_plan_type\":\"pro\"},\"https://api.openai.com/profile\":{\"email\":\"user@example.com\","
                         "\"email_verified\":true},\"iat\":1761652000,\"iss\":\"https://auth.openai.com\","
                         "\"scp\":[\"openid\",\"email\",\"profile\",\"offline_access\",\"model.request\"],"
                         "\"session_id\":\"authsess_ZZZ\",\"sub\":\"google-oauth2|1234567890\"}");
    int64_t exp = 0;
    CHECK_EQ_INT(sc_jwt_get_exp(jwt, &exp), SC_OK);
    CHECK_EQ_INT(exp, 1762300000);
    char cid[64];
    CHECK_EQ_INT(sc_jwt_get_string_claim(jwt, "client_id", cid, sizeof cid), SC_OK);
    CHECK_STR(cid, "app_X8zY6vW2pQ9tR3dE7nK1jL5gH");
    char small[4];
    CHECK_EQ_INT(sc_jwt_get_string_claim(jwt, "client_id", small, sizeof small), SC_ERR_TOO_BIG);
    CHECK_EQ_INT(sc_jwt_get_string_claim(jwt, "nope", cid, sizeof cid), SC_ERR_NOT_FOUND);
    free(jwt);
}

static void exp_missing_or_garbage(void)
{
    int64_t exp = 0;
    char *jwt = make_jwt("{\"sub\":\"x\"}");
    CHECK_EQ_INT(sc_jwt_get_exp(jwt, &exp), SC_ERR_NOT_FOUND);
    free(jwt);
    jwt = make_jwt("{\"exp\":\"1762300000\"}"); /* string, not number */
    CHECK_EQ_INT(sc_jwt_get_exp(jwt, &exp), SC_ERR_NOT_FOUND);
    free(jwt);
    CHECK_EQ_INT(sc_jwt_get_exp("not-a-jwt", &exp), SC_ERR_PARSE);
    CHECK_EQ_INT(sc_jwt_get_exp("a.!!!.c", &exp), SC_ERR_PARSE);
    CHECK_EQ_INT(sc_jwt_get_exp("a..c", &exp), SC_ERR_PARSE);
    CHECK_EQ_INT(sc_jwt_get_exp(NULL, &exp), SC_ERR_ARG);
    /* two-segment token (no signature) still works */
    char p[64], tok[96];
    b64url("{\"exp\":42}", p);
    snprintf(tok, sizeof tok, "e30.%s", p);
    CHECK_EQ_INT(sc_jwt_get_exp(tok, &exp), SC_OK);
    CHECK_EQ_INT(exp, 42);
}

void suite_jwt(void)
{
    RUN(exp_from_realistic_token);
    RUN(exp_missing_or_garbage);
}
