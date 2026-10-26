// Copyright Foundry Media. FoundryFSDK default UI kit: a toast (success / error / info), shown by UFoundryToastSubsystem.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryToast.generated.h"

class UBorder;
class UTextBlock;

UENUM(BlueprintType)
enum class EFoundryToastKind : uint8
{
	Success,
	Error,
	Info
};

/**
 * One toast: a pill (Pill, a Border) with white text (MessageText), tinted by kind - Success =
 * Palette.Online, Error = Palette.Error, Info = Palette.Primary. Fades / slides in, holds, fades out,
 * then removes itself and fires OnToastFinished. Never created directly - UFoundryToastSubsystem::ShowToast
 * stacks and positions them.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryToast : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	void Setup(const FText& Message, EFoundryToastKind InKind, float HoldSeconds);

	/** Restarts the hold (the same message shown again while this one is still up). */
	void Restart(float HoldSeconds);

	/** Starts the fade-out now (a newer toast pushed this one past the visible limit). */
	void Dismiss();

	const FString& GetMessage() const { return Message; }
	EFoundryToastKind GetKind() const { return Kind; }

	DECLARE_MULTICAST_DELEGATE_OneParam(FFoundryToastFinished, UFoundryToast*);
	FFoundryToastFinished OnToastFinished;

protected:
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UBorder> Pill;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> MessageText;

private:
	void ApplyFrame();

	FString Message;
	EFoundryToastKind Kind = EFoundryToastKind::Info;
	float Age = 0.0f;
	float Hold = 2.5f;
	bool bLeaving = false;
	float LeaveAge = 0.0f;
	bool bFinished = false;
};
