// Copyright Foundry Media. FoundryFSDK social-state poller.

#pragma once

#include "CoreMinimal.h"
#include "FoundryFSDKSubsystem.h" // FFoundryFriend / FFoundryParty / FFoundryPartyMember / EFoundryFsdkResult
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "FoundrySocialSubsystem.generated.h"

/**
 * A development fixture: while bActive, Friends() / Blocked() / Invites() / PendingRequests() /
 * HasParty() / Party() / IsLocalLeader() / PartyCapacity() / JoinedCount() / JoinRequests() /
 * PartyCapForQueue() answer from here instead of the cached platform reads, and every action below
 * (SendFriendRequest, InviteByUsername, ...) is refused with an OnActionFailed sentence ("Dev fixture
 * active") - a simulated state can never reach the platform, and the player is never left clicking a
 * silent no-op. bHasParty / Party default to no party (false / empty); a consumer that wants a
 * simulated party sets them explicitly.
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

	/** The simulated local player's foundryId while the fixture is active (LocalFoundryId() answers it,
	 *  and IsLocalLeader / IsSelfReady / AllReadyIn compare against it). Empty = the real signed-in id. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	FString LocalFoundryId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	FFoundryParty Party;

	/** Simulated pending join requests on the party the fixture's local player leads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	TArray<FFoundryJoinRequest> JoinRequests;

	/** Simulated per-queue party caps (queue key -> cap) for PartyCapForQueue. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry|Social")
	TMap<FString, int32> PartyCaps;
};

/** A social action succeeded. Action is the same name OnActionFailed / OnSocialActionComplete use
 *  ("party.invite", "party.cancel_invite", "party.kick", "party.join_request", ...); Message is a short
 *  player-facing sentence a UI can toast verbatim ("Invite sent to Ada", "Invite cancelled"). Only
 *  actions a player asked for announce themselves - background bookkeeping (ready, in-match) stays quiet. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundrySocialActionSucceeded,
	FName, Action, const FString&, Message);

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
	/** Pending requests to join the party YOU lead (empty unless you lead one). Polled on the same
	 *  cadence as everything else, and ONLY while IsLocalLeader() in a party. */
	const TArray<FFoundryJoinRequest>& JoinRequests() const { return Fixture.bActive ? Fixture.JoinRequests : CachedJoinRequests; }

	bool IsSignedIn() const { return bSignedIn; }
	FString LocalFoundryId() const { return EffectiveLocalId(); }
	FString LocalDisplayName() const { return LocalDisplayNameCached; }
	FString LocalUsername() const { return LocalUsernameCached; }

	bool HasParty() const { return Fixture.bActive ? Fixture.bHasParty : !CachedParty.PartyId.IsEmpty(); }
	const FFoundryParty& Party() const { return Fixture.bActive ? Fixture.Party : CachedParty; }
	bool IsLocalLeader() const;
	/** True when FoundryId leads the current party; false when not in a party or FoundryId is empty. */
	bool IsLeader(const FString& FoundryId) const;
	/** The local player's own ready flag from the cached party (false when not in a party). Ready in ANY
	 *  game - the Find Match gate is per game, see AllReadyIn. */
	bool IsSelfReady() const;
	/** The local player is ready IN THIS GAME (their member row's ReadyGameSlug == GameSlug). False when not
	 *  in a party or the slug is empty. */
	bool IsSelfReadyIn(const FString& InGameSlug) const;

	// -- ready belongs to the GAME (Round 2: "you shouldn't be able to ready up in the launcher") ----------

	/** The game slug SetReady sends and the Find Match gate measures against. The UI module pushes
	 *  UFoundryUISettings::GameSlug here at startup (like the copy overrides); empty clears the override. */
	void SetGameSlug(const FString& InSlug);
	/** Fallback when no explicit slug is set: a queue key is "{gameSlug}/{modeSlug}", so the first
	 *  queueable mode names the game. Accepts the FULL queue key ("goo-crew/solo", or an FRN) and takes its
	 *  game through FoundryQueueKey::GameSlug. A bare id / UUID leaves the default unset. */
	void SetDefaultGameSlugFromQueue(const FString& QueueKey);
	/** The explicit slug, else the one derived from a queue key, else empty. */
	FString GameSlug() const;

	/** True when every JOINED member (the local player included) is ready in GameSlug - the Find Match gate
	 *  for a party. A party of one needs nobody, so < 2 joined (or no party) answers true. An EMPTY slug
	 *  answers false for a party of 2+ (nothing can be ready in no game). */
	bool AllReadyIn(const FString& InGameSlug) const;
	/** JOINED members ready in GameSlug (the "2" of "ready 2 / 3"). 0 when not in a party or the slug is empty. */
	int32 ReadyCountIn(const FString& InGameSlug) const;
	/** Display names of the JOINED members NOT ready in GameSlug (display name, else username, else id) -
	 *  the local player included. Empty when not in a party. */
	TArray<FString> UnreadyNames(const FString& InGameSlug) const;
	/** The party leader's display name, else empty (not in a party / the leader's row is not loaded). */
	FString LeaderDisplayName() const;
	/** True while at least one social surface is registered (RegisterConsumer) - i.e. the cached party is
	 *  being kept fresh. UFMMSSubsystem's follow-mode poll gates on it (the same idle gating as the poller). */
	bool HasConsumers() const { return ConsumerCount > 0; }
	/** The server's MaxSize when known (in a party), else DefaultPartyCapacity. */
	int32 PartyCapacity() const;
	/** 1 when not in a party (you are your own one-person "party"); otherwise the number of JOINED
	 *  (not merely invited/pending) members. */
	int32 JoinedCount() const;
	/** INVITED rows whose invite is still alive (ExpiresAt zero = never expires, or in the future). The
	 *  server counts JOINED + these against the cap, so an expired invite frees its seat. 0 when not in a
	 *  party. */
	int32 LiveInvitedCount() const;

	/** The largest party the queue seats (min(queue maxPartySize ?? teamSize, teamSize), fid's
	 *  /party-cap). -1 while unknown - and the first unknown (or >5-min-old) read of a key kicks
	 *  RefreshPartyCap(QueueKey) once, so a consumer can simply call this every refresh and listen for
	 *  OnChanged. Empty key = -1, no request. */
	int32 PartyCapForQueue(const FString& QueueKey) const;

	/** The mode the match cluster currently has selected, as a queue key. The GAME sets it (the mode
	 *  picker does, on every change); the subsystem then passes it as queue_key on invite / re-invite /
	 *  join-accept so fid holds the party to THAT mode's cap. The launcher never sets it and so gets the
	 *  platform cap. Empty = no mode selected. Changing it broadcasts OnChanged and warms the mode's cap. */
	FString SelectedQueueKey() const { return SelectedQueue; }
	void SetSelectedQueueKey(const FString& Key);

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
	 *  party arrives via HandleParty). Passes SelectedQueueKey(). A name that is already an INVITED
	 *  member is a RE-invite (resets the clock, re-notifies); one that is JOINED is refused. */
	void InviteByUsername(const FString& Username);
	/** Leader: invite an INVITED member again (their invite expired, or they missed it). A JOINED member
	 *  is refused locally ("already in your party"). Passes SelectedQueueKey(). ("party.invite") */
	void ReInvite(const FString& FoundryId);
	/** Leader: withdraw a pending invite ("party.cancel_invite"); remove a JOINED member, never yourself
	 *  ("party.kick"). */
	void CancelInvite(const FString& FoundryId);
	void Kick(const FString& FoundryId);
	/** Leader: answer a request from JoinRequests() ("party.join_accept" / "party.join_decline").
	 *  Accept passes SelectedQueueKey() so the seat is refused over the mode's cap. */
	void AcceptJoinRequest(const FString& FoundryId);
	void DeclineJoinRequest(const FString& FoundryId);
	/** Refresh the cached cap for a queue now (also what PartyCapForQueue kicks). No-op in a fixture. */
	void RefreshPartyCap(const FString& QueueKey);
	void AcceptInvite(const FString& PartyId);
	void DeclineInvite(const FString& PartyId);
	/** Leaves the CACHED party (Party().PartyId); a no-op when not in one. */
	void LeaveParty();
	/** Sets your own ready flag on the CACHED party (Party().PartyId) FOR THIS GAME (GameSlug()); a no-op
	 *  when not in a party. Ready is only ever set from inside a game - the platform refuses a ready with no
	 *  game - and fid clears it when you travel into a match, so every match needs a fresh ready-up. With
	 *  no game slug known a ready=true says so through OnActionFailed instead of sending the refused request. */
	void SetReady(bool bReady);
	/** Sets your own in-match flag on the CACHED party (Party().PartyId); a no-op when not in one.
	 *  UFMMSSubsystem calls it: true when the client travels into a match, false at match end / leave, so
	 *  the lobby can show "In match" on a member's row and nameplate. A failure is logged, never shown:
	 *  it is background bookkeeping the player did not ask for (an older fid answers 404). */
	void SetInMatch(bool bInMatch);
	void RemoveFriend(const FString& FoundryId);
	void BlockPlayer(const FString& FoundryId);
	void UnblockPlayer(const FString& FoundryId);
	/** Ask to join a friend's party (they approve). Succeeds with "Asked to join <name>'s party". */
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
	/** A social action the player asked for succeeded - see FFoundrySocialActionSucceeded. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry|Social")
	FFoundrySocialActionSucceeded OnActionSucceeded;

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
	UFUNCTION() void HandleJoinRequests(EFoundryFsdkResult Result, const TArray<FFoundryJoinRequest>& Requests);
	UFUNCTION() void HandlePartyCap(EFoundryFsdkResult Result, const FString& QueueKey, int32 PartyCap, int32 TeamSize);
	UFUNCTION() void HandleSocialAction(EFoundryFsdkResult Result, const FString& Action);

	/** Called from the tail of every one of the five poll handlers (party/invites/friends/pending/
	 *  blocked), success or failure - counts it answered and flushes the coalesced OnChanged once
	 *  every outstanding read of the current poll has answered, or 1.5s after the first one has. */
	void NotePollAnswered();
	void MaybeFlushPollNotify();

	/** Fixture guard for a user action: when a dev fixture is active, say so through OnActionFailed and
	 *  return true (the caller returns) - a simulated state must never reach the platform, and a silent
	 *  no-op reads as a broken button. */
	bool RefuseInFixture(const TCHAR* Action);
	/** The local player's foundryId: the fixture's simulated one when a fixture sets it, else the signed-in id. */
	FString EffectiveLocalId() const;
	/** The member row for FoundryId in the (fixture-aware) party, or null. */
	const FFoundryPartyMember* FindMember(const FString& FoundryId) const;
	/** The cap an invite is held to right now: the selected mode's party cap when known, else the
	 *  party's platform cap. */
	int32 EffectiveInviteCap() const;
	/** JOINED + live INVITED against EffectiveInviteCap(); on a full party sets OutMessage and returns true. */
	bool PartyIsFull(FString& OutMessage) const;
	/** Queue the success sentence for the next completion of Action (FIFO per action; the platform call
	 *  answers with only the action name). Call right before handing the action to the bridge. */
	void ExpectSuccess(const TCHAR* Action, const FString& Message);
	/** Hand one invite to the bridge (by username, with the selected queue key) and queue its toast. */
	void SendInvite(const FString& Username);

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
	TArray<FFoundryJoinRequest> CachedJoinRequests;
	FString SelectedQueue; // SetSelectedQueueKey - passed as queue_key on invite / re-invite / join-accept
	FString GameSlugOverride; // SetGameSlug - the kit setting UFoundryUISettings::GameSlug
	FString DefaultGameSlug;  // SetDefaultGameSlugFromQueue - the first queueable mode's game
	FString PendingInviteUsername; // queued behind a CreateParty, sent once the new party arrives
	bool bCreatePartyPending = false; // a CreateParty this subsystem fired is in flight - a second
	                                   // InviteByUsername queues its name instead of firing another

	/** One queue's cached party cap. LastRequestTime throttles the refresh PartyCapForQueue kicks. */
	struct FPartyCapEntry
	{
		int32 Cap = -1;      // -1 = unknown
		int32 TeamSize = 0;
		double LastRequestTime = -1.0e9;
	};
	mutable TMap<FString, FPartyCapEntry> PartyCaps; // mutable: PartyCapForQueue is a read that warms its cache

	/** Success sentences waiting for their action's completion (see ExpectSuccess). */
	TMap<FString, TArray<FString>> ExpectedSuccess;

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
