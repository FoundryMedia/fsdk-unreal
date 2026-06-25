/*
 * fsdk.h - Foundry SDK (FSDK) public C ABI.
 *
 * This header is the SOURCE OF TRUTH for the FSDK surface. Every engine binding
 * (Unreal C++ MVP, future Unity P/Invoke, future Godot GDExtension) consumes
 * THIS ABI. There is exactly one implementation of auth / matchmaking / token
 * logic (the shared core); engines are thin bindings over it.
 *
 * Design rules (see SECURITY.md - these are load-bearing):
 *   - extern "C", opaque handle types, a single result enum.
 *   - NO secrets anywhere in this surface. The ONLY client credential is the
 *     player's own FID session token, PASSED IN by the game at call time -
 *     never stored by the SDK, never baked into the binary.
 *   - The CLIENT surface touches only the minimal player-scoped endpoint set
 *     (auth + matchmaking + receive-connection). No admin / operator paths.
 *   - All authorization is enforced SERVER-SIDE. Security never depends on SDK
 *     obscurity - the client SDK is assumed fully reverse-engineered.
 *
 * Status: SCAFFOLD. Implementations are stubs (real HTTP + Agones SDK + JWT
 * verification are follow-ons). See src/ TODOs.
 */
#ifndef FOUNDRY_FSDK_H
#define FOUNDRY_FSDK_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* Versioning                                                                 */
/* -------------------------------------------------------------------------- */

#define FSDK_VERSION_MAJOR 0
#define FSDK_VERSION_MINOR 1
#define FSDK_VERSION_PATCH 0

/* Returns a static, NUL-terminated semantic version string (e.g. "0.1.0").
 * The returned pointer is owned by the library; do not free it. */
const char* fsdk_version(void);

/* -------------------------------------------------------------------------- */
/* Result / error codes                                                       */
/* -------------------------------------------------------------------------- */

typedef enum fsdk_result {
    FSDK_OK = 0,                 /* Success.                                   */
    FSDK_NOT_IMPLEMENTED = 1,    /* Scaffold stub - real impl is a follow-on.  */
    FSDK_ERR_INVALID_ARG = 2,    /* A NULL/out-of-range argument was passed.   */
    FSDK_ERR_NOT_AUTHENTICATED = 3, /* Client op requires a prior authenticate.*/
    FSDK_ERR_UNAUTHORIZED = 4,   /* Server rejected the credential (401/403).  */
    FSDK_ERR_NETWORK = 5,        /* Transport/connectivity failure.            */
    FSDK_ERR_TIMEOUT = 6,        /* Operation timed out.                       */
    FSDK_ERR_PROTOCOL = 7,       /* Malformed / unexpected wire response.      */
    FSDK_ERR_TOKEN_INVALID = 8,  /* Match token failed signature/claims check. */
    FSDK_ERR_TOKEN_EXPIRED = 9,  /* Match token signature OK but expired.      */
    FSDK_ERR_NO_MATCH = 10,      /* Ticket has no connection yet / cancelled.  */
    FSDK_ERR_AGONES = 11,        /* Agones SDK lifecycle call failed.          */
    FSDK_ERR_INTERNAL = 12       /* Unexpected internal error.                 */
} fsdk_result;

/* Returns a static human-readable string for a result code (for logging). */
const char* fsdk_result_str(fsdk_result result);

/* -------------------------------------------------------------------------- */
/* Opaque handles                                                             */
/* -------------------------------------------------------------------------- */

/* Client-side SDK handle (game client - attacker-controlled environment). */
typedef struct fsdk_client fsdk_client;

/* Server-side SDK handle (dedicated game server - our trusted environment). */
typedef struct fsdk_server fsdk_server;

/* Matchmaking ticket handle (returned by request_match, polled, then resolved
 * to a connection). Opaque; lifetime is bound to its client. */
typedef struct fsdk_ticket fsdk_ticket;

/* -------------------------------------------------------------------------- */
/* Value types (POD, caller-owned storage)                                    */
/* -------------------------------------------------------------------------- */

/* Match status as reported by FMMS while a ticket is in flight. */
typedef enum fsdk_match_status {
    FSDK_MATCH_PENDING = 0,    /* Ticket accepted, searching.                  */
    FSDK_MATCH_SEARCHING = 1,  /* Actively matchmaking.                        */
    FSDK_MATCH_FOUND = 2,      /* Allocated - get_connection() will succeed.   */
    FSDK_MATCH_CANCELLED = 3,  /* Ticket cancelled by player or system.        */
    FSDK_MATCH_FAILED = 4,     /* Matchmaking failed / timed out.              */
    FSDK_MATCH_EXPIRED = 5     /* Ticket TTL elapsed.                          */
} fsdk_match_status;

/* Maximum byte length (including NUL) of a serialized match token. The match
 * token is a FID-signed JWT; this bound is generous for a compact JWT. */
#define FSDK_MATCH_TOKEN_MAX 1024

/* Connection details handed back to the game once a match is FOUND. The game
 * hands {ip, port} to the engine netcode to connect, and sends match_token to
 * the server, which validates it before admitting the player. */
typedef struct fsdk_connection {
    char     ip[64];                          /* IPv4/IPv6 literal, NUL-term.  */
    uint16_t port;                            /* UDP/TCP port.                 */
    char     match_token[FSDK_MATCH_TOKEN_MAX]; /* FID-signed JWT, NUL-term.   */
} fsdk_connection;

/* Result of server-side validation of a connecting player's match token.
 * Populated only on FSDK_OK from fsdk_server_validate_player. Carries the
 * minimal player identity the server needs; no privileged data. */
typedef struct fsdk_player_info {
    char    foundry_id[64];   /* Platform user id (FID), NUL-terminated.       */
    char    match_id[64];     /* Match this token authorizes, NUL-terminated.  */
    int64_t expires_at;       /* Token expiry, Unix epoch seconds.             */
} fsdk_player_info;

/* -------------------------------------------------------------------------- */
/* Logging (optional, no-secrets contract)                                    */
/* -------------------------------------------------------------------------- */

typedef enum fsdk_log_level {
    FSDK_LOG_DEBUG = 0,
    FSDK_LOG_INFO = 1,
    FSDK_LOG_WARN = 2,
    FSDK_LOG_ERROR = 3
} fsdk_log_level;

/* Log sink supplied by the host engine. The core NEVER logs secrets (tokens,
 * credentials) through this sink - messages are descriptive only. */
typedef void (*fsdk_log_fn)(fsdk_log_level level, const char* message, void* user_data);

/* Install a process-wide log sink (pass NULL to disable). user_data is passed
 * back verbatim. Not thread-safe with respect to concurrent logging; set once
 * at startup. */
void fsdk_set_log_sink(fsdk_log_fn sink, void* user_data);

/* -------------------------------------------------------------------------- */
/* HTTP transport (host-provided)                                             */
/* -------------------------------------------------------------------------- */

/* HTTP method for a transport request. */
typedef enum fsdk_http_method {
    FSDK_HTTP_GET = 0,
    FSDK_HTTP_POST = 1,
    FSDK_HTTP_DELETE = 2
} fsdk_http_method;

/* Host-provided HTTP transport. The core builds every request (the full URL, the
 * optional bearer token, the optional JSON body) and hands it to this callback;
 * the host performs the actual TLS HTTP using whatever stack it owns (e.g. an
 * engine HTTP module) and returns the response. This is the ONLY part of the core
 * that touches the network, so a host can supply its own stack - or a future
 * relay-aware transport - WITHOUT any other ABI change.
 *
 * Contract:
 *   url          : full request URL (base_url + path), NUL-terminated.
 *   bearer_token : when non-NULL, send "Authorization: Bearer <bearer_token>".
 *   body_json    : when non-NULL, send as the request body with
 *                  "Content-Type: application/json".
 *   out_body     : when non-NULL, set on success to a malloc()'d, NUL-terminated
 *                  response body; the core frees it with free(). Leave untouched
 *                  (or set NULL) to discard the body.
 *   out_status   : when non-NULL, set to the HTTP status code.
 *   user_data    : passed back verbatim from fsdk_set_http_transport.
 *
 * Return FSDK_OK once an HTTP response (of ANY status) was obtained - the core
 * maps the status itself. Map a transport failure to FSDK_ERR_NETWORK /
 * FSDK_ERR_TIMEOUT. The callback MUST enforce TLS (verify peer + host) and MUST
 * NEVER log bearer_token or body_json (they carry the player's session token). */
typedef fsdk_result (*fsdk_http_fn)(fsdk_http_method method,
                                    const char* url,
                                    const char* bearer_token,
                                    const char* body_json,
                                    char** out_body,
                                    long* out_status,
                                    void* user_data);

/* Install a process-wide HTTP transport (pass NULL to remove). Set once at
 * startup, before creating clients. With NO transport installed, every networked
 * call FAILS CLOSED (FSDK_NOT_IMPLEMENTED) rather than guessing - the core never
 * bakes in its own network stack. */
void fsdk_set_http_transport(fsdk_http_fn transport, void* user_data);

/* Host-provided RS256 signature verifier for SERVER-side match-token validation.
 * The core does ALL the dependency-free JWT work itself (compact split, base64url
 * decode, claim + match-binding checks, algorithm pinning); it delegates ONLY the
 * raw signature check to this callback, so the core links NO crypto library and
 * stays vendorable into any binary. The dedicated-server binding backs this with
 * OpenSSL (or its platform crypto), resolving the signing key by `kid` from FID's
 * published JWKS - the server is trusted and MAY link such a library (the CLIENT
 * never does; this seam is only used by the server path).
 *
 * Contract:
 *   kid           : the JWT header "kid" (which JWKS key signed it); "" if absent.
 *   signing_input : the exact ASCII "<header_b64url>.<payload_b64url>" that was
 *                   signed (NUL-terminated). RS256 = RSASSA-PKCS1-v1_5 over
 *                   SHA-256(signing_input).
 *   signature     : the decoded signature bytes.
 *   signature_len : length of signature in bytes.
 *   user_data     : passed back verbatim from fsdk_set_jwt_verifier.
 *
 * Return FSDK_OK iff the signature is valid for the key identified by kid;
 * otherwise FSDK_ERR_TOKEN_INVALID (unknown kid / bad signature). */
typedef fsdk_result (*fsdk_jwt_verify_fn)(const char* kid,
                                          const char* signing_input,
                                          const unsigned char* signature,
                                          size_t signature_len,
                                          void* user_data);

/* Install the process-wide JWT signature verifier (pass NULL to remove). With NO
 * verifier installed, fsdk_token_verify / fsdk_server_validate_player FAIL CLOSED
 * (FSDK_NOT_IMPLEMENTED) - an unverifiable token is a rejected token. */
void fsdk_set_jwt_verifier(fsdk_jwt_verify_fn verifier, void* user_data);

/* -------------------------------------------------------------------------- */
/* CLIENT API (ships inside the game client - assume reverse-engineered)      */
/* -------------------------------------------------------------------------- */

/* Create a client bound to a FID/FMMS base URL (e.g. "https://api.foundryplatform.app").
 * base_url is copied. On success *out_client is set and owned by the caller,
 * who must call fsdk_client_destroy. */
fsdk_result fsdk_client_create(const char* base_url, fsdk_client** out_client);

/* Destroy a client and free its resources. Safe to call with NULL. */
void fsdk_client_destroy(fsdk_client* client);

/* Authenticate the SDK with the PLAYER'S OWN FID session token. The token is
 * provided by the game (obtained through the platform's normal sign-in); the
 * SDK does NOT store it beyond the in-memory client lifetime and never persists
 * it. This is the only credential the client SDK ever holds. */
fsdk_result fsdk_authenticate(fsdk_client* client, const char* player_token);

/* Request a match in a named queue with opaque, queue-specific attributes
 * encoded as a JSON object string (attrs_json may be NULL for none). On success
 * *out_ticket is set and owned by the caller, who frees it with
 * fsdk_ticket_destroy. Requires a prior successful fsdk_authenticate. */
fsdk_result fsdk_request_match(fsdk_client* client,
                               const char* queue,
                               const char* attrs_json,
                               fsdk_ticket** out_ticket);

/* Poll the current status of a ticket. Writes the status to *out_status.
 * This is a lightweight call intended to be invoked on a timer / each frame. */
fsdk_result fsdk_poll_match(fsdk_client* client,
                            fsdk_ticket* ticket,
                            fsdk_match_status* out_status);

/* Fetch connection details for a ticket once its status is FSDK_MATCH_FOUND.
 * Fills *out (caller-allocated). Returns FSDK_ERR_NO_MATCH if not yet ready. */
fsdk_result fsdk_get_connection(fsdk_client* client,
                                fsdk_ticket* ticket,
                                fsdk_connection* out);

/* Cancel an in-flight ticket (best-effort). */
fsdk_result fsdk_cancel_match(fsdk_client* client, fsdk_ticket* ticket);

/* Destroy a ticket and free its resources. Safe to call with NULL. */
void fsdk_ticket_destroy(fsdk_ticket* ticket);

/* -------------------------------------------------------------------------- */
/* SERVER API (runs in the dedicated server - our trusted box)               */
/* -------------------------------------------------------------------------- */

/* Create a server-side SDK handle. agones_addr is the local Agones SDK sidecar
 * gRPC address (e.g. "127.0.0.1:9357"); pass NULL to use the Agones default.
 * The server identity token used for any FMMS callbacks is a SHORT-LIVED,
 * SCOPED token minted at allocation time and supplied by the orchestrator via
 * the environment - it is NEVER baked into the binary (see SECURITY.md). */
fsdk_result fsdk_server_create(const char* agones_addr, fsdk_server** out_server);

/* Destroy a server handle and free its resources. Safe to call with NULL. */
void fsdk_server_destroy(fsdk_server* server);

/* Signal the orchestrator that this server is ready to accept players
 * (-> Agones SDK Ready()). */
fsdk_result fsdk_server_ready(fsdk_server* server);

/* Send a health ping to the orchestrator (-> Agones SDK Health()). Call on a
 * regular interval; missing pings cause the orchestrator to recycle the box. */
fsdk_result fsdk_server_health(fsdk_server* server);

/* Validate a connecting player's match token. This is the admission GATE: the
 * server calls it for each joining player BEFORE accepting their netcode
 * connection. Verifies the FID signature, expiry, and that the token authorizes
 * THIS server's match binding. On FSDK_OK, *out is populated with the player's
 * minimal identity. On failure the connection MUST be dropped.
 * out may be NULL if the caller only needs the accept/reject verdict. */
fsdk_result fsdk_server_validate_player(fsdk_server* server,
                                        const char* match_token,
                                        fsdk_player_info* out);

/* Read this server's match binding (which match it was allocated for) as a JSON
 * object string. *out_json is heap-allocated by the SDK and must be freed by the
 * caller with fsdk_string_free. */
fsdk_result fsdk_server_get_binding(fsdk_server* server, char** out_json);

/* Signal the orchestrator that this server is shutting down
 * (-> Agones SDK Shutdown()). */
fsdk_result fsdk_server_shutdown(fsdk_server* server);

/* -------------------------------------------------------------------------- */
/* Memory helpers                                                             */
/* -------------------------------------------------------------------------- */

/* Free a NUL-terminated string returned by the SDK (e.g. server_get_binding).
 * Safe to call with NULL. */
void fsdk_string_free(char* str);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* FOUNDRY_FSDK_H */
