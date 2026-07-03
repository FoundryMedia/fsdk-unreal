// Copyright Foundry Media. Server-side FoundryFSDK facade (dedicated server only).
//
// Wraps fsdk-core's SERVER C ABI (Agones lifecycle + match-token admission gate)
// for the dedicated game server. Driven from the game's authoritative server code
// (e.g. the GameMode): create at startup, GetBindingAndReady, Health on a timer,
// ValidatePlayer on each join (PreLogin), Shutdown on teardown.
//
// SERVER-ONLY: the whole class is gated behind UE_SERVER so it is absent from the
// player/client/editor binary. The match-token VERIFY logic and OpenSSL never ship
// to players. The only credential involved is the player's FID-signed match token,
// which the server VALIDATES (it mints nothing). See .claude/rules/fsdk-security.md
// and ../../fsdk-core/SECURITY.md.

#pragma once

#include "CoreMinimal.h"

#if UE_SERVER

// Opaque holder for the fsdk-core server handle (defined in the .cpp so this public
// header never leaks the C ABI).
struct FFsdkServerState;

class FOUNDRYFSDK_API FFoundryFSDKServer
{
public:
	/**
	 * Create the server handle, install the OpenSSL RS256 verifier pointed at
	 * <AuthBaseUrl>/.well-known/jwks.json, and pre-warm the JWKS cache.
	 * @param AgonesAddr  Agones SDK sidecar HTTP gateway base (empty -> the Agones
	 *                    default http://127.0.0.1:9358).
	 * @param AuthBaseUrl auth-efga base URL whose JWKS verifies match tokens.
	 * @return the server facade, or nullptr on failure.
	 */
	static TUniquePtr<FFoundryFSDKServer> Create(const FString& AgonesAddr, const FString& AuthBaseUrl);

	~FFoundryFSDKServer();

	FFoundryFSDKServer(const FFoundryFSDKServer&) = delete;
	FFoundryFSDKServer& operator=(const FFoundryFSDKServer&) = delete;

	/** Read this box's match binding from the sidecar (latches the bound match id so
	 *  ValidatePlayer can enforce it), then signal Agones Ready. Returns Ready's ok. */
	bool GetBindingAndReady();

	/** Agones Health() ping; call on a fixed interval. */
	void Health();

	/**
	 * True once the platform asked this server to DRAIN (a customer/operator clicked Drain: the
	 * fcg/drain annotation is stamped on this GameServer). Latched - once true, stays true. The
	 * game should stop admitting players and call Shutdown once the session empties; the platform
	 * hard-stops a non-compliant server after a grace window. Poll on the Health cadence.
	 */
	bool IsDrainRequested();

	/**
	 * True once a match has been PLACED on this server (Agones state Allocated). Pure latch read -
	 * fed by the same sidecar polls as IsDrainRequested. Gates idle-empty auto-shutdown: a warm
	 * Ready replica is always empty and must never idle-exit.
	 */
	bool IsAllocated();

	/**
	 * Validate a joining player's match token (the admission gate). Returns true to
	 * ADMIT (fills OutFoundryId / OutMatchId with the verified identity); false to
	 * DROP (bad signature, expired, wrong audience/match, or no verifier).
	 */
	bool ValidatePlayer(const FString& MatchToken, FString& OutFoundryId, FString& OutMatchId);

	/** Agones Shutdown(); the orchestrator reclaims the box. */
	void Shutdown();

private:
	FFoundryFSDKServer() = default;

	FFsdkServerState* State = nullptr;
};

#endif // UE_SERVER
