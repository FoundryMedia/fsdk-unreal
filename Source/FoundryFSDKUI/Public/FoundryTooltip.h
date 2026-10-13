// Copyright Foundry Media. FoundryFSDK default UI kit: the tooltip pill.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryTooltip.generated.h"

class UTextBlock;

/**
 * The kit's own tooltip look - a small pill replacing the engine's default tooltip style
 * (UFoundryUIWidgetBase::ApplyKitTooltips / SetKitToolTip wrap plain ToolTipText in one of these
 * via UWidget::SetToolTip, rather than letting Slate draw its stock tooltip bubble). Owns no
 * behavior beyond showing a label - the look lives in the WBP, like every other kit widget.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryTooltip : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Sets the pill's label text. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void SetLabel(const FText& Label);

protected:
	// Lifecycle logs at Verbose (the 2026-10-13 tooltip-window investigation).
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> LabelText;
};
