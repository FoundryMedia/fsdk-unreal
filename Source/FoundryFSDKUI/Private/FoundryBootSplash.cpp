#include "FoundryBootSplash.h"

#include "Animation/WidgetAnimation.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Engine/Texture2D.h"
#include "FoundryFSDKUILog.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundBase.h"
#include "Styling/SlateBrush.h"

namespace
{
	/** A hitch on the first frame(s) must not eat the hold. */
	constexpr float MaxHoldTickSeconds = 0.1f;
}

void UFoundryBootSplash::Setup(UTexture2D* StudioTexture, UTexture2D* PlatformTexture, USoundBase* Stinger, float FadeInSeconds, float HoldSeconds, float FadeOutSeconds)
{
	FadeInDuration = FMath::Max(0.0f, FadeInSeconds);
	HoldDuration = FMath::Max(0.0f, HoldSeconds);
	FadeOutDuration = FMath::Max(0.0f, FadeOutSeconds);
	StingerSound = Stinger;
	ApplyLogo(StudioLogo, StudioTexture);
	ApplyLogo(PlatformLogo, PlatformTexture);

	// The generated default carries the logo gap as PlatformLogo's own slot padding; with no studio
	// logo that gap would park the lone logo half a gap right of centre. Drop it when the studio side collapsed.
	if (StudioTexture == nullptr && PlatformLogo != nullptr)
	{
		if (UHorizontalBoxSlot* GapSlot = Cast<UHorizontalBoxSlot>(PlatformLogo->Slot))
		{
			GapSlot->SetPadding(FMargin(0.0f));
		}
	}
}

void UFoundryBootSplash::ApplyLogo(UImage* Image, UTexture2D* Texture) const
{
	if (Image == nullptr)
	{
		return;
	}
	if (Texture == nullptr)
	{
		Image->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	// Aspect-fit inside LogoMaxSize. A degenerate texture or box falls back to the box itself.
	FVector2D Size = LogoMaxSize;
	const float TexW = static_cast<float>(Texture->GetSizeX());
	const float TexH = static_cast<float>(Texture->GetSizeY());
	if (TexW > 0.0f && TexH > 0.0f && LogoMaxSize.X > 0.0 && LogoMaxSize.Y > 0.0)
	{
		const float Scale = FMath::Min(static_cast<float>(LogoMaxSize.X) / TexW, static_cast<float>(LogoMaxSize.Y) / TexH);
		Size = FVector2D(TexW * Scale, TexH * Scale);
	}

	FSlateBrush Brush; // fresh: no engine texture name inherited from a stock style
	Brush.DrawAs = ESlateBrushDrawType::Image;
	Brush.SetResourceObject(Texture);
	Brush.SetImageSize(Size);
	Brush.TintColor = FSlateColor(FLinearColor::White);
	Image->SetBrush(Brush);
	Image->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UFoundryBootSplash::ForceFinish()
{
	Finish();
}

bool UFoundryBootSplash::IsFinished() const
{
	return Phase == EPhase::Finished;
}

void UFoundryBootSplash::Finish()
{
	if (Phase == EPhase::Finished)
	{
		return;
	}
	Phase = EPhase::Finished;
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] finished"));
	OnFinished.Broadcast();
}

void UFoundryBootSplash::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true);
}

void UFoundryBootSplash::NativeConstruct()
{
	Super::NativeConstruct();
	SetKeyboardFocus();
	if (LogoRow && !FadeAnim)
	{
		LogoRow->SetRenderOpacity(0.0f); // the first value of the fade, so the logos never flash before the first tick
	}
}

void UFoundryBootSplash::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (Phase == EPhase::Finished)
	{
		return;
	}

	// Non-skippable: if anything stole focus, take it back.
	if (!HasKeyboardFocus())
	{
		SetKeyboardFocus();
	}

	if (Phase == EPhase::NotStarted)
	{
		// First tick, not Construct: the world is up and the audio device is ready.
		Phase = EPhase::FadeIn;
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] in"));
		if (StingerSound)
		{
			UGameplayStatics::PlaySound2D(this, StingerSound);
		}
		PlayOrTween(FadeAnim, true, FadeInDuration, LogoRow);
		return;
	}

	if (Phase == EPhase::Hold)
	{
		HoldElapsed += FMath::Min(InDeltaTime, MaxHoldTickSeconds);
		if (HoldElapsed >= HoldDuration)
		{
			Phase = EPhase::FadeOut;
			UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] out"));
			PlayOrTween(FadeAnim, false, FadeOutDuration, LogoRow);
		}
	}
}

void UFoundryBootSplash::NativeOnTransitionFinished(bool bForward)
{
	Super::NativeOnTransitionFinished(bForward);
	if (bForward)
	{
		if (Phase == EPhase::FadeIn)
		{
			Phase = EPhase::Hold;
			HoldElapsed = 0.0f;
			UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] hold"));
		}
		return;
	}
	if (Phase == EPhase::FadeOut)
	{
		Finish();
	}
}

FReply UFoundryBootSplash::NativeOnPreviewKeyDown(const FGeometry&, const FKeyEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnKeyDown(const FGeometry&, const FKeyEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnKeyUp(const FGeometry&, const FKeyEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnKeyChar(const FGeometry&, const FCharacterEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnAnalogValueChanged(const FGeometry&, const FAnalogInputEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnPreviewMouseButtonDown(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseButtonDown(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseButtonUp(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseButtonDoubleClick(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseMove(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseWheel(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnTouchStarted(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnTouchMoved(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnTouchEnded(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
