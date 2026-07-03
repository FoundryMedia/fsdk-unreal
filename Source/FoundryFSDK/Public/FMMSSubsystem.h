// Copyright Foundry Media. FMMS matchmaking orchestrator.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FoundryFSDKSubsystem.h" // EFoundryFsdkResult / EFoundryMatchStatus / FFoundryConnection
#include "FMMSSubsystem.generated.h"

/** Player-visible matchmaking phase (drives menu UI). */
UENUM(BlueprintType)
enum class EFMMSPhase : uint8
{
	Idle           UMETA(DisplayName = "Idle"),
	Authenticating UMETA(DisplayName = "Authenticating"),
	Requesting     UMETA(DisplayName = "Requesting Match"),
	Searching      UMETA(DisplayName = "Searching"),
	Connecting     UMETA(DisplayName = "Connecting"),
	Traveling      UMETA(DisplayName = "Joining Server"),
	Failed         UMETA(DisplayName = "Failed")
};

/** Broadcast on the game thread on every phase change. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFMMSStatusEvent, EFMMSPhase, Phase, const FString&, Message);

/**
 * FMMS matchmaking orchestrator. Drives the full player loop on top of {@link UFoundryFSDKSubsystem}:
 *   InitializeClient -> Authenticate -> RequestMatch -> (poll) -> GetConnection
 *   -> ClientTravel("<ip>:<port>?token=<matchToken>")
 * The dedicated server validates that match token (FID JWKS) in PreLogin before admitting the player.
 *
 * SECURITY: the player token is PASSED IN and forwarded to the SDK only - never stored or logged here.
 * The travel URL carries the match token, so it is never logged either. See
 * .claude/rules/fsdk-security.md.
 */
UCLASS()
class FOUNDRYFSDK_API UFMMSSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	/**
	 * Run the matchmaking flow end to end. {@code AttributesJson} may be empty (defaults to "{}").
	 * {@code ApiBaseUrl} may be empty (defaults to https://api.foundryplatform.app). A flow already in
	 * progress is a no-op.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FMMS")
	void FindMatch(const FString& Queue, const FString& AttributesJson,
	               const FString& PlayerToken, const FString& ApiBaseUrl);

	/**
	 * Run matchmaking using the session already established on UFoundryFSDKSubsystem
	 * (via Login / TryResumeSession) - the token stays inside the SDK and never
	 * passes through Blueprint. Fails if not signed in. {@code AttributesJson} may be
	 * empty (-> "{}"). This is the path Foundry-account games (e.g. Conquest) use.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FMMS")
	void FindMatchAuthenticated(const FString& Queue, const FString& AttributesJson);

	/** Cancel an in-flight flow (also cancels the FMMS ticket, best-effort). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FMMS")
	void Cancel();

	UFUNCTION(BlueprintCallable, BlueprintPure, Category = "Foundry|FMMS")
	EFMMSPhase GetPhase() const { return Phase; }

	/** Bind in UMG/Blueprint to surface progress + errors to the player. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|FMMS")
	FFMMSStatusEvent OnFMMSStatus;

private:
	UFUNCTION() void HandleAuth(EFoundryFsdkResult Result);
	UFUNCTION() void HandleRequest(EFoundryFsdkResult Result);
	UFUNCTION() void HandlePoll(EFoundryFsdkResult Result, EFoundryMatchStatus Status);
	UFUNCTION() void HandleConnection(EFoundryFsdkResult Result, FFoundryConnection Connection);

	void Poll();
	void StartPolling();
	void StopPolling();
	void SetPhase(EFMMSPhase NewPhase, const FString& Message);
	void Fail(const FString& Message);
	UFoundryFSDKSubsystem* Fsdk() const;
	void BindOnce(UFoundryFSDKSubsystem* Sdk);

	EFMMSPhase Phase = EFMMSPhase::Idle;
	FString PendingQueue;
	FString PendingAttributes;
	FTimerHandle PollTimer;
	bool bBound = false;

	static constexpr float PollIntervalSeconds = 2.0f;
};
