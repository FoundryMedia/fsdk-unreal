// Copyright Foundry Media. FoundryFSDK: the leader's post-match leave takes the party with it.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "FoundryPartyFollowComponent.generated.h"

class APlayerController;

/**
 * The game adds ONE of these to its PlayerController (one line in the constructor:
 * CreateDefaultSubobject<UFoundryPartyFollowComponent>(TEXT("PartyFollow"))) and gets the platform
 * default for free: once a match is OVER, when the party LEADER leaves, every other party member who is
 * still sitting on the results screen leaves with them (Justin, 2026-10-16: "if the party leader leaves
 * the match they should bring their party with them by default. A non party leader can leave back to
 * lobby and wait for the rest of their team"). A dev opts out with the kit setting
 * UFoundryUISettings::bPartyFollowsLeaderOnLeave (or SetFollowLeaderOnLeave(false)).
 *
 * WHY an RPC through the game server and not a platform poll: the game server is the one place every
 * seated client already shares a channel, so the fan-out is instant and costs the platform nothing
 * (Justin: "we dont want to use polling for this... we want the RPC"). The server does not know parties
 * and does not try to: it only relays "this party's leader left" to every OTHER controller's component,
 * and each receiver decides for itself against its OWN cached party (UFoundrySocialSubsystem).
 *
 * Scope is AFTER the horn only: a leader leaving a match that is still running leaves alone (members
 * "will leave on their own"), and a receiver whose own match-over flag (UFMMSSubsystem::WasMatchEnded)
 * is not set ignores the message. A crashed leader sends nothing - honest: a crash is not a leave.
 *
 * Trust trade (v1): any party member who knows the leader's foundry id can pull the party out post-horn.
 * The match is over, nothing is lost; the server-signed party claim is the strong version if it is ever
 * wanted. The kit's own leave paths (match-results RETURN, pause-menu LEAVE MATCH) call
 * NotifyLocalLeavingFor on the way out; a game with its own leave button calls NotifyLocalLeaving.
 */
UCLASS(ClassGroup = (Foundry), meta = (BlueprintSpawnableComponent))
class FOUNDRYFSDK_API UFoundryPartyFollowComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UFoundryPartyFollowComponent();

	/** How a receiving client actually leaves. The core module cannot see the UI kit's
	 *  IFoundryMenuHost, so FoundryFSDKUI installs this at startup (UFoundryUISettings::
	 *  ApplyPartyDefaults); a game without the UI kit installs its own. Returns false when the
	 *  controller has no way to leave (logged as one WARN by the component). */
	using FLeaveExecutor = TFunction<bool(APlayerController*)>;

	/** Process-wide default switch (true). Pushed from UFoundryUISettings at FoundryFSDKUI startup:
	 *  the core module must not reference the UI settings class. */
	static void SetFollowLeaderOnLeave(bool bEnabled);
	static bool GetFollowLeaderOnLeave();
	static void SetLeaveExecutor(FLeaveExecutor InExecutor);

	/**
	 * Call as the LOCAL player leaves a match (BEFORE the leave itself, so the message is queued while
	 * the connection is still up). Does nothing unless the local player LEADS a party with other joined
	 * members, the match is already over for them, and the switch is on - so it is safe to call
	 * unconditionally from any leave path.
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Party")
	void NotifyLocalLeaving();

	/** The same, resolved from a controller (the kit's leave paths use this); no component = a quiet no-op. */
	static void NotifyLocalLeavingFor(APlayerController* Controller);

	/** Leader -> server. Validated: both ids non-empty and at most 64 characters. */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerPartyLeaderLeaving(const FString& InPartyId, const FString& InLeaderFoundryId);

	/** Server -> every OTHER controller's component. The receiver checks it against its own party. */
	UFUNCTION(Client, Reliable)
	void ClientPartyLeaderLeft(const FString& InPartyId, const FString& InLeaderFoundryId);
};
