#include "FoundryDisplayModeSubsystem.h"

#include "Engine/Engine.h"
#include "FoundryConfirmDialog.h"
#include "FoundryFSDKUILog.h"
#include "FoundryToastSubsystem.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/PlatformTime.h"

void UFoundryDisplayModeSubsystem::ApplyPending(EWindowMode::Type Mode, FIntPoint Resolution, float RevertSeconds)
{
	UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (GUS == nullptr)
	{
		return;
	}
	if (!bPending)
	{
		OldMode = GUS->GetFullscreenMode();
		OldResolution = GUS->GetScreenResolution();
		bPending = true;
	}
	GUS->SetFullscreenMode(Mode);
	GUS->SetScreenResolution(Resolution);
	GUS->ApplyResolutionSettings(false);
	// The prompt's own countdown runs on paint ticks; this is the backstop, half a second later.
	DeadlineSeconds = FPlatformTime::Seconds() + FMath::Max(1.0f, RevertSeconds) + 0.5;
	if (!TickHandle.IsValid())
	{
		TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UFoundryDisplayModeSubsystem::Tick));
	}
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[Display] pending %dx%d mode %d (reverts to %dx%d mode %d)"),
		Resolution.X, Resolution.Y, int32(Mode), OldResolution.X, OldResolution.Y, int32(OldMode));
}

void UFoundryDisplayModeSubsystem::SetPrompt(UFoundryConfirmDialog* Dialog)
{
	Prompt = Dialog;
	if (Dialog)
	{
		Dialog->OnConfirmed.AddDynamic(this, &UFoundryDisplayModeSubsystem::Keep);
		Dialog->OnCancelled.AddDynamic(this, &UFoundryDisplayModeSubsystem::Revert);
	}
}

UFoundryConfirmDialog* UFoundryDisplayModeSubsystem::GetLivePrompt() const
{
	UFoundryConfirmDialog* Dialog = Prompt.Get();
	return Dialog && Dialog->IsInViewport() && !Dialog->IsAnswered() ? Dialog : nullptr;
}

void UFoundryDisplayModeSubsystem::Keep()
{
	Resolve(true);
}

void UFoundryDisplayModeSubsystem::Revert()
{
	Resolve(false);
}

bool UFoundryDisplayModeSubsystem::Tick(float DeltaTime)
{
	if (bPending && FPlatformTime::Seconds() >= DeadlineSeconds)
	{
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[Display] no answer before the deadline - reverting"));
		Resolve(false);
	}
	return true;
}

void UFoundryDisplayModeSubsystem::Resolve(bool bKeep)
{
	if (!bPending)
	{
		return;
	}
	bPending = false;
	if (UFoundryConfirmDialog* Dialog = GetLivePrompt())
	{
		Dialog->CloseWithoutAnswer(); // the deadline answered for the player
	}
	Prompt.Reset();
	if (UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		if (!bKeep)
		{
			GUS->SetFullscreenMode(OldMode);
			GUS->SetScreenResolution(OldResolution);
			GUS->ApplyResolutionSettings(false);
		}
		GUS->ConfirmVideoMode();
		GUS->SaveSettings();
	}
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	UFoundryToastSubsystem::ShowToast(this, FText::FromString(bKeep ? TEXT("Display settings saved") : TEXT("Display settings reverted")),
		bKeep ? EFoundryToastKind::Success : EFoundryToastKind::Info);
	OnResolved.Broadcast();
}

void UFoundryDisplayModeSubsystem::Deinitialize()
{
	if (bPending)
	{
		Resolve(false); // never leave an unconfirmed mode in the saved settings
	}
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	Super::Deinitialize();
}
