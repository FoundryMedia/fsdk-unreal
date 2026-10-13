// Copyright Foundry Media. FoundryFSDK social-state poller.

#pragma once

#include "CoreMinimal.h"
#include "FoundryFSDKSubsystem.h" // FFoundryFriend / FFoundryParty / FFoundryPartyMember / EFoundryFsdkResult
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "FoundrySocialSubsystem.generated.h"

/**
 * A development fixture: while bActive, Friends() / Blocked() / Invites() / PendingRequests() /
 * HasParty() / Party() / IsLocalLeader() / PartyCapacity() / JoinedCount() answer from here instead
 * of the cached platform reads, and every action below (SendFriendRequest, InviteByUsername, ...) is
 * a logged no-op - a simulated state can never reach the platform. bHasParty / Party default to no
 * party (false / empty); a consumer that wants a simulated party sets them explicitly.
 */
USTRUCT(BlueprintType)
struct FOUNDRYFSDK_API FFoundrySocialFixture
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	bool bActive = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	TArray<FFoundryFriend> Friends;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	TArray<FFoundryFriend> Blocked;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	TArray<FFoundryParty> Invites;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	int32 PendingRequests = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	bool bHasParty = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	FFoundryParty Party;
};

/**
 * The ONE social-state poller every UI surface (the default drawer, a game's chat panel, a future
 * console, a game's own HUD) shares, so the platform is polled once no matter how many surfaces read
 * it. Binds UFoundryFSDKSubsystem's identity / party / friends / pending / blocked / social-action
 * delegates ONCE (Initialize), caches the results for the game thread, and drives the OPEN/CLOSED
 * poll cadence a panel being on screen raises (SetPanelOpen) - ported from a lobby model's social
 * half (bind-once + 10s/30s cadence + re-poll-on-action-complete), generalized off any one game.
 *
 * A consumer reads Friends() / Blocked() / Party() / Invites() / PendingRequests() and calls the
 * actions below; it never talks to UFoundryFSDKSubsystem's social surface directly. SetFixture
 * replaces the lists for DISPLAY (dev tooling) and makes every action a logged no-op.
 */
UCLASS()
class FOUNDRYFSDK_API UFoundrySocialSubsystem : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	/** The cap PartyCapacity() falls back to when the server hasn't reported a MaxSize. */
	static constexpr int32 DefaultPartyCapacity = 8;

	// -- USubsystem ----------------------------------------------------------------------------------
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// -- FTickableGameObject ---------------------------------------------------------------------------
	virtual void Tick(float DeltaTime) override;
	virtual bool IsTickable() const override { return true; }
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(UFoundrySocialSubsystem, STATGROUP_Tickables); }

	// -- reads (cached, game thread; never block) -------------------------------------------------------

	const TArray<FFoundryFriend>& Friends() const { return Fixture.bActive ? Fixture.Friends : CachedFriends; }
	const TArray<FFoundryFriend>& Blocked() const { return Fixture.bActive ? Fixture.Blocked : CachedBlocked; }
	const TArray<FFoundryParty>& Invites() const { return Fixture.bActive ? Fixture.Invites : CachedInvites; }
	int32 PendingRequests() const { return Fixture.bActive ? Fixture.PendingRequests : CachedPendingRequests; }

	bool IsSignedIn() const { return bSignedIn; }
	FString LocalFoundryId() const { return LocalId; }
	FString LocalDisplayName() const { return LocalDisplayNameCached; }
	FString LocalUsername() const { return LocalUsernameCached; }

	bool HasParty() const { return Fixture.bActive ? Fixture.bHasParty : !CachedParty.PartyId.IsEmpty(); }
	const FFoundryParty& Party() const { return Fixture.bActive ? Fixture.Party : CachedParty; }
	bool IsLocalLeader() const;
	/** True when FoundryId leads the current party; false when not in a party or FoundryId is empty. */
	bool IsLeader(const FString& FoundryId) const;
	/** The local player's own ready flag from the cached party (false when not in a party). */
	bool IsSelfReady() const;
	/** The server's MaxSize when known (in a party), else DefaultPartyCapacity. */
	int32 PartyCapacity() const;
	/** 1 when not in a party (you are your own one-person "party"); otherwise the number of JOINED
	 *  (not merely invited/pending) members. */
	int32 JoinedCount() const;

	/** True while a dev fixture replaces the social lists and every action below is a no-op. */
	bool IsFixtureActive() const { return Fixture.bActive; }

	// -- cadence -----------------------------------------------------------------------------------------

	/** A social panel came on/off screen: 10s poll while any panel is open, 30s while every panel is
	 *  closed - an open flips the next Tick to poll at once. */
	void SetPanelOpen(bool bOpen);

	/** Ref-counted: the tick only POLLS while Consumers > 0 - zero consumers (e.g. every social
	 *  surface torn down for a match) means no network traffic at all, not even the 30s closed
	 *  cadence. Pair every RegisterConsumer with exactly one UnregisterConsumer (a widget's
	 *  NativeConstruct / NativeDestruct is the usual home). */
	void RegisterConsumer();
	void UnregisterConsumer();

	/** RefreshParty + RefreshPartyInvites + RefreshFriends + RefreshPendingRequests + RefreshBlocked. */
	void RefreshNow();

	// -- actions (through UFoundryFSDKSubsystem; fixture => no-op + OnActionFailed) -----------------------

	void SendFriendRequest(const FString& Username);
	/** Creates a party first when not already in one (the invite is queued and sent once the new
	 *  party arrives via HandleParty). */
	void InviteByUsername(const FString& Username);
	void AcceptInvite(const FString& PartyId);
	void DeclineInvite(const FString& PartyId);
	/** Leaves the CACHED party (Party().PartyId); a no-op when not in one. */
	void LeaveParty();
	/** Sets your own ready flag on the CACHED party (Party().PartyId); a no-op when not in one. */
	void SetReady(bool bReady);
	void RemoveFriend(const FString& FoundryId);
	void BlockPlayer(const FString& FoundryId);
	void UnblockPlayer(const FString& FoundryId);
	void RequestToJoin(const FString& FriendFoundryId);

	/** Dev fixture: replaces the cached lists for display and makes every action above a no-op. */
	void SetFixture(const FFoundrySocialFixture& InFixture);

	// -- events (native; any list / identity / party change) ---------------------------------------------

	DECLARE_MULTICAST_DELEGATE(FFoundrySocialChanged);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FFoundrySocialActionFailed, const FString& /*Action*/, const FString& /*Message*/);

	/** Any list, identity, or party change. */
	FFoundrySocialChanged OnChanged;
	/** The party roster specifically - a subset of OnChanged, for a consumer that only cares about
	 *  the roster (e.g. a game's own party-member display). */
	FFoundrySocialChanged OnPartyChanged;
	/** A social action's platform call failed, or was refused locally (fixture active, not signed
	 *  in, party full, ...): Action names it ("party.invite", "friend.request", ...), Message is a
	 *  human-readable reason a UI can show directly. */
	FFoundrySocialActionFailed OnActionFailed;

private:
	UFoundryFSDKSubsystem* Fsdk() const;
	void BindOnce();

	UFUNCTION() void HandleLogin(EFoundryFsdkResult Result, const FString& DisplayName);
	UFUNCTION() void HandleLoggedOut();
	UFUNCTION() void HandleParty(EFoundryFsdkResult Result, const FFoundryParty& Party);
	UFUNCTION() void HandleInvites(EFoundryFsdkResult Result, const TArray<FFoundryParty>& InInvites);
	UFUNCTION() void HandleFriends(EFoundryFsdkResult Result, const TArray<FFoundryFriend>& InFriends);
	UFUNCTION() void HandlePending(EFoundryFsdkResult Result, const TArray<FFoundryFriend>& Pending);
	UFUNCTION() void HandleBlocked(EFoundryFsdkResult Result, const TArray<FFoundryFriend>& InBlocked);
	UFUNCTION() void HandleSocialAction(EFoundryFsdkResult Result, const FString& Action);

	/** Called from the tail of every one of the five poll handlers (party/invites/friends/pending/
	 *  blocked), success or failure - counts it answered and flushes the coalesced OnChanged once
	 *  every outstanding read of the current poll has answered, or 1.5s after the first one has. */
	void NotePollAnswered();
	void MaybeFlushPollNotify();

	bool bBound = false;
	bool bTriedBindOnFirstTick = false; // Initialize() can race UFoundryFSDKSubsystem's own creation
	                                     // despite InitializeDependency on some platforms/modes; retry
	                                     // ONCE, on the first Tick, never every tick (that would spam
	                                     // the warning below forever when Fsdk() is genuinely absent).

	// identity (game-thread snapshot)
	bool bSignedIn = false;
	FString LocalId;
	FString LocalDisplayNameCached;
	FString LocalUsernameCached;

	// cached platform state
	FFoundryParty CachedParty;
	TArray<FFoundryFriend> CachedFriends;
	TArray<FFoundryFriend> CachedBlocked;
	TArray<FFoundryParty> CachedInvites;
	int32 CachedPendingRequests = 0;
	TSet<FString> SentFriendRequests;      // usernames (lowercase) this session already asked - a repeat says so
	FString InFlightFriendRequestUsername; // the one SendFriendRequest just fired; joins the set on Ok
	FString PendingInviteUsername; // queued behind a CreateParty, sent once the new party arrives
	bool bCreatePartyPending = false; // a CreateParty this subsystem fired is in flight - a second
	                                   // InviteByUsername queues its name instead of firing another

	// dev fixture
	FFoundrySocialFixture Fixture;

	// cadence
	bool bPanelOpen = false;
	double NextPollTime = 0.0;
	int32 ConsumerCount = 0; // RegisterConsumer/UnregisterConsumer - Tick polls only while > 0

	// OnChanged coalescing: RefreshNow fires 5 async reads (party/invites/friends/pending/blocked),
	// each landing on its own frame - without this, one poll cycle rebuilt the drawer up to 5 times
	// in a row, and a row (e.g. YOUR PARTY) could render a half-updated in-between state on one of
	// those rebuilds before the next handler's answer caught it up - the "flicker".
	int32 PollReadsOutstanding = 0; // 0 = no poll in flight
	bool bPollNotifyPending = false; // at least one handler landed and wants OnChanged to fire
	double PollFirstAnswerTime = -1.0; // FPlatformTime::Seconds() of this poll's first answer; -1 = none yet
	double LastPollStartTime = -1000.0; // for the "RefreshNow called again before the last poll landed" warning
};
