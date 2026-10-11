#include "FoundryKeybindRow.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"

void UFoundryKeybindRow::Setup(FName InMappingName, const FText& InDisplayName, const FKey& CurrentKey, bool bCustomized)
{
	MappingName = InMappingName;
	DisplayNameCached = InDisplayName;
	CurrentKeyCached = CurrentKey;
	bCapturing = false;
	SetText(ActionText, InDisplayName.ToString());
	SetText(KeyText, CurrentKeyLabel());
	Show(ResetButton, bCustomized, true); // a button must be hit-testable, not just visible
}

FString UFoundryKeybindRow::CurrentKeyLabel() const
{
	return CurrentKeyCached.IsValid() ? CurrentKeyCached.GetDisplayName().ToString() : TEXT("Unbound");
}

void UFoundryKeybindRow::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (KeyButton) { KeyButton->OnClicked.AddDynamic(this, &UFoundryKeybindRow::HandleKeyButtonClicked); }
	if (ResetButton) { ResetButton->OnClicked.AddDynamic(this, &UFoundryKeybindRow::HandleResetButtonClicked); }
	SetIsFocusable(true);
}

void UFoundryKeybindRow::HandleKeyButtonClicked()
{
	BeginCapture();
}

void UFoundryKeybindRow::BeginCapture()
{
	bCapturing = true;
	SetText(KeyText, TEXT("Press a key..."));
	SetKeyboardFocus();
}

void UFoundryKeybindRow::EndCapture(const FKey& ChosenKey)
{
	bCapturing = false;
	OnKeyChosen.Broadcast(MappingName, ChosenKey);
}

FReply UFoundryKeybindRow::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (bCapturing)
	{
		if (InKeyEvent.GetKey() == EKeys::Escape)
		{
			bCapturing = false;
			SetText(KeyText, CurrentKeyLabel());
			return FReply::Handled();
		}
		EndCapture(InKeyEvent.GetKey());
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UFoundryKeybindRow::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (bCapturing)
	{
		EndCapture(InMouseEvent.GetEffectingButton());
		return FReply::Handled();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

void UFoundryKeybindRow::HandleResetButtonClicked()
{
	OnResetRequested.Broadcast(MappingName);
}

void UFoundryKeybindRow::NativeOnFocusLost(const FFocusEvent& InFocusEvent)
{
	Super::NativeOnFocusLost(InFocusEvent);
	if (bCapturing)
	{
		bCapturing = false;
		SetText(KeyText, CurrentKeyLabel());
	}
}
