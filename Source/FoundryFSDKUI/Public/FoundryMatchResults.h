// Copyright Foundry Media. FoundryFSDK default UI kit: the match-end results screen.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryMatchResults.generated.h"

class UButton;
class UTextBlock;
class UWidgetAnimation;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnFoundryReturnRequested);

/**
 * The default MATCH RESULTS screen: a title ("VICTORY", the winner's name...), a detail line, a
 * "Returning to lobby in N s" countdown and a RETURN TO LOBBY button. A full-screen layer the
 * game pushes through IFoundryMenuHost::RequestPushMenu when the match ends; it never adds or
 * removes itself from the viewport. The match is over, so there is no confirm and no cooldown:
 * the button goes straight to OnReturnRequested, or - when nothing is bound - to the host's
 * RequestLeaveMatch (the game's leave path, which releases the seat). Escape and every other key
 * are left alone, so the host's own pause handling stays reachable.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryMatchResults : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Fired by the RETURN TO LOBBY button. When no listener is bound the widget asks the menu host
	 *  to leave the match instead. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FOnFoundryReturnRequested OnReturnRequested;

	/** Sets the headline and the detail line (an empty Detail collapses its text block). Safe to
	 *  call before the widget is pushed, and again later. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void SetResult(const FText& Title, const FText& Detail);

	/** The SERVER-authoritative moment (the game state's server world time, in seconds) at which the
	 *  server sends everyone back to the lobby. The countdown line reads "Returning to lobby in N s"
	 *  (N rounded up) and "Returning to lobby..." at 0. A deadline of 0 or less hides the line. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void SetReturnDeadline(double ServerTimeSeconds);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	/** The host's PushMenu focuses this ROOT after NativeConstruct; hand that focus to ReturnButton
	 *  so a gamepad's A press lands on it instead of falling to the viewport. */
	virtual FReply NativeOnFocusReceived(const FGeometry& InGeometry, const FFocusEvent& InFocusEvent) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DetailText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CountdownText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ReturnButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ReturnText;

	/** Optional - an animation authored on this WBP's own Sequencer; unbound uses PlayOrTween's code
	 *  tween fallback (a fade of the whole screen). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetAnimOptional))
	TObjectPtr<UWidgetAnimation> FadeAnim;

private:
	UFUNCTION() void HandleReturnButtonClicked();

	/** Re-renders CountdownText from the deadline and the replicated server time; writes the text
	 *  only when the displayed second changes. */
	void RefreshCountdown();

	double ReturnDeadline = 0.0;
	// Sentinel that no real value takes, so the first RefreshCountdown after SetReturnDeadline
	// always applies.
	int32 LastShownSeconds = MIN_int32;
	bool bReturnRequested = false;
};
