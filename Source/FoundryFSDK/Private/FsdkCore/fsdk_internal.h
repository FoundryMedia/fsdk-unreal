/*
 * fsdk_internal.h - internal-only declarations shared across the core .c files.
 *
 * NOT installed / NOT part of the public ABI. Engine bindings must include only
 * <foundry/fsdk.h>. This header defines the opaque handle structs and the
 * internal logging + transport helpers.
 */
#ifndef FOUNDRY_FSDK_INTERNAL_H
#define FOUNDRY_FSDK_INTERNAL_H

#include "foundry/fsdk.h"

#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Shared small helpers (header-inline; one source instead of per-.c copies).  */
/* Identical logic previously duplicated across client.c/token.c/server.c.     */
/* -------------------------------------------------------------------------- */

/* Heap-duplicate a NUL-terminated string; NULL on NULL input or allocation failure. */
static inline char* fsdk_strdup(const char* s) {
    if (s == NULL) {
        return NULL;
    }
    size_t n = strlen(s) + 1;
    char* copy = (char*)malloc(n);
    if (copy != NULL) {
        memcpy(copy, s, n);
    }
    return copy;
}

/* Bounded copy into a fixed buffer (always NUL-terminates; NULL-safe; no CRT _s). */
static inline void copy_bounded(char* dst, size_t dst_sz, const char* src) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    size_t i = 0;
    for (; src != NULL && src[i] != '\0' && i + 1 < dst_sz; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Minimal flat-JSON readers (NOT a general parser): read only the small, well-known
 * fid JsonApiResponse / JWT-claim shapes. A production core links a real JSON
 * library; this stays zero-dependency. */

/* Pointer to the value just after `"key":` (skipping whitespace), or NULL. Quoted key only. */
static inline const char* json_value_after(const char* from, const char* key) {
    if (from == NULL || key == NULL) {
        return NULL;
    }
    size_t klen = strlen(key);
    const char* p = from;
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char* q = p + 1 + klen + 1; /* past the key's closing quote */
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
                q++;
            }
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') {
                    q++;
                }
                return q;
            }
        }
        p++;
    }
    return NULL;
}

/* Extract a string field's value into out (bounded). Returns 1 on success. */
static inline int json_extract_string(const char* body, const char* key,
                                      char* out, size_t out_sz) {
    const char* v = json_value_after(body, key);
    if (out_sz > 0) {
        out[0] = '\0';
    }
    if (v == NULL || *v != '"') {
        return 0;
    }
    v++; /* opening quote of the value */
    size_t i = 0;
    while (*v != '\0' && *v != '"') {
        char c = *v;
        if (c == '\\' && v[1] != '\0') {
            v++;
            c = *v; /* copy the escaped char literally (ids/states have none) */
        }
        if (i + 1 < out_sz) {
            out[i++] = c;
        }
        v++;
    }
    if (i < out_sz) {
        out[i] = '\0';
    }
    return (*v == '"');
}

/* -------------------------------------------------------------------------- */
/* Opaque handle definitions                                                  */
/* -------------------------------------------------------------------------- */

struct fsdk_client {
    char* base_url;       /* Copied at create.                                 */
    char* player_token;   /* In-memory only; PLAYER's FID token. Never persisted. */
    int   authenticated;  /* 0 = not authenticated, 1 = authenticated.         */
};

struct fsdk_server {
    char* agones_addr;    /* Local Agones sidecar gRPC address (copied).       */
    char* match_id;       /* This box's bound match id (the admission gate checks
                           * a joining token's match_id against it). NULL until
                           * fsdk_server_get_binding populates it (Agones - TODO);
                           * NULL means validate_player skips the binding check.  */
    /* TODO(server identity): hold the short-lived, scoped server token minted
     * at allocation time, read from the environment - NEVER baked in. */
};

struct fsdk_ticket {
    char*             ticket_id; /* Server-assigned ticket id (copied).        */
    fsdk_match_status status;    /* Last known status.                         */
};

/* -------------------------------------------------------------------------- */
/* Internal logging helper (routes through the host-installed sink)           */
/* -------------------------------------------------------------------------- */

/* Emits a message to the installed log sink, if any. NEVER pass secrets
 * (tokens, credentials) as arguments - log descriptions only. */
void fsdk_log(fsdk_log_level level, const char* message);

/* -------------------------------------------------------------------------- */
/* Internal transport abstraction (HTTP). Implemented in transport.c.         */
/* -------------------------------------------------------------------------- */

/* fsdk_http_method, fsdk_http_fn and fsdk_set_http_transport are PUBLIC (in
 * <foundry/fsdk.h>): the network stack is host-provided, not baked into the core. */

/* Perform an HTTP request against base_url + path. Builds the full URL and
 * dispatches it to the host-installed transport (fsdk_set_http_transport).
 *
 *   bearer_token : optional Authorization: Bearer value (may be NULL).
 *   body_json    : optional request body (may be NULL).
 *   out_body     : on FSDK_OK, set to a heap-allocated NUL-terminated response
 *                  body; caller frees with fsdk_string_free (may be NULL to
 *                  discard).
 *   out_status   : on FSDK_OK, set to the HTTP status code (may be NULL).
 *
 * Returns FSDK_NOT_IMPLEMENTED when no transport is installed (fails closed). */
fsdk_result fsdk_http_request(const char* base_url,
                              fsdk_http_method method,
                              const char* path,
                              const char* bearer_token,
                              const char* body_json,
                              char** out_body,
                              long* out_status);

/* Dispatch a fully-built request to the host-installed transport. Implemented in
 * fsdk.c (where the transport pointer is stored). FSDK_NOT_IMPLEMENTED if none. */
fsdk_result fsdk_dispatch_http(fsdk_http_method method,
                               const char* url,
                               const char* bearer_token,
                               const char* body_json,
                               char** out_body,
                               long* out_status);

/* -------------------------------------------------------------------------- */
/* Internal token verification. Implemented in token.c.                       */
/* -------------------------------------------------------------------------- */

/* Verify a platform-signed (auth-efga) match token (a JWT): RS256 signature (via
 * the host-installed verifier seam, keyed by the header `kid`), algorithm pinning,
 * iss/aud, exp/nbf, and that it binds to expected_match_id (NULL skips the binding
 * check). On FSDK_OK, *out_info is populated (foundry_id=sub, match_id, expires_at=exp).
 * Fails closed (FSDK_NOT_IMPLEMENTED) when no verifier is installed. */
fsdk_result fsdk_token_verify(const char* match_token,
                              const char* expected_match_id,
                              fsdk_player_info* out_info);

/* Dispatch a raw RS256 signature check to the host-installed verifier
 * (fsdk_set_jwt_verifier). Implemented in fsdk.c. FSDK_NOT_IMPLEMENTED if none. */
fsdk_result fsdk_dispatch_jwt_verify(const char* kid,
                                     const char* signing_input,
                                     const unsigned char* signature,
                                     size_t signature_len);

#endif /* FOUNDRY_FSDK_INTERNAL_H */
