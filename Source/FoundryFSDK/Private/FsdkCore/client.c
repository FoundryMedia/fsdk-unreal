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

#include <limits.h>
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
#define FSDK_PATH_MY_TICKET     "/v1/fmms/tickets/mine"

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
    if (status == 429) {
        /* Polled sooner than the server's own retryAfterSeconds (the S5 poll backstop) - "keep
         * waiting, poll later", never a failure. fsdk_poll_match recovers the real wait interval
         * via the optional fsdk_set_http_retry_after_fn seam when the host installed one. */
        return FSDK_ERR_RATE_LIMITED;
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

/* Extract a nested JSON OBJECT field's raw text (brace-matched, string-aware so
 * a quoted "}" inside a value never ends the walk early) into out (bounded,
 * NUL-terminated, includes the braces). Returns 1 on success, 0 if the key is
 * absent / not an object / the object overflows out_sz - used for the S5
 * matchmaking-wait "wait": {...} sub-object (see wait_reason_from_string /
 * fsdk_poll_match below). */
static int json_extract_object(const char* body, const char* key, char* out, size_t out_sz) {
    const char* v = json_value_after(body, key);
    if (out_sz > 0) {
        out[0] = '\0';
    }
    if (v == NULL || *v != '{') {
        return 0;
    }
    int depth = 0;
    int in_string = 0;
    const char* p = v;
    for (; *p != '\0'; p++) {
        char c = *p;
        if (in_string) {
            if (c == '\\' && p[1] != '\0') {
                p++;
            } else if (c == '"') {
                in_string = 0;
            }
            continue;
        }
        if (c == '"') {
            in_string = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                size_t n = (size_t)(p - v) + 1;
                if (n + 1 > out_sz) {
                    return 0; /* oversized object - never a partial/garbled copy */
                }
                memcpy(out, v, n);
                out[n] = '\0';
                return 1;
            }
        }
    }
    return 0;
}

/* Map a server "wait.reason" string to fsdk_wait_reason. Anything unrecognized
 * (incl. a future reason an older SDK doesn't know yet) reads as SEARCHING -
 * never invent a harsher wait than the server actually said. */
static int wait_reason_from_string(const char* s) {
    if (s == NULL) {
        return FSDK_WAIT_SEARCHING;
    }
    if (strcmp(s, "CAPACITY_STARTING") == 0) {
        return FSDK_WAIT_CAPACITY_STARTING;
    }
    if (strcmp(s, "NO_CAPACITY") == 0) {
        return FSDK_WAIT_NO_CAPACITY;
    }
    if (strcmp(s, "NO_FLEET") == 0) {
        return FSDK_WAIT_NO_FLEET;
    }
    return FSDK_WAIT_SEARCHING;
}

/* Days since 1970-01-01 for a UTC civil date (y/m/d) - Howard Hinnant's
 * days_from_civil algorithm (pure integer arithmetic, no libc timezone calls,
 * so the conversion is identical on every platform regardless of local tz).
 * Valid for any proleptic-Gregorian y/m/d; only ever fed values the regex in
 * parse_iso8601_utc already bounds. */
static long long days_from_civil(long long y, int m, int d) {
    y -= (m <= 2);
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;                                 /* [0, 399]    */
    long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; /* [0, 365]    */
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

/* Read exactly `width` ASCII digits starting at *pp into *out and advance *pp
 * past them. Returns 1 on success; on failure *pp and *out are left untouched (not
 * even partial digits are consumed). */
static int read_fixed_digits(const char** pp, int width, int* out) {
    const char* p = *pp;
    int v = 0;
    int i;
    for (i = 0; i < width; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return 0;
        }
        v = v * 10 + (p[i] - '0');
    }
    *out = v;
    *pp = p + width;
    return 1;
}

/* Best-effort parse of an ISO-8601 UTC "YYYY-MM-DDTHH:MM:SS" prefix (fractional
 * seconds and the trailing 'Z'/offset, if any, are ignored, and never even
 * read) into Unix epoch seconds - manual field-by-field parsing (no scanf
 * family: this core stays free of the MSVC "unsafe CRT function" warning
 * class, matching json_extract_int's strtol-based style). Returns 1 on
 * success; 0 on anything that doesn't match - the caller treats that as
 * "unknown" (never a hard failure over a field the server may format
 * differently someday). */
static int parse_iso8601_utc(const char* s, long long* out_unix) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    const char* p = s;
    if (s == NULL || out_unix == NULL) {
        return 0;
    }
    if (!read_fixed_digits(&p, 4, &y) || *p != '-') {
        return 0;
    }
    p++;
    if (!read_fixed_digits(&p, 2, &mo) || *p != '-') {
        return 0;
    }
    p++;
    if (!read_fixed_digits(&p, 2, &d) || *p != 'T') {
        return 0;
    }
    p++;
    if (!read_fixed_digits(&p, 2, &h) || *p != ':') {
        return 0;
    }
    p++;
    if (!read_fixed_digits(&p, 2, &mi) || *p != ':') {
        return 0;
    }
    p++;
    if (!read_fixed_digits(&p, 2, &se)) {
        return 0;
    }
    if (mo < 1 || mo > 12 || d < 1 || d > 31
            || h < 0 || h > 23 || mi < 0 || mi > 59 || se < 0 || se > 60) {
        return 0;
    }
    *out_unix = days_from_civil(y, mo, d) * 86400LL + h * 3600LL + mi * 60LL + se;
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
        json_extract_string(data, "username", client->username, sizeof(client->username));
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
    client->last_error[0] = '\0';
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_POST, FSDK_PATH_TICKETS,
                                      client->player_token, body, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        /* Keep fid's sentence ("Only the party leader can start a search.", "Not everyone is
         * ready in goo-crew: ...") - the host shows it verbatim; the bare status code maps to
         * a generic result that says nothing about WHY a party search was refused. */
        fsdk_client_note_error(client, status, resp);
        fsdk_string_free(resp);
        fsdk_log(FSDK_LOG_WARN, "fsdk request_match rejected");
        return http_status_to_result(status);
    }

    fsdk_ticket* ticket = (fsdk_ticket*)calloc(1, sizeof(fsdk_ticket));
    if (ticket == NULL) {
        fsdk_string_free(resp);
        return FSDK_ERR_INTERNAL;
    }
    /* Default wait cadence until the first real poll refreshes it from the
     * server (reason/estimate/expiry default to the calloc'd zero already). */
    ticket->wait.retry_after_seconds = 2;

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
        *out_status = ticket->status; /* unchanged - a 429 is a throttle, not a status change */
        if (status == 429) {
            /* The server's poll backstop: we arrived sooner than its own
             * retryAfterSeconds. Recover the real interval from the optional
             * Retry-After reader when the host installed one; otherwise keep
             * whatever cadence the ticket already had (still a sane wait). */
            long retry_seconds = -1;
            if (fsdk_dispatch_http_retry_after(&retry_seconds) && retry_seconds >= 0) {
                ticket->wait.retry_after_seconds =
                    retry_seconds > INT_MAX ? INT_MAX : (int)retry_seconds;
            }
            fsdk_log(FSDK_LOG_DEBUG, "fsdk poll_match rate limited - keep waiting, poll later");
        }
        return http_status_to_result(status);
    }

    const char* data = json_data_object(resp);
    char state_buf[32];
    int got_state = json_extract_string(data, "state", state_buf, sizeof(state_buf));
    if (got_state) {
        ticket->status = ticket_state_to_status(state_buf);
    }

    /* "expiresAt" is a top-level ISO-8601 field independent of state - parsed
     * best-effort; a miss leaves the ticket's previously-known value (0 if
     * there never was one) rather than clobbering it with "expired". */
    char expires_buf[40];
    long long expires_unix = 0;
    if (json_extract_string(data, "expiresAt", expires_buf, sizeof(expires_buf))
            && parse_iso8601_utc(expires_buf, &expires_unix)) {
        ticket->wait.expires_at_unix = expires_unix;
    }

    /* "wait" rides the wire ONLY while state == QUEUED. Off QUEUED (or state
     * missing/unrecognized), there is nothing to wait on - reset to the same
     * default an unpolled ticket starts with. */
    if (got_state && strcmp(state_buf, "QUEUED") == 0) {
        char wait_obj[256];
        if (json_extract_object(data, "wait", wait_obj, sizeof(wait_obj))) {
            char reason_buf[32];
            long estimate = 0;
            long retry = 0;
            json_extract_string(wait_obj, "reason", reason_buf, sizeof(reason_buf));
            ticket->wait.reason = wait_reason_from_string(reason_buf);
            ticket->wait.estimate_seconds = json_extract_int(wait_obj, "estimateSeconds", &estimate)
                                                 ? (int)estimate : 0;
            ticket->wait.retry_after_seconds = json_extract_int(wait_obj, "retryAfterSeconds", &retry)
                                                   ? (int)retry : 2;
        } else {
            /* Older fid, no "wait" object yet - degrade to a plain search. */
            ticket->wait.reason = FSDK_WAIT_SEARCHING;
            ticket->wait.estimate_seconds = 0;
            ticket->wait.retry_after_seconds = 2;
        }
    } else {
        ticket->wait.reason = FSDK_WAIT_SEARCHING;
        ticket->wait.estimate_seconds = 0;
        ticket->wait.retry_after_seconds = 2;
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

fsdk_result fsdk_ticket_wait_info(const fsdk_ticket* ticket, fsdk_ticket_wait* out) {
    if (ticket == NULL || out == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out = ticket->wait;
    return FSDK_OK;
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

fsdk_result fsdk_fmms_my_ticket(fsdk_client* client,
                                char* out_ticket_id, size_t ticket_id_sz,
                                char* out_queue_key, size_t queue_key_sz,
                                char* out_leader_foundry_id, size_t leader_sz) {
    if (client == NULL || out_ticket_id == NULL || ticket_id_sz == 0
            || out_queue_key == NULL || queue_key_sz == 0
            || out_leader_foundry_id == NULL || leader_sz == 0) {
        return FSDK_ERR_INVALID_ARG;
    }
    /* Empty first so any early return reads as "no ticket", never stale stack data. */
    out_ticket_id[0] = '\0';
    out_queue_key[0] = '\0';
    out_leader_foundry_id[0] = '\0';
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    /* GET {base_url}/v1/fmms/tickets/mine -> {ticketId, queueKey, state, partyId,
     * leaderFoundryId}: the caller's newest live (QUEUED or MATCHED) ticket. It is POLLED
     * every ~2 s by party members waiting for their leader's search, so fid answers the
     * "none" case with a QUIET 404 - not an error worth a log line or a stored sentence. */
    client->last_error[0] = '\0';
    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_GET, FSDK_PATH_MY_TICKET,
                                      client->player_token, NULL, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        if (status != 404) {
            fsdk_client_note_error(client, status, resp);
        }
        fsdk_string_free(resp);
        return http_status_to_result(status);
    }

    const char* data = json_data_object(resp);
    json_extract_string(data, "ticketId", out_ticket_id, ticket_id_sz);
    json_extract_string(data, "queueKey", out_queue_key, queue_key_sz);
    /* null for a solo ticket - json_extract_string leaves the empty string. */
    json_extract_string(data, "leaderFoundryId", out_leader_foundry_id, leader_sz);
    fsdk_string_free(resp);
    /* A 200 without a ticket id is unusable - treat it as "none". */
    return out_ticket_id[0] != '\0' ? FSDK_OK : FSDK_ERR_NO_MATCH;
}

fsdk_result fsdk_report_match_ended(fsdk_client* client, fsdk_ticket* ticket) {
    if (client == NULL || ticket == NULL || ticket->ticket_id == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated || client->player_token == NULL) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }

    /* POST {base_url}/v1/fmms/tickets/{id}/match-ended, empty body, player
     * bearer. The ticket id came from the server (submit / my-session), the
     * same trust level as cancel and connection. 200 {ticketId,matchId,status};
     * 404 = no such seat (or an older fid without the route); idempotent. */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s/match-ended", FSDK_PATH_TICKETS, ticket->ticket_id);

    client->last_error[0] = '\0';
    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(client->base_url, FSDK_HTTP_POST, path,
                                      client->player_token, NULL, &resp, &status);
    if (r != FSDK_OK) {
        return r;
    }
    fsdk_client_note_error(client, status, resp);
    fsdk_string_free(resp);
    if (status >= 200 && status < 300) {
        fsdk_log(FSDK_LOG_INFO, "fsdk report_match_ended: match closed to new seats");
    }
    return http_status_to_result(status);
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
    /* Not QUEUED - wait never applies; seed the same sane default anyway. */
    ticket->wait.retry_after_seconds = 2;
    *out_ticket = ticket;
    return FSDK_OK;
}
