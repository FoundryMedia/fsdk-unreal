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

/* Verify a FID-signed match token (a JWT): signature against FID's published
 * JWKS, expiry, audience, and that it binds to expected_match_id (NULL skips
 * the binding check). On FSDK_OK, *out_info is populated.
 *
 * SCAFFOLD: returns FSDK_NOT_IMPLEMENTED. A real JWT/JWKS verifier gets linked
 * in a follow-on. */
fsdk_result fsdk_token_verify(const char* match_token,
                              const char* expected_match_id,
                              fsdk_player_info* out_info);

#endif /* FOUNDRY_FSDK_INTERNAL_H */
