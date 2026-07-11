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

/* Extract a number field into *out. Returns 1 on success. */
static inline int fsdk_json_extract_ll(const char* body, const char* key, long long* out) {
    const char* v = json_value_after(body, key);
    char* end = NULL;
    if (v == NULL || out == NULL) {
        return 0;
    }
    *out = strtoll(v, &end, 10);
    return end != v;
}

/* Extract a boolean field. Returns 1 on success. */
static inline int fsdk_json_extract_bool(const char* body, const char* key, int* out) {
    const char* v = json_value_after(body, key);
    if (v == NULL || out == NULL) {
        return 0;
    }
    if (strncmp(v, "true", 4) == 0) {
        *out = 1;
        return 1;
    }
    if (strncmp(v, "false", 5) == 0) {
        *out = 0;
        return 1;
    }
    return 0;
}

/* Escape a string into a JSON string literal body (no surrounding quotes).
 * Returns 0 if the escaped form would overflow out. */
static inline int fsdk_json_escape(const char* s, char* out, size_t out_sz) {
    size_t o = 0;
    size_t i;
    if (s == NULL || out == NULL || out_sz == 0) {
        return 0;
    }
    for (i = 0; s[i] != '\0'; i++) {
        const char c = s[i];
        const char* rep = NULL;
        switch (c) {
            case '"':  rep = "\\\""; break;
            case '\\': rep = "\\\\"; break;
            case '\n': rep = "\\n"; break;
            case '\r': rep = "\\r"; break;
            case '\t': rep = "\\t"; break;
            default: break;
        }
        if (rep != NULL) {
            if (o + 2 >= out_sz) {
                return 0;
            }
            out[o++] = rep[0];
            out[o++] = rep[1];
        } else {
            if (o + 1 >= out_sz) {
                return 0;
            }
            out[o++] = c;
        }
    }
    out[o] = '\0';
    return 1;
}

/* Top-level JSON-array iteration over a response body: find the '[' of the
 * "data" array (or the body's first '['), then step object-by-object. Each
 * object is COPIED (bounded) into the caller's buffer so the flat readers can
 * run on a NUL-terminated slice. String-aware brace matching; not a validator
 * (the platform emits well-formed JSON; a malformed body just ends the walk). */
static inline const char* fsdk_json_array_start(const char* body) {
    const char* v = json_value_after(body, "data");
    if (v == NULL) {
        v = body;
    }
    if (v == NULL) {
        return NULL;
    }
    v = strchr(v, '[');
    return v == NULL ? NULL : v + 1;
}

/* From cursor, copy the next {...} object into obj_buf and return the cursor
 * just past it; NULL when the array is exhausted (or the object overflows). */
static inline const char* fsdk_json_next_object(const char* cursor,
                                                char* obj_buf, size_t obj_buf_sz) {
    const char* p = cursor;
    const char* start;
    int depth = 0;
    int in_string = 0;
    size_t n;
    if (p == NULL || obj_buf == NULL || obj_buf_sz == 0) {
        return NULL;
    }
    while (*p != '\0' && *p != '{') {
        if (*p == ']') {
            return NULL; /* end of array */
        }
        p++;
    }
    if (*p == '\0') {
        return NULL;
    }
    start = p;
    for (; *p != '\0'; p++) {
        const char c = *p;
        if (in_string) {
            if (c == '\\' && p[1] != '\0') {
                p++;
            } else if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                n = (size_t)(p - start) + 1;
                if (n + 1 > obj_buf_sz) {
                    return NULL; /* oversized object - stop the walk */
                }
                memcpy(obj_buf, start, n);
                obj_buf[n] = '\0';
                return p + 1;
            }
        }
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Opaque handle definitions                                                  */
/* -------------------------------------------------------------------------- */

struct fsdk_client {
    char* base_url;       /* api base (FMMS), copied at create.                 */
    char* auth_base_url;  /* auth host override for FID login (NULL -> default).*/
    char* player_token;   /* In-memory only; PLAYER's FID access token. Never persisted. */
    char* refresh_token;  /* In-memory only; PERSISTED via the secret store (keyring). */
    int   authenticated;  /* 0 = not authenticated, 1 = authenticated.         */
    char  foundry_id[64]; /* Logged-in player's FID (set by login/refresh).     */
    char  display_name[128]; /* Display name (may be empty).                    */
};

struct fsdk_server {
    char* agones_addr;    /* Local Agones sidecar gRPC address (copied).       */
    char* match_id;       /* This box's bound match id (the admission gate checks
                           * a joining token's match_id against it). NULL until
                           * fsdk_server_get_binding populates it (Agones - TODO);
                           * NULL means validate_player skips the binding check.  */
    int sidecar_contacted; /* 1 once ANY sidecar HTTP call completed. The boot-race
                            * retry loop (server.c) only runs while this is 0 - a
                            * post-boot transport failure fails fast, never stalls. */
    int draining;          /* 1 once the platform's fcg/drain annotation was seen
                            * (fsdk_server_check_drain). Latched - never cleared.  */
    int allocated;         /* 1 once this GameServer was seen in state Allocated
                            * (a match was placed on it). Latched by the /gameserver
                            * reads (check_drain / get_binding). Drives the game's
                            * idle-empty auto-shutdown - a WARM Ready replica is
                            * always empty and must never idle-exit.               */
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

/* Agones sidecar first-contact retry tuning (defined in server.c). The sidecar's
 * HTTP gateway races the game server boot; until the sidecar has answered once,
 * transport-level failures retry at interval_ms up to max_attempts (~30s total,
 * like Agones's own SDKs). Internal-only: tests shrink these for speed. */
extern unsigned int fsdk_agones_retry_interval_ms; /* default 1000 */
extern int          fsdk_agones_retry_max_attempts; /* default 30  */

/* -------------------------------------------------------------------------- */
/* Internal secret-store dispatch (routes through fsdk_set_secret_store).      */
/* Implemented in fsdk.c. Return 0 on success, non-zero if no store/op failed. */
/* -------------------------------------------------------------------------- */

int fsdk_secret_save(const char* key, const char* value);
/* On success sets *out_value to a malloc()'d copy the caller frees with free(). */
int fsdk_secret_load(const char* key, char** out_value);
int fsdk_secret_delete(const char* key);

/* -------------------------------------------------------------------------- */
/* Internal WS dispatch (routes through fsdk_set_ws_transport).                */
/* Implemented in fsdk.c. FSDK_NOT_IMPLEMENTED when no transport installed.    */
/* -------------------------------------------------------------------------- */

fsdk_result fsdk_dispatch_ws_connect(const char* url, void** out_handle);
fsdk_result fsdk_dispatch_ws_send(void* handle, const char* text);
void fsdk_dispatch_ws_close(void* handle);

/* Chat keepalive cadence (defined in chat.c; the platform edge idles at 60s).
 * Internal-only: tests shrink it. */
extern long long fsdk_chat_ping_interval_ms; /* default 25000 */

/* One multiplexed channel slot: a room subscription on the shared socket. */
typedef struct fsdk_chat_room_slot {
    char room_id[64];            /* Resolved room UUID; empty = channel unused. */
    int  joined;                 /* room.sub.ok seen on the current socket.     */
} fsdk_chat_room_slot;

/* Chat session state (client-side FRC rooms). Lifetime bound to its client.
 * ONE socket, N channel subscriptions (fsdk_chat_channel indexes rooms[]). */
struct fsdk_chat {
    fsdk_client* client;         /* Borrowed: token + base url (must outlive).  */
    void*        ws_handle;      /* Host-owned socket handle (NULL when down).  */
    int          ws_authed;      /* auth.ok seen on the current socket.         */
    fsdk_chat_room_slot rooms[FSDK_CHAT_CHANNEL__COUNT];
    long long    last_ping_ms;   /* Host-clock stamp of the last ping sent.     */
    fsdk_chat_message_fn on_message;
    void*        on_message_user_data;
};

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
