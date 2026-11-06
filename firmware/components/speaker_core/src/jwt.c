#include "jwt.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "sc_platform.h"
#include "sc_util.h"

/* Decodes the payload (second segment) and parses it as JSON. Caller deletes. */
static cJSON *jwt_payload(const char *jwt, int *err)
{
    *err = SC_ERR_PARSE;
    if (!jwt) {
        *err = SC_ERR_ARG;
        return NULL;
    }
    const char *dot1 = strchr(jwt, '.');
    if (!dot1) return NULL;
    const char *start = dot1 + 1;
    const char *dot2 = strchr(start, '.');
    size_t len = dot2 ? (size_t)(dot2 - start) : strlen(start);
    if (len == 0) return NULL;
    size_t cap = len * 3 / 4 + 4;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) {
        *err = SC_ERR_NOMEM;
        return NULL;
    }
    int n = sc_b64url_decode(start, len, buf, cap - 1);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = 0;
    cJSON *root = cJSON_ParseWithLength((const char *)buf, (size_t)n);
    free(buf);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return NULL;
    }
    *err = SC_OK;
    return root;
}

int sc_jwt_get_exp(const char *jwt, int64_t *exp_out)
{
    int err;
    cJSON *root = jwt_payload(jwt, &err);
    if (!root) return err;
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "exp");
    int rc = SC_ERR_NOT_FOUND;
    if (cJSON_IsNumber(exp) && exp->valuedouble > 0) {
        if (exp_out) *exp_out = (int64_t)exp->valuedouble;
        rc = SC_OK;
    }
    cJSON_Delete(root);
    return rc;
}

int sc_jwt_get_string_claim(const char *jwt, const char *claim, char *out, size_t cap)
{
    if (!claim || !out || cap == 0) return SC_ERR_ARG;
    int err;
    cJSON *root = jwt_payload(jwt, &err);
    if (!root) return err;
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, claim);
    int rc = SC_ERR_NOT_FOUND;
    if (cJSON_IsString(item)) {
        size_t n = strlen(item->valuestring);
        if (n >= cap) {
            rc = SC_ERR_TOO_BIG;
        } else {
            memcpy(out, item->valuestring, n + 1);
            rc = SC_OK;
        }
    }
    cJSON_Delete(root);
    return rc;
}
