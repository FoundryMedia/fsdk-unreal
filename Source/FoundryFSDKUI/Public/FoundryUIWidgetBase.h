// Copyright Foundry Media. FoundryFSDK default UI kit widget base.

#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.generated.h"

class UTextBlock;
class UWidget;
class UWidgetAnimation;

/**
 * Base for the default UI kit's C++ parent classes. The LAYOUT is a Widget Blueprint (WBP_Foundry*)
 * whose parent is one of these; the C++ side only binds the named widgets it needs
 * (meta=(BindWidgetOptional), so a WBP can be laid out a piece at a time - a missing name is a
 * compiler note, never a crash) and pushes state into them: text, visibility, enabled, tint.
 * Anything that is a LOOK (hover, selected styling) is left to the WBP; the code never styles a
 * brush.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryUIWidgetBase : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * Every Button and ComboBox in the widget's tree takes the Hand cursor. Called from
	 * NativeOnInitialized here; a plain (non-UFoundryUIWidgetBase) row widget calls it itself.
	 */
	static void UseHandCursor(UUserWidget* Widget);

	static void SetText(UTextBlock* Text, const FString& Value);

	/** Collapsed when hidden; Visible (hit-testable) or SelfHitTestInvisible (display only) when
	 *  shown. */
	static void Show(UWidget* Widget, bool bVisible, bool bHitTestable = false);

	static FString Truncate(const FString& S, int32 MaxChars);

	/**
	 * Plays Anim forward / reverse (reversing mid-flight when it is already playing) when it is
	 * bound; otherwise tweens TweenTarget's RenderOpacity between 0 and 1 (plus a RenderTranslation
	 * X slide from TweenOffset, when non-zero) over FallbackSeconds, ease-out cubic, driven from
	 * NativeTick - reversible mid-flight the same way. Either path ends in
	 * OnTransitionFinished(bForward); bind the WBP's own styling to it, or override
	 * NativeOnTransitionFinished in a C++ subclass.
	 */
	void PlayOrTween(UWidgetAnimation* Anim, bool bForward, float FallbackSeconds, UWidget* TweenTarget);

	/** Extra distance (slate units) TweenTarget's RenderTranslation X eases from/to during the code
	 *  fallback tween; 0 = opacity only. Ignored when Anim plays instead. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	float TweenOffset = 0.0f;

	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnTransitionFinished(bool bForward);

	/** Native hook for OnTransitionFinished; override in a C++ subclass. The WBP's own
	 *  BlueprintImplementableEvent runs after it. */
	virtual void NativeOnTransitionFinished(bool bForward) {}

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void OnAnimationFinished_Implementation(const UWidgetAnimation* Animation) override;

private:
	// The animation (if any) the last PlayOrTween call drove, and which direction - so
	// OnAnimationFinished_Implementation knows which one just finished and what bForward to report.
	TWeakObjectPtr<UWidgetAnimation> LastAnim;
	bool LastAnimForward = false;

	// Code-tween fallback state (PlayOrTween with a null Anim). TweenProgress is persistent (0 =
	// hidden, 1 = shown) so a toggle mid-flight reverses smoothly instead of snapping.
	TWeakObjectPtr<UWidget> TweenWidget;
	bool bTweenActive = false;
	bool bTweenForward = false;
	float TweenProgress = 0.0f;
	float TweenDuration = 0.3f;
};
