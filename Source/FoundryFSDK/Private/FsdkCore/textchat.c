/* fsdk-core - the client TEXT CHAT module (the chat box: channels, input line,
 * slash commands, whisper, history) built ON TOP of fsdk_chat (FRC rooms) and
 * fsdk_dm_* (whisper/DM). This file owns none of the wire protocol itself -
 * it composes the existing transports and adds the semantics a chat panel
 * needs: three channels (PARTY/WHISPER/SYSTEM), slash commands, whisper name
 * resolution against the friends cache, the whisper poll (player sockets are
 * room-only - an inbound whisper can only ever be polled), and auto-rejoin of
 * a dropped FRC room subscription.
 *
 * SECURITY SHAPE: this file ships inside the game binary, same as chat.c and
 * social.c. It holds no authority of its own - every network call it makes
 * goes through the existing player-token-authorized surfaces; this module
 * only decides WHEN to call them and WHAT to show for the result.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "foundry/fsdk.h"
#include "fsdk_internal.h"

/* ---- tiny ASCII helpers (no locale dependence, no UB on signed char) ------ */

static int ascii_isspace(char c) {
    return isspace((unsigned char)c) != 0;
}

static char ascii_tolower(char c) {
    return (char)tolower((unsigned char)c);
}

static int ascii_ieq(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return a == b;
    }
    while (*a != '\0' && *b != '\0') {
        if (ascii_tolower(*a) != ascii_tolower(*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* ---- ring buffer ----------------------------------------------------------- */

static void ring_init(textchat_ring* r, size_t capacity) {
    r->buf = NULL;
    r->capacity = capacity;
    r->count = 0;
    r->next_write = 0;
}

static int ring_ensure_alloc(textchat_ring* r) {
    if (r->buf != NULL || r->capacity == 0) {
        return r->capacity != 0;
    }
    r->buf = (fsdk_textchat_line*)calloc(r->capacity, sizeof(fsdk_textchat_line));
    return r->buf != NULL;
}

static void ring_append(textchat_ring* r, const fsdk_textchat_line* line) {
    if (!ring_ensure_alloc(r)) {
        return; /* OOM or zero-capacity: drop the line rather than crash. */
    }
    r->buf[r->next_write] = *line;
    r->next_write = (r->next_write + 1) % r->capacity;
    if (r->count < r->capacity) {
        r->count++;
    }
}

/* Copy up to cap lines, oldest -> newest. When count > cap, the NEWEST cap
 * lines are returned (still oldest -> newest). Returns the count written. */
static size_t ring_copy_tail(const textchat_ring* r, fsdk_textchat_line* out, size_t cap) {
    size_t n;
    size_t skip;
    size_t start;
    size_t i;
    if (r->capacity == 0 || r->count == 0 || cap == 0) {
        return 0;
    }
    n = r->count < cap ? r->count : cap;
    skip = r->count - n;
    start = (r->next_write + r->capacity - r->count) % r->capacity;
    for (i = 0; i < n; i++) {
        size_t idx = (start + skip + i) % r->capacity;
        out[i] = r->buf[idx];
    }
    return n;
}

static void ring_destroy(textchat_ring* r) {
    free(r->buf);
    r->buf = NULL;
    r->count = 0;
    r->next_write = 0;
}

/* ---- self identity ---------------------------------------------------------- */

static const char* self_foundry_id(const fsdk_textchat* tc) {
    return tc->client != NULL ? tc->client->foundry_id : "";
}

static const char* self_display_name(const fsdk_textchat* tc) {
    if (tc->client != NULL && tc->client->display_name[0] != '\0') {
        return tc->client->display_name;
    }
    return "You";
}

/* ---- recording -------------------------------------------------------------- */

static void record_line(fsdk_textchat* tc, fsdk_textchat_channel channel,
                        fsdk_textchat_line_kind kind,
                        const char* from_id, const char* from_name,
                        const char* peer_id, const char* peer_name,
                        const char* body) {
    fsdk_textchat_line line;
    memset(&line, 0, sizeof(line));
    line.id = tc->next_line_id++;
    line.channel = channel;
    line.kind = kind;
    copy_bounded(line.from_foundry_id, sizeof(line.from_foundry_id), from_id);
    copy_bounded(line.from_name, sizeof(line.from_name), from_name);
    copy_bounded(line.peer_foundry_id, sizeof(line.peer_foundry_id), peer_id);
    copy_bounded(line.peer_name, sizeof(line.peer_name), peer_name);
    copy_bounded(line.body, sizeof(line.body), body);
    line.ts_ms = tc->clock_now_ms;
    ring_append(&tc->rings[channel], &line);
    if (tc->on_line != NULL) {
        tc->on_line(&line, tc->on_line_user_data);
    }
}

/* A SYSTEM-channel notice (informational or error). Every validation/failure
 * message in this module lands on SYSTEM - see the "System channel" design
 * note in the plan: content (CHAT/WHISPER_*) is the only thing that ever
 * lands on PARTY/WHISPER. */
static void note(fsdk_textchat* tc, fsdk_textchat_line_kind kind, const char* text) {
    record_line(tc, FSDK_TEXTCHAT_SYSTEM, kind, "", "System", "", "", text);
}

/* ---- send-result -> human copy (the "System channel" error mapping) ------- */

static void record_send_error(fsdk_textchat* tc, fsdk_result rc) {
    char msg[160];
    switch (rc) {
        case FSDK_ERR_NO_MATCH:
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, "You can only whisper friends.");
            return;
        case FSDK_ERR_UNAUTHORIZED:
        case FSDK_ERR_NOT_AUTHENTICATED:
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Signed out - sign in again.");
            return;
        case FSDK_ERR_RATE_LIMITED:
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Slow down - try again in a moment.");
            return;
        case FSDK_ERR_NETWORK:
        case FSDK_ERR_TIMEOUT:
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Could not reach Foundry - message not sent.");
            return;
        case FSDK_NOT_IMPLEMENTED:
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Chat transport is not installed.");
            return;
        default:
            (void)snprintf(msg, sizeof(msg), "Message not sent (%s).", fsdk_result_str(rc));
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, msg);
            return;
    }
}

/* ---- friends cache (whisper name resolution) ------------------------------- */

static void refresh_friends(fsdk_textchat* tc) {
    fsdk_result rc;
    if (tc->mode != FSDK_TEXTCHAT_MODE_PLATFORM) {
        return; /* LOCAL mode: zero transport calls, ever. */
    }
    if (tc->client == NULL || !tc->client->authenticated) {
        return;
    }
    rc = fsdk_social_friends(tc->client, tc->friends, FSDK_TEXTCHAT_MAX_FRIENDS, &tc->friends_count);
    if (rc == FSDK_OK) {
        tc->friends_last_refresh_ms = tc->clock_now_ms;
        tc->friends_ever_loaded = 1;
    } else {
        fsdk_log(FSDK_LOG_DEBUG, "fsdk textchat: friends refresh failed");
    }
}

typedef enum { RESOLVE_OK, RESOLVE_NOT_FOUND, RESOLVE_AMBIGUOUS } resolve_status;

/* Resolves a /w target: exact username (case-insensitive) first, else a
 * UNIQUE display-name match, else not-found - refreshing the friends cache
 * ONCE and retrying on a miss (never on an ambiguous hit). A leading '@' is
 * stripped (an explicit "use their @username" hint). */
static resolve_status resolve_friend(fsdk_textchat* tc, const char* raw_name,
                                     char* out_id, size_t out_id_sz,
                                     char* out_name, size_t out_name_sz) {
    const char* name = raw_name;
    int attempt;
    if (*name == '@') {
        name++;
    }
    for (attempt = 0; attempt < 2; attempt++) {
        size_t i;
        int exact = -1;
        int display_match = -1;
        int display_count = 0;
        for (i = 0; i < tc->friends_count; i++) {
            if (ascii_ieq(tc->friends[i].username, name)) {
                exact = (int)i;
                break;
            }
        }
        if (exact < 0) {
            for (i = 0; i < tc->friends_count; i++) {
                if (ascii_ieq(tc->friends[i].display_name, name)) {
                    display_count++;
                    display_match = (int)i;
                }
            }
        }
        if (exact >= 0) {
            const fsdk_friend* f = &tc->friends[exact];
            copy_bounded(out_id, out_id_sz, f->foundry_id);
            copy_bounded(out_name, out_name_sz, f->display_name[0] != '\0' ? f->display_name : f->username);
            return RESOLVE_OK;
        }
        if (display_count == 1) {
            const fsdk_friend* f = &tc->friends[display_match];
            copy_bounded(out_id, out_id_sz, f->foundry_id);
            copy_bounded(out_name, out_name_sz, f->display_name);
            return RESOLVE_OK;
        }
        if (display_count >= 2) {
            return RESOLVE_AMBIGUOUS;
        }
        if (attempt == 0) {
            refresh_friends(tc);
        }
    }
    return RESOLVE_NOT_FOUND;
}

/* ---- whisper poll ------------------------------------------------------------ */

static size_t find_seen_index(const fsdk_textchat* tc, const char* foundry_id) {
    size_t i;
    for (i = 0; i < tc->whisper_seen_count; i++) {
        if (strcmp(tc->whisper_seen[i].foundry_id, foundry_id) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

static size_t ensure_seen_slot(fsdk_textchat* tc, const char* foundry_id) {
    size_t idx = find_seen_index(tc, foundry_id);
    if (idx != (size_t)-1) {
        return idx;
    }
    if (tc->whisper_seen_count < FSDK_TEXTCHAT_MAX_WHISPER_PEERS) {
        idx = tc->whisper_seen_count++;
    } else {
        size_t i;
        long long min_touch = tc->whisper_seen[0].touched_at_ms;
        idx = 0;
        for (i = 1; i < FSDK_TEXTCHAT_MAX_WHISPER_PEERS; i++) {
            if (tc->whisper_seen[i].touched_at_ms < min_touch) {
                min_touch = tc->whisper_seen[i].touched_at_ms;
                idx = i;
            }
        }
    }
    copy_bounded(tc->whisper_seen[idx].foundry_id, sizeof(tc->whisper_seen[idx].foundry_id), foundry_id);
    tc->whisper_seen[idx].last_seen_id = 0;
    tc->whisper_seen[idx].touched_at_ms = tc->clock_now_ms;
    return idx;
}

/* Polls every unread conversation and surfaces new FROM-PEER messages as
 * WHISPER_IN lines, oldest first, then marks the conversation read. Only
 * called while whisper is bound PLATFORM (see fsdk_textchat_tick). */
static void poll_whispers(fsdk_textchat* tc) {
    fsdk_dm_conversation convos[FSDK_TEXTCHAT_MAX_WHISPER_PEERS];
    size_t convo_count = 0;
    size_t i;
    fsdk_result rc = fsdk_dm_conversations(tc->client, convos, FSDK_TEXTCHAT_MAX_WHISPER_PEERS, &convo_count);
    if (rc != FSDK_OK) {
        fsdk_log(FSDK_LOG_DEBUG, "fsdk textchat: whisper poll (conversations) failed");
        return;
    }
    for (i = 0; i < convo_count; i++) {
        fsdk_dm_message hist[50];
        size_t hist_count = 0;
        size_t idx;
        size_t fresh_count;
        size_t j;
        long long last_seen;
        long long new_last_seen;

        if (convos[i].unread <= 0) {
            continue;
        }
        rc = fsdk_dm_history(tc->client, convos[i].foundry_id, hist, 50, &hist_count);
        if (rc != FSDK_OK) {
            fsdk_log(FSDK_LOG_DEBUG, "fsdk textchat: whisper poll (history) failed");
            continue;
        }
        idx = ensure_seen_slot(tc, convos[i].foundry_id);
        last_seen = tc->whisper_seen[idx].last_seen_id;
        new_last_seen = last_seen;

        /* hist is newest-first; the "fresh" prefix is everything with an id
         * above what we have already surfaced (ids are monotonic, so the
         * first id <= last_seen ends the fresh run). */
        fresh_count = 0;
        for (j = 0; j < hist_count; j++) {
            if (hist[j].id <= last_seen) {
                break;
            }
            fresh_count++;
        }
        /* Walk the fresh run backwards (oldest of the fresh set first) so
         * WHISPER_IN lines are recorded in chronological order. */
        while (fresh_count > 0) {
            size_t hi = fresh_count - 1;
            fresh_count--;
            if (hist[hi].id > new_last_seen) {
                new_last_seen = hist[hi].id;
            }
            if (hist[hi].from_me) {
                continue;
            }
            record_line(tc, FSDK_TEXTCHAT_WHISPER, FSDK_TEXTCHAT_LINE_WHISPER_IN,
                        convos[i].foundry_id, convos[i].display_name,
                        convos[i].foundry_id, convos[i].display_name, hist[hi].body);
            copy_bounded(tc->reply_target_id, sizeof(tc->reply_target_id), convos[i].foundry_id);
            copy_bounded(tc->reply_target_name, sizeof(tc->reply_target_name), convos[i].display_name);
            copy_bounded(tc->whisper_target_id, sizeof(tc->whisper_target_id), convos[i].foundry_id);
            copy_bounded(tc->whisper_target_name, sizeof(tc->whisper_target_name), convos[i].display_name);
        }
        tc->whisper_seen[idx].last_seen_id = new_last_seen;
        tc->whisper_seen[idx].touched_at_ms = tc->clock_now_ms;
        {
            fsdk_result mrc = fsdk_dm_mark_read(tc->client, convos[i].foundry_id);
            if (mrc != FSDK_OK) {
                /* Local seen-ids already prevent a re-emit next poll. */
                fsdk_log(FSDK_LOG_DEBUG, "fsdk textchat: whisper mark_read failed");
            }
        }
    }
}

/* ---- outbound sends --------------------------------------------------------- */

static fsdk_result send_party(fsdk_textchat* tc, const char* body) {
    fsdk_textchat_bind bind = tc->bind[FSDK_TEXTCHAT_PARTY];
    fsdk_result rc;

    if (bind == FSDK_TEXTCHAT_BIND_OFF) {
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Party chat is not available.");
        return FSDK_ERR_UNAVAILABLE;
    }
    if (bind == FSDK_TEXTCHAT_BIND_LOCAL) {
        record_line(tc, FSDK_TEXTCHAT_PARTY, FSDK_TEXTCHAT_LINE_CHAT,
                    self_foundry_id(tc), self_display_name(tc), "", "", body);
        return FSDK_OK;
    }
    if (bind == FSDK_TEXTCHAT_BIND_HOST) {
        if (tc->host_send == NULL) {
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, "No party chat transport.");
            return FSDK_ERR_UNAVAILABLE;
        }
        rc = tc->host_send(FSDK_TEXTCHAT_PARTY, body, tc->host_send_user_data);
        if (rc == FSDK_OK) {
            record_line(tc, FSDK_TEXTCHAT_PARTY, FSDK_TEXTCHAT_LINE_CHAT,
                        self_foundry_id(tc), self_display_name(tc), "", "", body);
            return FSDK_OK;
        }
        record_send_error(tc, rc);
        return rc;
    }

    /* PLATFORM */
    if (strlen(body) > 500) {
        note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Message too long (max 500).");
        return FSDK_ERR_INVALID_ARG;
    }
    if (tc->chat == NULL) {
        note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Party chat is not connected.");
        return FSDK_ERR_UNAVAILABLE;
    }
    rc = fsdk_chat_send_channel(tc->chat, FSDK_CHAT_CHANNEL_PARTY, body);
    if (rc == FSDK_OK) {
        return FSDK_OK; /* No local echo - the server fans it back via room.message. */
    }
    if (rc == FSDK_ERR_UNAVAILABLE) {
        note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Party chat is not connected.");
        return rc;
    }
    record_send_error(tc, rc);
    return rc;
}

static fsdk_result send_whisper(fsdk_textchat* tc, const char* peer_id, const char* peer_name,
                                const char* body) {
    fsdk_result rc;
    if (strlen(body) > 2000) {
        note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Message too long (max 2000).");
        return FSDK_ERR_INVALID_ARG;
    }
    rc = fsdk_dm_send(tc->client, peer_id, body);
    if (rc == FSDK_OK) {
        record_line(tc, FSDK_TEXTCHAT_WHISPER, FSDK_TEXTCHAT_LINE_WHISPER_OUT,
                    self_foundry_id(tc), self_display_name(tc), peer_id, peer_name, body);
        return FSDK_OK;
    }
    record_send_error(tc, rc);
    return rc;
}

/* ---- command-line parsing helpers ------------------------------------------- */

static void trim_copy(char* out, size_t out_sz, const char* in) {
    size_t len = strlen(in);
    size_t start = 0;
    size_t end = len;
    size_t n;
    while (start < len && ascii_isspace(in[start])) {
        start++;
    }
    while (end > start && ascii_isspace(in[end - 1])) {
        end--;
    }
    n = end - start;
    if (out_sz == 0) {
        return;
    }
    if (n + 1 > out_sz) {
        n = out_sz - 1;
    }
    memcpy(out, in + start, n);
    out[n] = '\0';
}

/* Reads one token from s (double-quoted allows embedded whitespace) into out
 * (bounded), then skips trailing whitespace. Returns a pointer, into the
 * ORIGINAL string s, at the first char after the token (and any whitespace
 * that followed it). */
static const char* parse_first_token(const char* s, char* out, size_t out_sz) {
    size_t i = 0;
    if (out_sz > 0) {
        out[0] = '\0';
    }
    if (*s == '"') {
        s++;
        while (*s != '\0' && *s != '"') {
            if (i + 1 < out_sz) {
                out[i++] = *s;
            }
            s++;
        }
        if (out_sz > 0) {
            out[i] = '\0';
        }
        if (*s == '"') {
            s++;
        }
    } else {
        while (*s != '\0' && !ascii_isspace(*s)) {
            if (i + 1 < out_sz) {
                out[i++] = *s;
            }
            s++;
        }
        if (out_sz > 0) {
            out[i] = '\0';
        }
    }
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

#define TEXTCHAT_MAX_ARGS 8
#define TEXTCHAT_ARG_LEN 128

/* Splits the WHOLE command line (name + args, quote-aware) into argv/storage. */
static int tokenize_full(const char* s, char storage[TEXTCHAT_MAX_ARGS][TEXTCHAT_ARG_LEN],
                         const char** argv) {
    int argc = 0;
    while (*s != '\0' && argc < TEXTCHAT_MAX_ARGS) {
        s = parse_first_token(s, storage[argc], TEXTCHAT_ARG_LEN);
        if (storage[argc][0] == '\0') {
            break; /* nothing left to tokenize */
        }
        argv[argc] = storage[argc];
        argc++;
    }
    return argc;
}

/* ---- built-in / reserved command tables ------------------------------------- */

static int is_whisper_alias(const char* n) {
    return strcmp(n, "w") == 0 || strcmp(n, "whisper") == 0 || strcmp(n, "msg") == 0
           || strcmp(n, "tell") == 0 || strcmp(n, "dm") == 0;
}
static int is_reply_alias(const char* n) {
    return strcmp(n, "r") == 0 || strcmp(n, "reply") == 0;
}
static int is_party_alias(const char* n) {
    return strcmp(n, "p") == 0 || strcmp(n, "party") == 0;
}
static int is_help_alias(const char* n) {
    return strcmp(n, "help") == 0 || strcmp(n, "?") == 0;
}
static int is_reserved_alias(const char* n) {
    return strcmp(n, "t") == 0 || strcmp(n, "team") == 0 || strcmp(n, "a") == 0
           || strcmp(n, "all") == 0 || strcmp(n, "g") == 0 || strcmp(n, "global") == 0;
}
static int is_builtin_or_reserved(const char* n) {
    return is_whisper_alias(n) || is_reply_alias(n) || is_party_alias(n)
           || is_help_alias(n) || is_reserved_alias(n);
}

static void cmd_whisper(fsdk_textchat* tc, const char* rest) {
    char target[128];
    const char* text;
    char peer_id[64];
    char peer_name[128];
    resolve_status rs;

    if (tc->bind[FSDK_TEXTCHAT_WHISPER] == FSDK_TEXTCHAT_BIND_OFF) {
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Whispers need Foundry social - not available here.");
        return;
    }
    if (rest[0] == '\0') {
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Usage: /w <friend> <message>");
        return;
    }
    text = parse_first_token(rest, target, sizeof(target));
    if (target[0] == '\0' || text[0] == '\0') {
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Usage: /w <friend> <message>");
        return;
    }
    rs = resolve_friend(tc, target, peer_id, sizeof(peer_id), peer_name, sizeof(peer_name));
    if (rs == RESOLVE_AMBIGUOUS) {
        char msg[192];
        (void)snprintf(msg, sizeof(msg), "Several friends are named %s - use their @username.", target);
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, msg);
        return;
    }
    if (rs == RESOLVE_NOT_FOUND) {
        char msg[192];
        (void)snprintf(msg, sizeof(msg), "No friend named %s.", target);
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, msg);
        return;
    }
    if (send_whisper(tc, peer_id, peer_name, text) == FSDK_OK) {
        copy_bounded(tc->whisper_target_id, sizeof(tc->whisper_target_id), peer_id);
        copy_bounded(tc->whisper_target_name, sizeof(tc->whisper_target_name), peer_name);
    }
}

static void cmd_reply(fsdk_textchat* tc, const char* rest) {
    if (tc->reply_target_id[0] == '\0') {
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Nobody has whispered you yet.");
        return;
    }
    if (rest[0] == '\0') {
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Usage: /r <message>");
        return;
    }
    (void)send_whisper(tc, tc->reply_target_id, tc->reply_target_name, rest);
}

static void cmd_party(fsdk_textchat* tc, const char* rest) {
    if (rest[0] != '\0') {
        (void)send_party(tc, rest);
        return;
    }
    tc->active_channel = FSDK_TEXTCHAT_PARTY;
    note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Party chat selected.");
}

static void cmd_help(fsdk_textchat* tc) {
    size_t i;
    note(tc, FSDK_TEXTCHAT_LINE_SYSTEM,
         "/w /whisper /msg /tell /dm <friend> <message>  - whisper a friend");
    note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "/r /reply <message>  - reply to the last whisper");
    note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "/p /party [message]  - party chat");
    note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "/help");
    for (i = 0; i < tc->command_count; i++) {
        char msg[FSDK_TEXTCHAT_CMD_NAME_MAX + FSDK_TEXTCHAT_CMD_HELP_MAX + 8];
        (void)snprintf(msg, sizeof(msg), "/%s  - %s", tc->commands[i].name, tc->commands[i].help);
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, msg);
    }
}

static void dispatch_command(fsdk_textchat* tc, const char* cmd_line) {
    char name[FSDK_TEXTCHAT_CMD_NAME_MAX];
    const char* rest;
    size_t i = 0;
    size_t k;

    while (cmd_line[i] != '\0' && !ascii_isspace(cmd_line[i])) {
        if (i + 1 < sizeof(name)) {
            name[i] = ascii_tolower(cmd_line[i]);
        }
        i++;
    }
    name[i < sizeof(name) - 1 ? i : sizeof(name) - 1] = '\0';
    rest = cmd_line + i;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }

    if (name[0] == '\0') {
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Unknown command: /. Type /help.");
        return;
    }
    if (is_whisper_alias(name)) {
        cmd_whisper(tc, rest);
        return;
    }
    if (is_reply_alias(name)) {
        cmd_reply(tc, rest);
        return;
    }
    if (is_party_alias(name)) {
        cmd_party(tc, rest);
        return;
    }
    if (is_help_alias(name)) {
        cmd_help(tc);
        return;
    }
    if (is_reserved_alias(name)) {
        char msg[64];
        (void)snprintf(msg, sizeof(msg), "/%s is not available in this game.", name);
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, msg);
        return;
    }
    for (k = 0; k < tc->command_count; k++) {
        if (strcmp(tc->commands[k].name, name) == 0) {
            char storage[TEXTCHAT_MAX_ARGS][TEXTCHAT_ARG_LEN];
            const char* argv[TEXTCHAT_MAX_ARGS];
            int argc = tokenize_full(cmd_line, storage, argv);
            tc->commands[k].fn(tc, argc, argv, rest, tc->commands[k].user_data);
            return;
        }
    }
    {
        char msg[64];
        (void)snprintf(msg, sizeof(msg), "Unknown command: /%s. Type /help.", name);
        note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, msg);
    }
}

static fsdk_result send_active(fsdk_textchat* tc, const char* text) {
    switch (tc->active_channel) {
        case FSDK_TEXTCHAT_PARTY:
            return send_party(tc, text);
        case FSDK_TEXTCHAT_WHISPER:
            if (tc->whisper_target_id[0] == '\0') {
                note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "No one to whisper. Use /w <friend> <message>.");
                return FSDK_ERR_INVALID_ARG;
            }
            return send_whisper(tc, tc->whisper_target_id, tc->whisper_target_name, text);
        case FSDK_TEXTCHAT_SYSTEM:
        default:
            note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "The system channel is read-only.");
            return FSDK_ERR_INVALID_ARG;
    }
}

/* ---- rejoin-on-drop (PLATFORM binding, chat != NULL) ----------------------- */

static int chat_has_any_join_path(const fsdk_chat* chat) {
    int i;
    for (i = 0; i < FSDK_CHAT_CHANNEL__COUNT; i++) {
        if (chat->rooms[i].join_path[0] != '\0') {
            return 1;
        }
    }
    return 0;
}

static void handle_rejoin(fsdk_textchat* tc, long long now_ms) {
    int ws_up = tc->chat->ws_handle != NULL;
    int had_any_join_path;

    if (ws_up) {
        if (tc->rejoin_active) {
            if (tc->rejoin_disconnected_notified) {
                note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Chat reconnected.");
            }
            tc->rejoin_active = 0;
            tc->rejoin_disconnected_notified = 0;
            tc->rejoin_backoff_ms = 0;
            tc->rejoin_next_at_ms = 0;
        }
        return;
    }

    had_any_join_path = chat_has_any_join_path(tc->chat);
    if (!had_any_join_path) {
        tc->rejoin_active = 0;
        return;
    }
    if (!tc->rejoin_active) {
        tc->rejoin_active = 1;
        tc->rejoin_disconnected_notified = 0;
        tc->rejoin_backoff_ms = 1000;
        tc->rejoin_next_at_ms = now_ms + 1000;
        tc->rejoin_disconnected_since_ms = now_ms;
        return; /* the tick that FIRST notices the drop only schedules. */
    }
    if (now_ms < tc->rejoin_next_at_ms) {
        return;
    }

    {
        fsdk_result rc = fsdk_chat_rejoin_all(tc->chat);
        if (rc != FSDK_OK) {
            fsdk_log(FSDK_LOG_DEBUG, "fsdk textchat: rejoin attempt failed");
        }
        tc->rejoin_backoff_ms *= 2;
        if (tc->rejoin_backoff_ms > 30000) {
            tc->rejoin_backoff_ms = 30000;
        }
        tc->rejoin_next_at_ms = now_ms + tc->rejoin_backoff_ms;
        if (!tc->rejoin_disconnected_notified
                && now_ms - tc->rejoin_disconnected_since_ms >= 30000) {
            note(tc, FSDK_TEXTCHAT_LINE_SYSTEM, "Chat disconnected - reconnecting...");
            tc->rejoin_disconnected_notified = 1;
        }
    }
}

/* ---- fsdk_chat message bridge ------------------------------------------------ */

static void on_chat_message(const fsdk_chat_message* msg, void* user_data) {
    fsdk_textchat* tc = (fsdk_textchat*)user_data;
    if (tc == NULL || msg == NULL) {
        return;
    }
    if (tc->room_passthrough != NULL) {
        tc->room_passthrough(msg, tc->room_passthrough_user_data);
    }
    if (msg->channel != FSDK_CHAT_CHANNEL_PARTY) {
        return; /* GLOBAL/MATCH/TEAM are out of the v1 textchat channel set. */
    }
    record_line(tc, FSDK_TEXTCHAT_PARTY, FSDK_TEXTCHAT_LINE_CHAT,
                msg->from_foundry_id, msg->display_name, "", "", msg->body);
}

/* ---- binding resolution ------------------------------------------------------ */

static fsdk_textchat_bind preset_default(fsdk_textchat_mode mode, fsdk_textchat_channel ch) {
    if (mode == FSDK_TEXTCHAT_MODE_PLATFORM) {
        return FSDK_TEXTCHAT_BIND_PLATFORM;
    }
    return ch == FSDK_TEXTCHAT_PARTY ? FSDK_TEXTCHAT_BIND_HOST : FSDK_TEXTCHAT_BIND_OFF;
}

/* ---- public API -------------------------------------------------------------- */

void fsdk_textchat_config_default(fsdk_textchat_config* out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->mode = FSDK_TEXTCHAT_MODE_PLATFORM;
    out->log_sink_min_level = FSDK_LOG_WARN;
}

fsdk_result fsdk_textchat_create(fsdk_client* client, fsdk_chat* chat_or_null,
                                 const fsdk_textchat_config* config_or_null,
                                 fsdk_textchat** out) {
    fsdk_textchat_config cfg;
    fsdk_textchat* tc;
    int ch;

    if (out == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (config_or_null != NULL) {
        cfg = *config_or_null;
    } else {
        fsdk_textchat_config_default(&cfg);
    }
    if (cfg.mode == FSDK_TEXTCHAT_MODE_PLATFORM && chat_or_null == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }

    tc = (fsdk_textchat*)calloc(1, sizeof(*tc));
    if (tc == NULL) {
        return FSDK_ERR_INTERNAL;
    }
    tc->client = client;
    tc->chat = chat_or_null;
    tc->mode = cfg.mode;
    tc->history_lines = cfg.history_lines > 0 ? cfg.history_lines : 200;
    tc->whisper_poll_ms = cfg.whisper_poll_ms > 0 ? cfg.whisper_poll_ms : 5000;
    tc->friends_refresh_ms = cfg.friends_refresh_ms > 0 ? cfg.friends_refresh_ms : 120000;
    tc->log_sink_min_level = cfg.log_sink_min_level;
    tc->next_line_id = 1;
    tc->active_channel = FSDK_TEXTCHAT_PARTY;

    for (ch = 0; ch < FSDK_TEXTCHAT_CHANNEL__COUNT; ch++) {
        if (ch == FSDK_TEXTCHAT_SYSTEM) {
            tc->bind[ch] = FSDK_TEXTCHAT_BIND_LOCAL; /* Always, regardless of any override. */
        } else if (cfg.bind[ch] != FSDK_TEXTCHAT_BIND_PLATFORM) {
            tc->bind[ch] = cfg.bind[ch];
        } else {
            tc->bind[ch] = preset_default(cfg.mode, (fsdk_textchat_channel)ch);
        }
        ring_init(&tc->rings[ch], (size_t)tc->history_lines);
    }

    if (tc->mode == FSDK_TEXTCHAT_MODE_PLATFORM && client->authenticated) {
        refresh_friends(tc);
    }
    if (tc->chat != NULL) {
        fsdk_chat_set_message_callback(tc->chat, on_chat_message, tc);
    }

    *out = tc;
    return FSDK_OK;
}

void fsdk_textchat_destroy(fsdk_textchat* tc) {
    int i;
    if (tc == NULL) {
        return;
    }
    if (tc->chat != NULL) {
        fsdk_chat_set_message_callback(tc->chat, NULL, NULL);
    }
    for (i = 0; i < FSDK_TEXTCHAT_CHANNEL__COUNT; i++) {
        ring_destroy(&tc->rings[i]);
    }
    free(tc);
}

void fsdk_textchat_set_line_callback(fsdk_textchat* tc, fsdk_textchat_line_fn fn, void* user_data) {
    if (tc == NULL) {
        return;
    }
    tc->on_line = fn;
    tc->on_line_user_data = user_data;
}

void fsdk_textchat_set_host_send(fsdk_textchat* tc, fsdk_textchat_host_send_fn fn, void* user_data) {
    if (tc == NULL) {
        return;
    }
    tc->host_send = fn;
    tc->host_send_user_data = user_data;
}

void fsdk_textchat_set_room_passthrough(fsdk_textchat* tc, fsdk_chat_message_fn fn, void* user_data) {
    if (tc == NULL) {
        return;
    }
    tc->room_passthrough = fn;
    tc->room_passthrough_user_data = user_data;
}

fsdk_result fsdk_textchat_inject(fsdk_textchat* tc, fsdk_textchat_channel channel,
                                 const char* from_name, const char* from_foundry_id_or_null,
                                 const char* body) {
    const char* from_id;
    if (tc == NULL || body == NULL || body[0] == '\0'
            || channel < 0 || channel >= FSDK_TEXTCHAT_CHANNEL__COUNT
            || channel == FSDK_TEXTCHAT_SYSTEM) {
        return FSDK_ERR_INVALID_ARG;
    }
    from_id = from_foundry_id_or_null != NULL ? from_foundry_id_or_null : "";
    if (channel == FSDK_TEXTCHAT_WHISPER) {
        record_line(tc, channel, FSDK_TEXTCHAT_LINE_WHISPER_IN,
                    from_id, from_name != NULL ? from_name : "",
                    from_id, from_name != NULL ? from_name : "", body);
    } else {
        record_line(tc, channel, FSDK_TEXTCHAT_LINE_CHAT,
                    from_id, from_name != NULL ? from_name : "", "", "", body);
    }
    return FSDK_OK;
}

fsdk_result fsdk_textchat_submit(fsdk_textchat* tc, const char* line) {
    char buf[FSDK_TEXTCHAT_BODY_MAX + 8];
    if (tc == NULL || line == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    trim_copy(buf, sizeof(buf), line);
    if (buf[0] == '\0') {
        return FSDK_ERR_INVALID_ARG;
    }
    if (buf[0] == '/') {
        if (buf[1] == '/') {
            (void)send_active(tc, buf + 1); /* the literal "/text", single-slashed */
            return FSDK_OK;
        }
        dispatch_command(tc, buf + 1);
        return FSDK_OK;
    }
    (void)send_active(tc, buf);
    return FSDK_OK;
}

fsdk_result fsdk_textchat_set_active(fsdk_textchat* tc, fsdk_textchat_channel channel) {
    if (tc == NULL || channel < 0 || channel >= FSDK_TEXTCHAT_CHANNEL__COUNT) {
        return FSDK_ERR_INVALID_ARG;
    }
    tc->active_channel = channel;
    return FSDK_OK;
}

fsdk_textchat_channel fsdk_textchat_active(const fsdk_textchat* tc) {
    return tc != NULL ? tc->active_channel : FSDK_TEXTCHAT_PARTY;
}

fsdk_result fsdk_textchat_set_party(fsdk_textchat* tc, const char* party_id_or_null) {
    fsdk_textchat_bind bind;
    if (tc == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    bind = tc->bind[FSDK_TEXTCHAT_PARTY];
    if (party_id_or_null == NULL || party_id_or_null[0] == '\0') {
        tc->party_id[0] = '\0';
        if (bind == FSDK_TEXTCHAT_BIND_PLATFORM && tc->chat != NULL) {
            return fsdk_chat_leave_party(tc->chat);
        }
        return FSDK_OK;
    }
    copy_bounded(tc->party_id, sizeof(tc->party_id), party_id_or_null);
    if (bind == FSDK_TEXTCHAT_BIND_PLATFORM) {
        fsdk_result rc;
        if (tc->chat == NULL) {
            return FSDK_ERR_INVALID_ARG;
        }
        rc = fsdk_chat_join_party(tc->chat, party_id_or_null);
        if (rc != FSDK_OK) {
            fsdk_log(FSDK_LOG_WARN, "fsdk textchat: party join failed");
            note(tc, FSDK_TEXTCHAT_LINE_ERROR, "Party chat is not connected.");
        }
        return rc;
    }
    return FSDK_OK; /* HOST/LOCAL: remembered locally only, no network. */
}

fsdk_result fsdk_textchat_system(fsdk_textchat* tc, fsdk_log_level level, const char* text) {
    if (tc == NULL || text == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    note(tc, level == FSDK_LOG_ERROR ? FSDK_TEXTCHAT_LINE_ERROR : FSDK_TEXTCHAT_LINE_SYSTEM, text);
    return FSDK_OK;
}

void fsdk_textchat_log_sink(fsdk_log_level level, const char* message, void* tc_ptr) {
    fsdk_textchat* tc = (fsdk_textchat*)tc_ptr;
    if (tc == NULL || message == NULL || level < tc->log_sink_min_level) {
        return;
    }
    if (tc->log_sink_reentrant_guard) {
        return; /* recording a line must never re-enter this sink. */
    }
    tc->log_sink_reentrant_guard = 1;
    note(tc, level == FSDK_LOG_ERROR ? FSDK_TEXTCHAT_LINE_ERROR : FSDK_TEXTCHAT_LINE_SYSTEM, message);
    tc->log_sink_reentrant_guard = 0;
}

fsdk_result fsdk_textchat_register_command(fsdk_textchat* tc, const char* name, const char* help,
                                           fsdk_textchat_command_fn fn, void* user_data) {
    char lname[FSDK_TEXTCHAT_CMD_NAME_MAX];
    size_t len;
    size_t i;
    if (tc == NULL || name == NULL || name[0] == '\0' || fn == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    len = strlen(name);
    if (len > FSDK_TEXTCHAT_CMD_NAME_MAX - 1) {
        return FSDK_ERR_INVALID_ARG;
    }
    for (i = 0; i < len; i++) {
        char c = name[i];
        if (c == '/' || ascii_isspace(c)) {
            return FSDK_ERR_INVALID_ARG;
        }
        lname[i] = ascii_tolower(c);
    }
    lname[len] = '\0';
    if (is_builtin_or_reserved(lname)) {
        return FSDK_ERR_INVALID_ARG;
    }
    for (i = 0; i < tc->command_count; i++) {
        if (strcmp(tc->commands[i].name, lname) == 0) {
            tc->commands[i].fn = fn;
            tc->commands[i].user_data = user_data;
            copy_bounded(tc->commands[i].help, sizeof(tc->commands[i].help), help != NULL ? help : "");
            return FSDK_OK;
        }
    }
    if (tc->command_count >= FSDK_TEXTCHAT_MAX_COMMANDS) {
        return FSDK_ERR_INTERNAL;
    }
    copy_bounded(tc->commands[tc->command_count].name, sizeof(tc->commands[tc->command_count].name), lname);
    copy_bounded(tc->commands[tc->command_count].help, sizeof(tc->commands[tc->command_count].help),
                help != NULL ? help : "");
    tc->commands[tc->command_count].fn = fn;
    tc->commands[tc->command_count].user_data = user_data;
    tc->command_count++;
    return FSDK_OK;
}

size_t fsdk_textchat_command_completions(const fsdk_textchat* tc, const char* prefix,
                                         const char** out_names, size_t capacity) {
    static const char* const kBuiltins[] = {
        "w", "whisper", "msg", "tell", "dm", "r", "reply", "p", "party", "help", "?",
        "t", "team", "a", "all", "g", "global"
    };
    size_t n = 0;
    size_t plen;
    size_t i;
    if (tc == NULL || out_names == NULL || capacity == 0) {
        return 0;
    }
    plen = prefix != NULL ? strlen(prefix) : 0;
    for (i = 0; i < sizeof(kBuiltins) / sizeof(kBuiltins[0]) && n < capacity; i++) {
        if (plen == 0 || strncmp(kBuiltins[i], prefix, plen) == 0) {
            out_names[n++] = kBuiltins[i];
        }
    }
    for (i = 0; i < tc->command_count && n < capacity; i++) {
        if (plen == 0 || strncmp(tc->commands[i].name, prefix, plen) == 0) {
            out_names[n++] = tc->commands[i].name;
        }
    }
    return n;
}

static int cmp_line_by_id(const void* a, const void* b) {
    const fsdk_textchat_line* la = (const fsdk_textchat_line*)a;
    const fsdk_textchat_line* lb = (const fsdk_textchat_line*)b;
    if (la->id < lb->id) {
        return -1;
    }
    if (la->id > lb->id) {
        return 1;
    }
    return 0;
}

size_t fsdk_textchat_history(const fsdk_textchat* tc, int channel_or_minus1,
                             fsdk_textchat_line* out, size_t capacity) {
    if (tc == NULL || out == NULL || capacity == 0) {
        return 0;
    }
    if (channel_or_minus1 >= 0 && channel_or_minus1 < FSDK_TEXTCHAT_CHANNEL__COUNT) {
        return ring_copy_tail(&tc->rings[(size_t)channel_or_minus1], out, capacity);
    }
    if (channel_or_minus1 == -1) {
        size_t total = tc->rings[0].count + tc->rings[1].count + tc->rings[2].count;
        fsdk_textchat_line* tmp;
        size_t n = 0;
        size_t ch;
        size_t result;
        size_t skip;
        size_t i;
        if (total == 0) {
            return 0;
        }
        tmp = (fsdk_textchat_line*)malloc(total * sizeof(fsdk_textchat_line));
        if (tmp == NULL) {
            return 0;
        }
        for (ch = 0; ch < FSDK_TEXTCHAT_CHANNEL__COUNT; ch++) {
            n += ring_copy_tail(&tc->rings[ch], tmp + n, tc->rings[ch].count);
        }
        qsort(tmp, n, sizeof(fsdk_textchat_line), cmp_line_by_id);
        result = n < capacity ? n : capacity;
        skip = n - result;
        for (i = 0; i < result; i++) {
            out[i] = tmp[skip + i];
        }
        free(tmp);
        return result;
    }
    return 0;
}

void fsdk_textchat_set_whisper_polling(fsdk_textchat* tc, int enabled) {
    if (tc == NULL) {
        return;
    }
    if (enabled && !tc->whisper_polling_enabled) {
        tc->whisper_poll_due_now = 1; /* fire on the very next tick */
    }
    tc->whisper_polling_enabled = enabled ? 1 : 0;
}

void fsdk_textchat_tick(fsdk_textchat* tc, long long now_ms) {
    if (tc == NULL) {
        return;
    }
    tc->clock_now_ms = now_ms;
    if (tc->chat != NULL) {
        (void)fsdk_chat_tick(tc->chat, now_ms);
        handle_rejoin(tc, now_ms);
    }
    if (tc->whisper_polling_enabled && tc->bind[FSDK_TEXTCHAT_WHISPER] == FSDK_TEXTCHAT_BIND_PLATFORM) {
        if (tc->whisper_poll_due_now || now_ms >= tc->next_whisper_poll_ms) {
            poll_whispers(tc);
            tc->next_whisper_poll_ms = now_ms + tc->whisper_poll_ms;
            tc->whisper_poll_due_now = 0;
        }
        if (!tc->friends_ever_loaded
                || now_ms - tc->friends_last_refresh_ms >= tc->friends_refresh_ms) {
            refresh_friends(tc);
        }
    }
}
