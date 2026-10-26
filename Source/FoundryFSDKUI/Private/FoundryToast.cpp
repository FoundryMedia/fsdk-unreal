#include "FoundryToast.h"

#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "FoundryGameUserSettings.h"
#include "FoundryUISettings.h"

namespace
{
	constexpr float InSeconds = 0.18f;
	constexpr float OutSeconds = 0.25f;
	constexpr float SlidePixels = 10.0f;
}

void UFoundryToast::Setup(const FText& InMessage, EFoundryToastKind InKind, float HoldSeconds)
{
	Kind = InKind;
	Message = InMessage.ToString();
	Hold = FMath::Max(0.5f, HoldSeconds);
	SetText(MessageText, Message);
	if (MessageText)
	{
		MessageText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	}
	if (Pill)
	{
		const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
		const FLinearColor Fill = Kind == EFoundryToastKind::Success ? Palette.Online
			: (Kind == EFoundryToastKind::Error ? Palette.Error : Palette.Primary);
		FSlateBrush Brush; // fresh: no engine texture name, outline RGB = fill so the AA edge never fringes
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Fill);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::HalfHeightRadius;
		Brush.OutlineSettings.Width = 0.0f;
		Brush.OutlineSettings.Color = FSlateColor(FLinearColor(Fill.R, Fill.G, Fill.B, 0.0f));
		Pill->SetBrush(Brush);
		Pill->SetBrushColor(FLinearColor::White);
	}
	Age = 0.0f;
	bLeaving = false;
	ApplyFrame();
}

void UFoundryToast::Restart(float HoldSeconds)
{
	Hold = FMath::Max(0.5f, HoldSeconds);
	Age = FMath::Min(Age, InSeconds);
	bLeaving = false;
	LeaveAge = 0.0f;
}

void UFoundryToast::Dismiss()
{
	if (!bLeaving)
	{
		bLeaving = true;
		LeaveAge = 0.0f;
	}
}

void UFoundryToast::NativeConstruct()
{
	Super::NativeConstruct();
	SetVisibility(ESlateVisibility::HitTestInvisible); // never steals a click or the cursor
	ApplyFrame();
}

void UFoundryToast::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (bFinished)
	{
		return;
	}
	const float Dt = FMath::Min(InDeltaTime, 0.1f);
	Age += Dt;
	if (!bLeaving && Age >= InSeconds + Hold)
	{
		Dismiss();
	}
	if (bLeaving)
	{
		LeaveAge += Dt;
		if (LeaveAge >= OutSeconds)
		{
			bFinished = true;
			RemoveFromParent();
			OnToastFinished.Broadcast(this);
			return;
		}
	}
	ApplyFrame();
}

void UFoundryToast::ApplyFrame()
{
	const UFoundryGameUserSettings* User = UFoundryGameUserSettings::Get();
	const bool bReduced = User && User->IsReducedMotion();
	float Opacity = FMath::Clamp(Age / InSeconds, 0.0f, 1.0f);
	float Slide = bReduced ? 0.0f : -SlidePixels * (1.0f - Opacity);
	if (bLeaving)
	{
		Opacity *= 1.0f - FMath::Clamp(LeaveAge / OutSeconds, 0.0f, 1.0f);
	}
	SetRenderOpacity(Opacity);
	SetRenderTranslation(FVector2D(0.0, Slide));
}
