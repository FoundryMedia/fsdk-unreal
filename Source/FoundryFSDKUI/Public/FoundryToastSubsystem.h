// Copyright Foundry Media. FoundryFSDK default UI kit: the toast stack.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FoundryToast.h"
#include "FoundryToastSubsystem.generated.h"

/**
 * Shows toasts (UFoundryUISettings::DefaultToast) stacked top-centre above every menu. Success for a
 * completed action ("Settings saved"), Error for a failure the player must see (a social action the
 * platform refused); chat errors stay in the chat. The same message while it is still up restarts that
 * toast instead of stacking a copy. At most ToastMaxVisible at once - the oldest fades early.
 */
UCLASS()
class FOUNDRYFSDKUI_API UFoundryToastSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Shows a toast in WorldContextObject's game instance. A missing toast class logs once and shows nothing. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI", meta = (WorldContext = "WorldContextObject"))
	static void ShowToast(const UObject* WorldContextObject, const FText& Message, EFoundryToastKind Kind);

	void Show(const FText& Message, EFoundryToastKind Kind);

private:
	void HandleToastFinished(UFoundryToast* Toast);
	void Relayout();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UFoundryToast>> Active;
};
