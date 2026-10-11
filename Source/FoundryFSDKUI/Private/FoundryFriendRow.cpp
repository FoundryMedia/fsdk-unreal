#include "FoundryFriendRow.h"

#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "FoundryUISettings.h"
#include "FoundryUITheme.h"

void UFoundryFriendRow::Setup(const FFoundryFriend& InFriend, EFoundryFriendRowKind InKind, bool bInHost, bool bInLocal,
	bool bInJoined, const FLinearColor& InTint, const FString& InStatusOverride, bool bInReady)
{
	Friend = InFriend;
	Kind = InKind;
	bIsHost = bInHost;
	bIsLocal = bInLocal;
	bIsJoined = bInJoined;
	bIsReady = bInReady;
	Presence = InFriend.Presence;
	Tint = InTint;

	const bool bBlocked = InKind == EFoundryFriendRowKind::Blocked;
	const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;

	SetText(NameText, InFriend.DisplayName);
	if (NameText)
	{
		// A blocked row reads as inert: name + status both drop to the secondary/helper colour
		// (never the player's own tint), on top of the row-wide opacity dim below.
		NameText->SetColorAndOpacity(FSlateColor(bBlocked ? Palette.TextSecondary : Palette.Text));
	}

	// StatusOverride replaces the default presence label when given (e.g. "Invited - not joined",
	// "Playing Conquest"); empty falls back to the plain presence label. A blocked row always reads
	// "Blocked" - never the blocked player's own (possibly online) presence.
	const FString Status = bBlocked ? TEXT("Blocked")
		: (InStatusOverride.IsEmpty() ? FoundryUITheme::PresenceLabel(InFriend.Presence) : InStatusOverride);
	SetText(StatusText, Status);
	if (StatusText)
	{
		StatusText->SetColorAndOpacity(FSlateColor(bBlocked ? Palette.TextSecondary : FoundryUITheme::PresenceColor(InFriend.Presence)));
	}
	Show(StatusText, !(bInHost && bInJoined));
	Show(HostBadge, bInHost && bInJoined);
	Show(ReadyIcon, InKind == EFoundryFriendRowKind::Party && bInJoined && bInReady);

	const FLinearColor AvatarTint = (bInJoined && !bBlocked) ? InTint : Palette.TextSecondary;
	if (Avatar)
	{
		Avatar->SetColorAndOpacity(AvatarTint);
	}
	if (PresenceDot)
	{
		PresenceDot->SetColorAndOpacity(FoundryUITheme::PresenceColor(InFriend.Presence));
		Show(PresenceDot, bInJoined && InKind != EFoundryFriendRowKind::Blocked);
	}
	// ActionButton's visibility is left ALONE here - the WBP decides (the kit's own generated row
	// already starts it Collapsed in the tree; a game that wires its own inline action controls it).
	SetRenderOpacity(bBlocked ? 0.6f : 1.0f);
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
