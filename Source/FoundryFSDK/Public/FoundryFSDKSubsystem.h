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

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkResultEvent OnAuthenticateComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkResultEvent OnRequestMatchComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkStatusEvent OnPollMatchComplete;

	UPROPERTY(BlueprintAssignable, Category = "Foundry|FSDK")
	FFoundryFsdkConnectionEvent OnGetConnectionComplete;

private:
	/** Ref-counted fsdk-core handles + serialization lock (owned). */
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> Core;
};
