#include "FoundryFriendRow.h"

#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "FoundryPartyRules.h"
#include "FoundryUISettings.h"
#include "FoundryUITheme.h"
#include "HAL/PlatformTime.h"

void UFoundryFriendRow::Setup(const FFoundryFriend& InFriend, EFoundryFriendRowKind InKind, bool bInHost, bool bInLocal,
	bool bInJoined, const FLinearColor& InTint, const FString& InStatusOverride, bool bInReady)
{
	Friend = InFriend;
	Kind = InKind;
	bIsHost = bInHost;
	bIsLocal = bInLocal;
	bIsJoined = bInJoined;
	bIsReady = bInReady;
	InviteClock = FoundryPartyRules::FInviteClock(); // only SetInviteExpiry (after this Setup) arms the expiry line
	InviteExpiryAccumulator = 0.0f;
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
		// A joined party member's line is their READY state, coloured like the launcher's chips: green when
		// ready or in a match, muted otherwise ("Not ready" in presence-green read as ready, 2026-10-17).
		// Everyone else wears their presence colour; a blocked row the secondary.
		const bool bReadyLine = InKind == EFoundryFriendRowKind::Party && bInJoined && !InStatusOverride.IsEmpty();
		const bool bGreen = bInReady || InStatusOverride == TEXT("In match");
		StatusText->SetColorAndOpacity(FSlateColor(bBlocked ? Palette.TextSecondary
			: bReadyLine ? (bGreen ? FoundryUITheme::PresenceColor(TEXT("online")) : Palette.TextSecondary)
			: FoundryUITheme::PresenceColor(InFriend.Presence)));
	}
	// The status line always shows; the leader's HostBadge sits BESIDE the name in the generated row (and in
	// the launcher, as a crown). It used to replace the status line - which overflowed the 60 px row the moment
	// every joined member carried a ready line (2026-10-17).
	Show(StatusText, true);
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

void UFoundryFriendRow::SetInviteExpiry(const FoundryPartyRules::FInviteClock& InClock)
{
	InviteClock = InClock;
	InviteExpiryAccumulator = 0.0f;
	RefreshInviteStatus();
}

bool UFoundryFriendRow::IsInviteExpired() const
{
	return !bIsJoined && FoundryPartyRules::IsExpired(InviteClock.RemainingNow(FPlatformTime::Seconds()));
}

void UFoundryFriendRow::RefreshInviteStatus()
{
	if (bIsJoined || !InviteClock.IsSet())
	{
		return;
	}
	const double Remaining = InviteClock.RemainingNow(FPlatformTime::Seconds());
	const bool bExpiredNow = FoundryPartyRules::IsExpired(Remaining);
	SetText(StatusText, bExpiredNow
		? FString(TEXT("Invite expired"))
		: FString::Printf(TEXT("Invited - %s"), *FoundryPartyRules::ExpiryText(Remaining)));
	if (StatusText)
	{
		const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
		StatusText->SetColorAndOpacity(FSlateColor(bExpiredNow ? Palette.Error : Palette.TextSecondary));
	}
}

void UFoundryFriendRow::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// Pending rows only: once a second is enough for a minutes-granular line.
	if (bIsJoined || !InviteClock.IsSet())
	{
		return;
	}
	InviteExpiryAccumulator += InDeltaTime;
	if (InviteExpiryAccumulator >= 1.0f)
	{
		InviteExpiryAccumulator = 0.0f;
		RefreshInviteStatus();
	}
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
