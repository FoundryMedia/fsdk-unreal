#include "FoundryFriendRow.h"

#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "FoundryUISettings.h"
#include "FoundryUITheme.h"

void UFoundryFriendRow::Setup(const FFoundryFriend& InFriend, EFoundryFriendRowKind InKind, bool bInHost, bool bInLocal,
	bool bInJoined, const FLinearColor& InTint, const FString& InStatusOverride)
{
	Friend = InFriend;
	Kind = InKind;
	bIsHost = bInHost;
	bIsLocal = bInLocal;
	bIsJoined = bInJoined;
	Presence = InFriend.Presence;
	Tint = InTint;

	SetText(NameText, InFriend.DisplayName);

	// StatusOverride replaces the default presence label when given (e.g. "Invited - not joined",
	// "Playing Conquest"); empty falls back to the plain presence label.
	const FString Status = InStatusOverride.IsEmpty() ? FoundryUITheme::PresenceLabel(InFriend.Presence) : InStatusOverride;
	SetText(StatusText, Status);
	Show(StatusText, !(bInHost && bInJoined));
	Show(HostBadge, bInHost && bInJoined);

	if (Avatar)
	{
		Avatar->SetColorAndOpacity(bInJoined ? InTint : UFoundryUISettings::Get()->Palette.TextSecondary);
	}
	if (PresenceDot)
	{
		PresenceDot->SetColorAndOpacity(FoundryUITheme::PresenceColor(InFriend.Presence));
		Show(PresenceDot, bInJoined && InKind != EFoundryFriendRowKind::Blocked);
	}
	// ActionButton's visibility is left ALONE here - the WBP decides (the kit's own generated row
	// already starts it Collapsed in the tree; a game that wires its own inline action controls it).
	OnRowUpdated();
}

FReply UFoundryFriendRow::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (InMouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		OnContextMenuRequested.Broadcast(this, InMouseEvent.GetScreenSpacePosition());
		return FReply::Handled();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}
