/* fsdk-core - client chat (FRC rooms over the realtime WebSocket).
 *
 * The core owns the PROTOCOL (frame build/parse, ordering, keepalive); the host
 * owns the SOCKET (fsdk_set_ws_transport) and the clock (fsdk_chat_tick). Every
 * message is authorized, rate-limited, and logged by the platform server-side -
 * this file ships inside the game binary and is assumed reverse-engineered, so
 * it holds no authority: the player token is the only credential and room
 * membership is enforced by fid, never here.
 *
 * MULTIPLEXED CHANNELS: one socket, one auth, one keepalive - every channel
 * (GLOBAL, PARTY) is just a room.sub on that socket. Concurrent-chatter cost
 * is per socket, so adding a channel never adds a connection.
 *
 * Wire (JSON text frames, envelope {"t":...,"ts":...,"d":{...}}):
 *   out: {"t":"auth","d":{"token":...}}          (first frame; native WS could
 *                                                 also Bearer the upgrade, but
 *                                                 the auth frame works on every
 *                                                 host transport)
 *        {"t":"room.sub","d":{"roomId":...}}
 *        {"t":"room.unsub","d":{"roomId":...}}
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
#define CHAT_ROOM_PARTY_PATH "/v1/chat/rooms/party/"

/* Narrow to the "data" envelope object; fall back to the whole body. */
static const char* json_data_object(const char* body) {
    const char* data = json_value_after(body, "data");
    return data != NULL ? data : body;
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

int fsdk_chat_channel_ready(const fsdk_chat* chat, fsdk_chat_channel channel) {
    if (chat == NULL || channel < 0 || channel >= FSDK_CHAT_CHANNEL__COUNT) {
        return 0;
    }
    return chat->ws_handle != NULL && chat->ws_authed && chat->rooms[channel].joined;
}

int fsdk_chat_ready(const fsdk_chat* chat) {
    return fsdk_chat_channel_ready(chat, FSDK_CHAT_CHANNEL_GLOBAL);
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
    if (!fsdk_json_escape(chat->client->player_token, token_escaped, sizeof(token_escaped))) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(frame, sizeof(frame),
                   "{\"t\":\"auth\",\"d\":{\"token\":\"%s\"}}", token_escaped);
    return fsdk_dispatch_ws_send(chat->ws_handle, frame);
}

static fsdk_result send_room_sub(fsdk_chat* chat, const fsdk_chat_room_slot* slot) {
    char frame[160];
    (void)snprintf(frame, sizeof(frame),
                   "{\"t\":\"room.sub\",\"d\":{\"roomId\":\"%s\"}}", slot->room_id);
    return fsdk_dispatch_ws_send(chat->ws_handle, frame);
}

static fsdk_result send_room_unsub(fsdk_chat* chat, const fsdk_chat_room_slot* slot) {
    char frame[160];
    (void)snprintf(frame, sizeof(frame),
                   "{\"t\":\"room.unsub\",\"d\":{\"roomId\":\"%s\"}}", slot->room_id);
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

/* A resolve key is a slug or a party UUID - a single path segment, never a path. */
static int valid_room_key(const char* key) {
    return key != NULL && key[0] != '\0' && strlen(key) <= 120
           && strchr(key, '/') == NULL && strchr(key, '?') == NULL
           && strchr(key, '#') == NULL;
}

/* Resolve a channel's room over HTTP (the server authorizes membership), stamp
 * the slot, then subscribe: directly when the socket is already authed, or by
 * connect+auth (all populated slots sub on auth.ok). */
static fsdk_result chat_join(fsdk_chat* chat, fsdk_chat_channel channel,
                             const char* resolve_prefix, const char* key) {
    char path[192];
    char url[512];
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;
    fsdk_chat_room_slot* slot;

    if (chat == NULL || !valid_room_key(key)) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!chat->client->authenticated || chat->client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    /* 1. Resolve the room over HTTP - the server authorizes membership here. */
    (void)snprintf(path, sizeof(path), "%s%s", resolve_prefix, key);
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
    slot = &chat->rooms[channel];
    {
        const char* data = json_data_object(resp);
        if (!json_extract_string(data, "id", slot->room_id, sizeof(slot->room_id))
                || slot->room_id[0] == '\0') {
            fsdk_string_free(resp);
            return FSDK_ERR_PROTOCOL;
        }
    }
    fsdk_string_free(resp);
    slot->joined = 0;

    /* 2. Socket already authed (channel add / re-join): subscribe directly. */
    if (chat->ws_handle != NULL && chat->ws_authed) {
        return send_room_sub(chat, slot);
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

fsdk_result fsdk_chat_join_global(fsdk_chat* chat, const char* game_slug) {
    return chat_join(chat, FSDK_CHAT_CHANNEL_GLOBAL, CHAT_ROOM_GLOBAL_PATH, game_slug);
}

fsdk_result fsdk_chat_join_party(fsdk_chat* chat, const char* party_id) {
    return chat_join(chat, FSDK_CHAT_CHANNEL_PARTY, CHAT_ROOM_PARTY_PATH, party_id);
}

fsdk_result fsdk_chat_leave_party(fsdk_chat* chat) {
    fsdk_chat_room_slot* slot;
    fsdk_result rc = FSDK_OK;
    if (chat == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    slot = &chat->rooms[FSDK_CHAT_CHANNEL_PARTY];
    if (slot->room_id[0] == '\0') {
        return FSDK_OK; /* nothing joined */
    }
    if (chat->ws_handle != NULL && chat->ws_authed && slot->joined) {
        rc = send_room_unsub(chat, slot);
    }
    slot->room_id[0] = '\0';
    slot->joined = 0;
    return rc;
}

/* ---- send ----------------------------------------------------------------- */

fsdk_result fsdk_chat_send_channel(fsdk_chat* chat, fsdk_chat_channel channel, const char* body) {
    char body_escaped[FSDK_CHAT_BODY_MAX * 2];
    char frame[FSDK_CHAT_BODY_MAX * 2 + 128];
    if (chat == NULL || body == NULL || body[0] == '\0'
            || channel < 0 || channel >= FSDK_CHAT_CHANNEL__COUNT) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (strlen(body) >= FSDK_CHAT_BODY_MAX) {
        return FSDK_ERR_INVALID_ARG; /* server caps at 500 chars */
    }
    if (!fsdk_chat_channel_ready(chat, channel)) {
        return FSDK_ERR_UNAVAILABLE;
    }
    if (!fsdk_json_escape(body, body_escaped, sizeof(body_escaped))) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(frame, sizeof(frame),
                   "{\"t\":\"room.send\",\"d\":{\"roomId\":\"%s\",\"body\":\"%s\"}}",
                   chat->rooms[channel].room_id, body_escaped);
    return fsdk_dispatch_ws_send(chat->ws_handle, frame);
}

fsdk_result fsdk_chat_send(fsdk_chat* chat, const char* body) {
    return fsdk_chat_send_channel(chat, FSDK_CHAT_CHANNEL_GLOBAL, body);
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
    int i;
    if (chat == NULL || text == NULL) {
        return;
    }
    if (!json_extract_string(text, "t", type, sizeof(type))) {
        return; /* not an envelope - ignore */
    }
    if (strcmp(type, "auth.ok") == 0) {
        chat->ws_authed = 1;
        for (i = 0; i < FSDK_CHAT_CHANNEL__COUNT; i++) {
            if (chat->rooms[i].room_id[0] != '\0' && !chat->rooms[i].joined) {
                (void)send_room_sub(chat, &chat->rooms[i]);
            }
        }
        return;
    }
    if (strcmp(type, "room.sub.ok") == 0) {
        char room_id[64];
        if (!json_extract_string(text, "roomId", room_id, sizeof(room_id))) {
            return;
        }
        for (i = 0; i < FSDK_CHAT_CHANNEL__COUNT; i++) {
            if (strcmp(chat->rooms[i].room_id, room_id) == 0) {
                chat->rooms[i].joined = 1;
                fsdk_log(FSDK_LOG_INFO, "fsdk chat: room subscription live");
                return;
            }
        }
        return;
    }
    if (strcmp(type, "room.message") == 0) {
        fsdk_chat_message msg;
        memset(&msg, 0, sizeof(msg));
        (void)json_extract_string(text, "roomId", msg.room_id, sizeof(msg.room_id));
        for (i = 0; i < FSDK_CHAT_CHANNEL__COUNT; i++) {
            if (chat->rooms[i].room_id[0] != '\0'
                    && strcmp(chat->rooms[i].room_id, msg.room_id) == 0) {
                break;
            }
        }
        if (i >= FSDK_CHAT_CHANNEL__COUNT) {
            return; /* a room we are not subscribed to - not ours */
        }
        msg.channel = (fsdk_chat_channel)i;
        (void)json_extract_string(text, "from", msg.from_subject, sizeof(msg.from_subject));
        (void)json_extract_string(text, "foundryId", msg.from_foundry_id,
                                  sizeof(msg.from_foundry_id));
        (void)json_extract_string(text, "displayName", msg.display_name,
                                  sizeof(msg.display_name));
        (void)json_extract_string(text, "body", msg.body, sizeof(msg.body));
        (void)fsdk_json_extract_ll(text, "id", &msg.id);
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
    int i;
    if (chat == NULL) {
        return;
    }
    /* The host owns the handle; after close it is gone. The game re-joins each
     * channel (fsdk_chat_join_*) to reconnect - history REST is the resync
     * path. Room ids are kept so the host can see what was active. */
    chat->ws_handle = NULL;
    chat->ws_authed = 0;
    for (i = 0; i < FSDK_CHAT_CHANNEL__COUNT; i++) {
        chat->rooms[i].joined = 0;
    }
    chat->last_ping_ms = 0;
    fsdk_log(FSDK_LOG_INFO, "fsdk chat: realtime socket closed");
}
