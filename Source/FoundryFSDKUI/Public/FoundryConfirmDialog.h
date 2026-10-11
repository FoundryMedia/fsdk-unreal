// Copyright Foundry Media. FoundryFSDK default UI kit: the confirm dialog.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryConfirmDialog.generated.h"

class UButton;
class UTextBlock;
class UWidget;
class UWidgetAnimation;

/**
 * The default CONFIRM dialog: a title, an optional body, and a Confirm/Cancel pair. Confirm can
 * carry a cooldown (Setup's CooldownSeconds / GetCooldownRemaining) - while it counts down,
 * ConfirmButton is disabled and ConfirmText reads "<label> (N)"; a fat-finger guard for a
 * destructive action (the kit's own pause menu uses it for LEAVE MATCH, never for QUIT TO
 * DESKTOP). Never adds/removes itself from the viewport - see UFoundryUIWidgetBase::FindMenuHost.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryConfirmDialog : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Turns on bRetakeFocusOnClick (UFoundryUIWidgetBase) - a click on the backdrop still lets
	 *  Escape close this dialog afterward. */
	UFoundryConfirmDialog();

	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFoundryConfirmDialogEvent);

	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryConfirmDialogEvent OnConfirmed;

	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryConfirmDialogEvent OnCancelled;

	/** Sets the labels and (re)starts the cooldown; 0 seconds enables Confirm immediately. Call
	 *  once, right after CreateWidget - before the dialog is pushed/added to the viewport. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void Setup(const FText& Title, const FText& Body, const FText& ConfirmLabel, const FText& CancelLabel, float CooldownSeconds);

	UFUNCTION(BlueprintPure, Category = "Foundry UI")
	float GetCooldownRemaining() const { return CooldownRemaining; }

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Backdrop;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Panel;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BodyText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ConfirmButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ConfirmText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CancelButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CancelText;

	/** Optional - an animation authored on this WBP's own Sequencer; unbound uses PlayOrTween's code
	 *  tween fallback (fade only - TweenOffset is left at 0, the base class default). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetAnimOptional))
	TObjectPtr<UWidgetAnimation> DialogAnim;

private:
	UFUNCTION() void HandleConfirmButtonClicked();
	UFUNCTION() void HandleCancelButtonClicked();

	/** FindMenuHost() ? Execute_RequestPopMenu(host) : RemoveFromParent(). */
	void Dismiss();
	void ApplyCooldownLabel(int32 RemainingCeil);

	FText ConfirmLabelCached;
	float CooldownRemaining = 0.0f;
	// -1 so the very first ApplyCooldownLabel call (from Setup) always applies, even when the
	// computed ceil happens to be 0 (the member's own default).
	int32 LastAppliedCooldownCeil = -1;
};
