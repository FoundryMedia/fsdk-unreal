/* fsdk-core - client social reads (in-game friends list, party discovery,
 * whisper/DMs) over the fid GameScope carve-out.
 *
 * SECURITY SHAPE (fsdk-security): this file ships inside the game binary. It
 * can only READ the redacted player-scoped surface fid deliberately opened for
 * ROOT player tokens (friends list, own party, DM converse/send/history/read).
 * Graph mutations (add/remove friend, block, party create/invite, presence)
 * are account-session endpoints a player token cannot reach - by server-side
 * enforcement, never SDK restraint. Whisper is POLL-based: player sessions
 * never receive dm frames on the realtime socket (the room-only pin), so the
 * host polls fsdk_dm_* on its own cadence.
 *
 * All calls are synchronous over the host HTTP transport - drive them from a
 * worker thread, never the game thread.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "foundry/fsdk.h"
#include "fsdk_internal.h"

#define SOCIAL_FRIENDS_PATH "/v1/social/friends"
#define SOCIAL_PARTIES_MINE_PATH "/v1/social/parties/mine"
#define SOCIAL_DM_PATH "/v1/social/dm"

/* Per-object scratch: the largest row is a DM message (2000-char body + JSON
 * overhead); friends/conversations are far smaller. */
#define SOCIAL_OBJ_BUF (FSDK_DM_BODY_MAX + 1024)

/* ---- shared plumbing ------------------------------------------------------ */

static fsdk_result social_status_to_result(long status) {
    if (status >= 200 && status < 300) {
        return FSDK_OK;
    }
    if (status == 401 || status == 403) {
        return FSDK_ERR_UNAUTHORIZED;
    }
    return status == 404 ? FSDK_ERR_NO_MATCH : FSDK_ERR_PROTOCOL;
}

static fsdk_result social_get(fsdk_client* client, const char* path, char** out_body) {
    long status = 0;
    fsdk_result rc;
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_GET, path,
                           client->player_token, NULL, out_body, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    rc = social_status_to_result(status);
    if (rc != FSDK_OK) {
        fsdk_string_free(*out_body);
        *out_body = NULL;
    }
    return rc;
}

/* A foundryId path segment: UUID chars only - never trust it into a URL raw. */
static int valid_foundry_id(const char* id) {
    size_t i;
    if (id == NULL || id[0] == '\0' || strlen(id) > 40) {
        return 0;
    }
    for (i = 0; id[i] != '\0'; i++) {
        const char c = id[i];
        const int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                       || (c >= 'A' && c <= 'F') || c == '-';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* ---- friends --------------------------------------------------------------- */

fsdk_result fsdk_social_friends(fsdk_client* client,
                                fsdk_friend* out, size_t capacity, size_t* out_count) {
    char* body = NULL;
    const char* cursor;
    char* obj;
    fsdk_result rc;
    size_t n = 0;

    if (out == NULL || capacity == 0 || out_count == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_count = 0;
    rc = social_get(client, SOCIAL_FRIENDS_PATH, &body);
    if (rc != FSDK_OK) {
        return rc;
    }
    obj = (char*)malloc(SOCIAL_OBJ_BUF);
    if (obj == NULL) {
        fsdk_string_free(body);
        return FSDK_ERR_INTERNAL;
    }
    cursor = fsdk_json_array_start(body);
    while (n < capacity && (cursor = fsdk_json_next_object(cursor, obj, SOCIAL_OBJ_BUF)) != NULL) {
        fsdk_friend* f = &out[n];
        memset(f, 0, sizeof(*f));
        (void)json_extract_string(obj, "foundryId", f->foundry_id, sizeof(f->foundry_id));
        (void)json_extract_string(obj, "displayName", f->display_name, sizeof(f->display_name));
        (void)json_extract_string(obj, "username", f->username, sizeof(f->username));
        (void)json_extract_string(obj, "presence", f->presence, sizeof(f->presence));
        (void)json_extract_string(obj, "presenceGame", f->presence_game, sizeof(f->presence_game));
        if (f->foundry_id[0] != '\0') {
            n++;
        }
    }
    free(obj);
    fsdk_string_free(body);
    *out_count = n;
    return FSDK_OK;
}

/* ---- party ------------------------------------------------------------------ */

fsdk_result fsdk_social_my_party(fsdk_client* client, char* out_party_id, size_t out_sz) {
    char* body = NULL;
    const char* cursor;
    char obj[512];
    fsdk_result rc;

    if (out_party_id == NULL || out_sz == 0) {
        return FSDK_ERR_INVALID_ARG;
    }
    out_party_id[0] = '\0';
    rc = social_get(client, SOCIAL_PARTIES_MINE_PATH, &body);
    if (rc != FSDK_OK) {
        return rc;
    }
    /* The list is the caller's JOINED parties (at most one today) - take the
     * first row's id. Only the id is read, so a small scratch buffer is fine:
     * an oversized row (many members) just ends the walk with no party. */
    cursor = fsdk_json_array_start(body);
    if (fsdk_json_next_object(cursor, obj, sizeof(obj)) != NULL) {
        (void)json_extract_string(obj, "id", out_party_id, out_sz);
    } else if (cursor != NULL) {
        /* Row too big for the scratch: re-scan the raw body for the first id. */
        (void)json_extract_string(fsdk_json_array_start(body), "id", out_party_id, out_sz);
    }
    fsdk_string_free(body);
    return FSDK_OK;
}

/* ---- whisper (DMs) ----------------------------------------------------------- */

fsdk_result fsdk_dm_conversations(fsdk_client* client,
                                  fsdk_dm_conversation* out, size_t capacity, size_t* out_count) {
    char* body = NULL;
    const char* cursor;
    char* obj;
    fsdk_result rc;
    size_t n = 0;

    if (out == NULL || capacity == 0 || out_count == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_count = 0;
    rc = social_get(client, SOCIAL_DM_PATH, &body);
    if (rc != FSDK_OK) {
        return rc;
    }
    obj = (char*)malloc(SOCIAL_OBJ_BUF);
    if (obj == NULL) {
        fsdk_string_free(body);
        return FSDK_ERR_INTERNAL;
    }
    cursor = fsdk_json_array_start(body);
    while (n < capacity && (cursor = fsdk_json_next_object(cursor, obj, SOCIAL_OBJ_BUF)) != NULL) {
        fsdk_dm_conversation* c = &out[n];
        memset(c, 0, sizeof(*c));
        (void)json_extract_string(obj, "foundryId", c->foundry_id, sizeof(c->foundry_id));
        (void)json_extract_string(obj, "displayName", c->display_name, sizeof(c->display_name));
        (void)json_extract_string(obj, "presence", c->presence, sizeof(c->presence));
        (void)json_extract_string(obj, "lastBody", c->last_body, sizeof(c->last_body));
        (void)fsdk_json_extract_ll(obj, "unread", &c->unread);
        (void)fsdk_json_extract_bool(obj, "lastFromMe", &c->last_from_me);
        if (c->foundry_id[0] != '\0') {
            n++;
        }
    }
    free(obj);
    fsdk_string_free(body);
    *out_count = n;
    return FSDK_OK;
}

fsdk_result fsdk_dm_history(fsdk_client* client, const char* friend_foundry_id,
                            fsdk_dm_message* out, size_t capacity, size_t* out_count) {
    char path[128];
    char* body = NULL;
    const char* cursor;
    char* obj;
    fsdk_result rc;
    size_t n = 0;

    if (out == NULL || capacity == 0 || out_count == NULL
            || !valid_foundry_id(friend_foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_count = 0;
    (void)snprintf(path, sizeof(path), "%s/%s", SOCIAL_DM_PATH, friend_foundry_id);
    rc = social_get(client, path, &body);
    if (rc != FSDK_OK) {
        return rc;
    }
    obj = (char*)malloc(SOCIAL_OBJ_BUF);
    if (obj == NULL) {
        fsdk_string_free(body);
        return FSDK_ERR_INTERNAL;
    }
    cursor = fsdk_json_array_start(body);
    while (n < capacity && (cursor = fsdk_json_next_object(cursor, obj, SOCIAL_OBJ_BUF)) != NULL) {
        fsdk_dm_message* m = &out[n];
        long long id = 0;
        memset(m, 0, sizeof(*m));
        if (!fsdk_json_extract_ll(obj, "id", &id)) {
            continue; /* not a message row (e.g. a nested reactions object) */
        }
        m->id = id;
        (void)fsdk_json_extract_bool(obj, "fromMe", &m->from_me);
        (void)fsdk_json_extract_bool(obj, "unsent", &m->unsent);
        (void)json_extract_string(obj, "body", m->body, sizeof(m->body));
        (void)json_extract_string(obj, "createdAt", m->created_at, sizeof(m->created_at));
        n++;
    }
    free(obj);
    fsdk_string_free(body);
    *out_count = n;
    return FSDK_OK;
}

fsdk_result fsdk_dm_send(fsdk_client* client, const char* friend_foundry_id, const char* body) {
    char path[128];
    char body_escaped[FSDK_DM_BODY_MAX * 2];
    char* payload;
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;

    if (!valid_foundry_id(friend_foundry_id) || body == NULL || body[0] == '\0') {
        return FSDK_ERR_INVALID_ARG;
    }
    if (strlen(body) >= FSDK_DM_BODY_MAX) {
        return FSDK_ERR_INVALID_ARG; /* server caps at 2000 chars */
    }
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    if (!fsdk_json_escape(body, body_escaped, sizeof(body_escaped))) {
        return FSDK_ERR_INVALID_ARG;
    }
    payload = (char*)malloc(sizeof(body_escaped) + 32);
    if (payload == NULL) {
        return FSDK_ERR_INTERNAL;
    }
    (void)snprintf(payload, sizeof(body_escaped) + 32, "{\"body\":\"%s\"}", body_escaped);
    (void)snprintf(path, sizeof(path), "%s/%s", SOCIAL_DM_PATH, friend_foundry_id);
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_POST, path,
                           client->player_token, payload, &resp, &status);
    free(payload);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_string_free(resp);
    return social_status_to_result(status);
}

fsdk_result fsdk_dm_mark_read(fsdk_client* client, const char* friend_foundry_id) {
    char path[128];
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;

    if (!valid_foundry_id(friend_foundry_id) || client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/read", SOCIAL_DM_PATH, friend_foundry_id);
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_POST, path,
                           client->player_token, "{}", &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_string_free(resp);
    return social_status_to_result(status);
}
