/*
 * transport.c - HTTP transport abstraction.
 *
 * The core never speaks HTTP directly at call sites; everything funnels through
 * fsdk_http_request, which builds the full URL (base_url + path) and dispatches
 * it to the HOST-INSTALLED transport (fsdk_set_http_transport). The concrete
 * client - an engine HTTP stack, libcurl, or a future relay-aware transport - is
 * a single swappable seam, and no network stack is ever baked into the core.
 *
 * With no transport installed the dispatch fails closed (FSDK_NOT_IMPLEMENTED);
 * the core never guesses. The host (e.g. the Unreal binding) registers a callback
 * backed by its own TLS HTTP module at startup.
 */
#include "fsdk_internal.h"

#include <stdlib.h>
#include <string.h>

fsdk_result fsdk_http_request(const char* base_url,
                              fsdk_http_method method,
                              const char* path,
                              const char* bearer_token,
                              const char* body_json,
                              char** out_body,
                              long* out_status) {
    if (base_url == NULL || path == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (out_body != NULL) {
        *out_body = NULL;
    }
    if (out_status != NULL) {
        *out_status = 0;
    }

    /* Build base_url + path. Drop any trailing '/' on base so a base that ends in
     * '/' and a path that starts with '/' don't produce a doubled separator. */
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') {
        base_len--;
    }
    size_t path_len = strlen(path);
    char* url = (char*)malloc(base_len + path_len + 1);
    if (url == NULL) {
        return FSDK_ERR_INTERNAL;
    }
    memcpy(url, base_url, base_len);
    memcpy(url + base_len, path, path_len + 1); /* + NUL; path begins with '/' */

    fsdk_result result = fsdk_dispatch_http(method, url, bearer_token, body_json,
                                            out_body, out_status);
    free(url);
    return result;
}
