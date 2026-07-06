/*
 * server.c - server-side SDK (runs inside the dedicated game server, our box).
 *
 * Responsibilities:
 *   - Agones SDK lifecycle: Ready / Health / Shutdown.
 *   - Read this server's match binding.
 *   - VALIDATE joining players' FID-signed match tokens (the admission gate);
 *     drop unauthenticated traffic.
 *
 * SECURITY: the server identity used for any FMMS callbacks is a SHORT-LIVED,
 * SCOPED token minted at allocation time and injected via the environment -
 * NEVER a long-lived secret baked into the binary. See SECURITY.md.
 *
 * Agones lifecycle goes through the local SDK sidecar's HTTP gateway (default
 * http://127.0.0.1:9358) via the HOST-PROVIDED http transport seam
 * (fsdk_set_http_transport) - the SAME seam the client uses, so the core links no
 * HTTP stack itself (the trusted server's host backs it with libcurl). This is the
 * REST gateway, not the gRPC SDK: functionally equivalent for ready/health/
 * shutdown/get-binding and far lighter for a C core (no gRPC/protobuf C++ dep).
 */
/* ============================ SERVER-ONLY GATE =============================
 * This whole translation unit compiles to an EMPTY TU on any non-server target
 * (FOUNDRY_FSDK_SERVER undefined or 0), so the match-token verify + Agones server
 * code is genuinely ABSENT from the shipped player/client/editor binary - not
 * merely dead. Build.cs defines FOUNDRY_FSDK_SERVER=1 only for Target.Type ==
 * Server; the sole caller (FoundryFSDKServer.cpp) is itself #if UE_SERVER, and no
 * client TU references any fsdk_server_* / fsdk_token_verify symbol, so excluding
 * this TU breaks nothing on a client build.
 *
 * VENDORING DIVERGENCE: this gate is applied to the fsdk-unreal VENDORED copy only.
 * Upstream fsdk-core/src/server.c does NOT carry it (its standalone CMake lib +
 * CTest suite need this code always compiled). After any re-vendor from fsdk-core,
 * RE-APPLY this gate - or adopt it upstream together with a build-time
 * FOUNDRY_FSDK_SERVER=1 for the standalone lib/tests.
 * See .claude/rules/fsdk-security.md + .claude/rules/unreal-plugin-conventions.md.
 * ========================================================================== */
#if defined(FOUNDRY_FSDK_SERVER) && FOUNDRY_FSDK_SERVER

/* Expose nanosleep (POSIX.1b) even under a strict -std C compile: the boot-race retry
 * loop needs a sub-second sleep and this is the core's only time dependency. Must
 * precede every libc include in this TU. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 199309L
#endif

#include "fsdk_internal.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

/* Default Agones SDK sidecar REST gateway (the gRPC SDK is :9357; the HTTP gateway is :9358). */
#define FSDK_AGONES_DEFAULT_ADDR "http://127.0.0.1:9358"

/* First-contact retry: the sidecar's HTTP gateway RACES the game server boot - a fast
 * server can be up in <1s, before :9358 listens (seen live: UE server boot 0.8s ->
 * POST /ready connection refused -> GameServer stuck Scheduled forever). Like Agones's
 * own SDKs, retry the connection with a fixed backoff until the sidecar answers ONCE;
 * after first contact every call is one-shot again (a health tick has its own cadence
 * and a mid-match call must never stall the caller for the retry budget). */
unsigned int fsdk_agones_retry_interval_ms = 1000u;
int          fsdk_agones_retry_max_attempts = 30; /* ~30s total at 1s apart */

/* Millisecond sleep for the boot-race retry loop. */
static void fsdk_sleep_ms(unsigned int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)(ms % 1000u) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* fsdk_strdup + json_extract_string (used for the flat fcg/match-id annotation) are shared
 * via fsdk_internal.h. */

/* Latch interesting facts from a /gameserver body wherever we happen to read one (get_binding,
 * check_drain): the Allocated state. The flat reader finds the FIRST "state" key - the sidecar's
 * GameServer JSON carries it only under status (our fleets define no label/annotation named
 * "state"), so the flat scan is safe here. Latched: once Allocated, stays Allocated (Agones never
 * moves a GameServer back to Ready; the pod is recycled instead). */
static void latch_gameserver_facts(fsdk_server* server, const char* body) {
    if (body == NULL || server->allocated) {
        return;
    }
    char state[32];
    if (json_extract_string(body, "state", state, sizeof state) && strcmp(state, "Allocated") == 0) {
        server->allocated = 1;
        fsdk_log(FSDK_LOG_INFO, "fsdk: GameServer is Allocated (match placed on this server)");
    }
}

/* One sidecar HTTP call with the first-contact retry loop. Retries ONLY transport-level
 * failures (FSDK_ERR_NETWORK / FSDK_ERR_TIMEOUT - connection refused/hung) and ONLY
 * while the sidecar has never answered. A completed HTTP response (any status) marks
 * contact and is returned as-is; FSDK_NOT_IMPLEMENTED (no transport installed) and
 * other errors fail immediately - retrying cannot help them. */
static fsdk_result agones_request(fsdk_server* server, fsdk_http_method method,
                                  const char* path, const char* req_body,
                                  char** out_body, long* out_status) {
    int attempt = 0;
    for (;;) {
        fsdk_result r = fsdk_http_request(server->agones_addr, method, path, NULL,
                                          req_body, out_body, out_status);
        if (r == FSDK_OK) {
            server->sidecar_contacted = 1;
            return FSDK_OK;
        }
        if (server->sidecar_contacted ||
            (r != FSDK_ERR_NETWORK && r != FSDK_ERR_TIMEOUT)) {
            return r;
        }
        attempt++;
        if (attempt >= fsdk_agones_retry_max_attempts) {
            fsdk_log(FSDK_LOG_WARN, "fsdk agones: sidecar unreachable after retries");
            return r;
        }
        if (attempt == 1) {
            fsdk_log(FSDK_LOG_INFO, "fsdk agones: sidecar not up yet (boot race) - retrying");
        }
        if (out_body != NULL && *out_body != NULL) {
            fsdk_string_free(*out_body);
            *out_body = NULL;
        }
        fsdk_sleep_ms(fsdk_agones_retry_interval_ms);
    }
}

/* POST {} to an Agones sidecar path through the host http transport; 2xx => FSDK_OK. */
static fsdk_result agones_post(fsdk_server* server, const char* path) {
    long status = 0;
    fsdk_result r = agones_request(server, FSDK_HTTP_POST, path, "{}", NULL, &status);
    if (r != FSDK_OK) {
        fsdk_log(FSDK_LOG_WARN, "fsdk agones: sidecar call failed (no/failed http transport)");
        return FSDK_ERR_AGONES;
    }
    if (status < 200 || status >= 300) {
        fsdk_log(FSDK_LOG_WARN, "fsdk agones: sidecar returned non-2xx");
        return FSDK_ERR_AGONES;
    }
    return FSDK_OK;
}

fsdk_result fsdk_server_create(const char* agones_addr, fsdk_server** out_server) {
    if (out_server == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_server = NULL;

    fsdk_server* server = (fsdk_server*)calloc(1, sizeof(fsdk_server));
    if (server == NULL) {
        return FSDK_ERR_INTERNAL;
    }

    /* agones_addr is the sidecar HTTP gateway base; NULL -> the Agones default. */
    const char* addr = (agones_addr != NULL && agones_addr[0] != '\0')
            ? agones_addr : FSDK_AGONES_DEFAULT_ADDR;
    server->agones_addr = fsdk_strdup(addr);
    if (server->agones_addr == NULL) {
        free(server);
        return FSDK_ERR_INTERNAL;
    }

    /* TODO(server identity): read the short-lived, scoped server token from the
     * environment (injected at allocation time) - NEVER bake it in. */
    *out_server = server;
    fsdk_log(FSDK_LOG_INFO, "fsdk server created");
    return FSDK_OK;
}

void fsdk_server_destroy(fsdk_server* server) {
    if (server == NULL) {
        return;
    }
    free(server->agones_addr);
    free(server->match_id);
    free(server);
}

fsdk_result fsdk_server_ready(fsdk_server* server) {
    if (server == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    fsdk_result r = agones_post(server, "/ready"); /* Agones SDK Ready() over the sidecar gateway */
    fsdk_log(r == FSDK_OK ? FSDK_LOG_INFO : FSDK_LOG_WARN,
             r == FSDK_OK ? "fsdk server ready (Agones Ready ok)" : "fsdk server ready FAILED");
    return r;
}

fsdk_result fsdk_server_health(fsdk_server* server) {
    if (server == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    /* Agones SDK Health() ping. Call on a fixed interval; the orchestrator recycles the box if
     * pings stop. */
    return agones_post(server, "/health");
}

fsdk_result fsdk_server_validate_player(fsdk_server* server,
                                        const char* match_token,
                                        fsdk_player_info* out) {
    if (server == NULL || match_token == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }

    /* This is the admission GATE. The server calls it for every joining player
     * before accepting their netcode connection. A scraped ip:port without a
     * valid token is useless: verification fails here and the caller drops the
     * connection (and the UDP traffic) fast.
     *
     * Delegates to fsdk_token_verify (signature via the host verifier seam, then
     * iss/aud/exp/nbf), bound to THIS server's allocated match id so a token for
     * match A cannot admit a player to match B. server->match_id is NULL until
     * fsdk_server_get_binding populates it (Agones - TODO); NULL skips the binding
     * check. The result is returned verbatim: FSDK_OK admits, anything else drops
     * (FSDK_NOT_IMPLEMENTED = no verifier installed = also a drop, fail-closed). */
    fsdk_player_info info_local;
    fsdk_player_info* info = (out != NULL) ? out : &info_local;

    /* Lazy binding read: the fcg/match-id annotation is applied at ALLOCATION (after the box is
     * Ready), so a startup get_binding misses it. Read it here on the FIRST validation rather than
     * forcing a restart - by the time any player connects the GameServer is Allocated and the
     * annotation is present. Once latched it sticks (cached on the handle), so this costs one extra
     * sidecar GET on the first join only; NULL stays NULL when no annotation exists (binding skipped). */
    if (server->match_id == NULL) {
        char* binding = NULL;
        (void)fsdk_server_get_binding(server, &binding);
        fsdk_string_free(binding);
    }

    fsdk_result vr = fsdk_token_verify(match_token, server->match_id, info);
    if (vr != FSDK_OK) {
        fsdk_log(FSDK_LOG_WARN, "fsdk validate_player: token rejected");
        return vr;
    }
    fsdk_log(FSDK_LOG_DEBUG, "fsdk validate_player: player admitted");
    return FSDK_OK;
}

fsdk_result fsdk_server_get_binding(fsdk_server* server, char** out_json) {
    if (server == NULL || out_json == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_json = NULL;

    /* Read the allocated GameServer from the sidecar; its metadata.annotations carry this box's
     * match binding. We hand the raw JSON back to the caller AND latch the match id into the server
     * so validate_player can enforce the binding (a token for match A can't admit to match B).
     * Annotation key fcg/match-id mirrors the FMMS->FCG allocation annotation; until that is wired
     * end-to-end the key may be absent (then match_id stays NULL and validate_player skips binding). */
    char* body = NULL;
    long status = 0;
    fsdk_result r = agones_request(server, FSDK_HTTP_GET, "/gameserver",
                                   NULL, &body, &status);
    if (r != FSDK_OK || status < 200 || status >= 300) {
        fsdk_string_free(body);
        fsdk_log(FSDK_LOG_WARN, "fsdk get_binding: sidecar /gameserver failed");
        return FSDK_ERR_AGONES;
    }

    if (body != NULL) {
        char mid[64];
        if (json_extract_string(body, "fcg/match-id", mid, sizeof mid) && mid[0] != '\0') {
            free(server->match_id);
            server->match_id = fsdk_strdup(mid);
            fsdk_log(FSDK_LOG_INFO, "fsdk get_binding: latched match binding");
        }
        latch_gameserver_facts(server, body);
    }
    *out_json = body; /* caller frees via fsdk_string_free */
    return FSDK_OK;
}

fsdk_result fsdk_server_check_drain(fsdk_server* server, int* out_draining) {
    if (server == NULL || out_draining == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    *out_draining = server->draining;
    if (server->draining) {
        return FSDK_OK; /* latched - no further sidecar reads needed */
    }

    /* The platform (node agent) merge-patches fcg/drain=true onto this GameServer when a
     * customer/operator asks for a graceful wind-down. Read our own object via the sidecar and
     * latch the flag; the flat json reader is fine here (annotations are a flat string map). */
    char* body = NULL;
    long status = 0;
    fsdk_result r = agones_request(server, FSDK_HTTP_GET, "/gameserver", NULL, &body, &status);
    if (r != FSDK_OK || status < 200 || status >= 300) {
        fsdk_string_free(body);
        fsdk_log(FSDK_LOG_WARN, "fsdk check_drain: sidecar /gameserver failed");
        return FSDK_ERR_AGONES;
    }
    if (body != NULL) {
        char val[16];
        if (json_extract_string(body, "fcg/drain", val, sizeof val) && strcmp(val, "true") == 0) {
            server->draining = 1;
            *out_draining = 1;
            fsdk_log(FSDK_LOG_INFO, "fsdk check_drain: DRAIN requested by the platform");
        }
        latch_gameserver_facts(server, body);
    }
    fsdk_string_free(body);
    return FSDK_OK;
}

fsdk_result fsdk_server_allocated(fsdk_server* server, int* out_allocated) {
    if (server == NULL || out_allocated == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    /* Pure latch read - no sidecar call. The latch is fed by the /gameserver reads the host
     * already makes on the health cadence (check_drain) and at boot (get_binding). */
    *out_allocated = server->allocated;
    return FSDK_OK;
}

fsdk_result fsdk_server_shutdown(fsdk_server* server) {
    if (server == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    fsdk_result r = agones_post(server, "/shutdown"); /* Agones SDK Shutdown(); orchestrator reclaims */
    fsdk_log(r == FSDK_OK ? FSDK_LOG_INFO : FSDK_LOG_WARN,
             r == FSDK_OK ? "fsdk server shutdown (Agones Shutdown ok)" : "fsdk server shutdown FAILED");
    return r;
}

#else /* !FOUNDRY_FSDK_SERVER */
/* Non-server target: the server SDK is excluded from the binary. A lone typedef
 * keeps this a non-empty translation unit (ISO C forbids an empty TU). */
typedef int fsdk_server_tu_not_empty;
#endif /* FOUNDRY_FSDK_SERVER */
