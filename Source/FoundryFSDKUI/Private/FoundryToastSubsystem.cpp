#include "FoundryToastSubsystem.h"

#include "Blueprint/GameViewportSubsystem.h"
#include "Blueprint/UserWidget.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FoundryFSDKUILog.h"
#include "FoundryUISettings.h"
#include "GameFramework/PlayerController.h"

void UFoundryToastSubsystem::ShowToast(const UObject* WorldContextObject, const FText& Message, EFoundryToastKind Kind)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	if (UFoundryToastSubsystem* Toasts = GI ? GI->GetSubsystem<UFoundryToastSubsystem>() : nullptr)
	{
		Toasts->Show(Message, Kind);
	}
}

void UFoundryToastSubsystem::Show(const FText& Message, EFoundryToastKind Kind)
{
	if (Message.IsEmpty())
	{
		return;
	}
	const UFoundryUISettings* Settings = UFoundryUISettings::Get();
	const float HoldSeconds = Kind == EFoundryToastKind::Error ? Settings->ErrorToastSeconds : Settings->ToastSeconds;

	// A map travel removes viewport widgets without telling us - drop the dead entries first.
	Active.RemoveAll([](const TObjectPtr<UFoundryToast>& T) { return T == nullptr || !T->IsInViewport(); });

	// The same message (and kind) still up: restart it rather than stacking a copy.
	for (UFoundryToast* Existing : Active)
	{
		if (Existing && Existing->GetKind() == Kind && Existing->GetMessage() == Message.ToString())
		{
			Existing->Restart(HoldSeconds);
			return;
		}
	}

	const TSubclassOf<UFoundryToast> ToastClass = UFoundryUISettings::LoadWidgetClass(Settings->DefaultToast, TEXT("DefaultToast")).Get();
	UGameInstance* GI = GetGameInstance();
	APlayerController* PC = GI ? GI->GetFirstLocalPlayerController() : nullptr;
	if (!ToastClass || PC == nullptr)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[Toast] no DefaultToast class or no local player - '%s' not shown"), *Message.ToString());
		}
		return;
	}
	UFoundryToast* Toast = CreateWidget<UFoundryToast>(PC, ToastClass);
	if (Toast == nullptr)
	{
		return;
	}
	Toast->Setup(Message, Kind, HoldSeconds);
	Toast->OnToastFinished.AddUObject(this, &UFoundryToastSubsystem::HandleToastFinished);
	Toast->AddToViewport(5000); // above menus (20+) and the boot splash (1000); placed in Relayout
	Active.Add(Toast);

	const int32 MaxVisible = FMath::Max(1, Settings->ToastMaxVisible);
	int32 Live = 0;
	for (int32 i = Active.Num() - 1; i >= 0; --i)
	{
		if (Active[i] && ++Live > MaxVisible)
		{
			Active[i]->Dismiss();
		}
	}
	Relayout();
}

void UFoundryToastSubsystem::HandleToastFinished(UFoundryToast* Toast)
{
	Active.Remove(Toast);
	Relayout();
}

void UFoundryToastSubsystem::Relayout()
{
	const UFoundryUISettings* Settings = UFoundryUISettings::Get();
	Active.RemoveAll([](const TObjectPtr<UFoundryToast>& T) { return T == nullptr || !T->IsInViewport(); });
	// One slot write per toast: SetPositionInViewport would reset the anchors to (0,0) and park the
	// pill half off the left edge. Offsets are slate units, so the stack scales with DPI.
	float Y = Settings->ToastTopOffset;
	for (UFoundryToast* Toast : Active)
	{
		UGameViewportSubsystem* Viewport = UGameViewportSubsystem::Get(Toast->GetWorld());
		if (Viewport == nullptr)
		{
			continue;
		}
		FGameViewportWidgetSlot Slot = Viewport->GetWidgetSlot(Toast);
		Slot.Anchors = FAnchors(0.5f, 0.0f);
		Slot.Alignment = FVector2D(0.5, 0.0);
		Slot.Offsets = FMargin(0.0f, Y, 0.0f, 0.0f); // zero size on a point anchor = auto-size
		Viewport->SetWidgetSlot(Toast, Slot);
		Y += Settings->ToastSpacing;
	}
}
