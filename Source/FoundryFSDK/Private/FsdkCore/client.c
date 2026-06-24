/*
 * client.c - client-side SDK (runs inside the game client).
 *
 * SECURITY: this code ships to players and is assumed fully reverse-engineered.
 * It holds NO secrets. The only credential is the player's own FID session
 * token, passed in via fsdk_authenticate and kept in memory only. It calls ONLY
 * the player-scoped endpoint set (auth + matchmaking + receive-connection) -
 * never an admin/operator path. See SECURITY.md and docs/contracts/.
 *
 * The network itself is performed by the HOST-installed transport
 * (fsdk_set_http_transport); this file builds the player-scoped requests, sends
 * the player bearer token, and maps responses. With no transport installed every
 * networked call fails closed (FSDK_NOT_IMPLEMENTED).
 */
#include "fsdk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Live fid routes (FMMS = Foundry Matchmaking Service). The session probe is the
 * fid-native /v1/me/user (the proxied /v1/me drops the bearer). connection/cancel
 * routes are designed and wired here, relay-ready; fid deploys them as a follow-on
 * (until then they return "not ready"). */
#define FSDK_PATH_ME            "/v1/me/user"
#define FSDK_PATH_TICKETS       "/v1/fmms/tickets"

/* Small internal helper: heap-duplicate a NUL-terminated string. Returns NULL
 * on NULL input or allocation failure. */
static char* fsdk_strdup(const char* s) {
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

/* Bounded copy into a fixed buffer (always NUL-terminates; no CRT _s funcs). */
static void copy_bounded(char* dst, size_t dst_sz, const char* src) {
    if (dst == NULL || dst_sz == 0) {
        return;
    }
    size_t i = 0;
    for (; src != NULL && src[i] != '\0' && i + 1 < dst_sz; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* Map an HTTP status to a result. 2xx -> OK; 401/403 -> UNAUTHORIZED (the
 * server-side authz boundary); 404 -> NO_MATCH (not found / not yet ready);
 * 408/504 -> TIMEOUT; anything else unexpected -> PROTOCOL. */
static fsdk_result http_status_to_result(long status) {
    if (status >= 200 && status < 300) {
        return FSDK_OK;
    }
    if (status == 401 || status == 403) {
        return FSDK_ERR_UNAUTHORIZED;
    }
    if (status == 404) {
        return FSDK_ERR_NO_MATCH;
    }
    if (status == 408 || status == 504) {
        return FSDK_ERR_TIMEOUT;
    }
    return FSDK_ERR_PROTOCOL;
}

/* Map a fid TicketState name to the SDK's match status. */
static fsdk_match_status ticket_state_to_status(const char* state) {
    if (state == NULL) {
        return FSDK_MATCH_PENDING;
    }
    if (strcmp(state, "QUEUED") == 0) {
        return FSDK_MATCH_SEARCHING;
    }
    if (strcmp(state, "MATCHED") == 0) {
        return FSDK_MATCH_FOUND;
    }
    if (strcmp(state, "EXPIRED") == 0) {
        return FSDK_MATCH_EXPIRED;
    }
    if (strcmp(state, "CANCELED") == 0) {
        return FSDK_MATCH_CANCELLED;
    }
    return FSDK_MATCH_PENDING;
}

/* --- Minimal JSON field readers -------------------------------------------
 * NOT a general JSON parser: these read only the flat fields the client needs
 * from the small, well-known fid JsonApiResponse shapes (the "data" object).
 * A production core links a real JSON library; this stays zero-dependency. */

/* Return a pointer to the value just after `"key":` (skipping whitespace), or
 * NULL. Search begins at `from`. Matches a quoted key only. */
static const char* json_value_after(const char* from, const char* key) {
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

/* Narrow to the envelope's "data" object so field lookups don't collide with
 * keys in "meta"/"errors". Falls back to the whole body if absent. */
static const char* json_data_object(const char* body) {
    const char* v = json_value_after(body, "data");
    return (v != NULL && *v == '{') ? v : body;
}

/* Extract a string field's value into out (bounded). Returns 1 on success. */
static int json_extract_string(const char* body, const char* key,
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

/* Extract an integer field's value. Returns 1 on success. */
static int json_extract_int(const char* body, const char* key, long* out) {
    const char* v = json_value_after(body, key);
    if (v == NULL) {
        return 0;
    }
    char* end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) {
        return 0;
    }
    *out = n;
    return 1;
}

/* Append a JSON-escaped copy of s (without surrounding quotes) into out. */
static void json_escape(const char* s, char* out, size_t out_sz) {
    size_t i = 0;
    for (; s != NULL && *s != '\0'; s++) {
        const char* esc;
        char one[2];
        switch (*s) {
            case '"':  esc = "\\\""; break;
            case '\\': esc = "\\\\"; break;
            case '\n': esc = "\\n";  break;
            case '\r': esc = "\\r";  break;
            case '\t': esc = "\\t";  break;
            default:   one[0] = *s; one[1] = '\0'; esc = one; break;
        }
        for (; *esc != '\0'; esc++) {
            if (i + 1 < out_sz) {
                out[i++] = *esc;
            }
        }
    }
    if (i < out_sz) {
        out[i] = '\0';
    } else if (out_sz > 0) {
        out[out_sz - 1] = '\0';
    }
}

/* -------------------------------------------------------------------------- */

fsdk_result fsdk_client_create(const char* base_url, fsdk_client** out_client) {
    if (base_url == NULL || out_client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_client = NULL;

    fsdk_client* client = (fsdk_client*)calloc(1, sizeof(fsdk_client));
    if (client == NULL) {
        return FSDK_ERR_INTERNAL;
    }

    client->base_url = fsdk_strdup(base_url);
    if (client->base_url == NULL) {
        free(client);
        return FSDK_ERR_INTERNAL;
    }
    client->player_token = NULL;
    client->authenticated = 0;

    *out_client = client;
    fsdk_log(FSDK_LOG_INFO, "fsdk client created");
    return FSDK_OK;
}

void fsdk_client_destroy(fsdk_client* client) {
    if (client == NULL) {
        return;
    }
    /* Best-effort scrub of the in-memory player token before freeing. */
    if (client->player_token != NULL) {
        memset(client->player_token, 0, strlen(client->player_token));
        free(client->player_token);
    }
    free(client->base_url);
    free(client);
}

fsdk_result fsdk_authenticate(fsdk_client* client, const char* player_token) {
    if (client == NULL || player_token == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }

    /* Store the player's FID token in memory for the client lifetime only.
     * NEVER persist it; NEVER log it. */
    char* token_copy = fsdk_strdup(player_token);
    if (token_copy == NULL) {
        return FSDK_ERR_INTERNAL;
    }
    if (client->player_token != NULL) {
        memset(client->player_token, 0, strlen(client->player_token));
        free(client->player_token);
    }
    client->player_token = token_copy;
    client->authenticated = 0;

    /* Validate the token against FID with a player-scoped "who am I" probe. The
     * SDK does NOT inspect the JWT locally - the platform is authoritative.
     *   GET {base_url}/v1/me/user   (Authorization: Bearer <player_token>)
     * 200 -> authenticated; 401/403 -> rejected. */
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_GET, FSDK_PATH_ME,
                                      client->player_token, NULL, NULL, &status);
    if (r != FSDK_OK) {
        /* No transport (NOT_IMPLEMENTED) or transport failure - fail closed. */
        return r;
    }
    if (status >= 200 && status < 300) {
        client->authenticated = 1;
        fsdk_log(FSDK_LOG_INFO, "fsdk authenticate ok");
        return FSDK_OK;
    }
    fsdk_log(FSDK_LOG_WARN, "fsdk authenticate rejected by FID");
    return http_status_to_result(status);
}

fsdk_result fsdk_request_match(fsdk_client* client,
                               const char* queue,
                               const char* attrs_json,
                               fsdk_ticket** out_ticket) {
    if (client == NULL || queue == NULL || out_ticket == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_ticket = NULL;
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    /* POST {base_url}/v1/fmms/tickets with { "queueId", "attributes" }. fid's
     * TicketRequest.attributes is a raw JSON STRING field, so attrs_json is
     * embedded as a JSON string value (escaped), or null when absent. */
    char body[1280];
    if (attrs_json != NULL && attrs_json[0] != '\0') {
        char attrs_escaped[768];
        json_escape(attrs_json, attrs_escaped, sizeof(attrs_escaped));
        snprintf(body, sizeof(body),
                 "{\"queueId\":\"%s\",\"attributes\":\"%s\"}", queue, attrs_escaped);
    } else {
        snprintf(body, sizeof(body),
                 "{\"queueId\":\"%s\",\"attributes\":null}", queue);
    }

    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_POST, FSDK_PATH_TICKETS,
                                      client->player_token, body, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        fsdk_log(FSDK_LOG_WARN, "fsdk request_match rejected");
        return http_status_to_result(status);
    }

    fsdk_ticket* ticket = (fsdk_ticket*)calloc(1, sizeof(fsdk_ticket));
    if (ticket == NULL) {
        fsdk_string_free(resp);
        return FSDK_ERR_INTERNAL;
    }

    const char* data = json_data_object(resp);
    char id_buf[128];
    char state_buf[32];
    if (json_extract_string(data, "id", id_buf, sizeof(id_buf)) && id_buf[0] != '\0') {
        ticket->ticket_id = fsdk_strdup(id_buf);
    }
    ticket->status = json_extract_string(data, "state", state_buf, sizeof(state_buf))
                         ? ticket_state_to_status(state_buf)
                         : FSDK_MATCH_PENDING;
    fsdk_string_free(resp);

    if (ticket->ticket_id == NULL) {
        /* No ticket id in the response - nothing to poll. */
        fsdk_ticket_destroy(ticket);
        return FSDK_ERR_PROTOCOL;
    }

    *out_ticket = ticket;
    fsdk_log(FSDK_LOG_INFO, "fsdk request_match accepted");
    return FSDK_OK;
}

fsdk_result fsdk_poll_match(fsdk_client* client,
                            fsdk_ticket* ticket,
                            fsdk_match_status* out_status) {
    if (client == NULL || ticket == NULL || out_status == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    if (ticket->ticket_id == NULL) {
        *out_status = ticket->status;
        return FSDK_ERR_NO_MATCH;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", FSDK_PATH_TICKETS, ticket->ticket_id);

    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_GET, path,
                                      client->player_token, NULL, &resp, &status);
    if (r != FSDK_OK) {
        *out_status = ticket->status; /* unchanged on transport failure */
        return r;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        *out_status = ticket->status;
        return http_status_to_result(status);
    }

    char state_buf[32];
    if (json_extract_string(json_data_object(resp), "state", state_buf, sizeof(state_buf))) {
        ticket->status = ticket_state_to_status(state_buf);
    }
    fsdk_string_free(resp);
    *out_status = ticket->status;
    return FSDK_OK;
}

fsdk_result fsdk_get_connection(fsdk_client* client,
                                fsdk_ticket* ticket,
                                fsdk_connection* out) {
    if (client == NULL || ticket == NULL || out == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    /* Zero the output so a caller never reads stale stack data. */
    memset(out, 0, sizeof(*out));

    if (ticket->ticket_id == NULL) {
        return FSDK_ERR_NO_MATCH;
    }

    /* GET {base_url}/v1/fmms/tickets/{id}/connection -> { ip, port, matchToken }.
     * The matchToken is a short-lived FID-signed JWT the game forwards to the
     * server, which validates it before admitting the connection.
     *
     * SECURITY: the ip:port is only ever produced here, post-allocation, to the
     * matched player. It is an OPAQUE RENDEZVOUS - a relay (SDR-style) can later
     * replace {ip,port} with a relay endpoint WITHOUT changing this ABI; bindings
     * must not assume it is the literal box. See SECURITY.md.
     *
     * The connection route is a fid follow-on; until it deploys this returns
     * FSDK_ERR_NO_MATCH (404), i.e. "not yet ready". */
    char path[288];
    snprintf(path, sizeof(path), "%s/%s/connection", FSDK_PATH_TICKETS, ticket->ticket_id);

    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_GET, path,
                                      client->player_token, NULL, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        return http_status_to_result(status);
    }

    const char* data = json_data_object(resp);
    char ip_buf[64];
    long port = 0;
    if (json_extract_string(data, "ip", ip_buf, sizeof(ip_buf))) {
        copy_bounded(out->ip, sizeof(out->ip), ip_buf);
    }
    if (json_extract_int(data, "port", &port) && port > 0 && port <= 65535) {
        out->port = (uint16_t)port;
    }
    json_extract_string(data, "matchToken", out->match_token, sizeof(out->match_token));
    fsdk_string_free(resp);

    if (out->ip[0] == '\0' || out->match_token[0] == '\0') {
        return FSDK_ERR_PROTOCOL;
    }
    return FSDK_OK;
}

fsdk_result fsdk_cancel_match(fsdk_client* client, fsdk_ticket* ticket) {
    if (client == NULL || ticket == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    /* Mark cancelled locally (best-effort) regardless of the wire outcome. */
    ticket->status = FSDK_MATCH_CANCELLED;

    if (ticket->ticket_id == NULL) {
        return FSDK_OK;
    }

    /* DELETE {base_url}/v1/fmms/tickets/{id}. A fid follow-on; 404 today. */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", FSDK_PATH_TICKETS, ticket->ticket_id);

    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_DELETE, path,
                                      client->player_token, NULL, NULL, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if ((status >= 200 && status < 300) || status == 404) {
        /* 404 = already gone -> the desired state holds. */
        return FSDK_OK;
    }
    return http_status_to_result(status);
}

void fsdk_ticket_destroy(fsdk_ticket* ticket) {
    if (ticket == NULL) {
        return;
    }
    free(ticket->ticket_id);
    free(ticket);
}
