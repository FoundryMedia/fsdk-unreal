#include "FoundryInviteRow.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "FoundryPartyRules.h"
#include "FoundryUISettings.h"
#include "HAL/PlatformTime.h"

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

void UFoundryInviteRow::Setup(const FString& InLeaderName, const FString& InPartyId, int32 InJoined, int32 InCap, const FoundryPartyRules::FInviteClock& InClock)
{
	LeaderName = InLeaderName;
	PartyId = InPartyId;
	Clock = InClock;
	ExpiryAccumulator = 0.0f;

	const FString Size = (InJoined > 0 && InCap > 0) ? FString::Printf(TEXT("%d / %d"), InJoined, InCap) : FString();
	// A WBP generated before SizeText existed still shows the size - appended to the leader line.
	const FString Leader = FString::Printf(TEXT("%s's party"), *InLeaderName);
	SetText(LeaderText, (SizeText || Size.IsEmpty()) ? Leader : Leader + TEXT("   ") + Size);
	SetText(SizeText, Size);
	Show(SizeText, !Size.IsEmpty());

	RefreshExpiry();
	OnRowUpdated();
}

void UFoundryInviteRow::SetupJoinRequest(const FString& InRequesterId, const FString& InDisplayName)
{
	LeaderName = InDisplayName;
	PartyId = InRequesterId;
	Clock = FoundryPartyRules::FInviteClock();
	ExpiryAccumulator = 0.0f;

	SetText(LeaderText, FString::Printf(TEXT("%s wants to join your party"), *InDisplayName));
	SetText(SizeText, FString());
	Show(SizeText, false);

	RefreshExpiry();
	OnRowUpdated();
}

void UFoundryInviteRow::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	// Once a second is enough for a minutes-granular line - and nothing at all for a row with no expiry.
	if (!Clock.IsSet())
	{
		return;
	}
	ExpiryAccumulator += InDeltaTime;
	if (ExpiryAccumulator >= 1.0f)
	{
		ExpiryAccumulator = 0.0f;
		RefreshExpiry();
	}
}

void UFoundryInviteRow::RefreshExpiry()
{
	const double Remaining = Clock.RemainingNow(FPlatformTime::Seconds());
	const FString Text = FoundryPartyRules::ExpiryText(Remaining);
	bExpired = FoundryPartyRules::IsExpired(Remaining);

	SetText(ExpiryText, Text);
	Show(ExpiryText, !Text.IsEmpty());
	if (ExpiryText)
	{
		const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
		ExpiryText->SetColorAndOpacity(FSlateColor(bExpired ? Palette.Error : Palette.TextSecondary));
	}
	// A dead invite never offers Accept (Decline stays: it clears the row if fid has not yet).
	if (AcceptButton)
	{
		AcceptButton->SetIsEnabled(!bExpired);
	}
}

void UFoundryInviteRow::HandleAccept()
{
	if (bExpired)
	{
		return;
	}
	OnAccept.Broadcast(PartyId);
}

void UFoundryInviteRow::HandleDecline()
{
	OnDecline.Broadcast(PartyId);
}
