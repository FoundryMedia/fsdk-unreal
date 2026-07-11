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
    FSDK_ERR_INTERNAL = 12,      /* Unexpected internal error.                 */
    FSDK_ERR_UNAVAILABLE = 13    /* No server capacity for the queue (503).    */
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

/* Chat session handle (client-side; FRC rooms over the realtime WebSocket).
 * Opaque; lifetime is bound to its client. */
typedef struct fsdk_chat fsdk_chat;

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
/* Secret store (host-provided keyring, optional)                             */
/* -------------------------------------------------------------------------- */

/* Host-provided persistent secret store (OS keyring / Windows Credential Manager
 * / macOS Keychain). The core uses it to persist ONLY the long-lived FID refresh
 * token across runs, so a player need not re-enter a password every launch. The
 * access token and the password are NEVER handed to this store. With NO store
 * installed, login still works for the session but nothing persists (no resume).
 * Callbacks return 0 on success, non-zero on failure/not-found.
 *
 *   save : persist value (NUL-terminated) under key (NUL-terminated).
 *   load : on success set *out_value to a malloc()'d, NUL-terminated copy the
 *          core frees with free(); return non-zero when the key is absent.
 *   del  : delete the entry for key (absent is fine).
 *   user_data : passed back verbatim from fsdk_set_secret_store. */
typedef int (*fsdk_secret_save_fn)(const char* key, const char* value, void* user_data);
typedef int (*fsdk_secret_load_fn)(const char* key, char** out_value, void* user_data);
typedef int (*fsdk_secret_delete_fn)(const char* key, void* user_data);

/* Install a process-wide secret store (pass NULLs to remove). Set once at startup. */
void fsdk_set_secret_store(fsdk_secret_save_fn save,
                           fsdk_secret_load_fn load,
                           fsdk_secret_delete_fn del,
                           void* user_data);

/* -------------------------------------------------------------------------- */
/* WebSocket transport (host-provided, required for chat)                     */
/* -------------------------------------------------------------------------- */

/* Host-provided WebSocket transport, mirroring the HTTP seam: the core stays
 * zero-dependency and vendorable; the engine binding backs this with its own
 * WS stack (UE: FWebSocketsModule). The host OWNS the socket:
 *
 *   connect : open a WS to url (wss://...). On success set *out_handle to an
 *             opaque host handle and return FSDK_OK. The connection MAY still
 *             be in-flight - the host feeds core state via the callbacks below.
 *   send    : send one TEXT frame (NUL-terminated UTF-8 JSON). Map transport
 *             failure to FSDK_ERR_NETWORK.
 *   close   : close + release the handle (idempotent, never fails).
 *
 * The host MUST deliver every inbound TEXT frame to fsdk_chat_on_ws_text and
 * call fsdk_chat_on_ws_closed when the socket drops. Delivery thread is the
 * host's choice; the chat handle is not thread-safe - feed it from one thread.
 * TLS is enforced by the host (wss). Never log frame payloads (they carry the
 * session token in the auth frame). */
typedef fsdk_result (*fsdk_ws_connect_fn)(const char* url, void** out_handle, void* user_data);
typedef fsdk_result (*fsdk_ws_send_fn)(void* handle, const char* text, void* user_data);
typedef void (*fsdk_ws_close_fn)(void* handle, void* user_data);

/* Install a process-wide WS transport (pass NULLs to remove). Set once at startup.
 * With NO transport installed, chat calls return FSDK_NOT_IMPLEMENTED. */
void fsdk_set_ws_transport(fsdk_ws_connect_fn connect_fn,
                           fsdk_ws_send_fn send_fn,
                           fsdk_ws_close_fn close_fn,
                           void* user_data);

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
 * it. This is the only credential the client SDK ever holds. On success the
 * identity snapshot (foundryId/displayName from /v1/me/user) is cached for
 * fsdk_current_session. */
fsdk_result fsdk_authenticate(fsdk_client* client, const char* player_token);

/* Re-fetch the identity snapshot (GET /v1/me/user with the stored token) and
 * cache it for fsdk_current_session. Best-effort identity for tokens set via
 * fsdk_set_player_token (e.g. the launcher handoff): a failure leaves the
 * authenticated state untouched - a BYO token that FID doesn't recognize simply
 * has no platform display name. FSDK_ERR_NOT_AUTHENTICATED if no token is set. */
fsdk_result fsdk_refresh_session(fsdk_client* client);

/* -------------------------------------------------------------------------- */
/* CLIENT FID auth (optional - for games whose players are Foundry accounts)  */
/* -------------------------------------------------------------------------- */
/* These obtain the player token FOR the game via the player's Foundry login, so
 * the game need not source a token elsewhere. The access token becomes the
 * client's credential (exactly as if passed to fsdk_authenticate); the refresh
 * token is persisted via the installed secret store (host keyring) and rotated.
 * A game that brings its own identity skips these and calls fsdk_set_player_token.
 *
 * The auth endpoints live on a SEPARATE host (auth-efga) from the FMMS api base;
 * fsdk_set_auth_base overrides it (default https://auth.foundryplatform.app). */

/* Logged-in player identity snapshot (for UI). No privileged data. */
typedef struct fsdk_session {
    char foundry_id[64];     /* Player's FID, NUL-terminated.                  */
    char display_name[128];  /* Display name (may be empty), NUL-terminated.   */
} fsdk_session;

/* Override the auth host (e.g. a local auth-efga for dev). auth_base is copied. */
void fsdk_set_auth_base(fsdk_client* client, const char* auth_base);

/* Log in with the player's Foundry credentials (email or username + password).
 * On success the access token authenticates the client and the rotated refresh
 * token is saved via the secret store. The password is used only to build the
 * request body and is never stored or logged. remember -> rememberMe. */
fsdk_result fsdk_login(fsdk_client* client, const char* email_or_username,
                       const char* password, int remember);

/* Refresh the session using the stored/in-memory refresh token (rotates it and
 * re-saves the NEW token). FSDK_ERR_NOT_AUTHENTICATED if no refresh token. */
fsdk_result fsdk_refresh(fsdk_client* client);

/* Resume a session from the persisted refresh token (load -> refresh), with no
 * password. FSDK_ERR_NOT_AUTHENTICATED if nothing was stored. */
fsdk_result fsdk_try_resume(fsdk_client* client);

/* Revoke the session server-side (best-effort) and clear the persisted +
 * in-memory tokens. */
fsdk_result fsdk_logout(fsdk_client* client);

/* Set the player token directly (BYO identity): authenticate WITHOUT the FID
 * login or the /v1/me/user probe - the caller's backend already vouched. */
fsdk_result fsdk_set_player_token(fsdk_client* client, const char* player_token);

/* Snapshot the logged-in identity (foundry_id/display_name) for UI. */
fsdk_result fsdk_current_session(fsdk_client* client, fsdk_session* out);

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
/* CLIENT CHAT API (FRC rooms over the realtime WebSocket)                    */
/* -------------------------------------------------------------------------- */
/* Game chat rides the platform's control plane: every message is authorized,
 * rate-limited, and logged SERVER-side (this SDK ships to attacker-controlled
 * machines - nothing here is trusted). The client holds only the player's own
 * session token; the server enforces room membership. Requires BOTH the HTTP
 * transport (room resolve) and the WS transport (frames).
 *
 * Flow: fsdk_chat_create -> fsdk_chat_set_message_callback ->
 *       fsdk_chat_join_global("mygame") [-> fsdk_chat_join_party(partyId)] ->
 *       [host pumps fsdk_chat_on_ws_text / on_ws_closed; game calls
 *       fsdk_chat_tick(now_ms) each frame] -> fsdk_chat_send("gg") /
 *       fsdk_chat_send_channel(FSDK_CHAT_CHANNEL_PARTY, "inv"). On socket drop
 *       the host/game re-joins each channel (the server's history endpoint is
 *       the resync path).
 *
 * MULTIPLEXED: every channel rides ONE realtime socket (one auth, one ping) -
 * concurrent-chatter cost is per socket, so channels are subscriptions, never
 * extra connections. */

/* Longest accepted message body (server caps at 500 chars; +NUL headroom). */
#define FSDK_CHAT_BODY_MAX 512

/* A chat CHANNEL is a room subscription multiplexed over the one socket:
 * GLOBAL = the game's all-players room; PARTY = the player's current party. */
/* One room message, as fanned out by the platform. POD snapshot - copy what
 * you keep; the pointer is only valid inside the callback. */
typedef enum fsdk_chat_channel {
    FSDK_CHAT_CHANNEL_GLOBAL = 0,
    FSDK_CHAT_CHANNEL_PARTY  = 1,
    FSDK_CHAT_CHANNEL__COUNT = 2 /* internal bound - not a channel */
} fsdk_chat_channel;

typedef struct fsdk_chat_message {
    long long id;                        /* Server-assigned message id.        */
    fsdk_chat_channel channel;           /* Which joined channel this is from. */
    char room_id[64];                    /* Room UUID, NUL-terminated.         */
    char from_subject[136];              /* Sender (ns-scoped subject).        */
    char from_foundry_id[64];            /* Sender's FID; empty if opaque.     */
    char display_name[128];              /* Resolved name; empty if unknown.   */
    char body[FSDK_CHAT_BODY_MAX];       /* Message text, NUL-terminated.      */
} fsdk_chat_message;

/* Invoked for every room.message frame (including the caller's own echo). */
typedef void (*fsdk_chat_message_fn)(const fsdk_chat_message* message, void* user_data);

/* Create a chat session bound to an authenticated client (borrows its token +
 * base url; the client must outlive the chat). On success *out_chat is owned
 * by the caller (fsdk_chat_destroy). */
fsdk_result fsdk_chat_create(fsdk_client* client, fsdk_chat** out_chat);

/* Destroy the chat session (closes the socket via the WS seam). NULL-safe. */
void fsdk_chat_destroy(fsdk_chat* chat);

/* Install the message callback (pass NULL to remove). */
void fsdk_chat_set_message_callback(fsdk_chat* chat, fsdk_chat_message_fn cb, void* user_data);

/* Join a game's GLOBAL room: resolves the room over HTTP (server-authorized),
 * opens the realtime socket, authenticates with the player token, and
 * subscribes. Asynchronous past the resolve - fsdk_chat_ready() flips once the
 * server confirms the subscription. Requires an authenticated client. */
fsdk_result fsdk_chat_join_global(fsdk_chat* chat, const char* game_slug);

/* Join the player's PARTY room on the SAME socket (multiplexed subscription).
 * party_id comes from fsdk_social_my_party (or the host's own party system -
 * the partyId seam is data-only). Same async shape as join_global:
 * fsdk_chat_channel_ready(PARTY) flips on the server's confirmation. */
fsdk_result fsdk_chat_join_party(fsdk_chat* chat, const char* party_id);

/* Unsubscribe the party channel (player left/disbanded). The socket and the
 * other channels stay up. NULL-safe; a no-op when not joined. */
fsdk_result fsdk_chat_leave_party(fsdk_chat* chat);

/* Send to the GLOBAL channel. FSDK_ERR_UNAVAILABLE until that channel is
 * ready; FSDK_ERR_INVALID_ARG for an empty/oversized body. The echo arrives
 * via the message callback like everyone else's copy. */
fsdk_result fsdk_chat_send(fsdk_chat* chat, const char* body);

/* Send to a specific joined channel (the multi-tab chat box). */
fsdk_result fsdk_chat_send_channel(fsdk_chat* chat, fsdk_chat_channel channel, const char* body);

/* Drive the keepalive: call each frame/tick with a monotonic millisecond clock;
 * the core pings the socket every ~25s (the platform edge idles out at 60s). */
fsdk_result fsdk_chat_tick(fsdk_chat* chat, long long now_ms);

/* Host WS transport feeds: every inbound TEXT frame / the socket dropping. */
void fsdk_chat_on_ws_text(fsdk_chat* chat, const char* text);
void fsdk_chat_on_ws_closed(fsdk_chat* chat);

/* Whether the GLOBAL channel is live (auth.ok + its room.sub.ok both seen). */
int fsdk_chat_ready(const fsdk_chat* chat);

/* Whether a specific channel's subscription is live. */
int fsdk_chat_channel_ready(const fsdk_chat* chat, fsdk_chat_channel channel);

/* -------------------------------------------------------------------------- */
/* CLIENT SOCIAL API (in-game friends list, party discovery, whisper/DMs)     */
/* -------------------------------------------------------------------------- */
/* The in-game social session (fid GameScope, 2026-07-11): a ROOT-namespace
 * player token is a FULL social citizen - friends list/pending/add/accept/
 * remove/block, friend codes (mine/redeem), parties (create/invite/accept/
 * decline/leave), and the whole whisper (DM) surface, all server-authorized as
 * the player's own foundryId. Still outside the game session: presence PUT
 * (the launcher owns presence), friend-code ROTATE (the account-level kill
 * switch), and everything non-social (billing/support/account). Whisper is
 * POLL-based (call fsdk_dm_* on your own cadence; player sessions never
 * receive dm frames on the realtime socket). All calls are synchronous over
 * the HTTP transport - drive them from a worker thread, never the game
 * thread. */

/* One friend, with EFFECTIVE presence (invisible/stale read as offline). */
typedef struct fsdk_friend {
    char foundry_id[64];      /* The friend's platform id.                    */
    char display_name[128];   /* Resolved display name.                       */
    char username[64];        /* Unique handle.                               */
    char presence[16];        /* online|idle|away|dnd|offline                 */
    char presence_game[128];  /* Running game title (empty when none).        */
} fsdk_friend;

/* Fetch the player's friends. Fills up to capacity entries; *out_count gets
 * the number written. FSDK_ERR_UNAUTHORIZED on a rejected token. */
fsdk_result fsdk_social_friends(fsdk_client* client,
                                fsdk_friend* out, size_t capacity, size_t* out_count);

/* Incoming friend requests (people waiting on the player's accept). Same row
 * shape as the friends list; presence_game is not populated for pendings. */
fsdk_result fsdk_social_pending(fsdk_client* client,
                                fsdk_friend* out, size_t capacity, size_t* out_count);

/* Friend-graph mutations - server-authorized as the player themselves. A
 * non-friend/unknown target is a uniform FSDK_ERR_NO_MATCH. */
fsdk_result fsdk_social_friend_request(fsdk_client* client, const char* username);
fsdk_result fsdk_social_friend_accept(fsdk_client* client, const char* requester_foundry_id);
fsdk_result fsdk_social_friend_remove(fsdk_client* client, const char* friend_foundry_id);
fsdk_result fsdk_social_friend_block(fsdk_client* client, const char* foundry_id);
fsdk_result fsdk_social_friend_unblock(fsdk_client* client, const char* foundry_id);

/* The player's own share code (FDY-XXXXXX; shareable in chat) / redeem one for
 * an INSTANT friendship (the code is the owner's consent). Rotating the code
 * stays a launcher/account op (the leak kill switch). */
fsdk_result fsdk_social_friend_code(fsdk_client* client, char* out_code, size_t out_sz);
fsdk_result fsdk_social_redeem_code(fsdk_client* client, const char* code);

/* The player's current party id, or an empty string when not in a party.
 * This id is fsdk_chat_join_party's input (and the FMMS ticket partyId seam). */
fsdk_result fsdk_social_my_party(fsdk_client* client, char* out_party_id, size_t out_sz);

/* A party member, enriched with profile. state = "INVITED" | "JOINED". */
typedef struct fsdk_party_member {
    char foundry_id[64];
    char display_name[128];
    char username[64];
    char state[12];
} fsdk_party_member;

/* The party header (id empty when the player is not in a party). */
typedef struct fsdk_party {
    char id[64];
    char leader_foundry_id[64];
    int  max_size;
} fsdk_party;

/* Full party snapshot: header + members. *out_count = members written. */
fsdk_result fsdk_social_party_info(fsdk_client* client, fsdk_party* out_party,
                                   fsdk_party_member* members, size_t capacity,
                                   size_t* out_count);

/* Party lifecycle - create returns the new party's id (feed it straight to
 * fsdk_chat_join_party); invite is by username and requires an ACCEPTED
 * friendship (server-enforced). */
fsdk_result fsdk_social_party_create(fsdk_client* client, char* out_party_id, size_t out_sz);
fsdk_result fsdk_social_party_invite(fsdk_client* client, const char* party_id,
                                     const char* username);
fsdk_result fsdk_social_party_accept(fsdk_client* client, const char* party_id);
fsdk_result fsdk_social_party_decline(fsdk_client* client, const char* party_id);
fsdk_result fsdk_social_party_leave(fsdk_client* client, const char* party_id);

/* Longest accepted whisper body (server caps at 2000 chars; +NUL headroom). */
#define FSDK_DM_BODY_MAX 2048

/* One whisper conversation summary (newest activity first from the server). */
typedef struct fsdk_dm_conversation {
    char foundry_id[64];      /* The other side (a friend).                   */
    char display_name[128];
    char presence[16];
    char last_body[256];      /* Preview of the newest message (truncated).   */
    long long unread;         /* Unread count from them.                      */
    int last_from_me;         /* 1 when the newest message is the caller's.   */
} fsdk_dm_conversation;

/* One whisper message, caller-oriented (from_me flips the bubble side). */
typedef struct fsdk_dm_message {
    long long id;
    int from_me;
    int unsent;               /* Retracted tombstone - body is empty.         */
    char body[FSDK_DM_BODY_MAX];
    char created_at[40];      /* ISO-8601 server stamp.                       */
} fsdk_dm_message;

/* Conversation list / one conversation's newest page (newest first) / send /
 * mark that friend's messages read. friend_foundry_id must be the id of an
 * ACCEPTED friend - anything else is a uniform FSDK_ERR_NO_MATCH (404). */
fsdk_result fsdk_dm_conversations(fsdk_client* client,
                                  fsdk_dm_conversation* out, size_t capacity, size_t* out_count);
fsdk_result fsdk_dm_history(fsdk_client* client, const char* friend_foundry_id,
                            fsdk_dm_message* out, size_t capacity, size_t* out_count);
fsdk_result fsdk_dm_send(fsdk_client* client, const char* friend_foundry_id, const char* body);
fsdk_result fsdk_dm_mark_read(fsdk_client* client, const char* friend_foundry_id);

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

/* Check whether the platform asked this server to DRAIN (wind down gracefully:
 * stop admitting players, call fsdk_server_shutdown once the session empties).
 * The platform stamps the fcg/drain annotation on this GameServer; this reads it
 * through the local sidecar. Latching: once seen, *out_draining stays 1 forever
 * (a transient sidecar failure never un-drains). Call on the same cadence as
 * fsdk_server_health. On a read failure *out_draining still reports the latched
 * value and the error is returned. */
fsdk_result fsdk_server_check_drain(fsdk_server* server, int* out_draining);

/* Whether this GameServer has been ALLOCATED (a match was placed on it). Pure
 * latch read - no sidecar call; the latch is fed by the /gameserver reads the
 * host already makes (check_drain on the health cadence, get_binding at boot /
 * first join). Latched: once 1, stays 1. Use to gate idle-empty auto-shutdown -
 * a WARM Ready replica is always empty and must never idle-exit. */
fsdk_result fsdk_server_allocated(fsdk_server* server, int* out_allocated);

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
