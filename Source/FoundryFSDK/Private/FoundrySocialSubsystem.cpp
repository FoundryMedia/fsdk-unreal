#include "FoundrySocialSubsystem.h"

#include "Engine/GameInstance.h"
#include "FoundryQueueKey.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoundrySocial, Log, All);

UFoundryFSDKSubsystem* UFoundrySocialSubsystem::Fsdk() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
}

void UFoundrySocialSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency(UFoundryFSDKSubsystem::StaticClass());
	BindOnce();
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		// Snapshot getters never block; the login event (re)confirms them.
		bSignedIn = Sdk->IsLoggedIn();
		LocalId = Sdk->GetFoundryId();
		LocalDisplayNameCached = Sdk->GetDisplayName();
		LocalUsernameCached = Sdk->GetUsername();
	}
}

void UFoundrySocialSubsystem::Deinitialize()
{
	// A quit must not leave a stale ready behind: fid clears ready only when the member travels into a match, so
	// a player who readied and then closed the game would hold the whole party's gate open for a game nobody is
	// in. Best effort - the worker may or may not finish before the process exits (the member's party row is
	// also dropped when they leave or time out), but when it does the leader's gate is truthful again.
	if (!Fixture.bActive && bSignedIn && HasParty() && IsSelfReady())
	{
		if (UFoundryFSDKSubsystem* Sdk = Fsdk())
		{
			Sdk->SetPartyReadyInGame(CachedParty.PartyId, false, GameSlug());
		}
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->OnLoginComplete.RemoveAll(this);
		Sdk->OnLoggedOut.RemoveAll(this);
		Sdk->OnPartyUpdated.RemoveAll(this);
		Sdk->OnPartyInvitesUpdated.RemoveAll(this);
		Sdk->OnFriendsUpdated.RemoveAll(this);
		Sdk->OnPendingUpdated.RemoveAll(this);
		Sdk->OnBlockedUpdated.RemoveAll(this);
		Sdk->OnPartyJoinRequestsUpdated.RemoveAll(this);
		Sdk->OnPartyCapUpdated.RemoveAll(this);
		Sdk->OnSocialActionComplete.RemoveAll(this);
	}
	Super::Deinitialize();
}

void UFoundrySocialSubsystem::BindOnce()
{
	if (bBound)
	{
		return;
	}
	UFoundryFSDKSubsystem* Sdk = Fsdk();
	if (Sdk == nullptr)
	{
		UE_LOG(LogFoundrySocial, Warning, TEXT("[Social] UFoundryFSDKSubsystem unavailable - the social poller runs with empty lists."));
		return;
	}
	Sdk->OnLoginComplete.AddDynamic(this, &UFoundrySocialSubsystem::HandleLogin);
	Sdk->OnLoggedOut.AddDynamic(this, &UFoundrySocialSubsystem::HandleLoggedOut);
	Sdk->OnPartyUpdated.AddDynamic(this, &UFoundrySocialSubsystem::HandleParty);
	Sdk->OnPartyInvitesUpdated.AddDynamic(this, &UFoundrySocialSubsystem::HandleInvites);
	Sdk->OnFriendsUpdated.AddDynamic(this, &UFoundrySocialSubsystem::HandleFriends);
	Sdk->OnPendingUpdated.AddDynamic(this, &UFoundrySocialSubsystem::HandlePending);
	Sdk->OnBlockedUpdated.AddDynamic(this, &UFoundrySocialSubsystem::HandleBlocked);
	Sdk->OnPartyJoinRequestsUpdated.AddDynamic(this, &UFoundrySocialSubsystem::HandleJoinRequests);
	Sdk->OnPartyCapUpdated.AddDynamic(this, &UFoundrySocialSubsystem::HandlePartyCap);
	Sdk->OnSocialActionComplete.AddDynamic(this, &UFoundrySocialSubsystem::HandleSocialAction);
	bBound = true;
}

void UFoundrySocialSubsystem::Tick(float /*DeltaTime*/)
{
	// Catches the 1.5s safety-timeout case even when no new poll handler lands this frame (e.g. one
	// of the five reads never answers) - cheap no-op otherwise (bPollNotifyPending false).
	MaybeFlushPollNotify();

	if (!bBound && !bTriedBindOnFirstTick)
	{
		// Initialize() ran before UFoundryFSDKSubsystem existed - subsystem creation order beyond
		// InitializeDependency is not guaranteed on every platform/mode. Retry the bind ONCE, on
		// this first tick; never on every tick (Fsdk() can be genuinely absent for a whole session
		// - e.g. a lobby/fixture-only run with no FSDK session - and retrying forever would spam
		// the warning below every frame for nothing).
		bTriedBindOnFirstTick = true;
		BindOnce();
		if (bBound)
		{
			if (UFoundryFSDKSubsystem* Sdk = Fsdk())
			{
				bSignedIn = Sdk->IsLoggedIn();
				LocalId = Sdk->GetFoundryId();
				LocalDisplayNameCached = Sdk->GetDisplayName();
				LocalUsernameCached = Sdk->GetUsername();
			}
		}
	}
	if (Fixture.bActive || !bSignedIn || ConsumerCount <= 0)
	{
		return;
	}
	// Polling with idle controls: every 10s while any panel is open, every 30s while every panel is
	// closed (SetPanelOpen) - and the FAST cadence too while the local player LEADS a party of 2+ that is not
	// yet all ready in this game: the leader's Find Match gate opens the moment the last member readies, and
	// at the closed-drawer cadence it lagged up to 30 s behind (Find Match stayed disabled with everyone ready).
	const double Now = FPlatformTime::Seconds();
	const bool bLeaderWaitingOnReady = HasParty() && JoinedCount() >= 2 && IsLocalLeader()
		&& !GameSlug().IsEmpty() && !AllReadyIn(GameSlug());
	const double Interval = (bPanelOpen || bLeaderWaitingOnReady) ? 10.0 : 30.0;
	if (NextPollTime > Now + Interval)
	{
		NextPollTime = Now + Interval; // the fast cadence just began (a member joined, the drawer closed): do not wait out the slow slot
	}
	if (Now >= NextPollTime)
	{
		NextPollTime = Now + Interval;
		RefreshNow();
	}
}

void UFoundrySocialSubsystem::RegisterConsumer()
{
	++ConsumerCount;
	if (ConsumerCount == 1)
	{
		NextPollTime = 0.0; // the first consumer this round polls at once, like SetPanelOpen(true)
	}
}

void UFoundrySocialSubsystem::UnregisterConsumer()
{
	ConsumerCount = FMath::Max(0, ConsumerCount - 1);
}

void UFoundrySocialSubsystem::SetPanelOpen(bool bOpen)
{
	if (bPanelOpen == bOpen)
	{
		return;
	}
	bPanelOpen = bOpen;
	if (bOpen)
	{
		NextPollTime = 0.0; // refresh at once on open
	}
}

void UFoundrySocialSubsystem::RefreshNow()
{
	if (Fixture.bActive)
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		const double Now = FPlatformTime::Seconds();
		if (PollReadsOutstanding > 0 && (Now - LastPollStartTime) < 2.0)
		{
			UE_LOG(LogFoundrySocial, Warning, TEXT("[Social] social poll storm: RefreshNow called again %.2fs after the last one, with %d read(s) from it still outstanding."),
				Now - LastPollStartTime, PollReadsOutstanding);
		}
		LastPollStartTime = Now;
		// RefreshParty + RefreshPartyInvites + RefreshFriends + RefreshPendingRequests + RefreshBlocked
		// (+ the join requests while I lead a party) - NotePollAnswered (one per handler below) counts
		// these down and coalesces them into ONE OnChanged broadcast instead of up to six drawer rebuilds.
		// Join requests are leader-only server-side, so a non-leader never asks (it would only 404).
		const bool bPollJoinRequests = HasParty() && IsLocalLeader();
		PollReadsOutstanding = bPollJoinRequests ? 6 : 5;
		bPollNotifyPending = false;
		PollFirstAnswerTime = -1.0;
		Sdk->RefreshParty();
		Sdk->RefreshPartyInvites();
		Sdk->RefreshFriends();
		Sdk->RefreshPendingRequests();
		Sdk->RefreshBlocked();
		if (bPollJoinRequests)
		{
			Sdk->RefreshPartyJoinRequests(CachedParty.PartyId);
		}
		else
		{
			CachedJoinRequests.Reset(); // not leading a party: nobody can be waiting on me
		}
	}
}

void UFoundrySocialSubsystem::NotePollAnswered()
{
	if (PollFirstAnswerTime < 0.0)
	{
		PollFirstAnswerTime = FPlatformTime::Seconds();
	}
	PollReadsOutstanding = FMath::Max(0, PollReadsOutstanding - 1);
	bPollNotifyPending = true;
	MaybeFlushPollNotify();
}

void UFoundrySocialSubsystem::MaybeFlushPollNotify()
{
	if (!bPollNotifyPending)
	{
		return;
	}
	const bool bAllAnswered = PollReadsOutstanding <= 0;
	const bool bTimedOut = PollFirstAnswerTime >= 0.0 && (FPlatformTime::Seconds() - PollFirstAnswerTime) >= 1.5;
	if (!bAllAnswered && !bTimedOut)
	{
		return;
	}
	bPollNotifyPending = false;
	PollFirstAnswerTime = -1.0;
	OnChanged.Broadcast();
}

FString UFoundrySocialSubsystem::EffectiveLocalId() const
{
	return (Fixture.bActive && !Fixture.LocalFoundryId.IsEmpty()) ? Fixture.LocalFoundryId : LocalId;
}

bool UFoundrySocialSubsystem::IsLocalLeader() const
{
	if (!HasParty())
	{
		return true; // alone you are your own host
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	const FString Me = EffectiveLocalId();
	return !Me.IsEmpty() && P.LeaderFoundryId == Me;
}

bool UFoundrySocialSubsystem::IsLeader(const FString& FoundryId) const
{
	if (!HasParty() || FoundryId.IsEmpty())
	{
		return false;
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	return P.LeaderFoundryId == FoundryId;
}

bool UFoundrySocialSubsystem::IsSelfReady() const
{
	const FString Me = EffectiveLocalId();
	if (!HasParty() || Me.IsEmpty())
	{
		return false;
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	for (const FFoundryPartyMember& M : P.Members)
	{
		if (M.FoundryId == Me)
		{
			return M.bReady;
		}
	}
	return false;
}

bool UFoundrySocialSubsystem::IsSelfReadyIn(const FString& InGameSlug) const
{
	const FString Me = EffectiveLocalId();
	const FString Slug = InGameSlug.TrimStartAndEnd();
	if (!HasParty() || Me.IsEmpty() || Slug.IsEmpty())
	{
		return false;
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	for (const FFoundryPartyMember& M : P.Members)
	{
		if (M.FoundryId == Me)
		{
			return M.ReadyGameSlug.Equals(Slug, ESearchCase::IgnoreCase);
		}
	}
	return false;
}

// -- ready belongs to the GAME -------------------------------------------------------------------------------
// Justin, Round 2: "you shouldn't be able to ready up in the launcher. there is nothing to ready up for ...
// find match needs to be contingent on all players ready. that ensures that all players are actually playing
// the same game." fid stores WHICH game each member readied in (readyGameSlug) and refuses a search unless
// every joined member is ready in the queue's game; these reads are the client's view of the same rule.

namespace
{
	bool IsJoinedRow(const FFoundryPartyMember& M)
	{
		// Same test JoinedCount uses: an older fid sends no state on a joined row.
		return M.State.IsEmpty() || M.State.Equals(TEXT("JOINED"), ESearchCase::IgnoreCase);
	}

	bool IsReadyIn(const FFoundryPartyMember& M, const FString& Slug)
	{
		return !Slug.IsEmpty() && M.ReadyGameSlug.Equals(Slug, ESearchCase::IgnoreCase);
	}

	FString MemberLabel(const FFoundryPartyMember& M)
	{
		return !M.DisplayName.IsEmpty() ? M.DisplayName : (!M.Username.IsEmpty() ? M.Username : M.FoundryId);
	}
}

void UFoundrySocialSubsystem::SetGameSlug(const FString& InSlug)
{
	const FString Slug = InSlug.TrimStartAndEnd();
	if (Slug == GameSlugOverride)
	{
		return;
	}
	const FString Before = GameSlug();
	GameSlugOverride = Slug;
	if (GameSlug() != Before)
	{
		OnChanged.Broadcast(); // a ready label / the Find Match gate reads the slug
	}
}

void UFoundrySocialSubsystem::SetDefaultGameSlugFromQueue(const FString& QueueKey)
{
	const FString Slug = FoundryQueueKey::GameSlug(QueueKey); // one shared parser: plain keys AND FRNs
	if (Slug == DefaultGameSlug)
	{
		return;
	}
	const FString Before = GameSlug();
	DefaultGameSlug = Slug;
	if (GameSlug() != Before)
	{
		OnChanged.Broadcast();
	}
}

FString UFoundrySocialSubsystem::GameSlug() const
{
	return !GameSlugOverride.IsEmpty() ? GameSlugOverride : DefaultGameSlug;
}

bool UFoundrySocialSubsystem::AllReadyIn(const FString& InGameSlug) const
{
	if (!HasParty() || JoinedCount() < 2)
	{
		return true; // a party of one needs nobody
	}
	const FString Slug = InGameSlug.TrimStartAndEnd();
	if (Slug.IsEmpty())
	{
		return false;
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	for (const FFoundryPartyMember& M : P.Members)
	{
		if (IsJoinedRow(M) && !IsReadyIn(M, Slug))
		{
			return false;
		}
	}
	return true;
}

int32 UFoundrySocialSubsystem::ReadyCountIn(const FString& InGameSlug) const
{
	if (!HasParty())
	{
		return 0;
	}
	const FString Slug = InGameSlug.TrimStartAndEnd();
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	int32 N = 0;
	for (const FFoundryPartyMember& M : P.Members)
	{
		N += (IsJoinedRow(M) && IsReadyIn(M, Slug)) ? 1 : 0;
	}
	return N;
}

TArray<FString> UFoundrySocialSubsystem::UnreadyNames(const FString& InGameSlug) const
{
	TArray<FString> Names;
	if (!HasParty())
	{
		return Names;
	}
	const FString Slug = InGameSlug.TrimStartAndEnd();
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	for (const FFoundryPartyMember& M : P.Members)
	{
		if (IsJoinedRow(M) && !IsReadyIn(M, Slug))
		{
			Names.Add(MemberLabel(M));
		}
	}
	return Names;
}

FString UFoundrySocialSubsystem::LeaderDisplayName() const
{
	if (!HasParty())
	{
		return FString();
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	if (!P.LeaderDisplayName.IsEmpty())
	{
		return P.LeaderDisplayName;
	}
	for (const FFoundryPartyMember& M : P.Members)
	{
		if (!P.LeaderFoundryId.IsEmpty() && M.FoundryId == P.LeaderFoundryId)
		{
			return M.DisplayName;
		}
	}
	return FString();
}

int32 UFoundrySocialSubsystem::PartyCapacity() const
{
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	return P.MaxSize > 0 ? P.MaxSize : DefaultPartyCapacity;
}

int32 UFoundrySocialSubsystem::JoinedCount() const
{
	if (!HasParty())
	{
		return 1; // alone, you are your own one-person "party"
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	int32 N = 0;
	for (const FFoundryPartyMember& M : P.Members)
	{
		N += (M.State.IsEmpty() || M.State.Equals(TEXT("JOINED"), ESearchCase::IgnoreCase)) ? 1 : 0;
	}
	return N;
}

int32 UFoundrySocialSubsystem::LiveInvitedCount() const
{
	if (!HasParty())
	{
		return 0;
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	const FDateTime Now = FDateTime::UtcNow();
	int32 N = 0;
	for (const FFoundryPartyMember& M : P.Members)
	{
		// Zero ExpiresAt = absent (an older fid never expires an invite).
		const bool bLive = M.ExpiresAt.GetTicks() == 0 || M.ExpiresAt > Now;
		N += (M.State.Equals(TEXT("INVITED"), ESearchCase::IgnoreCase) && bLive) ? 1 : 0;
	}
	return N;
}

int32 UFoundrySocialSubsystem::PartyCapForQueue(const FString& InQueueKey) const
{
	const FString QueueKey = InQueueKey.TrimStartAndEnd();
	if (QueueKey.IsEmpty())
	{
		return -1;
	}
	if (Fixture.bActive)
	{
		// A fixture answers from its own table and never reaches the platform.
		const int32* Simulated = Fixture.PartyCaps.Find(QueueKey);
		return Simulated != nullptr ? *Simulated : -1;
	}
	FPartyCapEntry& Entry = PartyCaps.FindOrAdd(QueueKey);
	const double Now = FPlatformTime::Seconds();
	// Unknown, or known but >5 min old: ask once per window. A known value keeps answering meanwhile -
	// the cap only changes when a dev edits the queue, so a stale number beats flicker.
	if (Now - Entry.LastRequestTime > 300.0)
	{
		Entry.LastRequestTime = Now;
		const_cast<UFoundrySocialSubsystem*>(this)->RefreshPartyCap(QueueKey);
	}
	return Entry.Cap;
}

void UFoundrySocialSubsystem::SetSelectedQueueKey(const FString& InKey)
{
	const FString Key = InKey.TrimStartAndEnd();
	if (Key == SelectedQueue)
	{
		return;
	}
	SelectedQueue = Key;
	(void)PartyCapForQueue(Key); // warm the mode's cap so the picker can show which modes fit the party
	OnChanged.Broadcast();
}

void UFoundrySocialSubsystem::RefreshPartyCap(const FString& QueueKey)
{
	if (Fixture.bActive || QueueKey.IsEmpty())
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->RefreshPartyCap(QueueKey);
	}
}

bool UFoundrySocialSubsystem::RefuseInFixture(const TCHAR* Action)
{
	if (!Fixture.bActive)
	{
		return false;
	}
	OnActionFailed.Broadcast(Action, TEXT("Dev fixture active - actions are disabled."));
	return true;
}

const FFoundryPartyMember* UFoundrySocialSubsystem::FindMember(const FString& FoundryId) const
{
	if (!HasParty() || FoundryId.IsEmpty())
	{
		return nullptr;
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	return P.Members.FindByPredicate([&FoundryId](const FFoundryPartyMember& M) { return M.FoundryId == FoundryId; });
}

int32 UFoundrySocialSubsystem::EffectiveInviteCap() const
{
	if (!SelectedQueue.IsEmpty())
	{
		const int32 ModeCap = PartyCapForQueue(SelectedQueue);
		if (ModeCap > 0)
		{
			return ModeCap;
		}
	}
	const FFoundryParty& P = Party();
	return P.PlatformMaxSize > 0 ? P.PlatformMaxSize : PartyCapacity();
}

bool UFoundrySocialSubsystem::PartyIsFull(FString& OutMessage) const
{
	// The server's own count: JOINED + live INVITED (an expired invite frees its seat).
	const int32 Held = JoinedCount() + LiveInvitedCount();
	const int32 Cap = EffectiveInviteCap();
	if (Held < Cap)
	{
		return false;
	}
	const bool bModeCap = !SelectedQueue.IsEmpty() && PartyCapForQueue(SelectedQueue) == Cap;
	OutMessage = (bModeCap && JoinedCount() > Cap)
		? FString::Printf(TEXT("This mode's parties hold up to %d (you have %d)."), Cap, JoinedCount())
		: FString::Printf(TEXT("This party is full (%d of %d)."), Held, Cap);
	return true;
}

void UFoundrySocialSubsystem::ExpectSuccess(const TCHAR* Action, const FString& Message)
{
	ExpectedSuccess.FindOrAdd(Action).Add(Message);
}

void UFoundrySocialSubsystem::SendInvite(const FString& Username)
{
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		ExpectSuccess(TEXT("party.invite"), FString::Printf(TEXT("Invite sent to %s"), *Username));
		Sdk->InviteToPartyForQueue(CachedParty.PartyId, Username, SelectedQueue);
	}
}

// -- actions -------------------------------------------------------------------------------------------

void UFoundrySocialSubsystem::SendFriendRequest(const FString& InUsername)
{
	const FString Username = InUsername.TrimStartAndEnd();
	if (Username.IsEmpty())
	{
		return;
	}
	if (RefuseInFixture(TEXT("friend.request")))
	{
		return;
	}
	// fid answers a repeat request with the same 200 as a fresh one (idempotent by design), so the
	// "already" cases are known only here: the friends list and what this session already sent.
	const FString Key = Username.ToLower();
	for (const FFoundryFriend& F : Friends())
	{
		if (F.Username.Equals(Username, ESearchCase::IgnoreCase))
		{
			OnActionFailed.Broadcast(TEXT("friend.request"), FString::Printf(TEXT("You're already friends with %s."), *F.DisplayName));
			return;
		}
	}
	if (SentFriendRequests.Contains(Key))
	{
		OnActionFailed.Broadcast(TEXT("friend.request"), TEXT("You already sent them a request - it's waiting on them."));
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		InFlightFriendRequestUsername = Key;
		ExpectSuccess(TEXT("friend.request"), FString::Printf(TEXT("Friend request sent to %s"), *Username));
		Sdk->SendFriendRequest(Username);
	}
}

void UFoundrySocialSubsystem::InviteByUsername(const FString& InUsername)
{
	const FString Username = InUsername.TrimStartAndEnd();
	if (Username.IsEmpty())
	{
		return;
	}
	if (RefuseInFixture(TEXT("party.invite")))
	{
		return;
	}
	if (!bSignedIn)
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), TEXT("Sign in through the Foundry launcher before inviting."));
		return;
	}
	if (HasParty() && !IsLocalLeader())
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), TEXT("Only the party host can invite."));
		return;
	}
	// A name already on the roster: JOINED is refused, INVITED is a RE-invite (resets the clock and
	// re-notifies server-side). An INVITED row must never read as "already in your party" - that made
	// re-inviting impossible from the game, and a lapsed invite is exactly when you need it.
	bool bReInvite = false;
	if (HasParty())
	{
		for (const FFoundryPartyMember& M : Party().Members)
		{
			if (M.Username.Equals(Username, ESearchCase::IgnoreCase))
			{
				if (!M.State.Equals(TEXT("INVITED"), ESearchCase::IgnoreCase))
				{
					OnActionFailed.Broadcast(TEXT("party.invite"), FString::Printf(TEXT("%s is already in your party."), *M.DisplayName));
					return;
				}
				bReInvite = true;
				break;
			}
		}
	}
	FString FullMessage;
	if (!bReInvite && PartyIsFull(FullMessage))
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), FullMessage);
		return;
	}
	UFoundryFSDKSubsystem* Sdk = Fsdk();
	if (Sdk == nullptr)
	{
		return;
	}
	if (bCreatePartyPending)
	{
		// Already creating a party for an earlier InviteByUsername - queue the latest name instead
		// of firing a second CreateParty.
		PendingInviteUsername = Username;
		return;
	}
	if (HasParty())
	{
		SendInvite(Username);
	}
	else
	{
		// No party yet: create one, then invite once it arrives (HandleParty).
		PendingInviteUsername = Username;
		bCreatePartyPending = true;
		Sdk->CreateParty();
	}
}

void UFoundrySocialSubsystem::ReInvite(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("party.invite")))
	{
		return;
	}
	if (!HasParty() || !IsLocalLeader())
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), TEXT("Only the party host can invite."));
		return;
	}
	const FFoundryPartyMember* M = FindMember(FoundryId);
	if (M == nullptr)
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), TEXT("That player isn't on your invite list any more."));
		return;
	}
	if (!M->State.Equals(TEXT("INVITED"), ESearchCase::IgnoreCase))
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), FString::Printf(TEXT("%s is already in your party."), *M->DisplayName));
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		// No fullness pre-check: an INVITED row already holds its seat, so a re-invite never grows the party.
		ExpectSuccess(TEXT("party.invite"), FString::Printf(TEXT("Invite sent to %s"), *M->DisplayName));
		Sdk->InviteToPartyById(CachedParty.PartyId, FoundryId, SelectedQueue);
	}
}

void UFoundrySocialSubsystem::CancelInvite(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("party.cancel_invite")))
	{
		return;
	}
	if (!HasParty() || !IsLocalLeader())
	{
		OnActionFailed.Broadcast(TEXT("party.cancel_invite"), TEXT("Only the party host can cancel an invite."));
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		ExpectSuccess(TEXT("party.cancel_invite"), TEXT("Invite cancelled"));
		Sdk->CancelPartyInvite(CachedParty.PartyId, FoundryId);
	}
}

void UFoundrySocialSubsystem::Kick(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("party.kick")))
	{
		return;
	}
	if (!HasParty() || !IsLocalLeader())
	{
		OnActionFailed.Broadcast(TEXT("party.kick"), TEXT("Only the party host can remove a member."));
		return;
	}
	if (FoundryId == LocalId)
	{
		OnActionFailed.Broadcast(TEXT("party.kick"), TEXT("To go, leave the party."));
		return;
	}
	const FFoundryPartyMember* M = FindMember(FoundryId);
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		ExpectSuccess(TEXT("party.kick"), FString::Printf(TEXT("%s removed from the party"), M != nullptr ? *M->DisplayName : TEXT("Player")));
		Sdk->KickFromParty(CachedParty.PartyId, FoundryId);
	}
}

void UFoundrySocialSubsystem::AcceptJoinRequest(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("party.join_accept")))
	{
		return;
	}
	if (!HasParty() || !IsLocalLeader())
	{
		OnActionFailed.Broadcast(TEXT("party.join_accept"), TEXT("Only the party host can answer a request."));
		return;
	}
	FString FullMessage;
	if (PartyIsFull(FullMessage))
	{
		OnActionFailed.Broadcast(TEXT("party.join_accept"), FullMessage);
		return;
	}
	const FFoundryJoinRequest* Req = JoinRequests().FindByPredicate([&FoundryId](const FFoundryJoinRequest& R) { return R.FoundryId == FoundryId; });
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		ExpectSuccess(TEXT("party.join_accept"), FString::Printf(TEXT("%s joined the party"), Req != nullptr ? *Req->DisplayName : TEXT("Player")));
		Sdk->AcceptPartyJoinRequestForQueue(CachedParty.PartyId, FoundryId, SelectedQueue);
	}
}

void UFoundrySocialSubsystem::DeclineJoinRequest(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("party.join_decline")))
	{
		return;
	}
	if (!HasParty() || !IsLocalLeader())
	{
		OnActionFailed.Broadcast(TEXT("party.join_decline"), TEXT("Only the party host can answer a request."));
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		ExpectSuccess(TEXT("party.join_decline"), TEXT("Request declined"));
		Sdk->DeclinePartyJoinRequest(CachedParty.PartyId, FoundryId);
	}
}

void UFoundrySocialSubsystem::AcceptInvite(const FString& PartyId)
{
	if (RefuseInFixture(TEXT("party.accept")))
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		const FFoundryParty* Invite = CachedInvites.FindByPredicate([&PartyId](const FFoundryParty& P) { return P.PartyId == PartyId; });
		ExpectSuccess(TEXT("party.accept"), (Invite != nullptr && !Invite->LeaderDisplayName.IsEmpty())
			? FString::Printf(TEXT("Joined %s's party"), *Invite->LeaderDisplayName)
			: FString(TEXT("Joined the party")));
		Sdk->AcceptPartyInvite(PartyId);
	}
}

void UFoundrySocialSubsystem::DeclineInvite(const FString& PartyId)
{
	if (RefuseInFixture(TEXT("party.decline")))
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		ExpectSuccess(TEXT("party.decline"), TEXT("Invite declined"));
		Sdk->DeclinePartyInvite(PartyId);
	}
}

void UFoundrySocialSubsystem::LeaveParty()
{
	if (RefuseInFixture(TEXT("party.leave")))
	{
		return;
	}
	if (!HasParty())
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		ExpectSuccess(TEXT("party.leave"), TEXT("You left the party"));
		Sdk->LeaveParty(CachedParty.PartyId);
	}
}

void UFoundrySocialSubsystem::SetReady(bool bReady)
{
	if (RefuseInFixture(TEXT("party.ready")) || !HasParty())
	{
		return;
	}
	// Ready belongs to the game: the platform stores WHICH game (and refuses a ready with none), so the slug
	// rides every ready=true. No slug known = a misconfigured kit; say so rather than send the refused request.
	const FString Slug = GameSlug();
	if (bReady && Slug.IsEmpty())
	{
		OnActionFailed.Broadcast(TEXT("party.ready"), TEXT("This game hasn't told Foundry which game it is, so it can't ready up."));
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->SetPartyReadyInGame(CachedParty.PartyId, bReady, Slug);
	}
}

void UFoundrySocialSubsystem::SetInMatch(bool bInMatch)
{
	if (Fixture.bActive || !HasParty())
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->SetPartyInMatch(CachedParty.PartyId, bInMatch);
	}
}

void UFoundrySocialSubsystem::RemoveFriend(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("friend.remove")))
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->RemoveFriend(FoundryId);
	}
}

void UFoundrySocialSubsystem::BlockPlayer(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("friend.block")))
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->BlockPlayer(FoundryId);
	}
}

void UFoundrySocialSubsystem::UnblockPlayer(const FString& FoundryId)
{
	if (RefuseInFixture(TEXT("friend.unblock")))
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->UnblockPlayer(FoundryId);
	}
}

void UFoundrySocialSubsystem::RequestToJoin(const FString& FriendFoundryId)
{
	if (RefuseInFixture(TEXT("party.join_request")))
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		const FFoundryFriend* Friend = Friends().FindByPredicate([&FriendFoundryId](const FFoundryFriend& F) { return F.FoundryId == FriendFoundryId; });
		ExpectSuccess(TEXT("party.join_request"), (Friend != nullptr && !Friend->DisplayName.IsEmpty())
			? FString::Printf(TEXT("Asked to join %s's party"), *Friend->DisplayName)
			: FString(TEXT("Asked to join their party")));
		Sdk->RequestToJoinParty(FriendFoundryId);
	}
}

void UFoundrySocialSubsystem::SetFixture(const FFoundrySocialFixture& InFixture)
{
	Fixture = InFixture;
	OnChanged.Broadcast();
}

// -- handlers --------------------------------------------------------------------------------------------

void UFoundrySocialSubsystem::HandleLogin(EFoundryFsdkResult Result, const FString& DisplayName)
{
	bSignedIn = Result == EFoundryFsdkResult::Ok;
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		LocalId = Sdk->GetFoundryId();
		LocalUsernameCached = Sdk->GetUsername();
	}
	LocalDisplayNameCached = bSignedIn ? (DisplayName.IsEmpty() ? TEXT("(player)") : DisplayName) : FString();
	if (bSignedIn)
	{
		NextPollTime = 0.0; // refresh everything at once
	}
	OnChanged.Broadcast();
}

void UFoundrySocialSubsystem::HandleLoggedOut()
{
	bSignedIn = false;
	LocalId.Reset();
	LocalDisplayNameCached.Reset();
	LocalUsernameCached.Reset();
	PendingInviteUsername.Reset();
	bCreatePartyPending = false;
	CachedParty = FFoundryParty();
	CachedInvites.Reset();
	CachedFriends.Reset();
	CachedBlocked.Reset();
	CachedJoinRequests.Reset();
	ExpectedSuccess.Reset();
	CachedPendingRequests = 0;
	OnChanged.Broadcast();
	OnPartyChanged.Broadcast();
}

void UFoundrySocialSubsystem::HandleParty(EFoundryFsdkResult Result, const FFoundryParty& Party)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		const bool bHadParty = HasParty();
		CachedParty = Party;
		if (!bHadParty && HasParty())
		{
			bCreatePartyPending = false; // a party arrived - our own CreateParty (if any) is done
			if (!PendingInviteUsername.IsEmpty())
			{
				// The invite InviteByUsername queued behind its CreateParty.
				const FString Queued = PendingInviteUsername;
				PendingInviteUsername.Reset();
				SendInvite(Queued);
			}
		}

		if (!HasParty() || !IsLocalLeader())
		{
			CachedJoinRequests.Reset(); // only a leader sees requests; losing the crown / the party clears them
		}

		TArray<FString> MemberLines;
		for (const FFoundryPartyMember& M : CachedParty.Members)
		{
			MemberLines.Add(FString::Printf(TEXT("%s:%s"), *M.FoundryId, M.State.IsEmpty() ? TEXT("JOINED") : *M.State));
		}
		UE_LOG(LogFoundrySocial, Log, TEXT("[Social] party %s, leader %s, members [%s]"),
			CachedParty.PartyId.IsEmpty() ? TEXT("none") : *CachedParty.PartyId,
			CachedParty.LeaderFoundryId.IsEmpty() ? TEXT("none") : *CachedParty.LeaderFoundryId,
			*FString::Join(MemberLines, TEXT(", ")));

		OnPartyChanged.Broadcast();
	}
	// a failed read keeps the last party: a network blip must not empty it on screen
	NotePollAnswered();
}

void UFoundrySocialSubsystem::HandleInvites(EFoundryFsdkResult Result, const TArray<FFoundryParty>& InInvites)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		CachedInvites = InInvites;
	}
	NotePollAnswered();
}

void UFoundrySocialSubsystem::HandleFriends(EFoundryFsdkResult Result, const TArray<FFoundryFriend>& InFriends)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		CachedFriends = InFriends;
		OnPartyChanged.Broadcast(); // party-member presence is joined against the friends list downstream
	}
	NotePollAnswered();
}

void UFoundrySocialSubsystem::HandlePending(EFoundryFsdkResult Result, const TArray<FFoundryFriend>& Pending)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		CachedPendingRequests = Pending.Num();
	}
	NotePollAnswered();
}

void UFoundrySocialSubsystem::HandleBlocked(EFoundryFsdkResult Result, const TArray<FFoundryFriend>& InBlocked)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		CachedBlocked = InBlocked;
	}
	NotePollAnswered();
}

void UFoundrySocialSubsystem::HandleJoinRequests(EFoundryFsdkResult Result, const TArray<FFoundryJoinRequest>& Requests)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		CachedJoinRequests = Requests;
	}
	else if (Result == EFoundryFsdkResult::NoMatch)
	{
		CachedJoinRequests.Reset(); // fid's uniform 404: I no longer lead that party
	}
	// any other failure keeps the last list: a blip must not hide a request someone is waiting on
	NotePollAnswered();
}

void UFoundrySocialSubsystem::HandlePartyCap(EFoundryFsdkResult Result, const FString& QueueKey, int32 PartyCap, int32 TeamSize)
{
	if (Result != EFoundryFsdkResult::Ok)
	{
		// Stays unknown (-1) or keeps the last value; LastRequestTime already throttles the retry.
		UE_LOG(LogFoundrySocial, Log, TEXT("[Social] party cap for '%s' not read (%d)"), *QueueKey, static_cast<int32>(Result));
		return;
	}
	FPartyCapEntry& Entry = PartyCaps.FindOrAdd(QueueKey);
	const bool bChanged = Entry.Cap != PartyCap || Entry.TeamSize != TeamSize;
	Entry.Cap = PartyCap;
	Entry.TeamSize = TeamSize;
	if (bChanged)
	{
		OnChanged.Broadcast(); // the mode picker re-evaluates which modes fit the party
	}
}

namespace
{
	// Player-facing copy for a result the platform gave no sentence for (offline, a transport
	// failure, a refused token). Same tone as the matchmaking copy table.
	FString FallbackActionMessage(EFoundryFsdkResult Result, const FString& Action)
	{
		switch (Result)
		{
		case EFoundryFsdkResult::NotAuthenticated:
		case EFoundryFsdkResult::Unauthorized: return TEXT("Sign in through the Foundry launcher first.");
		case EFoundryFsdkResult::Network:
		case EFoundryFsdkResult::Timeout: return TEXT("Can't reach Foundry right now. Check your connection and try again.");
		case EFoundryFsdkResult::RateLimited: return TEXT("Slow down - try that again in a moment.");
		case EFoundryFsdkResult::NoMatch:
			// fid answers a party row that is gone (expired invite, disbanded party, not the leader) with its
			// uniform 404; "no one found by that name" would be a lie for those.
			return Action.StartsWith(TEXT("party.")) ? TEXT("That invite or party is gone - it may have expired.")
			                                           : TEXT("No one found by that name.");
		case EFoundryFsdkResult::Unavailable: return TEXT("Foundry is busy right now. Try again in a moment.");
		case EFoundryFsdkResult::InvalidArg: return TEXT("That doesn't look right - check what you typed.");
		default: return TEXT("Something went wrong. Try again.");
		}
	}
}

void UFoundrySocialSubsystem::HandleSocialAction(EFoundryFsdkResult Result, const FString& Action)
{
	// Consume this completion's queued success sentence (FIFO per action) whatever the outcome, so a
	// failure never leaves its sentence behind for the next success of the same action.
	FString SuccessMessage;
	if (TArray<FString>* Queued = ExpectedSuccess.Find(Action))
	{
		if (Queued->Num() > 0)
		{
			SuccessMessage = (*Queued)[0];
			Queued->RemoveAt(0);
		}
	}
	if (Result != EFoundryFsdkResult::Ok)
	{
		// The platform's own sentence when it gave one ("No player found. Ask your friend for their
		// exact @username..."), else a player-facing fallback for the transport-level results. Never
		// the enum's display name (Justin, 2026-10-13: add-friend showed "No Match / Not Ready").
		const UFoundryFSDKSubsystem* Sdk = Fsdk();
		FString Message = Sdk ? Sdk->LastSocialActionError() : FString();
		if (Action == TEXT("party.in_match"))
		{
			// Background bookkeeping the player never asked for (an older fid, a party that just
			// dissolved): log it, never put it in front of them.
			UE_LOG(LogFoundrySocial, Log, TEXT("[Social] in-match flag not recorded (%s)"),
				Message.IsEmpty() ? TEXT("no server message") : *Message);
			return;
		}
		if (Message.IsEmpty())
		{
			Message = FallbackActionMessage(Result, Action);
		}
		if (Action == TEXT("friend.request"))
		{
			InFlightFriendRequestUsername.Reset();
		}
		OnActionFailed.Broadcast(Action, Message);
		if (Action == TEXT("party.create"))
		{
			PendingInviteUsername.Reset();
			bCreatePartyPending = false;
		}
		return;
	}
	if (Action == TEXT("friend.request") && !InFlightFriendRequestUsername.IsEmpty())
	{
		SentFriendRequests.Add(InFlightFriendRequestUsername);
		InFlightFriendRequestUsername.Reset();
	}
	if (!SuccessMessage.IsEmpty())
	{
		OnActionSucceeded.Broadcast(FName(*Action), SuccessMessage);
	}
	// The platform follows every party mutation with its own RefreshParty; freshen everything else
	// (invites, blocked, pending, join requests) on the very next tick too.
	NextPollTime = 0.0;
}
