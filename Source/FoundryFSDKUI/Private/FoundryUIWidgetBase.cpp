#include "FoundryUIWidgetBase.h"

#include "Animation/WidgetAnimation.h" // TWeakObjectPtr<UWidgetAnimation> assignment needs the complete type
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "FoundryConfirmDialog.h"
#include "FoundryFSDKUILog.h"
#include "FoundryMenuHost.h"
#include "FoundryPartyFollowComponent.h"
#include "FoundryTooltip.h"
#include "FoundryUISettings.h"
#include "Framework/Application/IMenu.h"
#include "GameFramework/PlayerController.h"
#include "Layout/Children.h"
#include "Widgets/Input/SComboBox.h"

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
	ApplyKitTooltips(this);
}

void UFoundryUIWidgetBase::UseHandCursor(UUserWidget* Widget)
{
	if (!Widget || !Widget->WidgetTree)
	{
		return;
	}
	Widget->WidgetTree->ForEachWidget([](UWidget* W)
	{
		if (W->IsA<UButton>() || W->IsA<UComboBoxString>() || W->IsA<UCheckBox>() || W->IsA<USlider>())
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

	// Apply THIS frame's value synchronously - without this, TweenTarget shows whatever it was
	// last set to (often the opposite resting state, e.g. a freshly-constructed widget's default
	// opacity 1 / translation 0) for one full frame before NativeTick's first advance corrects it,
	// which flashes on any transition that starts from TweenProgress's resting value rather than
	// mid-flight (every FIRST open/close a widget ever plays).
	if (TweenTarget)
	{
		const float Eased = EaseOutCubic(TweenProgress);
		TweenTarget->SetRenderOpacity(Eased);
		if (TweenOffset != 0.0f)
		{
			FWidgetTransform T = TweenTarget->GetRenderTransform();
			T.Translation.X = FMath::Lerp(TweenOffset, 0.0f, Eased);
			TweenTarget->SetRenderTransform(T);
		}
	}
}

void UFoundryUIWidgetBase::TweenAngle(UWidget* Target, float FromDeg, float ToDeg, float Seconds)
{
	if (!Target)
	{
		return;
	}
	for (FFoundryAngleTween& Tween : AngleTweens)
	{
		if (Tween.Widget.Get() == Target)
		{
			Tween.FromDeg = FromDeg;
			Tween.ToDeg = ToDeg;
			Tween.Duration = FMath::Max(0.01f, Seconds);
			Tween.Elapsed = 0.0f;
			return;
		}
	}
	FFoundryAngleTween NewTween;
	NewTween.Widget = Target;
	NewTween.FromDeg = FromDeg;
	NewTween.ToDeg = ToDeg;
	NewTween.Duration = FMath::Max(0.01f, Seconds);
	AngleTweens.Add(MoveTemp(NewTween));
}

void UFoundryUIWidgetBase::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// Independent of bTweenActive below - zero, one or several angle tweens can be running
	// regardless of whether PlayOrTween's own opacity/translation tween is active.
	for (int32 i = AngleTweens.Num() - 1; i >= 0; --i)
	{
		FFoundryAngleTween& Tween = AngleTweens[i];
		UWidget* AngleWidget = Tween.Widget.Get();
		if (!AngleWidget)
		{
			AngleTweens.RemoveAtSwap(i);
			continue;
		}
		Tween.Elapsed += InDeltaTime;
		const float AngleT = FMath::Clamp(Tween.Elapsed / Tween.Duration, 0.0f, 1.0f);
		AngleWidget->SetRenderTransformAngle(FMath::Lerp(Tween.FromDeg, Tween.ToDeg, EaseOutCubic(AngleT)));
		if (AngleT >= 1.0f)
		{
			AngleTweens.RemoveAtSwap(i);
		}
	}

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

int32 UFoundryUIWidgetBase::HideEngineMenuChrome(const TSharedPtr<IMenu>& Menu)
{
	if (!Menu.IsValid())
	{
		return 0;
	}
	// Walk wrapper -> (single-child containers) -> the SOverlay that holds the chrome images.
	TSharedPtr<SWidget> Node = Menu->GetContent();
	const FName OverlayType(TEXT("SOverlay"));
	const FName ImageType(TEXT("SImage"));
	for (int32 Depth = 0; Node.IsValid() && Node->GetType() != OverlayType && Depth < 4; ++Depth)
	{
		FChildren* Children = Node->GetChildren();
		if (Children == nullptr || Children->Num() != 1)
		{
			return 0; // not the SMenuContentWrapper -> SBox -> SOverlay shape; leave it alone
		}
		Node = Children->GetChildAt(0);
	}
	if (!Node.IsValid() || Node->GetType() != OverlayType)
	{
		return 0;
	}
	FChildren* Slots = Node->GetChildren();
	if (Slots == nullptr)
	{
		return 0;
	}
	// Slots 0 and 1 are the engine's background + outline images; slot 2 (the SBorder) is ours.
	int32 Hidden = 0;
	for (int32 Index = 0; Index < Slots->Num() && Index < 2; ++Index)
	{
		TSharedRef<SWidget> Child = Slots->GetChildAt(Index);
		if (Child->GetType() == ImageType)
		{
			Child->SetVisibility(EVisibility::Collapsed);
			++Hidden;
		}
	}
	return Hidden;
}

void UFoundryUIWidgetBase::ScrubComboChrome(UComboBoxString* Combo)
{
	if (Combo == nullptr)
	{
		return;
	}
	// TakeWidget() is idempotent (returns the existing Slate widget on a later call) - safe to call
	// from NativeConstruct every time a host re-opens this menu, same as GooLobbyChatPanel does.
	TSharedRef<SWidget> Widget = Combo->TakeWidget();
	StaticCastSharedRef<SComboBox<TSharedPtr<FString>>>(Widget)->bShowMenuBackground = false;
}

void UFoundryUIWidgetBase::ApplyKitTooltips(UUserWidget* Widget)
{
	if (Widget == nullptr || Widget->WidgetTree == nullptr)
	{
		return;
	}

	// TSubclassOf<UUserWidget> -> TSubclassOf<UFoundryTooltip> via the raw UClass* (same pattern
	// UFoundrySettingsMenu::RebuildKeybindRows uses for DefaultKeybindRow) - LoadWidgetClass's own
	// return type can't name UFoundryTooltip (FoundryUISettings.h doesn't know this class exists).
	const TSubclassOf<UFoundryTooltip> TooltipClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultTooltip, TEXT("DefaultTooltip")).Get();
	if (!TooltipClass)
	{
		// LoadWidgetClass already logged the specific reason; this runs from every kit widget's
		// NativeOnInitialized, so warn about the CONSEQUENCE only once per process rather than once
		// per widget instance.
		static bool bWarnedMissingTooltipClass = false;
		if (!bWarnedMissingTooltipClass)
		{
			bWarnedMissingTooltipClass = true;
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] no DefaultTooltip class - tooltip text falls back to the engine's own tooltip style"));
		}
		return;
	}

	Widget->WidgetTree->ForEachWidget([Widget, TooltipClass](UWidget* W)
	{
		if (W == nullptr || W->GetToolTipText().IsEmpty() || W->GetToolTip() != nullptr)
		{
			return;
		}
		UFoundryTooltip* Tooltip = CreateWidget<UFoundryTooltip>(Widget->GetOwningPlayer(), TooltipClass);
		if (Tooltip == nullptr)
		{
			return;
		}
		Tooltip->SetLabel(W->GetToolTipText());
		W->SetToolTip(Tooltip);
	});
}

void UFoundryUIWidgetBase::SetKitToolTip(UWidget* Target, const FText& Text)
{
	if (Target == nullptr)
	{
		return;
	}

	if (Text.IsEmpty())
	{
		Target->SetToolTip(nullptr);
		Target->SetToolTipText(FText::GetEmpty());
		return;
	}

	// SetToolTipText first (keeps GetToolTipText() - and therefore ApplyKitTooltips' "already has
	// text" check - in step), then SetToolTip LAST so the custom widget wins: both ultimately write
	// the same Slate tooltip metadata slot, and whichever is set last is what actually shows.
	Target->SetToolTipText(Text);

	if (UFoundryTooltip* Existing = Cast<UFoundryTooltip>(Target->GetToolTip()))
	{
		// Relabel IN PLACE. Calling SetToolTip(Existing) again would SNew a fresh SToolTip around a
		// re-taken widget - the pill is destructed and constructed on every refresh (seen as
		// DESTRUCT/CONSTRUCT pairs on the app bar's social badge each poll, 2026-10-13).
		Existing->SetLabel(Text);
		return;
	}

	const TSubclassOf<UUserWidget> Loaded = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultTooltip, TEXT("DefaultTooltip"));
	const TSubclassOf<UFoundryTooltip> TooltipClass = Loaded.Get();
	if (!TooltipClass)
	{
		// A TSubclassOf<UFoundryTooltip> assignment silently nulls a class that is not a UFoundryTooltip -
		// say which case this is, once per class, instead of quietly showing the engine's own tooltip.
		static TSet<FName> Warned;
		const FName Key = Loaded ? Loaded->GetFName() : NAME_None;
		if (!Warned.Contains(Key))
		{
			Warned.Add(Key);
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[Tooltip] DefaultTooltip %s - falling back to the engine text tooltip for '%s'."),
				Loaded ? *FString::Printf(TEXT("'%s' is not a UFoundryTooltip"), *Loaded->GetName()) : TEXT("is unset or failed to load"),
				*Text.ToString());
		}
		return; // falls back to the plain SetToolTipText above - the text is never silently lost
	}
	UFoundryTooltip* Tooltip = CreateWidget<UFoundryTooltip>(Target->GetOwningPlayer(), TooltipClass);
	if (Tooltip == nullptr)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[Tooltip] CreateWidget(%s) returned null for '%s' (owning player %s) - engine text tooltip shown."),
			*TooltipClass->GetName(), *Text.ToString(), Target->GetOwningPlayer() ? TEXT("set") : TEXT("NULL"));
		return;
	}
	UE_LOG(LogFoundryFSDKUI, Verbose, TEXT("[Tooltip] kit pill '%s' (%s) on %s"), *Text.ToString(), *Tooltip->GetName(), *Target->GetName());
	Tooltip->SetLabel(Text);
	Target->SetToolTip(Tooltip);
}

FReply UFoundryUIWidgetBase::NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent)
{
	if (bRetakeFocusOnClick && !HasKeyboardFocus() && !HasFocusedDescendants())
	{
		SetKeyboardFocus();
	}
	return Super::NativeOnMouseButtonDown(InGeometry, InMouseEvent);
}

UObject* UFoundryUIWidgetBase::FindMenuHost() const
{
	APlayerController* PC = GetOwningPlayer();
	if (PC && PC->Implements<UFoundryMenuHost>())
	{
		return PC;
	}

	// Once per widget CLASS (never per instance - a pause menu and its confirm dialog each ask
	// this every time they open), so a game with no host implementation doesn't spam the log.
	static TSet<FName> WarnedClasses;
	const FName ClassName = GetClass()->GetFName();
	if (!WarnedClasses.Contains(ClassName))
	{
		WarnedClasses.Add(ClassName);
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s found no IFoundryMenuHost (owning controller %s) - falling back to AddToViewport/RemoveFromParent"),
			*ClassName.ToString(), PC ? *GetNameSafe(PC->GetClass()) : TEXT("null"));
	}
	return nullptr;
}

void UFoundryUIWidgetBase::NotifyPartyOfLeave() const
{
	UFoundryPartyFollowComponent::NotifyLocalLeavingFor(GetOwningPlayer());
}

bool UFoundryUIWidgetBase::LeaveMatchViaHost() const
{
	UObject* Host = FindMenuHost();
	if (Host == nullptr)
	{
		return false;
	}
	NotifyPartyOfLeave();
	IFoundryMenuHost::Execute_RequestLeaveMatch(Host);
	return true;
}

UFoundryConfirmDialog* UFoundryUIWidgetBase::OpenConfirmDialog(const FText& Title, const FText& Body, const FText& ConfirmLabel,
	const FText& CancelLabel, float CooldownSeconds, TSubclassOf<UFoundryConfirmDialog> ClassOverride)
{
	TSubclassOf<UFoundryConfirmDialog> DialogClass = ClassOverride;
	if (!DialogClass)
	{
		DialogClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultConfirmDialog, TEXT("DefaultConfirmDialog")).Get();
	}
	if (!DialogClass)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s has no confirm dialog class (pass one, or set UFoundryUISettings::DefaultConfirmDialog) - refusing the destructive action rather than skipping confirmation."), *GetClass()->GetName());
		return nullptr;
	}

	UFoundryConfirmDialog* Dialog = CreateWidget<UFoundryConfirmDialog>(GetOwningPlayer(), DialogClass);
	if (Dialog == nullptr)
	{
		return nullptr;
	}
	Dialog->Setup(Title, Body, ConfirmLabel, CancelLabel, CooldownSeconds);

	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestPushMenu(Host, Dialog);
	}
	else
	{
		Dialog->AddToViewport(30);
	}
	return Dialog;
}
