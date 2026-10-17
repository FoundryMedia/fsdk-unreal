#include "FoundryModeRow.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "FoundryPartyRules.h"
#include "FoundryUISettings.h"

void UFoundryModeRow::NativeOnInitialized()
{
	Super::NativeOnInitialized(); // hand cursor + kit tooltips
	if (SelectButton)
	{
		SelectButton->OnClicked.AddDynamic(this, &UFoundryModeRow::HandlePicked);
	}
}

void UFoundryModeRow::Setup(int32 InIndex, const FText& InLabel, const FText& InDetail, bool bInSelected, bool bInAvailable)
{
	ModeIndex = InIndex;
	bIsSelected = bInSelected;
	bIsAvailable = bInAvailable;
	bIsPartyTooBig = false; // SetPartyGate (after this Setup) re-arms it
	Show(PartyTooBigText, false);
	SetText(LabelText, InLabel.ToString());
	SetText(DetailText, bInAvailable ? InDetail.ToString() : InDetail.ToString() + TEXT(" (not queueable yet)"));
	Show(CheckIcon, bInSelected);
	if (SelectButton)
	{
		SelectButton->SetIsEnabled(bInAvailable);
	}
	OnRowUpdated();
}

void UFoundryModeRow::SetPartyGate(int32 InJoined, int32 InQueueCap)
{
	bIsPartyTooBig = FoundryPartyRules::IsTooBig(InJoined, InQueueCap);
	SetText(PartyTooBigText, bIsPartyTooBig ? FoundryPartyRules::TooBigText(InJoined, InQueueCap) : FString());
	Show(PartyTooBigText, bIsPartyTooBig);
	if (PartyTooBigText)
	{
		PartyTooBigText->SetColorAndOpacity(FSlateColor(UFoundryUISettings::Get()->Palette.Error));
	}
	if (SelectButton)
	{
		SelectButton->SetIsEnabled(bIsAvailable && !bIsPartyTooBig);
	}
	OnRowUpdated();
}

void UFoundryModeRow::HandlePicked()
{
	OnPicked.Broadcast(ModeIndex);
}
