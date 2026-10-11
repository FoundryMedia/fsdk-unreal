// Copyright Foundry Media. FMMS matchmaking orchestrator.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
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
	/** The server told us why the wait continues (no free capacity / a scale-up is in flight) - the
	 *  ticket stays alive, polling continues at the server's own cadence. See
	 *  FFoundryTicketWait / EFoundryWaitReason (S5 "matchmaking wait"). */
	WaitingForCapacity UMETA(DisplayName = "Waiting For Capacity"),
	Connecting     UMETA(DisplayName = "Connecting"),
	Traveling      UMETA(DisplayName = "Joining Server"),
	Failed         UMETA(DisplayName = "Failed"),
	/** The platform says this player is seated in a LIVE match - offer Reconnect. */
	Reconnectable  UMETA(DisplayName = "Match In Progress")
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

	/** Cancel an in-flight flow (also cancels the FMMS ticket, best-effort). From
	 *  Reconnectable this DECLINES the live seat (cancels its ticket server-side). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FMMS")
	void Cancel();

	/**
	 * Reconnect-aware menu-entry check: ask the PLATFORM whether this player is
	 * already seated in a live match (my-session). Active seat -> phase
	 * Reconnectable (offer the Reconnect button); no seat -> any STALE local
	 * state (Traveling/Failed left over after a server-initiated kick) resets to
	 * Idle so Find Match works immediately. No-op while a real search is in
	 * flight, or when not signed in. Call on menu-map entry / after a disconnect.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FMMS")
	void CheckActiveSession();

	/**
	 * Reconnect to the live match reported by CheckActiveSession: re-resolves the
	 * SAME ticket's connection (fid mints a fresh token, same match, same
	 * persisted team) and travels. Only valid in phase Reconnectable.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|FMMS")
	void Reconnect();

	/** True when the platform reported a live seat (menu shows Reconnect). */
	UFUNCTION(BlueprintPure, Category = "Foundry|FMMS")
	bool HasActiveMatch() const { return Phase == EFMMSPhase::Reconnectable; }

	/**
	 * The live match id this player is seated in, as last reported by the
	 * platform's my-session answer (CheckActiveSession / the reconnect flow).
	 * The GameInstance subsystem survives travel, so in-match UI (match/team
	 * chat) reads it after joining the server - call CheckActiveSession first
	 * if it is still empty (the connection payload deliberately carries no
	 * match id; my-session is the only client-side source). Empty = none.
	 */
	UFUNCTION(BlueprintPure, Category = "Foundry|FMMS")
	FString GetCurrentMatchId() const { return CurrentMatchId; }

	UFUNCTION(BlueprintPure, Category = "Foundry|FMMS")
	EFMMSPhase GetPhase() const { return Phase; }

	/** Bind in UMG/Blueprint to surface progress + errors to the player. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|FMMS")
	FFMMSStatusEvent OnFMMSStatus;

private:
	UFUNCTION() void HandleAuth(EFoundryFsdkResult Result);
	UFUNCTION() void HandleRequest(EFoundryFsdkResult Result);
	UFUNCTION() void HandlePoll(EFoundryFsdkResult Result, EFoundryMatchStatus Status, FFoundryTicketWait Wait);
	UFUNCTION() void HandleConnection(EFoundryFsdkResult Result, FFoundryConnection Connection);
	UFUNCTION() void HandleMySession(EFoundryFsdkResult Result, FFoundrySessionSeat Seat);

	void Poll();
	/** (Re)arm the poll timer as a ONE-SHOT, IntervalSeconds from now - every poll re-arms its own
	 *  next tick (S5: the server dictates the cadence via FFoundryTicketWait::RetryAfterSeconds), so
	 *  there is no looping timer to fall back on. */
	void StartPolling(float IntervalSeconds);
	void StopPolling();
	void AbandonReconnectSeat();
	void SetPhase(EFMMSPhase NewPhase, const FString& Message);
	void Fail(const FString& Message);
	/** Fail() with the player text resolved from FoundryMatchmakingCopy::ForResult(Result) - logs the
	 *  numeric Result separately (never in the player-facing string). */
	void FailWithResult(EFoundryFsdkResult Result);
	UFoundryFSDKSubsystem* Fsdk() const;
	void BindOnce(UFoundryFSDKSubsystem* Sdk);
	/** Anti-spam: true if enough time has passed since the last attempt (and stamps it); else false. */
	bool ThrottleFindMatch();
	/** Clamp a server-told (or fallback) retry interval to a sane range - never hammer fid, never go
	 *  quiet for minutes on a bad/absurd value. */
	static float ClampPollInterval(int32 RetryAfterSeconds);
	/** The client-side wait ceiling (S5): true once we've been waiting on this ticket past its known
	 *  expiresAt, or past NoExpiryWaitCeilingSeconds when that was never reported - never wait on a
	 *  QUEUED ticket forever, even while the server keeps answering QUEUED. */
	bool HasExceededWaitCeiling() const;

	EFMMSPhase Phase = EFMMSPhase::Idle;
	FString PendingQueue;
	FString PendingAttributes;
	/** Live-seat ticket id from my-session (set while phase is Reconnectable). */
	FString ReconnectTicketId;
	/** Live match id from my-session (survives travel; cleared when the seat
	 *  is gone / declined / a fresh search starts). */
	FString CurrentMatchId;
	FTimerHandle PollTimer;
	bool bBound = false;
	// Wall-clock of the last accepted FindMatch — throttles rapid re-clicks (a fast failure, e.g.
	// "no servers", returns to Idle/Failed, from which a re-click would otherwise submit again).
	double LastFindMatchSeconds = 0.0;

	// -- S5 client-side wait ceiling --
	// Wall-clock (FPlatformTime::Seconds) when the CURRENT search started - the fallback ceiling
	// when the ticket has never reported an expiresAt. Set in HandleRequest's success branch, read
	// by HasExceededWaitCeiling.
	double SearchStartedSeconds = 0.0;
	/** Last-known ticket TTL (Unix seconds, UTC) from FFoundryTicketWait::ExpiresAtUnix; 0 = unknown
	 *  (fall back to SearchStartedSeconds + NoExpiryWaitCeilingSeconds). */
	int64 TicketExpiresAtUnix = 0;

	static constexpr float PollIntervalSeconds = 2.0f;
	/** Minimum seconds between FindMatch attempts; sooner re-clicks are ignored (anti-spam). */
	static constexpr double FindMatchMinIntervalSeconds = 2.0;
	/** Poll re-arm clamp (S5): never faster than 2s (anti-spam toward fid), never slower than 60s
	 *  (a player must never look abandoned for minutes between pings). */
	static constexpr int32 MinPollIntervalSeconds = 2;
	static constexpr int32 MaxPollIntervalSeconds = 60;
	/** The client-side wait ceiling when the ticket has never reported an expiresAt (25 minutes). */
	static constexpr double NoExpiryWaitCeilingSeconds = 1500.0;
};
