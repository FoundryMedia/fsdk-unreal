/*
 * auth.c - client-side player-token plumbing + the OPTIONAL FID auth module.
 *
 * Split into two parts:
 *   1. ALWAYS compiled - fsdk_set_player_token / fsdk_current_session / fsdk_set_auth_base.
 *      This is all the DEFAULT (launcher-managed) path needs: a host hands in a token
 *      (from the launcher session daemon, or a BYO backend) and the SDK uses it. No
 *      credentials, no login endpoints, no keyring.
 *   2. GATED (FID-embedded, opt-in) - fsdk_login / fsdk_refresh / fsdk_try_resume /
 *      fsdk_logout against the auth-efga desktop bearer endpoints, with refresh-token
 *      rotation persisted via the host keyring seam. Compiled UNLESS the host sets
 *      FOUNDRY_FSDK_FID_AUTH=0 (the Unreal default), so the shipped player binary of a
 *      launcher-distributed game carries NO credential-login path. The standalone
 *      fsdk-core build (which doesn't define the macro) keeps it for tests.
 *
 * SECURITY (see SECURITY.md): ships to players, holds NO baked secrets. The password
 * is used only to build the login request body and scrubbed immediately - never stored
 * or logged. The access token is in-memory only; only the long-lived refresh token is
 * persisted, and only via the host keyring. Tokens are never passed to fsdk_log.
 */
#include "fsdk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scrub + replace a heap token field with a copy of val. */
static fsdk_result set_secret_field(char** field, const char* val) {
    char* copy = fsdk_strdup(val);
    if (copy == NULL) {
        return FSDK_ERR_INTERNAL;
    }
    if (*field != NULL) {
        memset(*field, 0, strlen(*field));
        free(*field);
    }
    *field = copy;
    return FSDK_OK;
}

void fsdk_set_auth_base(fsdk_client* client, const char* auth_base) {
    if (client == NULL) {
        return;
    }
    char* copy = (auth_base != NULL && auth_base[0] != '\0') ? fsdk_strdup(auth_base) : NULL;
    free(client->auth_base_url);
    client->auth_base_url = copy;
}

fsdk_result fsdk_set_player_token(fsdk_client* client, const char* player_token) {
    if (client == NULL || player_token == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    /* The caller (launcher daemon handoff / BYO backend) already vouched - set the
     * token without an FID login or the /v1/me/user probe. */
    fsdk_result r = set_secret_field(&client->player_token, player_token);
    if (r != FSDK_OK) {
        return r;
    }
    client->authenticated = 1;
    return FSDK_OK;
}

fsdk_result fsdk_current_session(fsdk_client* client, fsdk_session* out) {
    if (client == NULL || out == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    if (!client->authenticated) {
        return FSDK_ERR_NOT_AUTHENTICATED;
    }
    copy_bounded(out->foundry_id, sizeof(out->foundry_id), client->foundry_id);
    copy_bounded(out->display_name, sizeof(out->display_name), client->display_name);
    return FSDK_OK;
}

/* ========================================================================== */
/* FID-embedded in-game auth (opt-in). Compiled UNLESS FOUNDRY_FSDK_FID_AUTH=0. */
/* ========================================================================== */
#if !defined(FOUNDRY_FSDK_FID_AUTH) || FOUNDRY_FSDK_FID_AUTH

/* Default auth host (auth-efga). Override per-client with fsdk_set_auth_base. */
#define FSDK_DEFAULT_AUTH_BASE "https://auth.foundryplatform.app"

/* Desktop bearer auth routes (relative to the auth host). */
#define FSDK_PATH_LOGIN   "/auth/v1/desktop/login"
#define FSDK_PATH_REFRESH "/auth/v1/desktop/refresh"
#define FSDK_PATH_LOGOUT  "/auth/v1/desktop/logout"

/* Keyring key under which the rotated refresh token is persisted. */
#define FSDK_SECRET_KEY_REFRESH "foundry/refresh-token"

/* Token buffers: FID access/refresh JWTs are comfortably under this bound. */
#define FSDK_TOKEN_BUF 2048

/* Map an HTTP status to a result (2xx OK; 401/403 UNAUTHORIZED; 404 NO_MATCH;
 * 408/504 TIMEOUT; else PROTOCOL). */
static fsdk_result status_to_result(long status) {
    if (status >= 200 && status < 300) {
        return FSDK_OK;
    }
    if (status == 401 || status == 403) {
        return FSDK_ERR_UNAUTHORIZED;
    }
    if (status == 408 || status == 504) {
        return FSDK_ERR_TIMEOUT;
    }
    return FSDK_ERR_PROTOCOL;
}

/* Narrow to the JsonApiResponse "data" object so field lookups don't collide
 * with keys in "meta"/"errors". Falls back to the whole body if absent. */
static const char* json_data_object(const char* body) {
    const char* v = json_value_after(body, "data");
    return (v != NULL && *v == '{') ? v : body;
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

static const char* auth_base(fsdk_client* client) {
    return (client->auth_base_url != NULL && client->auth_base_url[0] != '\0')
               ? client->auth_base_url
               : FSDK_DEFAULT_AUTH_BASE;
}

static void scrub_field(char** field) {
    if (*field != NULL) {
        memset(*field, 0, strlen(*field));
        free(*field);
        *field = NULL;
    }
}

/* Parse a DesktopSession envelope, store tokens + identity, persist the (rotated)
 * refresh token. Returns FSDK_ERR_PROTOCOL if the access/refresh tokens are absent. */
static fsdk_result apply_session(fsdk_client* client, const char* resp) {
    const char* data = json_data_object(resp);
    char access[FSDK_TOKEN_BUF];
    char refresh[FSDK_TOKEN_BUF];
    int has_access = json_extract_string(data, "accessToken", access, sizeof(access)) && access[0] != '\0';
    int has_refresh = json_extract_string(data, "refreshToken", refresh, sizeof(refresh)) && refresh[0] != '\0';
    if (!has_access || !has_refresh) {
        memset(access, 0, sizeof(access));
        memset(refresh, 0, sizeof(refresh));
        return FSDK_ERR_PROTOCOL;
    }

    fsdk_result r = set_secret_field(&client->player_token, access);
    if (r == FSDK_OK) {
        r = set_secret_field(&client->refresh_token, refresh);
    }
    memset(access, 0, sizeof(access));
    memset(refresh, 0, sizeof(refresh));
    if (r != FSDK_OK) {
        return r;
    }
    client->authenticated = 1;

    /* Identity (best-effort) from the nested "user" object - unique keys, so a
     * flat lookup over "data" finds them. displayName may be null -> empty. */
    json_extract_string(data, "foundryId", client->foundry_id, sizeof(client->foundry_id));
    json_extract_string(data, "displayName", client->display_name, sizeof(client->display_name));

    /* Persist the rotated refresh token (best-effort; no store -> session only). */
    fsdk_secret_save(FSDK_SECRET_KEY_REFRESH, client->refresh_token);
    return FSDK_OK;
}

fsdk_result fsdk_login(fsdk_client* client, const char* email_or_username,
                       const char* password, int remember) {
    if (client == NULL || email_or_username == NULL || password == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }

    char eou_esc[512];
    char pw_esc[512];
    char body[1280];
    json_escape(email_or_username, eou_esc, sizeof(eou_esc));
    json_escape(password, pw_esc, sizeof(pw_esc));
    snprintf(body, sizeof(body),
             "{\"emailOrUsername\":\"%s\",\"password\":\"%s\",\"rememberMe\":%s}",
             eou_esc, pw_esc, remember ? "true" : "false");
    /* Scrub the escaped password copy immediately; the live copy is in `body`. */
    memset(pw_esc, 0, sizeof(pw_esc));

    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(auth_base(client), FSDK_HTTP_POST, FSDK_PATH_LOGIN,
                                      NULL, body, &resp, &status);
    /* The request body held the password - scrub it now, win or lose. */
    memset(body, 0, sizeof(body));
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        fsdk_log(FSDK_LOG_WARN, "fsdk login rejected");
        return status_to_result(status);
    }

    r = apply_session(client, resp);
    fsdk_string_free(resp);
    if (r == FSDK_OK) {
        fsdk_log(FSDK_LOG_INFO, "fsdk login ok");
    }
    return r;
}

fsdk_result fsdk_refresh(fsdk_client* client) {
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    /* Use the in-memory refresh token, else load the persisted one. */
    if (client->refresh_token == NULL) {
        char* stored = NULL;
        if (fsdk_secret_load(FSDK_SECRET_KEY_REFRESH, &stored) != 0 || stored == NULL) {
            return FSDK_ERR_NOT_AUTHENTICATED;
        }
        client->refresh_token = stored; /* take ownership */
    }

    char rt_esc[FSDK_TOKEN_BUF];
    char body[FSDK_TOKEN_BUF + 64];
    json_escape(client->refresh_token, rt_esc, sizeof(rt_esc));
    snprintf(body, sizeof(body), "{\"refreshToken\":\"%s\"}", rt_esc);
    memset(rt_esc, 0, sizeof(rt_esc));

    char* resp = NULL;
    long status = 0;
    fsdk_result r = fsdk_http_request(auth_base(client), FSDK_HTTP_POST, FSDK_PATH_REFRESH,
                                      NULL, body, &resp, &status);
    memset(body, 0, sizeof(body));
    if (r != FSDK_OK) {
        return r;
    }
    if (status < 200 || status >= 300) {
        fsdk_string_free(resp);
        /* A rejected refresh means the stored token is dead - clear it so a stale
         * token never trips reuse-detection on the next attempt. */
        if (status == 401 || status == 403) {
            fsdk_secret_delete(FSDK_SECRET_KEY_REFRESH);
            scrub_field(&client->refresh_token);
            client->authenticated = 0;
        }
        return status_to_result(status);
    }

    r = apply_session(client, resp);
    fsdk_string_free(resp);
    if (r == FSDK_OK) {
        fsdk_log(FSDK_LOG_INFO, "fsdk refresh ok");
    }
    return r;
}

fsdk_result fsdk_try_resume(fsdk_client* client) {
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }
    /* Drop any in-memory token so refresh() pulls the persisted one. */
    scrub_field(&client->refresh_token);
    return fsdk_refresh(client);
}

fsdk_result fsdk_logout(fsdk_client* client) {
    if (client == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }

    /* Best-effort server revoke if we have a refresh token (in-memory or stored). */
    char* loaded = NULL;
    const char* rt = client->refresh_token;
    if (rt == NULL && fsdk_secret_load(FSDK_SECRET_KEY_REFRESH, &loaded) == 0) {
        rt = loaded;
    }
    if (rt != NULL) {
        char rt_esc[FSDK_TOKEN_BUF];
        char body[FSDK_TOKEN_BUF + 64];
        json_escape(rt, rt_esc, sizeof(rt_esc));
        snprintf(body, sizeof(body), "{\"refreshToken\":\"%s\"}", rt_esc);
        memset(rt_esc, 0, sizeof(rt_esc));
        long status = 0;
        fsdk_http_request(auth_base(client), FSDK_HTTP_POST, FSDK_PATH_LOGOUT,
                          NULL, body, NULL, &status);
        memset(body, 0, sizeof(body));
    }
    if (loaded != NULL) {
        memset(loaded, 0, strlen(loaded));
        free(loaded);
    }

    /* Clear persisted + in-memory state regardless of the server outcome. */
    fsdk_secret_delete(FSDK_SECRET_KEY_REFRESH);
    scrub_field(&client->player_token);
    scrub_field(&client->refresh_token);
    client->authenticated = 0;
    client->foundry_id[0] = '\0';
    client->display_name[0] = '\0';
    fsdk_log(FSDK_LOG_INFO, "fsdk logout");
    return FSDK_OK;
}

#endif /* FID-embedded auth */
