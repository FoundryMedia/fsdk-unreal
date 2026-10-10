#include "FoundryUIWidgetBase.h"

#include "Animation/WidgetAnimation.h" // TWeakObjectPtr<UWidgetAnimation> assignment needs the complete type
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/ComboBoxString.h"
#include "Components/TextBlock.h"

namespace
{
	float EaseOutCubic(float T)
	{
		T = FMath::Clamp(T, 0.0f, 1.0f);
		const float U = 1.0f - T;
		return 1.0f - U * U * U;
	}
}

void UFoundryUIWidgetBase::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	UseHandCursor(this);
}

void UFoundryUIWidgetBase::UseHandCursor(UUserWidget* Widget)
{
	if (!Widget || !Widget->WidgetTree)
	{
		return;
	}
	Widget->WidgetTree->ForEachWidget([](UWidget* W)
	{
		if (W->IsA<UButton>() || W->IsA<UComboBoxString>())
		{
			W->SetCursor(EMouseCursor::Hand);
		}
	});
}

void UFoundryUIWidgetBase::SetText(UTextBlock* Text, const FString& Value)
{
	if (Text)
	{
		Text->SetText(FText::FromString(Value)); // plain text: user content is never parsed as markup
	}
}

void UFoundryUIWidgetBase::Show(UWidget* Widget, bool bVisible, bool bHitTestable)
{
	if (Widget)
	{
		Widget->SetVisibility(!bVisible ? ESlateVisibility::Collapsed
			: bHitTestable ? ESlateVisibility::Visible : ESlateVisibility::SelfHitTestInvisible);
	}
}

FString UFoundryUIWidgetBase::Truncate(const FString& S, int32 MaxChars)
{
	if (S.Len() <= MaxChars)
	{
		return S;
	}
	// ASCII-only (no U+2026 ellipsis glyph): three plain dots, so the budget they cost is 3 chars.
	return S.Left(FMath::Max(1, MaxChars - 3)) + TEXT("...");
}

void UFoundryUIWidgetBase::PlayOrTween(UWidgetAnimation* Anim, bool bForward, float FallbackSeconds, UWidget* TweenTarget)
{
	if (Anim)
	{
		const bool bAlreadyPlayingThisDirection = IsAnimationPlaying(Anim) && LastAnimForward == bForward;
		if (bAlreadyPlayingThisDirection)
		{
			return; // a request in the direction already playing is a no-op
		}
		bTweenActive = false; // a real animation wins over any in-flight code tween
		LastAnim = Anim;
		LastAnimForward = bForward; // the direction actually playing after this call, either way
		if (IsAnimationPlaying(Anim))
		{
			// Playing the OPPOSITE direction - reverse mid-flight (ReverseAnimation flips whatever
			// is currently playing, landing it on bForward).
			ReverseAnimation(Anim);
		}
		else if (bForward)
		{
			PlayAnimationForward(Anim);
		}
		else
		{
			PlayAnimationReverse(Anim);
		}
		return;
	}

	// Code fallback: tween TweenTarget's RenderOpacity (+ RenderTranslation X) over FallbackSeconds.
	// TweenProgress is NOT reset here, so a toggle mid-flight reverses smoothly from wherever it sat.
	LastAnim.Reset();
	TweenWidget = TweenTarget;
	bTweenForward = bForward;
	TweenDuration = FMath::Max(0.01f, FallbackSeconds);
	bTweenActive = true;
}

void UFoundryUIWidgetBase::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (!bTweenActive)
	{
		return;
	}
	const float Dir = bTweenForward ? 1.0f : -1.0f;
	TweenProgress = FMath::Clamp(TweenProgress + Dir * InDeltaTime / TweenDuration, 0.0f, 1.0f);
	const float Eased = EaseOutCubic(TweenProgress);
	if (UWidget* W = TweenWidget.Get())
	{
		W->SetRenderOpacity(Eased);
		if (TweenOffset != 0.0f)
		{
			FWidgetTransform T = W->GetRenderTransform();
			T.Translation.X = FMath::Lerp(TweenOffset, 0.0f, Eased);
			W->SetRenderTransform(T);
		}
	}
	const bool bDone = (bTweenForward && TweenProgress >= 1.0f) || (!bTweenForward && TweenProgress <= 0.0f);
	if (bDone)
	{
		bTweenActive = false;
		NativeOnTransitionFinished(bTweenForward);
		OnTransitionFinished(bTweenForward);
	}
}

void UFoundryUIWidgetBase::OnAnimationFinished_Implementation(const UWidgetAnimation* Animation)
{
	Super::OnAnimationFinished_Implementation(Animation);
	if (Animation && Animation == LastAnim.Get())
	{
		NativeOnTransitionFinished(LastAnimForward);
		OnTransitionFinished(LastAnimForward);
	}
}
