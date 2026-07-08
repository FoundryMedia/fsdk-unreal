// Copyright Foundry Media. FoundryFSDK Unreal subsystem facade.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FoundryFSDKSubsystem.generated.h"

// Owns the fsdk-core handles + a lock that serializes core access across worker
// threads. Defined in the .cpp so this public header doesn't leak the C ABI.
struct FFsdkCoreState;

/** Blueprint-facing match status, mirrors fsdk_match_status from the C ABI. */
UENUM(BlueprintType)
enum class EFoundryMatchStatus : uint8
{
	Pending    UMETA(DisplayName = "Pending"),
	Searching  UMETA(DisplayName = "Searching"),
	Found      UMETA(DisplayName = "Found"),
	Cancelled  UMETA(DisplayName = "Cancelled"),
	Failed     UMETA(DisplayName = "Failed"),
	Expired    UMETA(DisplayName = "Expired"),
	Unknown    UMETA(DisplayName = "Unknown")
};

/**
 * Blueprint-facing result, mirrors fsdk_result. Lets game code branch on the
 * outcome - e.g. re-acquire a token on Unauthorized, retry on Network/Timeout,
 * keep polling on NoMatch.
 */
UENUM(BlueprintType)
enum class EFoundryFsdkResult : uint8
{
	Ok               UMETA(DisplayName = "OK"),
	InvalidArg       UMETA(DisplayName = "Invalid Argument"),
	NotAuthenticated UMETA(DisplayName = "Not Authenticated"),
	Unauthorized     UMETA(DisplayName = "Unauthorized"),
	Network          UMETA(DisplayName = "Network Error"),
	Timeout          UMETA(DisplayName = "Timeout"),
	Protocol         UMETA(DisplayName = "Protocol Error"),
	NoMatch          UMETA(DisplayName = "No Match / Not Ready"),
	NotImplemented   UMETA(DisplayName = "Not Implemented"),
	Unavailable      UMETA(DisplayName = "No Servers Available"),
	Internal         UMETA(DisplayName = "Internal Error"),
	Unknown          UMETA(DisplayName = "Unknown")
};

/** Blueprint-facing connection details, mirrors fsdk_connection. */
USTRUCT(BlueprintType)
struct FFoundryConnection
{
	GENERATED_BODY()

	/** Opaque rendezvous host (the box today, a relay endpoint in future). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	FString Ip;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	int32 Port = 0;

	/** Short-lived FID-signed match token; forward to the server on connect. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry|FSDK")
	FString MatchToken;
};

// Completion delegates - broadcast on the GAME THREAD when an async op finishes.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryFsdkResultEvent, EFoundryFsdkResult, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFsdkStatusEvent, EFoundryFsdkResult, Result, EFoundryMatchStatus, Status);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryFsdkConnectionEvent, EFoundryFsdkResult, Result, FFoundryConnection, Connection);

// Auth completion delegates - broadcast on the GAME THREAD.
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryLoginEvent, EFoundryFsdkResult, Result, const FString&, DisplayName);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFoundryLoggedOutEvent);

/**
 * FoundryFSDK game-client facade.
 *
 * A GameInstance subsystem wrapping the fsdk-core CLIENT C ABI. The core ABI is
 * synchronous and performs blocking HTTPS; this subsystem runs each call on a
 * WORKER THREAD and broadcasts an On...Complete delegate back on the game thread,
 * so the game thread never blocks on the network. Core access is serialized (the
 * core is not thread-safe), and the core handles live behind a shared,
 * ref-counted state so an in-flight worker can't outlive a freed client.
 *
 * SECURITY: the player's FID token is PASSED IN to Authenticate by game code and
 * is never stored by this subsystem, never persisted, never logged. No secrets
 * live here. See ../../fsdk-core/SECURITY.md.
 */
UCLASS()
class FOUNDRYFSDK_API UFoundryFSDKSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// USubsystem lifecycle.
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Create the underlying fsdk-core client bound to a platform API base URL
	 * (e.g. "https://api.foundryplatform.app"). Synchronous (no network) - call
	 * once before Authenticate. @return true on success.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	bool InitializeClient(const FString& BaseUrl);

	/**
	 * Authenticate with the PLAYER'S OWN FID session token, obtained by game code
	 * through the platform's normal sign-in. The token is not persisted. Async:
	 * broadcasts OnAuthenticateComplete.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void Authenticate(const FString& PlayerToken);

	/**
	 * Request a match in a named queue. AttributesJson is an opaque, queue-specific
	 * JSON object string (may be empty). Async: broadcasts OnRequestMatchComplete.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void RequestMatch(const FString& Queue, const FString& AttributesJson);

	/** Poll the active ticket's status. Async: broadcasts OnPollMatchComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void PollMatch();

	/**
	 * Fetch connection details once the match is Found. Async: broadcasts
	 * OnGetConnectionComplete with {Ip, Port, MatchToken} - hand Ip/Port to the
	 * engine netcode and forward MatchToken to the server.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void GetConnection();

	/** Cancel the active ticket (best-effort, fire-and-forget). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void CancelMatch();

	// ── Auto-login (DEFAULT: the Foundry launcher's session daemon) ─────────────
	// The game gets a short-lived matchmaking token from the launcher's session
	// daemon over the local FOUNDRY_IPC handoff - no in-game credentials, nothing
	// long-lived stored in the game. No launcher session -> fail fast (no form).

	/**
	 * Auto-authenticate from the launcher handoff (FOUNDRY_IPC). Async: broadcasts
	 * OnLoginComplete (Ok on success; NotAuthenticated when there's no launcher
	 * session, which the caller surfaces as "sign in through the Foundry launcher").
	 * The default sign-in for launcher-distributed games (e.g. Conquest).
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void AutoLoginFromLauncher();

	/**
	 * Set the player token directly (the launcher handoff / BYO identity): authenticate
	 * WITHOUT a login or the /v1/me/user probe - the caller already vouched. Synchronous.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void SetPlayerToken(const FString& PlayerToken);

	// ── FID-embedded in-game auth (OPT-IN build: FOUNDRY_FSDK_FID_AUTH=1) ────────
	// Declared ALWAYS (UHT forbids a UFUNCTION inside a non-editor #if), but their
	// BODIES + the underlying fsdk_login/keyring machinery compile ONLY when
	// FOUNDRY_FSDK_FID_AUTH=1. In the default (launcher) build these are inert stubs
	// (NotImplemented) and NO credential-login code ships - auth comes from the
	// launcher handoff (AutoLoginFromLauncher). See FoundryFSDKSubsystem.cpp.

	/** Log in with Foundry credentials (email/username + password). Async: OnLoginComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void Login(const FString& EmailOrUsername, const FString& Password, bool bRememberMe);

	/** Refresh the session from the stored/in-memory refresh token (rotates it). Async: OnLoginComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void Refresh();

	/** Resume a session from the persisted refresh token (no password). Async: OnLoginComplete. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void TryResumeSession();

	/** Revoke the session server-side and clear the persisted + in-memory tokens. Async: OnLoggedOut. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void Logout();

	/** Override the auth host (dev; default https://auth.foundryplatform.app). Synchronous. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Auth")
	void SetAuthBaseUrl(const FString& AuthBaseUrl);

	/** Cached login state (game-thread snapshot - never blocks on the network). */
	UFUNCTION(BlueprintPure, Category = "Foundry|Auth")
	bool IsLoggedIn() const { return bIsLoggedIn; }

	UFUNCTION(BlueprintPure, Category = "Foundry|Auth")
	FString GetDisplayName() const { return CachedDisplayName; }

	UFUNCTION(BlueprintPure, Category = "Foundry|Auth")
	FString GetFoundryId() const { return CachedFoundryId; }

	/** Login established/refreshed (DisplayName empty for BYO). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Auth")
	FFoundryLoginEvent OnLoginComplete;

	/** Session cleared. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Auth")
	FFoundryLoggedOutEvent OnLoggedOut;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkResultEvent OnAuthenticateComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkResultEvent OnRequestMatchComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkStatusEvent OnPollMatchComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkConnectionEvent OnGetConnectionComplete;

private:
	/** Ensure a client exists (lazily create one bound to the default api base). */
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> EnsureClient();

	/** Apply a session result on the game thread: cache identity + broadcast OnLoginComplete. */
	void ApplyLoginResult(EFoundryFsdkResult Result, const FString& DisplayName, const FString& FoundryId);

	/** Ref-counted fsdk-core handles + serialization lock (owned). */
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> Core;

	// Game-thread-only login snapshot (read by the BlueprintPure getters without a
	// lock, so they never block on an in-flight network call). Updated in the
	// game-thread completion of Login/Refresh/TryResume/SetPlayerToken/Logout.
	bool bIsLoggedIn = false;
	FString CachedDisplayName;
	FString CachedFoundryId;
};
