#include "FoundrySocialSubsystem.h"

#include "Engine/GameInstance.h"

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
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->OnLoginComplete.RemoveAll(this);
		Sdk->OnLoggedOut.RemoveAll(this);
		Sdk->OnPartyUpdated.RemoveAll(this);
		Sdk->OnPartyInvitesUpdated.RemoveAll(this);
		Sdk->OnFriendsUpdated.RemoveAll(this);
		Sdk->OnPendingUpdated.RemoveAll(this);
		Sdk->OnBlockedUpdated.RemoveAll(this);
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
	// closed (SetPanelOpen).
	const double Now = FPlatformTime::Seconds();
	if (Now >= NextPollTime)
	{
		NextPollTime = Now + (bPanelOpen ? 10.0 : 30.0);
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
		// RefreshParty + RefreshPartyInvites + RefreshFriends + RefreshPendingRequests + RefreshBlocked -
		// NotePollAnswered (one per handler below) counts these five down and coalesces them into ONE
		// OnChanged broadcast instead of up to five separate drawer rebuilds.
		PollReadsOutstanding = 5;
		bPollNotifyPending = false;
		PollFirstAnswerTime = -1.0;
		Sdk->RefreshParty();
		Sdk->RefreshPartyInvites();
		Sdk->RefreshFriends();
		Sdk->RefreshPendingRequests();
		Sdk->RefreshBlocked();
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

bool UFoundrySocialSubsystem::IsLocalLeader() const
{
	if (!HasParty())
	{
		return true; // alone you are your own host
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	return !LocalId.IsEmpty() && P.LeaderFoundryId == LocalId;
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
	if (!HasParty() || LocalId.IsEmpty())
	{
		return false;
	}
	const FFoundryParty& P = Fixture.bActive ? Fixture.Party : CachedParty;
	for (const FFoundryPartyMember& M : P.Members)
	{
		if (M.FoundryId == LocalId)
		{
			return M.bReady;
		}
	}
	return false;
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

// -- actions -------------------------------------------------------------------------------------------

void UFoundrySocialSubsystem::SendFriendRequest(const FString& InUsername)
{
	const FString Username = InUsername.TrimStartAndEnd();
	if (Username.IsEmpty())
	{
		return;
	}
	if (Fixture.bActive)
	{
		OnActionFailed.Broadcast(TEXT("friend.request"), TEXT("Dev fixture active - actions are disabled."));
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
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
	if (Fixture.bActive)
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), TEXT("Dev fixture active - actions are disabled."));
		return;
	}
	if (!bSignedIn)
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), TEXT("Sign in through the Foundry launcher before inviting."));
		return;
	}
	if (JoinedCount() >= PartyCapacity())
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), FString::Printf(TEXT("Party full (%d / %d)."), JoinedCount(), PartyCapacity()));
		return;
	}
	if (HasParty() && !IsLocalLeader())
	{
		OnActionFailed.Broadcast(TEXT("party.invite"), TEXT("Only the party host can invite."));
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
		Sdk->InviteToParty(CachedParty.PartyId, Username);
	}
	else
	{
		// No party yet: create one, then invite once it arrives (HandleParty).
		PendingInviteUsername = Username;
		bCreatePartyPending = true;
		Sdk->CreateParty();
	}
}

void UFoundrySocialSubsystem::AcceptInvite(const FString& PartyId)
{
	if (Fixture.bActive)
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->AcceptPartyInvite(PartyId);
	}
}

void UFoundrySocialSubsystem::DeclineInvite(const FString& PartyId)
{
	if (Fixture.bActive)
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->DeclinePartyInvite(PartyId);
	}
}

void UFoundrySocialSubsystem::LeaveParty()
{
	if (Fixture.bActive || !HasParty())
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->LeaveParty(CachedParty.PartyId);
	}
}

void UFoundrySocialSubsystem::SetReady(bool bReady)
{
	if (Fixture.bActive || !HasParty())
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->SetPartyReady(CachedParty.PartyId, bReady);
	}
}

void UFoundrySocialSubsystem::RemoveFriend(const FString& FoundryId)
{
	if (Fixture.bActive)
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
	if (Fixture.bActive)
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
	if (Fixture.bActive)
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
	if (Fixture.bActive)
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
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
				if (UFoundryFSDKSubsystem* Sdk = Fsdk())
				{
					Sdk->InviteToParty(CachedParty.PartyId, PendingInviteUsername);
				}
				PendingInviteUsername.Reset();
			}
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

void UFoundrySocialSubsystem::HandleSocialAction(EFoundryFsdkResult Result, const FString& Action)
{
	if (Result != EFoundryFsdkResult::Ok)
	{
		OnActionFailed.Broadcast(Action, UEnum::GetDisplayValueAsText(Result).ToString());
		if (Action == TEXT("party.create"))
		{
			PendingInviteUsername.Reset();
			bCreatePartyPending = false;
		}
		return;
	}
	// The platform follows every party mutation with its own RefreshParty; freshen everything else
	// (invites, blocked, pending) on the very next tick too.
	NextPollTime = 0.0;
}
