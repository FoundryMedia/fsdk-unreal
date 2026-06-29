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
/* JWT signature verifier (host-provided, SERVER-side only)                   */
/* -------------------------------------------------------------------------- */

static fsdk_jwt_verify_fn g_jwt_verifier = NULL;
static void*              g_jwt_user_data = NULL;

void fsdk_set_jwt_verifier(fsdk_jwt_verify_fn verifier, void* user_data) {
    g_jwt_verifier = verifier;
    g_jwt_user_data = user_data;
}

fsdk_result fsdk_dispatch_jwt_verify(const char* kid,
                                     const char* signing_input,
                                     const unsigned char* signature,
                                     size_t signature_len) {
    if (g_jwt_verifier == NULL) {
        /* No crypto baked in - the server binding must install a verifier. Fail
         * closed: an unverifiable token is a rejected token. */
        fsdk_log(FSDK_LOG_DEBUG, "fsdk jwt: no verifier installed");
        return FSDK_NOT_IMPLEMENTED;
    }
    return g_jwt_verifier(kid, signing_input, signature, signature_len, g_jwt_user_data);
}

/* -------------------------------------------------------------------------- */
/* Secret store (host-provided keyring, optional)                             */
/* -------------------------------------------------------------------------- */

static fsdk_secret_save_fn   g_secret_save = NULL;
static fsdk_secret_load_fn   g_secret_load = NULL;
static fsdk_secret_delete_fn g_secret_delete = NULL;
static void*                 g_secret_user_data = NULL;

void fsdk_set_secret_store(fsdk_secret_save_fn save,
                           fsdk_secret_load_fn load,
                           fsdk_secret_delete_fn del,
                           void* user_data) {
    g_secret_save = save;
    g_secret_load = load;
    g_secret_delete = del;
    g_secret_user_data = user_data;
}

int fsdk_secret_save(const char* key, const char* value) {
    if (g_secret_save == NULL || key == NULL || value == NULL) {
        return -1; /* no store installed -> not persisted (session-only). */
    }
    return g_secret_save(key, value, g_secret_user_data);
}

int fsdk_secret_load(const char* key, char** out_value) {
    if (out_value != NULL) {
        *out_value = NULL;
    }
    if (g_secret_load == NULL || key == NULL || out_value == NULL) {
        return -1;
    }
    return g_secret_load(key, out_value, g_secret_user_data);
}

int fsdk_secret_delete(const char* key) {
    if (g_secret_delete == NULL || key == NULL) {
        return -1;
    }
    return g_secret_delete(key, g_secret_user_data);
}

/* -------------------------------------------------------------------------- */
/* Memory helpers                                                             */
/* -------------------------------------------------------------------------- */

void fsdk_string_free(char* str) {
    /* SDK-returned strings are allocated with malloc; free() pairs with that.
     * Safe to call with NULL (free(NULL) is a no-op). */
    free(str);
}
