// Copyright Foundry Media. FoundryFSDK Unreal subsystem facade.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FoundryFSDKSubsystem.generated.h"

// Forward declaration of the opaque fsdk-core client handle so this public
// header does not leak the C ABI into every includer. The .cpp includes
// <foundry/fsdk.h> and works with the real type.
struct fsdk_client;

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

/**
 * FoundryFSDK game-client facade.
 *
 * A GameInstance subsystem that wraps the fsdk-core CLIENT C ABI with idiomatic,
 * Blueprint-callable UE methods: Authenticate, RequestMatch, PollMatch,
 * GetConnection. The dedicated-server path (Agones lifecycle, player-token
 * validation) is a separate concern handled outside this client subsystem.
 *
 * SECURITY: the player's FID token is PASSED IN to Authenticate by game code and
 * is never stored by this subsystem beyond the underlying client handle's
 * lifetime. No secrets live here. See ../../fsdk-core/SECURITY.md.
 *
 * SCAFFOLD: wraps the C ABI stubs; real behavior lands when fsdk-core is
 * implemented and the ThirdParty lib is linked.
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
	 * (e.g. "https://api.foundryplatform.app"). Call once before Authenticate.
	 * @return true on success.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	bool InitializeClient(const FString& BaseUrl);

	/**
	 * Authenticate with the PLAYER'S OWN FID session token, obtained by game
	 * code through the platform's normal sign-in. The token is not persisted.
	 * @return true on success.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	bool Authenticate(const FString& PlayerToken);

	/**
	 * Request a match in a named queue. AttributesJson is an opaque,
	 * queue-specific JSON object string (may be empty).
	 * @return true if the ticket was accepted.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	bool RequestMatch(const FString& Queue, const FString& AttributesJson);

	/** Poll the current match status of the active ticket. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	EFoundryMatchStatus PollMatch();

	/**
	 * Fetch connection details once the match is Found. Hand OutConnection.Ip /
	 * Port to the engine netcode; forward MatchToken to the server.
	 * @return true if a connection was available.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	bool GetConnection(FFoundryConnection& OutConnection);

	/** Cancel the active ticket (best-effort). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FSDK")
	void CancelMatch();

private:
	/** Underlying fsdk-core client handle (owned). */
	fsdk_client* Client = nullptr;

	/**
	 * Active matchmaking ticket handle (owned). Typed as void* here to avoid
	 * leaking fsdk_ticket into this public header; cast in the .cpp.
	 */
	void* ActiveTicket = nullptr;
};
