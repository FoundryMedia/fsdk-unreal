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
 * overhead); friends/conversations are far smaller. The +4096 headroom is for a
 * PARTY row: a party of 16 members carries invitedAt/expiresAt on every pending
 * row, and a row that overflows the scratch ends the walk with no party at all. */
#define SOCIAL_OBJ_BUF (FSDK_DM_BODY_MAX + 4096)

/* ---- shared plumbing ------------------------------------------------------ */

static fsdk_result social_status_to_result(long status) {
    if (status >= 200 && status < 300) {
        return FSDK_OK;
    }
    if (status == 401 || status == 403) {
        return FSDK_ERR_UNAUTHORIZED;
    }
    if (status == 429) {
        return FSDK_ERR_RATE_LIMITED;
    }
    return status == 404 ? FSDK_ERR_NO_MATCH : FSDK_ERR_PROTOCOL;
}

const char* fsdk_last_error_message(const fsdk_client* client) {
    return client == NULL ? "" : client->last_error;
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
    client->last_error[0] = '\0';
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_GET, path,
                           client->player_token, NULL, out_body, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    rc = social_status_to_result(status);
    if (rc != FSDK_OK) {
        fsdk_client_note_error(client, status, *out_body);
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

/* The friend row's nullable `party {size, maxSize, joinable}` block - what the
 * friend's party looks like WITHOUT its id (fid never leaks the partyId: FMMS
 * groups tickets by attributes.partyId unvalidated). Absent/null = not in a
 * party = every field 0. The block is copied out first so a later key of the
 * row can never be mistaken for one of its fields. */
static void parse_friend_party(const char* obj, fsdk_friend* f) {
    const char* v = json_value_after(obj, "party");
    const char* end;
    char blk[160];
    size_t len;
    long long n = 0;
    int b = 0;
    if (v == NULL || *v != '{') {
        return;
    }
    end = strchr(v, '}');
    if (end == NULL) {
        return;
    }
    len = (size_t)(end - v) + 1;
    if (len >= sizeof(blk)) {
        len = sizeof(blk) - 1;
    }
    memcpy(blk, v, len);
    blk[len] = '\0';
    f->in_party = 1;
    if (fsdk_json_extract_ll(blk, "size", &n)) {
        f->party_size = (int)n;
    }
    if (fsdk_json_extract_ll(blk, "maxSize", &n)) {
        f->party_max_size = (int)n;
    }
    if (fsdk_json_extract_bool(blk, "joinable", &b)) {
        f->party_joinable = b;
    }
}

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
        parse_friend_party(obj, f);
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
    client->last_error[0] = '\0';
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
    fsdk_client_note_error(client, status, resp);
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
    client->last_error[0] = '\0';
    (void)snprintf(path, sizeof(path), "%s/%s/read", SOCIAL_DM_PATH, friend_foundry_id);
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_POST, path,
                           client->player_token, "{}", &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_client_note_error(client, status, resp);
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
    client->last_error[0] = '\0';
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_POST, path,
                           client->player_token, body_json, &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_client_note_error(client, status, resp);
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
    client->last_error[0] = '\0';
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_DELETE, path,
                           client->player_token, NULL, &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_client_note_error(client, status, resp);
    fsdk_string_free(resp);
    return social_status_to_result(status);
}

/* PUT with a JSON body; maps status like social_post. */
static fsdk_result social_put(fsdk_client* client, const char* path, const char* body_json) {
    char* resp = NULL;
    long status = 0;
    fsdk_result rc;
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    client->last_error[0] = '\0';
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_PUT, path,
                           client->player_token, body_json, &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    fsdk_client_note_error(client, status, resp);
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

fsdk_result fsdk_social_blocked(fsdk_client* client,
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
    rc = social_get(client, SOCIAL_FRIENDS_PATH "/blocks", &body);
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

/* Parse one PartyResponse object into a header; resolve the leader's display
 * name by scanning the nested members array for the leader's foundryId. When
 * self_id is non-empty, the caller's OWN member row also gives the invite's
 * lifetime (my_invited_at / my_expires_at) - the /invites list carries every
 * invitee of the party, so only the caller's row is THIS invite's clock. */
static void parse_party_header(const char* obj, const char* self_id, fsdk_party* out) {
    long long max_size = 0;
    long long platform_max = 0;
    const char* mcursor;
    char mobj[768];
    memset(out, 0, sizeof(*out));
    (void)json_extract_string(obj, "id", out->id, sizeof(out->id));
    (void)json_extract_string(obj, "leaderFoundryId", out->leader_foundry_id,
                              sizeof(out->leader_foundry_id));
    if (fsdk_json_extract_ll(obj, "maxSize", &max_size)) {
        out->max_size = (int)max_size;
    }
    /* platformMaxSize is the launcher's cap (the number invites are held to
     * outside a game mode); an older fid sends none, so it falls back to maxSize. */
    if (fsdk_json_extract_ll(obj, "platformMaxSize", &platform_max) && platform_max > 0) {
        out->platform_max_size = (int)platform_max;
    } else {
        out->platform_max_size = out->max_size;
    }
    mcursor = json_value_after(obj, "members");
    mcursor = mcursor != NULL ? strchr(mcursor, '[') : NULL;
    mcursor = mcursor != NULL ? mcursor + 1 : NULL;
    while ((mcursor = fsdk_json_next_object(mcursor, mobj, sizeof(mobj))) != NULL) {
        char member_id[64];
        (void)json_extract_string(mobj, "foundryId", member_id, sizeof(member_id));
        if (out->leader_display_name[0] == '\0'
                && strcmp(member_id, out->leader_foundry_id) == 0) {
            (void)json_extract_string(mobj, "displayName", out->leader_display_name,
                                      sizeof(out->leader_display_name));
        }
        if (self_id != NULL && self_id[0] != '\0' && strcmp(member_id, self_id) == 0) {
            (void)json_extract_string(mobj, "invitedAt", out->my_invited_at,
                                      sizeof(out->my_invited_at));
            (void)json_extract_string(mobj, "expiresAt", out->my_expires_at,
                                      sizeof(out->my_expires_at));
        }
    }
}

fsdk_result fsdk_social_party_invites(fsdk_client* client,
                                      fsdk_party* out, size_t capacity, size_t* out_count) {
    char* body = NULL;
    const char* cursor;
    char* obj;
    fsdk_result rc;
    size_t n = 0;

    if (out == NULL || capacity == 0 || out_count == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_count = 0;
    rc = social_get(client, SOCIAL_PARTIES_PATH "/invites", &body);
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
        parse_party_header(obj, client->foundry_id, &out[n]);
        if (out[n].id[0] != '\0') {
            n++;
        }
    }
    free(obj);
    fsdk_string_free(body);
    *out_count = n;
    return FSDK_OK;
}

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
        const char* mcursor;
        char mobj[768];
        parse_party_header(obj, NULL, out_party);
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
            (void)fsdk_json_extract_bool(mobj, "ready", &m->ready);
            (void)fsdk_json_extract_bool(mobj, "inMatch", &m->in_match);
            /* The invite's lifetime (INVITED rows only; null on JOINED = empty). */
            (void)json_extract_string(mobj, "invitedAt", m->invited_at, sizeof(m->invited_at));
            (void)json_extract_string(mobj, "expiresAt", m->expires_at, sizeof(m->expires_at));
            /* null (not ready / older fid) leaves the calloc-zeroed empty string. */
            (void)json_extract_string(mobj, "readyGameSlug", m->ready_game_slug,
                                      sizeof(m->ready_game_slug));
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
    client->last_error[0] = '\0';
    rc = fsdk_http_request(client->base_url, FSDK_HTTP_POST, SOCIAL_PARTIES_PATH,
                           client->player_token, "{}", &resp, &status);
    if (rc != FSDK_OK) {
        return rc;
    }
    /* Keep fid's sentence for a failed create ("You're already in a party.") -
     * without it a stale-cache 409 showed the generic fallback. */
    fsdk_client_note_error(client, status, resp);
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

/* One invite body: {"<field>":"<value>"} plus "queueKey" when the host names the
 * mode the party is being built for (the server then holds the party to THAT
 * queue's party cap instead of the platform cap). value is already validated. */
static fsdk_result party_invite_common(fsdk_client* client, const char* party_id,
                                       const char* field, const char* value,
                                       const char* queue_key) {
    char path[160];
    char payload[320];
    char qesc[192];
    (void)snprintf(path, sizeof(path), "%s/%s/invites", SOCIAL_PARTIES_PATH, party_id);
    if (queue_key != NULL && queue_key[0] != '\0') {
        if (strlen(queue_key) > FSDK_QUEUE_KEY_MAX
                || !fsdk_json_escape(queue_key, qesc, sizeof(qesc))) {
            return FSDK_ERR_INVALID_ARG;
        }
        (void)snprintf(payload, sizeof(payload), "{\"%s\":\"%s\",\"queueKey\":\"%s\"}",
                       field, value, qesc);
    } else {
        (void)snprintf(payload, sizeof(payload), "{\"%s\":\"%s\"}", field, value);
    }
    return social_post(client, path, payload);
}

fsdk_result fsdk_social_party_invite_for_queue(fsdk_client* client, const char* party_id,
                                               const char* username, const char* queue_key) {
    if (!valid_party_id(party_id) || !valid_username(username)) {
        return FSDK_ERR_INVALID_ARG;
    }
    return party_invite_common(client, party_id, "username", username, queue_key);
}

fsdk_result fsdk_social_party_invite_id_for_queue(fsdk_client* client, const char* party_id,
                                                  const char* foundry_id,
                                                  const char* queue_key) {
    if (!valid_party_id(party_id) || !valid_foundry_id(foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    return party_invite_common(client, party_id, "foundryId", foundry_id, queue_key);
}

fsdk_result fsdk_social_party_invite(fsdk_client* client, const char* party_id,
                                     const char* username) {
    return fsdk_social_party_invite_for_queue(client, party_id, username, NULL);
}

/* Leader-only: withdraw a pending invite (retires the invitee's notification
 * server-side). A uniform 404 for an unknown party / non-leader / no such invite. */
fsdk_result fsdk_social_party_cancel_invite(fsdk_client* client, const char* party_id,
                                            const char* foundry_id) {
    char path[192];
    if (!valid_party_id(party_id) || !valid_foundry_id(foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/invites/%s",
                   SOCIAL_PARTIES_PATH, party_id, foundry_id);
    return social_delete(client, path);
}

/* Leader-only: remove a JOINED member (never yourself - that is leave). */
fsdk_result fsdk_social_party_kick(fsdk_client* client, const char* party_id,
                                   const char* foundry_id) {
    char path[192];
    if (!valid_party_id(party_id) || !valid_foundry_id(foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/members/%s",
                   SOCIAL_PARTIES_PATH, party_id, foundry_id);
    return social_delete(client, path);
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

/* A game slug rides the body, so it is checked, not escaped: fid's slug alphabet. */
static int valid_game_slug(const char* s) {
    size_t i;
    if (s == NULL || s[0] == '\0' || strlen(s) > 64) {
        return 0;
    }
    for (i = 0; s[i] != '\0'; i++) {
        const char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return 0;
        }
    }
    return 1;
}

fsdk_result fsdk_social_party_set_ready_in_game(fsdk_client* client, const char* party_id,
                                                int ready, const char* game_slug) {
    char path[160];
    char body[128];
    if (!valid_party_id(party_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/members/me/ready", SOCIAL_PARTIES_PATH, party_id);
    if (!ready) {
        return social_put(client, path, "{\"ready\":false}");
    }
    if (game_slug == NULL) {
        /* Sent on purpose: the launcher-shaped request. fid refuses it with "Ready up from
         * inside the game." and that sentence lands in fsdk_last_error_message. */
        return social_put(client, path, "{\"ready\":true}");
    }
    if (!valid_game_slug(game_slug)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(body, sizeof(body), "{\"ready\":true,\"gameSlug\":\"%s\"}", game_slug);
    return social_put(client, path, body);
}

fsdk_result fsdk_social_party_set_ready(fsdk_client* client, const char* party_id, int ready) {
    return fsdk_social_party_set_ready_in_game(client, party_id, ready, NULL);
}

fsdk_result fsdk_social_party_set_in_match(fsdk_client* client, const char* party_id,
                                           int in_match) {
    char path[160];
    if (!valid_party_id(party_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/in-match", SOCIAL_PARTIES_PATH, party_id);
    return social_put(client, path,
                      in_match ? "{\"inMatch\":true}" : "{\"inMatch\":false}");
}

/* ---- party join requests (leader-gated; parties stay invite-only) ----------- */

fsdk_result fsdk_social_party_join_request(fsdk_client* client, const char* friend_foundry_id) {
    char payload[96];
    if (!valid_foundry_id(friend_foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(payload, sizeof(payload), "{\"friendFoundryId\":\"%s\"}", friend_foundry_id);
    return social_post(client, SOCIAL_PARTIES_PATH "/join-requests", payload);
}

fsdk_result fsdk_social_party_join_requests(fsdk_client* client, const char* party_id,
                                            fsdk_friend* out, size_t capacity, size_t* out_count) {
    char path[160];
    char* body = NULL;
    const char* cursor;
    char* obj;
    fsdk_result rc;
    size_t n = 0;

    if (!valid_party_id(party_id) || out == NULL || capacity == 0 || out_count == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_count = 0;
    (void)snprintf(path, sizeof(path), "%s/%s/join-requests", SOCIAL_PARTIES_PATH, party_id);
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
        fsdk_friend* f = &out[n];
        memset(f, 0, sizeof(*f));
        (void)json_extract_string(obj, "requesterFoundryId", f->foundry_id, sizeof(f->foundry_id));
        if (f->foundry_id[0] == '\0') {
            /* fid may send the friend-row shape (foundryId) for this list. */
            (void)json_extract_string(obj, "foundryId", f->foundry_id, sizeof(f->foundry_id));
        }
        (void)json_extract_string(obj, "displayName", f->display_name, sizeof(f->display_name));
        (void)json_extract_string(obj, "username", f->username, sizeof(f->username));
        if (f->foundry_id[0] != '\0') {
            n++;
        }
    }
    free(obj);
    fsdk_string_free(body);
    *out_count = n;
    return FSDK_OK;
}

fsdk_result fsdk_social_party_join_accept_for_queue(fsdk_client* client, const char* party_id,
                                                    const char* requester_foundry_id,
                                                    const char* queue_key) {
    char path[192];
    char payload[256];
    char qesc[192];
    if (!valid_party_id(party_id) || !valid_foundry_id(requester_foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/join-requests/%s/accept",
                   SOCIAL_PARTIES_PATH, party_id, requester_foundry_id);
    if (queue_key != NULL && queue_key[0] != '\0') {
        /* The leader's selected mode: the seat is refused over THAT queue's cap. */
        if (strlen(queue_key) > FSDK_QUEUE_KEY_MAX
                || !fsdk_json_escape(queue_key, qesc, sizeof(qesc))) {
            return FSDK_ERR_INVALID_ARG;
        }
        (void)snprintf(payload, sizeof(payload), "{\"queueKey\":\"%s\"}", qesc);
        return social_post(client, path, payload);
    }
    return social_post(client, path, "{}");
}

fsdk_result fsdk_social_party_join_accept(fsdk_client* client, const char* party_id,
                                         const char* requester_foundry_id) {
    return fsdk_social_party_join_accept_for_queue(client, party_id, requester_foundry_id, NULL);
}

fsdk_result fsdk_social_party_join_decline(fsdk_client* client, const char* party_id,
                                          const char* requester_foundry_id) {
    char path[192];
    if (!valid_party_id(party_id) || !valid_foundry_id(requester_foundry_id)) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "%s/%s/join-requests/%s/decline",
                   SOCIAL_PARTIES_PATH, party_id, requester_foundry_id);
    return social_post(client, path, "{}");
}

/* ---- the party cap a queue enforces --------------------------------------------- */

/* A queue key rides in a QUERY value (GET /v1/fmms/queues/party-cap?key=...): a derived
 * name is "{gameSlug}/{modeSlug}" and a slash is legal in a query value, whereas an
 * encoded %2F in a PATH segment is refused by Tomcat/Spring's firewall by default.
 * Only slug/uuid characters pass this check, so the encoder below is defense in depth. */
static int valid_queue_key(const char* key) {
    size_t i;
    if (key == NULL || key[0] == '\0' || key[0] == '/' || strlen(key) > FSDK_QUEUE_KEY_MAX
            || strstr(key, "..") != NULL) {
        return 0;
    }
    for (i = 0; key[i] != '\0'; i++) {
        const char c = key[i];
        const int ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                       || (c >= 'A' && c <= 'Z') || c == '_' || c == '-' || c == '.'
                       || c == '/';
        if (!ok) {
            return 0;
        }
    }
    return 1;
}

/* Percent-encode a query value: unreserved characters and '/' pass, every other
 * byte becomes %XX (so a space, '&', '#', '%' or '=' can never end or split the
 * value). Returns 0 when out would overflow. */
static int encode_query_value(const char* in, char* out, size_t out_sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    size_t i;
    for (i = 0; in[i] != '\0'; i++) {
        const unsigned char c = (unsigned char)in[i];
        const int plain = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                          || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.'
                          || c == '~' || c == '/';
        if (plain) {
            if (o + 1 >= out_sz) {
                return 0;
            }
            out[o++] = (char)c;
        } else {
            if (o + 3 >= out_sz) {
                return 0;
            }
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 15];
        }
    }
    out[o] = '\0';
    return 1;
}

fsdk_result fsdk_fmms_party_cap(fsdk_client* client, const char* queue_key,
                                int* out_party_cap, int* out_team_size) {
    char path[384];
    char enc[FSDK_QUEUE_KEY_MAX * 3 + 1];
    char* body = NULL;
    const char* data;
    long long cap = 0;
    long long team = 0;
    fsdk_result rc;
    if (out_party_cap == NULL || !valid_queue_key(queue_key)) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_party_cap = 0;
    if (out_team_size != NULL) {
        *out_team_size = 0;
    }
    if (!encode_query_value(queue_key, enc, sizeof(enc))) {
        return FSDK_ERR_INVALID_ARG;
    }
    (void)snprintf(path, sizeof(path), "/v1/fmms/queues/party-cap?key=%s", enc);
    rc = social_get(client, path, &body);
    if (rc != FSDK_OK) {
        return rc;
    }
    data = json_value_after(body, "data");
    if (data == NULL) {
        data = body;
    }
    if (!fsdk_json_extract_ll(data, "partyCap", &cap)) {
        fsdk_string_free(body);
        return FSDK_ERR_PROTOCOL;
    }
    (void)fsdk_json_extract_ll(data, "teamSize", &team);
    fsdk_string_free(body);
    *out_party_cap = (int)cap;
    if (out_team_size != NULL) {
        *out_team_size = (int)team;
    }
    return FSDK_OK;
}
