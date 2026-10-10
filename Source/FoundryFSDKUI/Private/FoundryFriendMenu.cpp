#include "FoundryFriendMenu.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "FoundrySocialSubsystem.h"

UFoundrySocialSubsystem* UFoundryFriendMenu::Subsystem() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UFoundrySocialSubsystem>() : nullptr;
}

void UFoundryFriendMenu::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (SendMessageButton) { SendMessageButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleSendMessage); }
	if (InviteButton) { InviteButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleInvite); }
	if (RequestJoinButton) { RequestJoinButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleRequestJoin); }
	if (RemoveFriendButton) { RemoveFriendButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleRemoveFriend); }
	if (BlockButton) { BlockButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleBlock); }
	if (UnblockButton) { UnblockButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleUnblock); }
	if (ConfirmButton) { ConfirmButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleConfirm); }
	if (CancelButton) { CancelButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleCancel); }
}

void UFoundryFriendMenu::Setup(EFoundryFriendRowKind InKind, const FString& InName, bool bCanInvite, bool bCanRequestJoin)
{
	Kind = InKind;
	TargetName = InName;
	SetText(TitleText, InName);

	const bool bOnline = InKind == EFoundryFriendRowKind::Online;
	const bool bOffline = InKind == EFoundryFriendRowKind::Offline;
	const bool bBlocked = InKind == EFoundryFriendRowKind::Blocked;
	const bool bParty = InKind == EFoundryFriendRowKind::Party;

	Show(SendMessageButton, bOnline || bOffline || bParty, true);
	Show(InviteButton, bOnline || bOffline, true);
	Show(RequestJoinButton, bOnline || bOffline, true);
	Show(RemoveFriendButton, bOnline || bOffline, true);
	Show(BlockButton, bOnline || bOffline, true);
	Show(UnblockButton, bBlocked, true);

	// Offline friends never get Invite/RequestToJoin (forced disabled); Online honors the caller's
	// flags (the drawer computes them from party/signed-in state).
	const bool bInviteEnabled = bOnline && bCanInvite;
	if (InviteButton)
	{
		InviteButton->SetIsEnabled(bInviteEnabled);
	}
	const bool bRequestJoinEnabled = bOnline && bCanRequestJoin;
	if (RequestJoinButton)
	{
		RequestJoinButton->SetIsEnabled(bRequestJoinEnabled);
		// Say why when disabled. This slice's RequestToJoinParty call always exists - "Coming soon"
		// is reserved for a future binding whose platform lacks the call entirely, never reached
		// from this Setup signature.
		FString Reason;
		if (!bRequestJoinEnabled)
		{
			if (bOffline)
			{
				Reason = TEXT("They're offline.");
			}
			else
			{
				const UFoundrySocialSubsystem* Sdk = Subsystem();
				Reason = (Sdk && !Sdk->IsSignedIn()) ? TEXT("Sign in to ask to join a party.") : TEXT("You're already in a party.");
			}
		}
		RequestJoinButton->SetToolTipText(FText::FromString(Reason));
	}

	Show(ItemsPanel, true, true);
	Show(ConfirmPanel, false);
}

void UFoundryFriendMenu::ShowConfirm(const FString& Text, const FString& ButtonLabel)
{
	Show(ItemsPanel, false);
	Show(ConfirmPanel, true, true);
	SetText(ConfirmText, Text);
	SetText(ConfirmButtonText, ButtonLabel);
}

void UFoundryFriendMenu::ShowItems()
{
	Show(ConfirmPanel, false);
	Show(ItemsPanel, true, true);
}

void UFoundryFriendMenu::HandleSendMessage() { OnItem.Broadcast(EFoundryFriendMenuItem::SendMessage); }
void UFoundryFriendMenu::HandleInvite() { OnItem.Broadcast(EFoundryFriendMenuItem::Invite); }
void UFoundryFriendMenu::HandleRequestJoin() { OnItem.Broadcast(EFoundryFriendMenuItem::RequestToJoin); }
void UFoundryFriendMenu::HandleRemoveFriend() { OnItem.Broadcast(EFoundryFriendMenuItem::RemoveFriend); }
void UFoundryFriendMenu::HandleBlock() { OnItem.Broadcast(EFoundryFriendMenuItem::Block); }
void UFoundryFriendMenu::HandleUnblock() { OnItem.Broadcast(EFoundryFriendMenuItem::Unblock); }
void UFoundryFriendMenu::HandleConfirm() { OnConfirm.Broadcast(); }
void UFoundryFriendMenu::HandleCancel() { OnCancel.Broadcast(); }
