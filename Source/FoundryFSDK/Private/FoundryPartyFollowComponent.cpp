// Copyright Foundry Media. FoundryFSDK: the leader's post-match leave takes the party with it.

#include "FoundryPartyFollowComponent.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FMMSSubsystem.h"
#include "FoundrySocialSubsystem.h"
#include "GameFramework/PlayerController.h"

// Per-TU, like LogFMMS / LogFoundrySocial in this module: the core module has no shared extern category.
// A distinct name (not LogFoundryFSDK, which FoundryFSDKSubsystem.cpp defines static) so a unity build of
// both files cannot redefine it.
DEFINE_LOG_CATEGORY_STATIC(LogFoundryPartyFollow, Log, All);

namespace
{
	bool GFollowLeaderOnLeave = true;
	UFoundryPartyFollowComponent::FLeaveExecutor GLeaveExecutor;

	constexpr int32 MaxIdLength = 64; // a foundry / party id is a UUID; anything longer is not one

	struct FFollowContext
	{
		APlayerController* PC = nullptr;
		UFoundrySocialSubsystem* Social = nullptr;
		UFMMSSubsystem* Fmms = nullptr;
		bool IsValid() const { return PC != nullptr && Social != nullptr && Fmms != nullptr; }
	};

	/** The LOCAL player's controller + the two game-instance subsystems the decision reads. Empty
	 *  (invalid) on a server-side or non-local component, where nothing here applies. */
	FFollowContext ResolveLocal(const UActorComponent* Comp)
	{
		FFollowContext Ctx;
		APlayerController* PC = Comp ? Cast<APlayerController>(Comp->GetOwner()) : nullptr;
		if (PC == nullptr || !PC->IsLocalController())
		{
			return Ctx;
		}
		UGameInstance* GI = PC->GetGameInstance();
		if (GI == nullptr)
		{
			return Ctx;
		}
		Ctx.PC = PC;
		Ctx.Social = GI->GetSubsystem<UFoundrySocialSubsystem>();
		Ctx.Fmms = GI->GetSubsystem<UFMMSSubsystem>();
		return Ctx;
	}
}

UFoundryPartyFollowComponent::UFoundryPartyFollowComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	// Required for the Server / Client RPCs below to travel: a component RPC is routed through the
	// component's own replication channel, which only exists while the component replicates.
	SetIsReplicatedByDefault(true);
}

void UFoundryPartyFollowComponent::SetFollowLeaderOnLeave(bool bEnabled)
{
	GFollowLeaderOnLeave = bEnabled;
}

bool UFoundryPartyFollowComponent::GetFollowLeaderOnLeave()
{
	return GFollowLeaderOnLeave;
}

void UFoundryPartyFollowComponent::SetLeaveExecutor(FLeaveExecutor InExecutor)
{
	GLeaveExecutor = MoveTemp(InExecutor);
}

void UFoundryPartyFollowComponent::NotifyLocalLeavingFor(APlayerController* Controller)
{
	if (Controller == nullptr || !Controller->IsLocalController())
	{
		return;
	}
	if (UFoundryPartyFollowComponent* Comp = Controller->FindComponentByClass<UFoundryPartyFollowComponent>())
	{
		Comp->NotifyLocalLeaving();
	}
	// No component = the game never opted in; stay silent (a kit leave path calls this every time).
}

void UFoundryPartyFollowComponent::NotifyLocalLeaving()
{
	if (!GFollowLeaderOnLeave)
	{
		return;
	}
	const FFollowContext Ctx = ResolveLocal(this);
	if (!Ctx.IsValid() || Ctx.Social->IsFixtureActive() || !Ctx.Social->HasParty())
	{
		return;
	}
	// IsLocalLeader() answers true for "alone, you are your own host" - HasParty() above rules that out.
	if (!Ctx.Social->IsLocalLeader() || Ctx.Social->JoinedCount() < 2)
	{
		return; // not the leader, or nobody to bring along
	}
	if (!Ctx.Fmms->WasMatchEnded())
	{
		return; // the match is still running for the leader: leaving alone is the rule (Justin, 2026-10-16)
	}
	const FFoundryParty& Party = Ctx.Social->Party();
	if (Party.PartyId.IsEmpty() || Party.LeaderFoundryId.IsEmpty())
	{
		return;
	}
	UE_LOG(LogFoundryPartyFollow, Log, TEXT("[PartyFollow] leader leaving the finished match - telling party %s"), *Party.PartyId);
	// Copies: the RPC args must not alias the cached party the social poller may rewrite.
	const FString PartyIdCopy = Party.PartyId;
	const FString LeaderIdCopy = Party.LeaderFoundryId;
	ServerPartyLeaderLeaving(PartyIdCopy, LeaderIdCopy);
}

bool UFoundryPartyFollowComponent::ServerPartyLeaderLeaving_Validate(const FString& InPartyId, const FString& InLeaderFoundryId)
{
	return !InPartyId.IsEmpty() && InPartyId.Len() <= MaxIdLength
		&& !InLeaderFoundryId.IsEmpty() && InLeaderFoundryId.Len() <= MaxIdLength;
}

void UFoundryPartyFollowComponent::ServerPartyLeaderLeaving_Implementation(const FString& InPartyId, const FString& InLeaderFoundryId)
{
	// The server knows no parties - it only relays. Every receiver checks the ids against its own party.
	UWorld* World = GetWorld();
	if (World == nullptr)
	{
		return;
	}
	const AActor* Sender = GetOwner();
	int32 Relayed = 0;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* Other = It->Get();
		if (Other == nullptr || Other == Sender)
		{
			continue;
		}
		if (UFoundryPartyFollowComponent* Comp = Other->FindComponentByClass<UFoundryPartyFollowComponent>())
		{
			Comp->ClientPartyLeaderLeft(InPartyId, InLeaderFoundryId);
			++Relayed;
		}
	}
	UE_LOG(LogFoundryPartyFollow, Log, TEXT("[PartyFollow] server relayed party %s leader-left to %d controller(s)"), *InPartyId, Relayed);
}

void UFoundryPartyFollowComponent::ClientPartyLeaderLeft_Implementation(const FString& InPartyId, const FString& InLeaderFoundryId)
{
	if (!GFollowLeaderOnLeave)
	{
		return;
	}
	const FFollowContext Ctx = ResolveLocal(this);
	if (!Ctx.IsValid() || Ctx.Social->IsFixtureActive() || !Ctx.Social->HasParty())
	{
		return;
	}
	const FFoundryParty& Party = Ctx.Social->Party();
	if (Party.PartyId != InPartyId || Party.LeaderFoundryId != InLeaderFoundryId)
	{
		return; // someone else's party (or ours, with a leader we do not know): not our message
	}
	if (Ctx.Social->IsLocalLeader())
	{
		return; // the leader sent it; never bounce it back
	}
	if (!Ctx.Fmms->WasMatchEnded())
	{
		// Post-horn only: a member still in a running match is not pulled out of it.
		UE_LOG(LogFoundryPartyFollow, Log, TEXT("[PartyFollow] leader left party %s but our match is still running - staying"), *InPartyId);
		return;
	}
	UE_LOG(LogFoundryPartyFollow, Log, TEXT("[PartyFollow] our party leader left the finished match - leaving with them"));
	if (!GLeaveExecutor || !GLeaveExecutor(Ctx.PC))
	{
		UE_LOG(LogFoundryPartyFollow, Warning, TEXT("[PartyFollow] %s has no way to leave the match (no leave executor installed, or the controller does not implement IFoundryMenuHost) - the party leader left without us"),
			*GetNameSafe(Ctx.PC));
	}
}
