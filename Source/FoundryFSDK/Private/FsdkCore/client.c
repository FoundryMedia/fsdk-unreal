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
/* Expose clock_gettime/CLOCK_MONOTONIC (POSIX.1b) even under a strict -std C compile:
 * the region-ping RTT clock below needs a monotonic source, and glibc hides it unless
 * this is defined. Must precede every libc include in this TU. Mirrors server.c. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 199309L
#endif

#include "fsdk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Monotonic clock for region-ping RTT measurement. GetTickCount64's ~16ms
 * granularity would blur real ping differences, so Windows uses QPC. */
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

/* Live fid routes (FMMS = Foundry Matchmaking Service). The session probe is the
 * fid-native /v1/me/user (the proxied /v1/me drops the bearer). connection/cancel
 * routes are designed and wired here, relay-ready; fid deploys them as a follow-on
 * (until then they return "not ready"). */
#define FSDK_PATH_ME            "/v1/me/user"
#define FSDK_PATH_TICKETS       "/v1/fmms/tickets"
#define FSDK_PATH_REGIONS       "/v1/fmms/regions"
#define FSDK_PATH_MY_SESSION    "/v1/fmms/my-session"

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
    if (status == 503) {
        /* No server capacity for the queue - the search can never succeed right now, so the caller
         * should stop searching and surface "no servers, try later" rather than poll forever. */
        return FSDK_ERR_UNAVAILABLE;
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

/* Narrow to the envelope's "data" object so field lookups don't collide with
 * keys in "meta"/"errors". Falls back to the whole body if absent. */
static const char* json_data_object(const char* body) {
    const char* v = json_value_after(body, "data");
    return (v != NULL && *v == '{') ? v : body;
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
    /* Best-effort scrub of the in-memory tokens before freeing (access + refresh). */
    if (client->player_token != NULL) {
        memset(client->player_token, 0, strlen(client->player_token));
        free(client->player_token);
    }
    if (client->refresh_token != NULL) {
        memset(client->refresh_token, 0, strlen(client->refresh_token));
        free(client->refresh_token);
    }
    free(client->auth_base_url);
    free(client->base_url);
    free(client);
}

/* Player-scoped "who am I" probe against FID (GET /v1/me/user with the stored
 * token). On 2xx the identity snapshot (foundryId/displayName) is cached on the
 * client for fsdk_current_session. Does NOT touch client->authenticated - the
 * caller decides what a failure means (fsdk_authenticate fails closed; a
 * best-effort fsdk_refresh_session leaves the session as it was). */
static fsdk_result me_probe(fsdk_client* client) {
    if (client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_GET, FSDK_PATH_ME,
                                      client->player_token, NULL, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if (status >= 200 && status < 300) {
        /* The response is authoritative: a missing key clears the cached field. */
        const char* data = json_data_object(resp != NULL ? resp : "");
        json_extract_string(data, "foundryId", client->foundry_id, sizeof(client->foundry_id));
        json_extract_string(data, "displayName", client->display_name, sizeof(client->display_name));
        fsdk_string_free(resp);
        return FSDK_OK;
    }
    fsdk_string_free(resp);
    return http_status_to_result(status);
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
     * 200 -> authenticated (and the identity snapshot is cached for
     * fsdk_current_session); 401/403 -> rejected. */
    fsdk_result r = me_probe(client);
    if (r == FSDK_OK) {
        client->authenticated = 1;
        fsdk_log(FSDK_LOG_INFO, "fsdk authenticate ok");
        return FSDK_OK;
    }
    if (r == FSDK_ERR_UNAUTHORIZED || r == FSDK_ERR_PROTOCOL || r == FSDK_ERR_NO_MATCH) {
        fsdk_log(FSDK_LOG_WARN, "fsdk authenticate rejected by FID");
    }
    /* Else: no transport (NOT_IMPLEMENTED) or transport failure - fail closed. */
    return r;
}

fsdk_result fsdk_refresh_session(fsdk_client* client) {
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    return me_probe(client);
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
     * embedded as a JSON string value (escaped), or null when absent. The queue
     * key is caller-supplied and OPAQUE to the SDK (fid resolves queue UUID, name,
     * or FRN "frn:fmms:<org>:queue/<game>/<mode>") - escape it too, never splice
     * a raw string into the JSON. */
    char body[2560];
    char queue_escaped[320];
    json_escape(queue, queue_escaped, sizeof(queue_escaped));
    if (attrs_json != NULL && attrs_json[0] != '\0') {
        /* Sized for the auto path: a 16-region latency map + caller attrs, escaped. */
        char attrs_escaped[2048];
        json_escape(attrs_json, attrs_escaped, sizeof(attrs_escaped));
        snprintf(body, sizeof(body),
                 "{\"queueId\":\"%s\",\"attributes\":\"%s\"}", queue_escaped, attrs_escaped);
    } else {
        snprintf(body, sizeof(body),
                 "{\"queueId\":\"%s\",\"attributes\":null}", queue_escaped);
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

/* --- Match-search regions (ping-based multi-region placement) ------------- */

/* Test override (-1 = real clock) so cache-TTL behavior is testable without waiting. */
long long fsdk_test_client_now_ms = -1;

static long long client_now_ms(void) {
    if (fsdk_test_client_now_ms >= 0) {
        return fsdk_test_client_now_ms;
    }
#ifdef _WIN32
    LARGE_INTEGER freq;
    LARGE_INTEGER count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (long long)(count.QuadPart / (freq.QuadPart / 1000));
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000L;
#endif
}

fsdk_result fsdk_list_regions(fsdk_client* client,
                              fsdk_region_info* out_regions,
                              size_t max_regions,
                              size_t* out_count) {
    if (client == NULL || out_regions == NULL || out_count == NULL || max_regions == 0) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_count = 0;
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_GET, FSDK_PATH_REGIONS,
                                      client->player_token, NULL, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        return http_status_to_result(status);
    }

    size_t count = 0;
    const char* cursor = fsdk_json_array_start(resp);
    char obj[768];
    while (count < max_regions
           && (cursor = fsdk_json_next_object(cursor, obj, sizeof(obj))) != NULL) {
        fsdk_region_info* region = &out_regions[count];
        memset(region, 0, sizeof(*region));
        region->latency_ms = -1;
        if (!json_extract_string(obj, "code", region->code, sizeof(region->code))
                || region->code[0] == '\0') {
            continue; /* a region without a code is unaddressable - skip */
        }
        json_extract_string(obj, "displayName", region->display_name, sizeof(region->display_name));
        json_extract_string(obj, "pingUrl", region->ping_url, sizeof(region->ping_url));
        count++;
    }
    fsdk_string_free(resp);
    *out_count = count;
    return FSDK_OK;
}

fsdk_result fsdk_measure_regions(fsdk_region_info* regions, size_t count, int samples) {
    if (regions == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (samples <= 0) {
        samples = FSDK_REGION_PING_SAMPLES_DEFAULT;
    }
    for (size_t i = 0; i < count; i++) {
        fsdk_region_info* region = &regions[i];
        region->latency_ms = -1;
        if (region->ping_url[0] == '\0') {
            continue;
        }
        /* Warmup: pays TLS/connection setup once so the timed samples measure the
         * round trip, not the handshake. NO bearer token - the ping host is not
         * the platform and must never see the player's session token. */
        long status = 0;
        if (fsdk_http_request(region->ping_url, FSDK_HTTP_GET, "",
                              NULL, NULL, NULL, &status) != FSDK_OK) {
            continue;
        }
        long best = -1;
        for (int s = 0; s < samples; s++) {
            long long t0 = client_now_ms();
            fsdk_result r = fsdk_http_request(region->ping_url, FSDK_HTTP_GET, "",
                                              NULL, NULL, NULL, &status);
            long long elapsed = client_now_ms() - t0;
            if (r != FSDK_OK) {
                continue;
            }
            if (best < 0 || elapsed < best) {
                best = (long)elapsed;
            }
        }
        region->latency_ms = best;
    }
    return FSDK_OK;
}

/* True when a top-level-ish key already appears in the caller's attrs (flat
 * objects in practice; a nested false-positive just means we defer to the
 * caller's value - the safe direction). */
static int attrs_has_key(const char* attrs_json, const char* key) {
    return attrs_json != NULL && json_value_after(attrs_json, key) != NULL;
}

/* The measured-region cache: refresh via list+measure when empty or stale. */
static void refresh_regions_cache(fsdk_client* client) {
    long long now = client_now_ms();
    if (client->regions_cache_count > 0
            && client->regions_cache_at_ms != 0
            && now - client->regions_cache_at_ms < FSDK_REGION_CACHE_TTL_MS) {
        return;
    }
    size_t count = 0;
    fsdk_region_info fresh[FSDK_MAX_REGIONS];
    if (fsdk_list_regions(client, fresh, FSDK_MAX_REGIONS, &count) != FSDK_OK) {
        return; /* keep whatever we had (possibly nothing) - degrade gracefully */
    }
    fsdk_measure_regions(fresh, count, FSDK_REGION_PING_SAMPLES_DEFAULT);
    memcpy(client->regions_cache, fresh, sizeof(fresh));
    client->regions_cache_count = count;
    client->regions_cache_at_ms = now;
}

fsdk_result fsdk_request_match_auto(fsdk_client* client,
                                    const char* queue,
                                    const char* attrs_json,
                                    fsdk_ticket** out_ticket) {
    if (client == NULL || queue == NULL || out_ticket == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    refresh_regions_cache(client);

    /* The best (lowest measured latency) region, if anything was measurable. */
    const fsdk_region_info* best = NULL;
    for (size_t i = 0; i < client->regions_cache_count; i++) {
        const fsdk_region_info* r = &client->regions_cache[i];
        if (r->latency_ms >= 0 && (best == NULL || r->latency_ms < best->latency_ms)) {
            best = r;
        }
    }
    if (best == NULL) {
        /* No region list / nothing pingable: a plain submit is strictly better
         * than failing - the matchmaker then falls back to its own ordering. */
        return fsdk_request_match(client, queue, attrs_json, out_ticket);
    }

    /* Merge {"region","latencyMs","latencies"} with the caller's attrs. Caller
     * keys WIN (that is the pin-a-region override), so each part is added only
     * when absent, and the caller's object body is spliced in LAST. */
    char merged[2048];
    size_t off = 0;
    int wrote_any = 0;
    merged[off++] = '{';
    merged[off] = '\0';

    char code_escaped[128];
    if (!attrs_has_key(attrs_json, "region")
            && fsdk_json_escape(best->code, code_escaped, sizeof(code_escaped))) {
        off += (size_t)snprintf(merged + off, sizeof(merged) - off,
                                "\"region\":\"%s\"", code_escaped);
        wrote_any = 1;
    }
    if (!attrs_has_key(attrs_json, "latencyMs") && off + 32 < sizeof(merged)) {
        off += (size_t)snprintf(merged + off, sizeof(merged) - off,
                                "%s\"latencyMs\":%ld", wrote_any ? "," : "", best->latency_ms);
        wrote_any = 1;
    }
    if (!attrs_has_key(attrs_json, "latencies")) {
        int wrote_map = 0;
        for (size_t i = 0; i < client->regions_cache_count; i++) {
            const fsdk_region_info* r = &client->regions_cache[i];
            if (r->latency_ms < 0
                    || !fsdk_json_escape(r->code, code_escaped, sizeof(code_escaped))) {
                continue;
            }
            if (off + strlen(code_escaped) + 32 >= sizeof(merged)) {
                break; /* never overflow - a truncated map is still valid JSON */
            }
            off += (size_t)snprintf(merged + off, sizeof(merged) - off, "%s\"%s\":%ld",
                                    wrote_map ? "," : (wrote_any ? ",\"latencies\":{" : "\"latencies\":{"),
                                    code_escaped, r->latency_ms);
            wrote_map = 1;
        }
        if (wrote_map && off + 1 < sizeof(merged)) {
            merged[off++] = '}';
            merged[off] = '\0';
            wrote_any = 1;
        }
    }

    /* Splice the caller's object body in (its keys land LAST - they win). */
    if (attrs_json != NULL && attrs_json[0] != '\0') {
        const char* open = strchr(attrs_json, '{');
        const char* close = strrchr(attrs_json, '}');
        if (open == NULL || close == NULL || close <= open) {
            /* Not an object we can merge into - defer entirely to the caller. */
            return fsdk_request_match(client, queue, attrs_json, out_ticket);
        }
        size_t inner_len = (size_t)(close - open) - 1;
        /* Skip a pure-whitespace body ("{}") - nothing to splice. */
        int inner_nonempty = 0;
        for (size_t i = 0; i < inner_len; i++) {
            const char c = open[1 + i];
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
                inner_nonempty = 1;
                break;
            }
        }
        if (inner_nonempty) {
            if (off + inner_len + 3 >= sizeof(merged)) {
                return fsdk_request_match(client, queue, attrs_json, out_ticket);
            }
            if (wrote_any) {
                merged[off++] = ',';
            }
            memcpy(merged + off, open + 1, inner_len);
            off += inner_len;
            merged[off] = '\0';
        }
    }
    if (off + 2 >= sizeof(merged)) {
        return fsdk_request_match(client, queue, attrs_json, out_ticket);
    }
    merged[off++] = '}';
    merged[off] = '\0';

    fsdk_log(FSDK_LOG_INFO, "fsdk request_match_auto submitting with measured regions");
    return fsdk_request_match(client, queue, merged, out_ticket);
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

fsdk_result fsdk_my_session(fsdk_client* client, fsdk_session_seat* out) {
    if (client == NULL || out == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    /* Zero first so any early return reads as "no seat", never stale stack data. */
    memset(out, 0, sizeof(*out));
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    /* GET {base_url}/v1/fmms/my-session -> { active, ticketId, matchId, queueKey }.
     * The platform's answer, not the client's local state - after a server-side
     * kick the local ticket handle is gone/stale and only fid knows whether the
     * match is still live. An old fid without the route answers 404 -> NO_MATCH,
     * which callers should treat as "no seat" (degrade to a fresh search). */
    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_GET, FSDK_PATH_MY_SESSION,
                                      client->player_token, NULL, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        return http_status_to_result(status);
    }

    const char* data = json_data_object(resp);
    int active = 0;
    fsdk_json_extract_bool(data, "active", &active);
    if (active) {
        json_extract_string(data, "ticketId", out->ticket_id, sizeof(out->ticket_id));
        json_extract_string(data, "matchId", out->match_id, sizeof(out->match_id));
        json_extract_string(data, "queueKey", out->queue_key, sizeof(out->queue_key));
        /* An active seat without a ticket id is unusable for reconnect. */
        out->active = out->ticket_id[0] != '\0' ? 1 : 0;
    }
    fsdk_string_free(resp);
    fsdk_log(FSDK_LOG_INFO, out->active ? "fsdk my_session: active seat"
                                        : "fsdk my_session: no live seat");
    return FSDK_OK;
}

fsdk_result fsdk_resume_ticket(fsdk_client* client,
                               const char* ticket_id,
                               fsdk_ticket** out_ticket) {
    if (client == NULL || ticket_id == NULL || ticket_id[0] == '\0' || out_ticket == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_ticket = NULL;
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    fsdk_ticket* ticket = (fsdk_ticket*)calloc(1, sizeof(fsdk_ticket));
    if (ticket == NULL) {
        return FSDK_ERR_INTERNAL;
    }
    ticket->ticket_id = fsdk_strdup(ticket_id);
    if (ticket->ticket_id == NULL) {
        free(ticket);
        return FSDK_ERR_INTERNAL;
    }
    /* FOUND: the seat came from my_session, so get_connection can resolve now
     * (fid re-mints a fresh token bound to the same match + persisted team). */
    ticket->status = FSDK_MATCH_FOUND;
    *out_ticket = ticket;
    return FSDK_OK;
}
