/* fsdk-core - the in-game social session (friends, parties, whisper/DMs) over
 * the fid GameScope surface.
 *
 * SECURITY SHAPE (fsdk-security): this file ships inside the game binary. The
 * game session is a FULL social citizen (2026-07-11): friends list/pending/
 * add/accept/remove/block, friend codes, party lifecycle, and the whole DM
 * surface - every call server-authorized as the player's own foundryId; the
 * SDK holds no authority. Still unreachable from a game token, by server-side
 * enforcement: presence PUT (the launcher owns presence), friend-code ROTATE
 * (the account-level kill switch), and every non-social surface (billing/
 * support/account). Whisper is POLL-based: player sessions never receive dm
 * frames on the realtime socket (the room-only pin), so the host polls
 * fsdk_dm_* on its own cadence.
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
#define SOCIAL_PARTIES_PATH "/v1/social/parties"
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

/* ---- graph mutations (full in-game social session, 2026-07-11) --------------- */

/* POST with an optional JSON body; maps status like the reads. */
static fsdk_result social_post(fsdk_client* client, const char* path, const char* body_json) {
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_POST, path,
                           client->player_token, body_json, &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_string_free(resp);
    return social_status_to_result(status);
}

static fsdk_result social_delete(fsdk_client* client, const char* path) {
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_DELETE, path,
                           client->player_token, NULL, &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_string_free(resp);
    return social_status_to_result(status);
}

/* A username: the platform's ^[a-zA-Z0-9_-]{3,32}$ (body-embedded, still validated). */
static int valid_username(const char* u) {
    size_t i;
    if (u == NULL || u[0] == '\0' || strlen(u) > 32) {
        return 0;
    }
    for (i = 0; u[i] != '\0'; i++) {
        const char c = u[i];
        const int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                       || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* A party id path segment: one segment, bounded, no separators. */
static int valid_party_id(const char* id) {
    return id != NULL && id[0] != '\0' && strlen(id) <= 64
           && strchr(id, '/') == NULL && strchr(id, '?') == NULL
           && strchr(id, '#') == NULL;
}

fsdk_result fsdk_social_pending(fsdk_client* client,
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
    rc = social_get(client, SOCIAL_FRIENDS_PATH "/pending", &body);
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
        if (f->foundry_id[0] != '\0') {
            n++;
        }
    }
    free(obj);
    fsdk_string_free(body);
    *out_count = n;
    return FSDK_OK;
}

fsdk_result fsdk_social_friend_request(fsdk_client* client, const char* username) {
    char payload[96];
    if (!valid_username(username)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(payload, sizeof(payload), "{\"username\":\"%s\"}", username);
    return social_post(client, SOCIAL_FRIENDS_PATH "/requests", payload);
}

fsdk_result fsdk_social_friend_accept(fsdk_client* client, const char* requester_foundry_id) {
    char path[128];
    if (!valid_foundry_id(requester_foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/requests/%s/accept",
                   SOCIAL_FRIENDS_PATH, requester_foundry_id);
    return social_post(client, path, "{}");
}

fsdk_result fsdk_social_friend_remove(fsdk_client* client, const char* friend_foundry_id) {
    char path[128];
    if (!valid_foundry_id(friend_foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s", SOCIAL_FRIENDS_PATH, friend_foundry_id);
    return social_delete(client, path);
}

fsdk_result fsdk_social_friend_block(fsdk_client* client, const char* foundry_id) {
    char payload[96];
    if (!valid_foundry_id(foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(payload, sizeof(payload), "{\"foundryId\":\"%s\"}", foundry_id);
    return social_post(client, SOCIAL_FRIENDS_PATH "/blocks", payload);
}

fsdk_result fsdk_social_friend_unblock(fsdk_client* client, const char* foundry_id) {
    char path[128];
    if (!valid_foundry_id(foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/blocks/%s", SOCIAL_FRIENDS_PATH, foundry_id);
    return social_delete(client, path);
}

fsdk_result fsdk_social_friend_code(fsdk_client* client, char* out_code, size_t out_sz) {
    char* body = NULL;
    fsdk_result rc;
    if (out_code == NULL || out_sz == 0) {
        return FSDK_ERR_INVALID_ARG;
    }
    out_code[0] = '\0';
    rc = social_get(client, "/v1/social/friend-code", &body);
    if (rc != FSDK_OK) {
        return rc;
    }
    (void)json_extract_string(json_value_after(body, "data"), "code", out_code, out_sz);
    fsdk_string_free(body);
    return FSDK_OK;
}

fsdk_result fsdk_social_redeem_code(fsdk_client* client, const char* code) {
    char code_escaped[96];
    char payload[128];
    if (code == NULL || code[0] == '\0' || strlen(code) > 40) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!fsdk_json_escape(code, code_escaped, sizeof(code_escaped))) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(payload, sizeof(payload), "{\"code\":\"%s\"}", code_escaped);
    return social_post(client, SOCIAL_FRIENDS_PATH "/redeem", payload);
}

/* ---- parties ------------------------------------------------------------------ */

fsdk_result fsdk_social_party_info(fsdk_client* client, fsdk_party* out_party,
                                   fsdk_party_member* members, size_t capacity,
                                   size_t* out_count) {
    char* body = NULL;
    const char* cursor;
    char* obj;
    fsdk_result rc;
    size_t n = 0;

    if (out_party == NULL || members == NULL || capacity == 0 || out_count == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    memset(out_party, 0, sizeof(*out_party));
    *out_count = 0;
    rc = social_get(client, SOCIAL_PARTIES_MINE_PATH, &body);
    if (rc != FSDK_OK) {
        return rc;
    }
    obj = (char*)malloc(SOCIAL_OBJ_BUF);
    if (obj == NULL) {
        fsdk_string_free(body);
        return FSDK_ERR_INTERNAL;
    }
    cursor = fsdk_json_array_start(body);
    if ((cursor = fsdk_json_next_object(cursor, obj, SOCIAL_OBJ_BUF)) != NULL) {
        long long max_size = 0;
        const char* mcursor;
        char mobj[512];
        (void)json_extract_string(obj, "id", out_party->id, sizeof(out_party->id));
        (void)json_extract_string(obj, "leaderFoundryId", out_party->leader_foundry_id,
                                  sizeof(out_party->leader_foundry_id));
        if (fsdk_json_extract_ll(obj, "maxSize", &max_size)) {
            out_party->max_size = (int)max_size;
        }
        /* walk the nested members array inside the copied party object */
        mcursor = json_value_after(obj, "members");
        mcursor = mcursor != NULL ? strchr(mcursor, '[') : NULL;
        mcursor = mcursor != NULL ? mcursor + 1 : NULL;
        while (n < capacity
                && (mcursor = fsdk_json_next_object(mcursor, mobj, sizeof(mobj))) != NULL) {
            fsdk_party_member* m = &members[n];
            memset(m, 0, sizeof(*m));
            (void)json_extract_string(mobj, "foundryId", m->foundry_id, sizeof(m->foundry_id));
            (void)json_extract_string(mobj, "displayName", m->display_name,
                                      sizeof(m->display_name));
            (void)json_extract_string(mobj, "username", m->username, sizeof(m->username));
            (void)json_extract_string(mobj, "state", m->state, sizeof(m->state));
            if (m->foundry_id[0] != '\0') {
                n++;
            }
        }
    }
    free(obj);
    fsdk_string_free(body);
    *out_count = n;
    return FSDK_OK;
}

fsdk_result fsdk_social_party_create(fsdk_client* client, char* out_party_id, size_t out_sz) {
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;
    if (out_party_id == NULL || out_sz == 0 || client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    out_party_id[0] = '\0';
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_POST, SOCIAL_PARTIES_PATH,
                           client->player_token, "{}", &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    rc = social_status_to_result(status);
    if (rc == FSDK_OK) {
        (void)json_extract_string(json_value_after(resp, "data"), "id", out_party_id, out_sz);
        if (out_party_id[0] == '\0') {
            rc = FSDK_ERR_PROTOCOL;
        }
    }
    fsdk_string_free(resp);
    return rc;
}

fsdk_result fsdk_social_party_invite(fsdk_client* client, const char* party_id,
                                     const char* username) {
    char path[160];
    char payload[96];
    if (!valid_party_id(party_id) || !valid_username(username)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/invites", SOCIAL_PARTIES_PATH, party_id);
    (void)snprintf(payload, sizeof(payload), "{\"username\":\"%s\"}", username);
    return social_post(client, path, payload);
}

static fsdk_result party_verb(fsdk_client* client, const char* party_id, const char* verb) {
    char path[160];
    if (!valid_party_id(party_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/%s", SOCIAL_PARTIES_PATH, party_id, verb);
    return social_post(client, path, "{}");
}

fsdk_result fsdk_social_party_accept(fsdk_client* client, const char* party_id) {
    return party_verb(client, party_id, "accept");
}

fsdk_result fsdk_social_party_decline(fsdk_client* client, const char* party_id) {
    return party_verb(client, party_id, "decline");
}

fsdk_result fsdk_social_party_leave(fsdk_client* client, const char* party_id) {
    return party_verb(client, party_id, "leave");
}
