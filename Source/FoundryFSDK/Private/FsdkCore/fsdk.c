/*
 * fsdk.c - version, result strings, and the logging sink.
 *
 * SCAFFOLD: complete and self-contained (no external deps).
 */
#include "fsdk_internal.h"

#include <stddef.h>
#include <stdlib.h>

/* Compile-time assembled version string "MAJOR.MINOR.PATCH". */
#define FSDK_STRINGIFY_(x) #x
#define FSDK_STRINGIFY(x) FSDK_STRINGIFY_(x)
#define FSDK_VERSION_STRING                                                   \
    FSDK_STRINGIFY(FSDK_VERSION_MAJOR) "."                                    \
    FSDK_STRINGIFY(FSDK_VERSION_MINOR) "."                                    \
    FSDK_STRINGIFY(FSDK_VERSION_PATCH)

const char* fsdk_version(void) {
    return FSDK_VERSION_STRING;
}

const char* fsdk_result_str(fsdk_result result) {
    switch (result) {
        case FSDK_OK:                   return "FSDK_OK";
        case FSDK_NOT_IMPLEMENTED:      return "FSDK_NOT_IMPLEMENTED";
        case FSDK_ERR_INVALID_ARG:      return "FSDK_ERR_INVALID_ARG";
        case FSDK_ERR_NOT_AUTHENTICATED:return "FSDK_ERR_NOT_AUTHENTICATED";
        case FSDK_ERR_UNAUTHORIZED:     return "FSDK_ERR_UNAUTHORIZED";
        case FSDK_ERR_NETWORK:          return "FSDK_ERR_NETWORK";
        case FSDK_ERR_TIMEOUT:          return "FSDK_ERR_TIMEOUT";
        case FSDK_ERR_PROTOCOL:         return "FSDK_ERR_PROTOCOL";
        case FSDK_ERR_TOKEN_INVALID:    return "FSDK_ERR_TOKEN_INVALID";
        case FSDK_ERR_TOKEN_EXPIRED:    return "FSDK_ERR_TOKEN_EXPIRED";
        case FSDK_ERR_NO_MATCH:         return "FSDK_ERR_NO_MATCH";
        case FSDK_ERR_AGONES:           return "FSDK_ERR_AGONES";
        case FSDK_ERR_INTERNAL:         return "FSDK_ERR_INTERNAL";
        default:                        return "FSDK_ERR_UNKNOWN";
    }
}

/* -------------------------------------------------------------------------- */
/* Logging                                                                    */
/* -------------------------------------------------------------------------- */

static fsdk_log_fn g_log_sink = NULL;
static void*       g_log_user_data = NULL;

void fsdk_set_log_sink(fsdk_log_fn sink, void* user_data) {
    g_log_sink = sink;
    g_log_user_data = user_data;
}

void fsdk_log(fsdk_log_level level, const char* message) {
    if (g_log_sink != NULL && message != NULL) {
        g_log_sink(level, message, g_log_user_data);
    }
}

/* -------------------------------------------------------------------------- */
/* HTTP transport (host-provided)                                             */
/* -------------------------------------------------------------------------- */

static fsdk_http_fn g_http_transport = NULL;
static void*        g_http_user_data = NULL;

void fsdk_set_http_transport(fsdk_http_fn transport, void* user_data) {
    g_http_transport = transport;
    g_http_user_data = user_data;
}

fsdk_result fsdk_dispatch_http(fsdk_http_method method,
                               const char* url,
                               const char* bearer_token,
                               const char* body_json,
                               char** out_body,
                               long* out_status) {
    if (g_http_transport == NULL) {
        /* No network stack baked in - the host must install one. Fail closed. */
        fsdk_log(FSDK_LOG_DEBUG, "fsdk http: no transport installed");
        return FSDK_NOT_IMPLEMENTED;
    }
    return g_http_transport(method, url, bearer_token, body_json,
                            out_body, out_status, g_http_user_data);
}

/* -------------------------------------------------------------------------- */
/* Memory helpers                                                             */
/* -------------------------------------------------------------------------- */

void fsdk_string_free(char* str) {
    /* SDK-returned strings are allocated with malloc; free() pairs with that.
     * Safe to call with NULL (free(NULL) is a no-op). */
    free(str);
}
