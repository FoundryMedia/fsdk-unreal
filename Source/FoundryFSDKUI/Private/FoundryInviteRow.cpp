#include "FoundryInviteRow.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"

void UFoundryInviteRow::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (AcceptButton)
	{
		AcceptButton->OnClicked.AddDynamic(this, &UFoundryInviteRow::HandleAccept);
	}
	if (DeclineButton)
	{
		DeclineButton->OnClicked.AddDynamic(this, &UFoundryInviteRow::HandleDecline);
	}
}

void UFoundryInviteRow::Setup(const FString& InLeaderName, const FString& InPartyId)
{
	LeaderName = InLeaderName;
	PartyId = InPartyId;
	SetText(LeaderText, FString::Printf(TEXT("%s's party"), *InLeaderName));
	OnRowUpdated();
}

void UFoundryInviteRow::HandleAccept()
{
	OnAccept.Broadcast(PartyId);
}

void UFoundryInviteRow::HandleDecline()
{
	OnDecline.Broadcast(PartyId);
}
