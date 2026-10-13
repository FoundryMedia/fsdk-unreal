#include "FoundryModeRow.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"

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
	SetText(LabelText, InLabel.ToString());
	SetText(DetailText, bInAvailable ? InDetail.ToString() : InDetail.ToString() + TEXT(" (not queueable yet)"));
	Show(CheckIcon, bInSelected);
	if (SelectButton)
	{
		SelectButton->SetIsEnabled(bInAvailable);
	}
	OnRowUpdated();
}

void UFoundryModeRow::HandlePicked()
{
	OnPicked.Broadcast(ModeIndex);
}
