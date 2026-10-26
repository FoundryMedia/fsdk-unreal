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
	CancelLabelCached = CancelLabel;
	AutoCancelRemaining = 0.0f;

	CooldownRemaining = FMath::Max(0.0f, CooldownSeconds);
	LastAppliedCooldownCeil = -1; // force an immediate (re)apply even if the ceil happens to match
	ApplyCooldownLabel(FMath::CeilToInt(CooldownRemaining));
}

void UFoundryConfirmDialog::SetAutoCancel(float Seconds)
{
	AutoCancelRemaining = bAnswered ? 0.0f : FMath::Max(0.0f, Seconds);
	LastAutoCancelCeil = -1;
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
	if (AutoCancelRemaining > 0.0f)
	{
		AutoCancelRemaining = FMath::Max(0.0f, AutoCancelRemaining - InDeltaTime);
		const int32 Ceil = FMath::CeilToInt(AutoCancelRemaining);
		if (Ceil != LastAutoCancelCeil)
		{
			LastAutoCancelCeil = Ceil;
			SetText(CancelText, FString::Printf(TEXT("%s (%d)"), *CancelLabelCached.ToString(), Ceil));
		}
		if (AutoCancelRemaining <= 0.0f)
		{
			HandleCancelButtonClicked();
		}
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
		IFoundryMenuHost::Execute_RequestRemoveMenu(Host, this); // this layer, not whatever is on top
	}
	else
	{
		RemoveFromParent();
	}
}

void UFoundryConfirmDialog::CloseWithoutAnswer()
{
	if (bAnswered)
	{
		return;
	}
	bAnswered = true;
	AutoCancelRemaining = 0.0f;
	Dismiss();
}

void UFoundryConfirmDialog::HandleConfirmButtonClicked()
{
	if (bAnswered)
	{
		return;
	}
	bAnswered = true;
	AutoCancelRemaining = 0.0f;
	Dismiss();
	OnConfirmed.Broadcast();
}

void UFoundryConfirmDialog::HandleCancelButtonClicked()
{
	if (bAnswered)
	{
		return;
	}
	bAnswered = true;
	AutoCancelRemaining = 0.0f;
	Dismiss();
	OnCancelled.Broadcast();
}
