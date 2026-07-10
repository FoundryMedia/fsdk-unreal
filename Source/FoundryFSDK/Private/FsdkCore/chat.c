/* fsdk-core - client chat (FRC rooms over the realtime WebSocket).
 *
 * The core owns the PROTOCOL (frame build/parse, ordering, keepalive); the host
 * owns the SOCKET (fsdk_set_ws_transport) and the clock (fsdk_chat_tick). Every
 * message is authorized, rate-limited, and logged by the platform server-side -
 * this file ships inside the game binary and is assumed reverse-engineered, so
 * it holds no authority: the player token is the only credential and room
 * membership is enforced by fid, never here.
 *
 * Wire (JSON text frames, envelope {"t":...,"ts":...,"d":{...}}):
 *   out: {"t":"auth","d":{"token":...}}          (first frame; native WS could
 *                                                 also Bearer the upgrade, but
 *                                                 the auth frame works on every
 *                                                 host transport)
 *        {"t":"room.sub","d":{"roomId":...}}
 *        {"t":"room.send","d":{"roomId":...,"body":...}}
 *        {"t":"ping"}
 *   in : auth.ok -> room.sub.ok -> room.message / room.ack / pong / error
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "foundry/fsdk.h"
#include "fsdk_internal.h"

/* Keepalive cadence: the platform edge (ALB) idles sockets out at 60s. */
long long fsdk_chat_ping_interval_ms = 25000;

#define CHAT_REALTIME_PATH "/v1/realtime"
#define CHAT_ROOM_GLOBAL_PATH "/v1/chat/rooms/global/"

/* ---- local JSON helpers (per-file copies; the internal readers are minimal
 *      on purpose - see fsdk_internal.h) ---------------------------------- */

/* Narrow to the "data" envelope object; fall back to the whole body. */
static const char* json_data_object(const char* body) {
    const char* data = json_value_after(body, "data");
    return data != NULL ? data : body;
}

static int json_extract_longlong(const char* body, const char* key, long long* out) {
    const char* v = json_value_after(body, key);
    char* end = NULL;
    if (v == NULL || out == NULL) {
        return 0;
    }
    *out = strtoll(v, &end, 10);
    return end != v;
}

/* Escape a string into a JSON string literal body (no surrounding quotes).
 * Returns 0 if the escaped form would overflow out. */
static int json_escape(const char* s, char* out, size_t out_sz) {
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

/* ---- lifecycle ----------------------------------------------------------- */

fsdk_result fsdk_chat_create(fsdk_client* client, fsdk_chat** out_chat) {
    fsdk_chat* chat;
    if (client == NULL || out_chat == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_chat = NULL;
    chat = (fsdk_chat*)calloc(1, sizeof(*chat));
    if (chat == NULL) {
        return FSDK_ERR_INTERNAL;
    }
    chat->client = client;
    *out_chat = chat;
    return FSDK_OK;
}

void fsdk_chat_destroy(fsdk_chat* chat) {
    if (chat == NULL) {
        return;
    }
    if (chat->ws_handle != NULL) {
        fsdk_dispatch_ws_close(chat->ws_handle);
        chat->ws_handle = NULL;
    }
    free(chat);
}

void fsdk_chat_set_message_callback(fsdk_chat* chat, fsdk_chat_message_fn cb, void* user_data) {
    if (chat == NULL) {
        return;
    }
    chat->on_message = cb;
    chat->on_message_user_data = user_data;
}

int fsdk_chat_ready(const fsdk_chat* chat) {
    return chat != NULL && chat->ws_handle != NULL && chat->ws_authed && chat->room_joined;
}

/* ---- outbound frames ----------------------------------------------------- */

static fsdk_result send_auth_frame(fsdk_chat* chat) {
    /* Token is a JWT (base64url + dots) - JSON-safe by construction, but escape
     * anyway; a malformed token must not be able to break out of the frame. */
    char token_escaped[2200];
    char frame[2304];
    if (chat->client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    if (!json_escape(chat->client->player_token, token_escaped, sizeof(token_escaped))) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(frame, sizeof(frame),
                   "{\"t\":\"auth\",\"d\":{\"token\":\"%s\"}}", token_escaped);
    return fsdk_dispatch_ws_send(chat->ws_handle, frame);
}

static fsdk_result send_room_sub(fsdk_chat* chat) {
    char frame[160];
    (void)snprintf(frame, sizeof(frame),
                   "{\"t\":\"room.sub\",\"d\":{\"roomId\":\"%s\"}}", chat->room_id);
    return fsdk_dispatch_ws_send(chat->ws_handle, frame);
}

/* ---- join ---------------------------------------------------------------- */

/* Build the realtime WS url from the client's api base (https -> wss). */
static int realtime_url(const fsdk_client* client, char* out, size_t out_sz) {
    const char* base = client->base_url;
    const char* rest;
    const char* scheme;
    size_t base_len;
    if (strncmp(base, "https://", 8) == 0) {
        scheme = "wss://";
        rest = base + 8;
    } else if (strncmp(base, "http://", 7) == 0) {
        scheme = "ws://"; /* local dev only; production is always wss */
        rest = base + 7;
    } else {
        return 0;
    }
    base_len = strlen(rest);
    while (base_len > 0 && rest[base_len - 1] == '/') {
        base_len--; /* strip trailing separators before appending the path */
    }
    if (strlen(scheme) + base_len + strlen(CHAT_REALTIME_PATH) + 1 > out_sz) {
        return 0;
    }
    (void)snprintf(out, out_sz, "%s%.*s%s", scheme, (int)base_len, rest, CHAT_REALTIME_PATH);
    return 1;
}

fsdk_result fsdk_chat_join_global(fsdk_chat* chat, const char* game_slug) {
    char path[192];
    char url[512];
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;

    if (chat == NULL || game_slug == NULL || game_slug[0] == '\0') {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!chat->client->authenticated || chat->client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    if (strlen(game_slug) > 120 || strchr(game_slug, '/') != NULL
            || strchr(game_slug, '?') != NULL || strchr(game_slug, '#') != NULL) {
        return FSDK_ERR_INVALID_ARG; /* a slug, not a path */
    }

    /* 1. Resolve the room over HTTP - the server authorizes membership here. */
    (void)snprintf(path, sizeof(path), "%s%s", CHAT_ROOM_GLOBAL_PATH, game_slug);
    rc = fsdk_http_request(chat->client->base_url, FSDK_HTTP_GET, path,
                           chat->client->player_token, NULL, &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        if (status == 401 || status == 403) {
            return FSDK_ERR_UNAUTHORIZED;
        }
        return status == 404 ? FSDK_ERR_NO_MATCH : FSDK_ERR_PROTOCOL;
    }
    {
        const char* data = json_data_object(resp);
        if (!json_extract_string(data, "id", chat->room_id, sizeof(chat->room_id))
                || chat->room_id[0] == '\0') {
            fsdk_string_free(resp);
            return FSDK_ERR_PROTOCOL;
        }
    }
    fsdk_string_free(resp);
    chat->room_joined = 0;

    /* 2. Socket already authed (re-join / room switch): subscribe directly. */
    if (chat->ws_handle != NULL && chat->ws_authed) {
        return send_room_sub(chat);
    }

    /* 3. Fresh socket: connect + authenticate; room.sub fires on auth.ok. */
    if (chat->ws_handle == NULL) {
        if (!realtime_url(chat->client, url, sizeof(url))) {
            return FSDK_ERR_INVALID_ARG;
        }
        rc = fsdk_dispatch_ws_connect(url, &chat->ws_handle);
        if (rc != FSDK_OK) {
            chat->ws_handle = NULL;
            return rc;
        }
        fsdk_log(FSDK_LOG_INFO, "fsdk chat: realtime socket connecting");
    }
    chat->ws_authed = 0;
    rc = send_auth_frame(chat);
    if (rc != FSDK_OK) {
        fsdk_dispatch_ws_close(chat->ws_handle);
        chat->ws_handle = NULL;
        return rc;
    }
    return FSDK_OK;
}

/* ---- send ----------------------------------------------------------------- */

fsdk_result fsdk_chat_send(fsdk_chat* chat, const char* body) {
    char body_escaped[FSDK_CHAT_BODY_MAX * 2];
    char frame[FSDK_CHAT_BODY_MAX * 2 + 128];
    if (chat == NULL || body == NULL || body[0] == '\0') {
        return FSDK_ERR_INVALID_ARG;
    }
    if (strlen(body) >= FSDK_CHAT_BODY_MAX) {
        return FSDK_ERR_INVALID_ARG; /* server caps at 500 chars */
    }
    if (!fsdk_chat_ready(chat)) {
        return FSDK_ERR_UNAVAILABLE;
    }
    if (!json_escape(body, body_escaped, sizeof(body_escaped))) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(frame, sizeof(frame),
                   "{\"t\":\"room.send\",\"d\":{\"roomId\":\"%s\",\"body\":\"%s\"}}",
                   chat->room_id, body_escaped);
    return fsdk_dispatch_ws_send(chat->ws_handle, frame);
}

/* ---- keepalive ------------------------------------------------------------ */

fsdk_result fsdk_chat_tick(fsdk_chat* chat, long long now_ms) {
    if (chat == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (chat->ws_handle == NULL || !chat->ws_authed) {
        return FSDK_OK; /* nothing to keep alive */
    }
    if (chat->last_ping_ms != 0 && now_ms - chat->last_ping_ms < fsdk_chat_ping_interval_ms) {
        return FSDK_OK;
    }
    chat->last_ping_ms = now_ms;
    return fsdk_dispatch_ws_send(chat->ws_handle, "{\"t\":\"ping\"}");
}

/* ---- inbound frames -------------------------------------------------------- */

void fsdk_chat_on_ws_text(fsdk_chat* chat, const char* text) {
    char type[32];
    if (chat == NULL || text == NULL) {
        return;
    }
    if (!json_extract_string(text, "t", type, sizeof(type))) {
        return; /* not an envelope - ignore */
    }
    if (strcmp(type, "auth.ok") == 0) {
        chat->ws_authed = 1;
        if (chat->room_id[0] != '\0' && !chat->room_joined) {
            (void)send_room_sub(chat);
        }
        return;
    }
    if (strcmp(type, "room.sub.ok") == 0) {
        chat->room_joined = 1;
        fsdk_log(FSDK_LOG_INFO, "fsdk chat: room subscription live");
        return;
    }
    if (strcmp(type, "room.message") == 0) {
        fsdk_chat_message msg;
        memset(&msg, 0, sizeof(msg));
        (void)json_extract_string(text, "roomId", msg.room_id, sizeof(msg.room_id));
        if (chat->room_id[0] != '\0' && strcmp(msg.room_id, chat->room_id) != 0) {
            return; /* another room's frame (future multi-room) - not ours */
        }
        (void)json_extract_string(text, "from", msg.from_subject, sizeof(msg.from_subject));
        (void)json_extract_string(text, "foundryId", msg.from_foundry_id,
                                  sizeof(msg.from_foundry_id));
        (void)json_extract_string(text, "displayName", msg.display_name,
                                  sizeof(msg.display_name));
        (void)json_extract_string(text, "body", msg.body, sizeof(msg.body));
        (void)json_extract_longlong(text, "id", &msg.id);
        if (chat->on_message != NULL && msg.body[0] != '\0') {
            chat->on_message(&msg, chat->on_message_user_data);
        }
        return;
    }
    if (strcmp(type, "error") == 0) {
        fsdk_log(FSDK_LOG_WARN, "fsdk chat: server error frame");
        return;
    }
    /* pong / room.ack / unknown types: nothing to do (forward compatibility). */
}

void fsdk_chat_on_ws_closed(fsdk_chat* chat) {
    if (chat == NULL) {
        return;
    }
    /* The host owns the handle; after close it is gone. The game re-joins
     * (fsdk_chat_join_global) to reconnect - history REST is the resync path. */
    chat->ws_handle = NULL;
    chat->ws_authed = 0;
    chat->room_joined = 0;
    chat->last_ping_ms = 0;
    fsdk_log(FSDK_LOG_INFO, "fsdk chat: realtime socket closed");
}
