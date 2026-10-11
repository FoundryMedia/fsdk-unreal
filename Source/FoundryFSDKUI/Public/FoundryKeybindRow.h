// Copyright Foundry Media. FoundryFSDK default UI kit: one row of the settings menu's Controls section.

#pragma once

#include "CoreMinimal.h"
#include "InputCoreTypes.h" // FKey
#include "FoundryUIWidgetBase.h"
#include "FoundryKeybindRow.generated.h"

class UButton;
class UTextBlock;

/**
 * One rebindable Enhanced Input mapping: a label, a KeyButton showing the current key (or
 * "Press a key..." while capturing one), and a Reset button shown only when the mapping has been
 * customized away from its default. The row only CAPTURES the next key press/click and reports it
 * via OnKeyChosen - the owning UFoundrySettingsMenu does the actual Enhanced Input mapping call
 * (MapPlayerKey) and calls Setup again afterward to reflect whatever was applied.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryKeybindRow : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FFoundryKeybindRowKeyChosen, FName, InMappingName, FKey, Key);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryKeybindRowResetRequested, FName, InMappingName);

	/** A key (keyboard/mouse, or a gamepad input - slot handling is the menu's) was chosen while
	 *  capturing. The row is no longer capturing by the time this fires. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryKeybindRowKeyChosen OnKeyChosen;

	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryKeybindRowResetRequested OnResetRequested;

	/** (Re)binds this row to InMappingName, showing CurrentKey; ResetButton shows only when
	 *  bCustomized. Call again after the menu has applied a change, to reflect the new state. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void Setup(FName InMappingName, const FText& InDisplayName, const FKey& CurrentKey, bool bCustomized);

	/** True while this row is mid-capture ("Press a key..."). The owning settings menu checks this
	 *  before letting Escape close the whole menu - a capturing row must get the cancel instead. */
	UFUNCTION(BlueprintPure, Category = "Foundry UI")
	bool IsCapturing() const { return bCapturing; }

	/** The display name last passed to Setup - what the settings menu's hover-to-describe pane shows
	 *  as the title while this row is hovered. */
	UFUNCTION(BlueprintPure, Category = "Foundry UI")
	FText GetDisplayName() const { return DisplayNameCached; }

protected:
	virtual void NativeOnInitialized() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	/** Left mouse on KeyButton is swallowed by SButton itself (it needs the press/release pair to
	 *  fire OnClicked -> BeginCapture), so this never sees a left-mouse-down while NOT capturing -
	 *  left mouse is therefore not bindable as a key in v1. While capturing, any OTHER mouse button
	 *  (e.g. a side button, middle click) reaches here and ends the capture normally. */
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	/** Cancels an in-progress capture (restores the key label) if this row loses focus while
	 *  capturing - e.g. RebuildKeybindRows() rebuilding the list out from under it. Without this, a
	 *  second row could start capturing while this one was still stuck showing "Press a key...". */
	virtual void NativeOnFocusLost(const FFocusEvent& InFocusEvent) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ActionText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> KeyButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> KeyText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ResetButton;

private:
	UFUNCTION() void HandleKeyButtonClicked();
	UFUNCTION() void HandleResetButtonClicked();

	void BeginCapture();
	void EndCapture(const FKey& ChosenKey);
	FString CurrentKeyLabel() const;

	FName MappingName;
	FText DisplayNameCached;
	FKey CurrentKeyCached;
	bool bCapturing = false;
};
