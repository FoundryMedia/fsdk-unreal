/*
 * token.c - match-token verification (platform-signed JWT).
 *
 * The server-side admission gate (fsdk_server_validate_player) delegates here. A
 * match token is a short-lived RS256 JWT minted by auth-efga (the platform's sole
 * token issuer) and handed to the player inside the connection payload. Verifying
 * it is the security boundary that makes a scraped ip:port useless without a valid
 * token.
 *
 * The core does ALL the dependency-free work itself - compact split, base64url
 * decode, claim + match-binding checks, algorithm pinning - and delegates ONLY the
 * raw RSA-SHA256 signature check to the host-installed verifier seam
 * (fsdk_set_jwt_verifier), so the core links NO crypto library and stays
 * vendorable into any binary. The dedicated-server binding backs the seam with
 * OpenSSL, resolving the key by `kid` from FID's JWKS (the server is trusted; the
 * CLIENT never installs a verifier - this path is server-only).
 *
 * Order is SIGNATURE-FIRST: the signature is verified before any claim is trusted.
 * Fails closed - an unverifiable token (no verifier, bad signature, bad shape) is
 * a REJECTED token.
 */
#include "fsdk_internal.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The issuer auth-efga stamps (matches AuthProperties.jwt.issuer) and the audience
 * generateMatchToken sets. Hardcoded for the proof; a config seam can follow. */
#define FSDK_EXPECTED_ISS "https://auth.foundryplatform.app"
#define FSDK_EXPECTED_AUD "fsdk-match"
/* Tolerate a little clock drift between the box and auth-efga. */
#define FSDK_CLOCK_SKEW_SECONDS 60

/* --- base64url ------------------------------------------------------------- */

static int b64url_val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Decode base64url (no padding required) into out. Returns 1 on success. */
static int b64url_decode(const char* in, size_t in_len,
                         unsigned char* out, size_t out_cap, size_t* out_len) {
    while (in_len > 0 && in[in_len - 1] == '=') {
        in_len--; /* tolerate stray padding */
    }
    size_t o = 0, i = 0;
    while (i + 4 <= in_len) {
        int a = b64url_val(in[i]), b = b64url_val(in[i + 1]);
        int c = b64url_val(in[i + 2]), d = b64url_val(in[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return 0;
        unsigned v = ((unsigned)a << 18) | ((unsigned)b << 12) | ((unsigned)c << 6) | (unsigned)d;
        if (o + 3 > out_cap) return 0;
        out[o++] = (unsigned char)((v >> 16) & 0xFF);
        out[o++] = (unsigned char)((v >> 8) & 0xFF);
        out[o++] = (unsigned char)(v & 0xFF);
        i += 4;
    }
    size_t rem = in_len - i;
    if (rem == 1) return 0;
    if (rem == 2) {
        int a = b64url_val(in[i]), b = b64url_val(in[i + 1]);
        if (a < 0 || b < 0) return 0;
        unsigned v = ((unsigned)a << 18) | ((unsigned)b << 12);
        if (o + 1 > out_cap) return 0;
        out[o++] = (unsigned char)((v >> 16) & 0xFF);
    } else if (rem == 3) {
        int a = b64url_val(in[i]), b = b64url_val(in[i + 1]), c = b64url_val(in[i + 2]);
        if (a < 0 || b < 0 || c < 0) return 0;
        unsigned v = ((unsigned)a << 18) | ((unsigned)b << 12) | ((unsigned)c << 6);
        if (o + 2 > out_cap) return 0;
        out[o++] = (unsigned char)((v >> 16) & 0xFF);
        out[o++] = (unsigned char)((v >> 8) & 0xFF);
    }
    *out_len = o;
    return 1;
}

/* --- minimal JSON readers (json_value_after / json_extract_string + copy_bounded
 * are shared via fsdk_internal.h; the readers below are token-specific) ------- */

static int json_extract_long(const char* body, const char* key, long* out) {
    const char* v = json_value_after(body, key);
    if (v == NULL) return 0;
    char* end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v) return 0;
    *out = n;
    return 1;
}

/* aud may be a string ("fsdk-match") OR an array (["fsdk-match", ...]) - jjwt
 * emits an array. True iff `expected` is the/a member. */
static int json_aud_matches(const char* body, const char* expected) {
    const char* v = json_value_after(body, "aud");
    if (v == NULL) return 0;
    if (*v == '"') {
        char buf[128];
        return json_extract_string(body, "aud", buf, sizeof buf) && strcmp(buf, expected) == 0;
    }
    if (*v == '[') {
        const char* p = v + 1;
        while (*p != '\0' && *p != ']') {
            if (*p == '"') {
                const char* s = p + 1;
                char buf[128];
                size_t i = 0;
                while (*s != '\0' && *s != '"') {
                    char c = *s;
                    if (c == '\\' && s[1] != '\0') { s++; c = *s; }
                    if (i + 1 < sizeof buf) buf[i++] = c;
                    s++;
                }
                buf[i] = '\0';
                if (strcmp(buf, expected) == 0) return 1;
                p = (*s == '"') ? s + 1 : s;
            } else {
                p++;
            }
        }
    }
    return 0;
}

/* --- verify ---------------------------------------------------------------- */

fsdk_result fsdk_token_verify(const char* match_token,
                              const char* expected_match_id,
                              fsdk_player_info* out_info) {
    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
    }
    if (match_token == NULL) {
        return FSDK_ERR_INVALID_ARG;
    }

    /* Compact JWT: header_b64 . payload_b64 . signature_b64 */
    const char* dot1 = strchr(match_token, '.');
    if (dot1 == NULL) return FSDK_ERR_TOKEN_INVALID;
    const char* dot2 = strchr(dot1 + 1, '.');
    if (dot2 == NULL) return FSDK_ERR_TOKEN_INVALID;
    const char* sig_b64 = dot2 + 1;
    size_t header_len = (size_t)(dot1 - match_token);
    size_t payload_len = (size_t)(dot2 - (dot1 + 1));
    size_t sig_b64_len = strlen(sig_b64);
    size_t signing_len = (size_t)(dot2 - match_token); /* header_b64 "." payload_b64 */
    if (header_len == 0 || payload_len == 0 || sig_b64_len == 0) {
        return FSDK_ERR_TOKEN_INVALID;
    }

    /* Header -> pin alg (RS256, never "none") + read kid. */
    char header_json[1024];
    size_t hlen = 0;
    if (!b64url_decode(match_token, header_len,
                       (unsigned char*)header_json, sizeof(header_json) - 1, &hlen)) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    header_json[hlen] = '\0';
    char alg[16];
    if (!json_extract_string(header_json, "alg", alg, sizeof alg) || strcmp(alg, "RS256") != 0) {
        fsdk_log(FSDK_LOG_WARN, "fsdk_token_verify: alg not RS256 (rejected)");
        return FSDK_ERR_TOKEN_INVALID;
    }
    char kid[128];
    if (!json_extract_string(header_json, "kid", kid, sizeof kid)) {
        kid[0] = '\0';
    }

    /* SIGNATURE FIRST: verify before trusting any claim. */
    unsigned char sig[512];
    size_t siglen = 0;
    if (!b64url_decode(sig_b64, sig_b64_len, sig, sizeof sig, &siglen)) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    if (signing_len >= 2048) {
        return FSDK_ERR_TOKEN_INVALID; /* match tokens are small */
    }
    char signing_input[2048];
    memcpy(signing_input, match_token, signing_len);
    signing_input[signing_len] = '\0';

    fsdk_result vr = fsdk_dispatch_jwt_verify(kid, signing_input, sig, siglen);
    if (vr != FSDK_OK) {
        /* NOT_IMPLEMENTED (no verifier installed) or TOKEN_INVALID (bad sig);
         * either way the caller drops the connection. */
        return vr;
    }

    /* Signature is valid - now the claims can be trusted. */
    char payload_json[2048];
    size_t plen = 0;
    if (!b64url_decode(dot1 + 1, payload_len,
                       (unsigned char*)payload_json, sizeof(payload_json) - 1, &plen)) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    payload_json[plen] = '\0';

    char iss[256], sub[64], mid[64];
    long exp = 0, nbf = 0;
    if (!json_extract_string(payload_json, "iss", iss, sizeof iss) || strcmp(iss, FSDK_EXPECTED_ISS) != 0) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    if (!json_aud_matches(payload_json, FSDK_EXPECTED_AUD)) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    if (!json_extract_string(payload_json, "sub", sub, sizeof sub)) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    if (!json_extract_string(payload_json, "match_id", mid, sizeof mid)) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    if (!json_extract_long(payload_json, "exp", &exp)) {
        return FSDK_ERR_TOKEN_INVALID;
    }
    (void)json_extract_long(payload_json, "nbf", &nbf); /* optional */

    long now = (long)time(NULL);
    if (now > exp + FSDK_CLOCK_SKEW_SECONDS) {
        return FSDK_ERR_TOKEN_EXPIRED;
    }
    if (nbf != 0 && now + FSDK_CLOCK_SKEW_SECONDS < nbf) {
        return FSDK_ERR_TOKEN_INVALID;
    }

    /* Match binding: a token for match A must not admit a player to match B. */
    if (expected_match_id != NULL && expected_match_id[0] != '\0'
            && strcmp(expected_match_id, mid) != 0) {
        fsdk_log(FSDK_LOG_WARN, "fsdk_token_verify: match_id mismatch (rejected)");
        return FSDK_ERR_TOKEN_INVALID;
    }

    if (out_info != NULL) {
        copy_bounded(out_info->foundry_id, sizeof out_info->foundry_id, sub);
        copy_bounded(out_info->match_id, sizeof out_info->match_id, mid);
        out_info->expires_at = (int64_t)exp;
    }
    fsdk_log(FSDK_LOG_DEBUG, "fsdk_token_verify: token accepted");
    return FSDK_OK;
}
