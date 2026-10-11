#include "FoundryConfirmDialog.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "FoundryMenuHost.h"

UFoundryConfirmDialog::UFoundryConfirmDialog()
{
	bRetakeFocusOnClick = true;
}

void UFoundryConfirmDialog::Setup(const FText& Title, const FText& Body, const FText& ConfirmLabel, const FText& CancelLabel, float CooldownSeconds)
{
	ConfirmLabelCached = ConfirmLabel;
	SetText(TitleText, Title.ToString());
	SetText(BodyText, Body.ToString());
	Show(BodyText, !Body.IsEmpty());
	SetText(CancelText, CancelLabel.ToString());

	CooldownRemaining = FMath::Max(0.0f, CooldownSeconds);
	LastAppliedCooldownCeil = -1; // force an immediate (re)apply even if the ceil happens to match
	ApplyCooldownLabel(FMath::CeilToInt(CooldownRemaining));
}

void UFoundryConfirmDialog::ApplyCooldownLabel(int32 RemainingCeil)
{
	if (RemainingCeil == LastAppliedCooldownCeil)
	{
		return;
	}
	LastAppliedCooldownCeil = RemainingCeil;
	if (ConfirmButton)
	{
		ConfirmButton->SetIsEnabled(RemainingCeil <= 0);
	}
	SetText(ConfirmText, RemainingCeil > 0
		? FString::Printf(TEXT("%s (%d)"), *ConfirmLabelCached.ToString(), RemainingCeil)
		: ConfirmLabelCached.ToString());
}

void UFoundryConfirmDialog::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (ConfirmButton) { ConfirmButton->OnClicked.AddDynamic(this, &UFoundryConfirmDialog::HandleConfirmButtonClicked); }
	if (CancelButton) { CancelButton->OnClicked.AddDynamic(this, &UFoundryConfirmDialog::HandleCancelButtonClicked); }
	SetIsFocusable(true);
}

void UFoundryConfirmDialog::NativeConstruct()
{
	Super::NativeConstruct();
	SetKeyboardFocus();
	PlayOrTween(DialogAnim, true, 0.18f, Panel ? Panel : this);
}

void UFoundryConfirmDialog::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (CooldownRemaining > 0.0f)
	{
		CooldownRemaining = FMath::Max(0.0f, CooldownRemaining - InDeltaTime);
		ApplyCooldownLabel(FMath::CeilToInt(CooldownRemaining));
	}
}

FReply UFoundryConfirmDialog::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		HandleCancelButtonClicked();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UFoundryConfirmDialog::Dismiss()
{
	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestPopMenu(Host);
	}
	else
	{
		RemoveFromParent();
	}
}

void UFoundryConfirmDialog::HandleConfirmButtonClicked()
{
	Dismiss();
	OnConfirmed.Broadcast();
}

void UFoundryConfirmDialog::HandleCancelButtonClicked()
{
	Dismiss();
	OnCancelled.Broadcast();
}
