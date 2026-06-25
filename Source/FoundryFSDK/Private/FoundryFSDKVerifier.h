// Copyright Foundry Media. Server-only OpenSSL-backed JWT verifier seam fill.
//
// fsdk-core does all the dependency-free JWT work (compact split, base64url,
// claim + match-binding checks, alg pinning) and delegates ONLY the raw RS256
// signature check to a host verifier (fsdk_set_jwt_verifier). This module fills
// that seam with UE's bundled OpenSSL (1.1.1), resolving the signing key by `kid`
// from auth-efga's published JWKS. SERVER-ONLY: gated out of the player binary.
// See ../../fsdk-core/SECURITY.md and .claude/rules/fsdk-security.md.

#pragma once

#include "CoreMinimal.h"

#if defined(FOUNDRY_FSDK_SERVER) && FOUNDRY_FSDK_SERVER

/** Install the OpenSSL RS256 verifier into fsdk-core (fsdk_set_jwt_verifier) and
 *  point it at the auth-efga JWKS at <AuthBaseUrl>/.well-known/jwks.json. Call once
 *  at server startup before any player join. */
void FoundryFSDKInstallJwtVerifier(const FString& AuthBaseUrl);

/** Best-effort: fetch + cache the JWKS now so the first PreLogin doesn't block on a
 *  synchronous fetch. Safe to call on the game thread. */
void FoundryFSDKPrewarmJwks();

/** Remove the verifier from fsdk-core and free the cached public keys. */
void FoundryFSDKShutdownJwtVerifier();

#endif // FOUNDRY_FSDK_SERVER
