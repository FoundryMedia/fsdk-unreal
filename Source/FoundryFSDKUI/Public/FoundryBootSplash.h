// Copyright Foundry Media. FoundryFSDK default UI kit: the boot splash (studio + platform logos, optional stinger).

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryBootSplash.generated.h"

class UImage;
class USoundBase;
class UTexture2D;
class UWidget;
class UWidgetAnimation;

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFoundryBootSplashFinished);

/**
 * The default BOOT SPLASH: an opaque Backdrop with a LogoRow (StudioLogo + PlatformLogo) that fades
 * in, holds, and fades out, with an optional stinger sound. NON-SKIPPABLE by design: it takes
 * keyboard focus and every key / mouse / touch / analog handler returns Handled, in both the
 * preview and the bubble pass (Tab and the arrow keys navigate in the bubble pass), so no input
 * reaches the menu behind it. The only ways out are the sequence ending or ForceFinish (the
 * subsystem's failsafe timer).
 *
 * Drive it through UFoundryBootSplashSubsystem::TryPlay; a game that builds its own splash calls
 * Setup before AddToViewport and binds OnFinished.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryBootSplash : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Fires exactly once, after the fade-out (or ForceFinish). */
	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryBootSplashFinished OnFinished;

	/** Fires once, the moment the fade-out begins: the host brings its scene / music in UNDER the splash
	 *  so the hand-over is a crossfade, not black then a cut. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryBootSplashFinished OnFadeOutStarted;

	/** During the fade-out the Backdrop fades with the logos, revealing whatever is behind the splash. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	bool bRevealSceneOnFadeOut = true;

	/** Logos (either may be null -> that Image collapses), optional stinger (null = silent), durations
	 *  in seconds. Call before AddToViewport. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void Setup(UTexture2D* StudioTexture, UTexture2D* PlatformTexture, USoundBase* Stinger, float FadeInSeconds, float HoldSeconds, float FadeOutSeconds);

	/** Failsafe: ends the splash now (broadcasts OnFinished if it has not yet). */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void ForceFinish();

	UFUNCTION(BlueprintPure, Category = "Foundry UI")
	bool IsFinished() const;

	/** Max logo box; each logo is aspect-fit inside it. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	FVector2D LogoMaxSize = FVector2D(520.f, 200.f);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void NativeOnTransitionFinished(bool bForward) override;

	// Non-skippable: EVERY input path is swallowed, none special-cased (no Escape, no Enter).
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnKeyUp(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnKeyChar(const FGeometry& InGeometry, const FCharacterEvent& InCharEvent) override;
	virtual FReply NativeOnAnalogValueChanged(const FGeometry& InGeometry, const FAnalogInputEvent& InAnalogEvent) override;
	virtual FReply NativeOnPreviewMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonUp(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseButtonDoubleClick(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseMove(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnMouseWheel(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;
	virtual FReply NativeOnTouchStarted(const FGeometry& InGeometry, const FPointerEvent& InGestureEvent) override;
	virtual FReply NativeOnTouchMoved(const FGeometry& InGeometry, const FPointerEvent& InGestureEvent) override;
	virtual FReply NativeOnTouchEnded(const FGeometry& InGeometry, const FPointerEvent& InGestureEvent) override;

	/** Opaque black cover; never fades. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Backdrop;
	/** The fade target: holds both logos. Unbound -> the sequence still runs on the same clock, with no fade. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> LogoRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> StudioLogo;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> PlatformLogo;

	/** Optional - an animation authored on this WBP's own Sequencer (LogoRow opacity 0 -> 1 forward);
	 *  unbound uses PlayOrTween's code tween fallback. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetAnimOptional))
	TObjectPtr<UWidgetAnimation> FadeAnim;

private:
	enum class EPhase : uint8
	{
		NotStarted,
		FadeIn,
		Hold,
		FadeOut,
		Finished,
	};

	/** A FRESH brush per call (never the WBP's default brush - the engine-texture-name trap); a null Texture collapses the Image. */
	void ApplyLogo(UImage* Image, UTexture2D* Texture) const;

	/** The one place OnFinished is broadcast; guarded so it fires exactly once. */
	void Finish();

	EPhase Phase = EPhase::NotStarted;
	float FadeInDuration = 0.4f;
	float HoldDuration = 2.6f;
	float FadeOutDuration = 0.6f;
	float HoldElapsed = 0.0f;
	float FadeOutElapsed = 0.0f;
	/** Real seconds since the stinger started (the clock the fade-out is scheduled on when bRealTimeHold). */
	float SinceStart = 0.0f;
	/** With a stinger the hold runs on the REAL clock (uncapped deltas) so the logos never drift from the
	 *  sound; without one the 0.1 s first-frame hitch guard applies. */
	bool bRealTimeHold = false;

	UPROPERTY(Transient)
	TObjectPtr<USoundBase> StingerSound;
};
