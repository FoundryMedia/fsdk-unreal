#include "FoundryFriendMenu.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "FoundryUIWidgetBase.h"

void UFoundryFriendMenu::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (SendMessageButton) { SendMessageButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleSendMessage); }
	if (InviteButton) { InviteButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleInvite); }
	if (RequestJoinButton) { RequestJoinButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleRequestJoin); }
	if (ReInviteButton) { ReInviteButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleReInvite); }
	if (CancelInviteButton) { CancelInviteButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleCancelInvite); }
	if (KickButton) { KickButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleKick); }
	if (RemoveFriendButton) { RemoveFriendButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleRemoveFriend); }
	if (BlockButton) { BlockButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleBlock); }
	if (UnblockButton) { UnblockButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleUnblock); }
	if (ConfirmButton) { ConfirmButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleConfirm); }
	if (CancelButton) { CancelButton->OnClicked.AddDynamic(this, &UFoundryFriendMenu::HandleCancel); }
}

void UFoundryFriendMenu::ApplyItem(UButton* Button, const FoundryPartyRules::FMenuItemState& State)
{
	if (!Button)
	{
		return;
	}
	Show(Button, State.bShown, true);
	Button->SetIsEnabled(State.bEnabled);
	// A disabled UButton still shows its own tooltip; an enabled one clears it.
	SetKitToolTip(Button, FText::FromString(State.bEnabled ? FString() : State.Reason));
}

void UFoundryFriendMenu::Setup(EFoundryFriendRowKind InKind, const FString& InName, const FoundryPartyRules::FFriendMenuPlan& Plan)
{
	Kind = InKind;
	TargetName = InName;
	SetText(TitleText, InName);

	ApplyItem(SendMessageButton, Plan.SendMessage);
	ApplyItem(InviteButton, Plan.Invite);
	ApplyItem(RequestJoinButton, Plan.AskToJoin);
	ApplyItem(ReInviteButton, Plan.ReInvite);
	ApplyItem(CancelInviteButton, Plan.CancelInvite);
	ApplyItem(KickButton, Plan.Kick);
	ApplyItem(RemoveFriendButton, Plan.RemoveFriend);
	ApplyItem(BlockButton, Plan.Block);
	ApplyItem(UnblockButton, Plan.Unblock);

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
void UFoundryFriendMenu::HandleReInvite() { OnItem.Broadcast(EFoundryFriendMenuItem::ReInvite); }
void UFoundryFriendMenu::HandleCancelInvite() { OnItem.Broadcast(EFoundryFriendMenuItem::CancelInvite); }
void UFoundryFriendMenu::HandleKick() { OnItem.Broadcast(EFoundryFriendMenuItem::RemoveFromParty); }
void UFoundryFriendMenu::HandleRemoveFriend() { OnItem.Broadcast(EFoundryFriendMenuItem::RemoveFriend); }
void UFoundryFriendMenu::HandleBlock() { OnItem.Broadcast(EFoundryFriendMenuItem::Block); }
void UFoundryFriendMenu::HandleUnblock() { OnItem.Broadcast(EFoundryFriendMenuItem::Unblock); }
void UFoundryFriendMenu::HandleConfirm() { OnConfirm.Broadcast(); }
void UFoundryFriendMenu::HandleCancel() { OnCancel.Broadcast(); }
