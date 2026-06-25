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
/* VENDORED + GATED: server-only translation unit. On a non-server target
 * (Game/Client/Editor - the shipped player binary) FOUNDRY_FSDK_SERVER is undefined
 * and this whole file compiles to an EMPTY TU, so no server/token code or OpenSSL
 * ever enters the client. The gate is set in FoundryFSDK.Build.cs for
 * TargetType.Server only. See .claude/rules/fsdk-security.md. */
#if defined(FOUNDRY_FSDK_SERVER) && FOUNDRY_FSDK_SERVER

#include "fsdk_internal.h"

#include <stdlib.h>
#include <string.h>

/* Default Agones SDK sidecar REST gateway (the gRPC SDK is :9357; the HTTP gateway is :9358). */
#define FSDK_AGONES_DEFAULT_ADDR "http://127.0.0.1:9358"

static char* fsdk_strdup_srv(const char* s) {
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

/* Minimal JSON string-field reader for the GameServer binding (flat key search; the annotation key
 * is unique in the doc so nesting under metadata.annotations is fine). Mirrors token.c/client.c's
 * readers - a shared json helper is a cleanup follow-on. */
static int srv_json_string(const char* body, const char* key, char* out, size_t out_sz) {
    if (out_sz > 0) {
        out[0] = '\0';
    }
    if (body == NULL || key == NULL) {
        return 0;
    }
    size_t klen = strlen(key);
    const char* p = body;
    while ((p = strchr(p, '"')) != NULL) {
        if (strncmp(p + 1, key, klen) == 0 && p[1 + klen] == '"') {
            const char* q = p + 1 + klen + 1;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
            if (*q == ':') {
                q++;
                while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') q++;
                if (*q != '"') return 0;
                q++;
                size_t i = 0;
                while (*q != '\0' && *q != '"') {
                    char c = *q;
                    if (c == '\\' && q[1] != '\0') { q++; c = *q; }
                    if (i + 1 < out_sz) out[i++] = c;
                    q++;
                }
                if (i < out_sz) out[i] = '\0';
                return (*q == '"');
            }
        }
        p++;
    }
    return 0;
}

/* POST {} to an Agones sidecar path through the host http transport; 2xx => FSDK_OK. */
static fsdk_result agones_post(fsdk_server* server, const char* path) {
    long status = 0;
    fsdk_result r = fsdk_http_request(server->agones_addr, FSDK_HTTP_POST, path,
                                      NULL, "{}", NULL, &status);
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
    server->agones_addr = fsdk_strdup_srv(addr);
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
    fsdk_result r = fsdk_http_request(server->agones_addr, FSDK_HTTP_GET, "/gameserver",
                                      NULL, NULL, &body, &status);
    if (r != FSDK_OK || status < 200 || status >= 300) {
        fsdk_string_free(body);
        fsdk_log(FSDK_LOG_WARN, "fsdk get_binding: sidecar /gameserver failed");
        return FSDK_ERR_AGONES;
    }

    if (body != NULL) {
        char mid[64];
        if (srv_json_string(body, "fcg/match-id", mid, sizeof mid) && mid[0] != '\0') {
            free(server->match_id);
            server->match_id = fsdk_strdup_srv(mid);
            fsdk_log(FSDK_LOG_INFO, "fsdk get_binding: latched match binding");
        }
    }
    *out_json = body; /* caller frees via fsdk_string_free */
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
typedef int fsdk_server_tu_not_empty; /* gated out of non-server targets; avoids ISO C empty-TU warning */
#endif
