// Copyright Foundry Media. FoundryFSDK default UI kit: one row of the match cluster's mode menu.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryModeRow.generated.h"

class UButton;
class UTextBlock;
class UWidget;

/**
 * One selectable matchmaking mode in UFoundryMatchCluster's mode menu: a label, a detail line, a
 * check mark on the selected row, and a select button that is disabled for a mode with no queue
 * yet. The match cluster spawns one per mode and calls Setup; a click broadcasts OnPicked with the
 * row's index (the cluster selects the mode, closes the menu and refreshes - the row owns no state
 * beyond what Setup last pushed). OnRowUpdated fires after every Setup so a WBP can restyle
 * (selected / unavailable) without new C++; the code never styles a brush.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryModeRow : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FFoundryModePicked, int32 /*Index*/);

	/** The select button was pressed. The cluster binds this with AddWeakLambda. */
	FFoundryModePicked OnPicked;

	/** Fills the row: label; detail (plus " (not queueable yet)" when unavailable); the check mark
	 *  shows only for the selected row; the select button is enabled only when available. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void Setup(int32 InIndex, const FText& InLabel, const FText& InDetail, bool bInSelected, bool bInAvailable);

	/**
	 * Call AFTER Setup. InJoined = the local party's joined members, InQueueCap = the mode's party cap
	 * (-1 while unknown - never reads as too big). Too big: "Party too big (8 > 4)" under the detail
	 * line and the select button disabled; otherwise the line is hidden and Setup's availability stands.
	 */
	void SetPartyGate(int32 InJoined, int32 InQueueCap);

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	int32 ModeIndex = 0;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	bool bIsSelected = false;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	bool bIsAvailable = false;

	/** True while the party is bigger than this mode's party cap (SetPartyGate). The row is disabled
	 *  and PartyTooBigText says why - the mode picker "must show which modes fit the current party"
	 *  (Justin, D4a). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	bool bIsPartyTooBig = false;

	/** Fires after every Setup, once every bound widget has been pushed. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnRowUpdated();

protected:
	virtual void NativeOnInitialized() override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> SelectButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> LabelText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DetailText;
	/** "Party too big (8 > 4)"; shown only while the party exceeds this mode's cap. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PartyTooBigText;
	/** Shown only on the selected row. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> CheckIcon;

private:
	UFUNCTION() void HandlePicked();
};
