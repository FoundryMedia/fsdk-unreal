#include "FoundryGameUserSettings.h"

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FoundryConsoleSubsystem.h"
#include "FoundryFSDKUILog.h"
#include "FoundryUISettings.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"

namespace
{
	/**
	 * Loudness is perceived roughly logarithmically, so a slider that sets raw amplitude crowds every
	 * useful level into its bottom 20 %. The kit maps slider -> gain as slider^k with k chosen so 50 % on
	 * the slider = 0.1 amplitude (-20 dB) - where a player actually expects "half" (Justin, Goo Crew
	 * lobby theme, 2026-10-24: 10 % linear sounded like the 50 % position). k = ln(0.1) / ln(0.5).
	 * Saved values stay SLIDER positions; only the applied gain follows the curve.
	 */
	constexpr float VolumeCurveExponent = 3.321928f;

	float SliderToGain(float Slider)
	{
		return FMath::Pow(FMath::Clamp(Slider, 0.0f, 1.0f), VolumeCurveExponent);
	}
}

void UFoundryGameUserSettings::SetMasterVolume(float Volume)
{
	MasterVolume = FMath::Clamp(Volume, 0.0f, 1.0f);
}

void UFoundryGameUserSettings::SetMusicVolume(float Volume)
{
	MusicVolume = FMath::Clamp(Volume, 0.0f, 1.0f);
}

void UFoundryGameUserSettings::SetSFXVolume(float Volume)
{
	SFXVolume = FMath::Clamp(Volume, 0.0f, 1.0f);
}

void UFoundryGameUserSettings::SetUIScale(float Scale)
{
	UIScale = FMath::Clamp(Scale, 0.75f, 1.5f);
	OnInterfaceSettingsChanged.Broadcast();
}

void UFoundryGameUserSettings::SetReducedMotion(bool bReduced)
{
	bReducedMotion = bReduced;
	OnInterfaceSettingsChanged.Broadcast();
}

void UFoundryGameUserSettings::SetDeveloperModeEnabled(bool bEnabled)
{
	bDeveloperModeEnabled = bEnabled;
}

void UFoundryGameUserSettings::SetConsoleKeyName(const FString& KeyName)
{
	if (FKey(*KeyName).IsValid())
	{
		ConsoleKey = KeyName;
	}
}

void UFoundryGameUserSettings::SetDebugOverlayKeyName(const FString& KeyName)
{
	if (FKey(*KeyName).IsValid())
	{
		DebugOverlayKey = KeyName;
	}
}

void UFoundryGameUserSettings::ApplyDeveloperMode(UGameInstance* GameInstance) const
{
	UFoundryConsoleSubsystem* Console = GameInstance ? GameInstance->GetSubsystem<UFoundryConsoleSubsystem>() : nullptr;
	if (Console == nullptr)
	{
		return;
	}

	// One switch in every configuration: the checkbox (or -DevMode / -DebugOverlay). A Development build no
	// longer gets the console for free, so what a tester sees is what a player with the box ticked sees.
	const bool bCommandLine = FParse::Param(FCommandLine::Get(), TEXT("DevMode"))
		|| FParse::Param(FCommandLine::Get(), TEXT("DebugOverlay"));
	Console->SetEnabled(bDeveloperModeEnabled || bCommandLine);

	// Invalid key names are ignored by the console (the previous key stays).
	Console->SetConsoleKey(FKey(*ConsoleKey));
	Console->SetOverlayKey(FKey(*DebugOverlayKey));

	// Engine commands are NOT part of developer mode: only -EngineConsole, only outside Shipping.
#if !UE_BUILD_SHIPPING
	Console->SetEngineExecEnabled(FParse::Param(FCommandLine::Get(), TEXT("EngineConsole")));
#endif
}

void UFoundryGameUserSettings::ApplyAudio(UObject* WorldContextObject)
{
	UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	if (World == nullptr)
	{
		return;
	}

	const UFoundryUISettings* Settings = UFoundryUISettings::Get();
	USoundMix* Mix = Settings ? Settings->SoundMix.LoadSynchronous() : nullptr;
	if (Mix == nullptr)
	{
		return; // unset - a silent no-op; the volumes still persist
	}

	// Prune worlds that no longer exist (a PIE session that ended, a level that was unloaded) so this
	// set doesn't grow across repeated PIE play/stop cycles - two PIE worlds alternating would
	// otherwise push the mix every single ApplyAudio call under a single TWeakObjectPtr, since each
	// PIE session is a different UWorld.
	for (auto It = AudioMixPushedWorlds.CreateIterator(); It; ++It)
	{
		if (!It->IsValid())
		{
			It.RemoveCurrent();
		}
	}
	if (!AudioMixPushedWorlds.Contains(World))
	{
		UGameplayStatics::PushSoundMixModifier(World, Mix);
		AudioMixPushedWorlds.Add(World);
	}
	if (USoundClass* MasterClass = Settings->MasterSoundClass.LoadSynchronous())
	{
		UGameplayStatics::SetSoundMixClassOverride(World, Mix, MasterClass, SliderToGain(MasterVolume), 1.0f, 0.1f, true);
	}
	if (USoundClass* MusicClass = Settings->MusicSoundClass.LoadSynchronous())
	{
		UGameplayStatics::SetSoundMixClassOverride(World, Mix, MusicClass, SliderToGain(MusicVolume), 1.0f, 0.1f, false);
	}
	if (USoundClass* SFXClass = Settings->SFXSoundClass.LoadSynchronous())
	{
		UGameplayStatics::SetSoundMixClassOverride(World, Mix, SFXClass, SliderToGain(SFXVolume), 1.0f, 0.1f, false);
	}
}

void UFoundryGameUserSettings::ApplyNonResolutionSettings()
{
	Super::ApplyNonResolutionSettings();
	if (GEngine == nullptr)
	{
		return;
	}
	for (const FWorldContext& Context : GEngine->GetWorldContexts())
	{
		if (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE)
		{
			if (UWorld* World = Context.World())
			{
				ApplyAudio(World);
			}
		}
	}
}

void UFoundryGameUserSettings::SetToDefaults()
{
	Super::SetToDefaults();
	MasterVolume = 1.0f;
	MusicVolume = 1.0f;
	SFXVolume = 1.0f;
	UIScale = 1.0f;
	bReducedMotion = false;
	bDeveloperModeEnabled = false;
	ConsoleKey = TEXT("Tilde");
	DebugOverlayKey = TEXT("F9");
}

UFoundryGameUserSettings* UFoundryGameUserSettings::Get()
{
	UFoundryGameUserSettings* Settings = GEngine ? Cast<UFoundryGameUserSettings>(GEngine->GetGameUserSettings()) : nullptr;
	if (Settings == nullptr)
	{
		static bool bWarned = false;
		if (!bWarned)
		{
			bWarned = true;
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] GEngine->GetGameUserSettings() is not a UFoundryGameUserSettings - set [/Script/Engine.Engine] GameUserSettingsClassName to a UFoundryGameUserSettings subclass"));
		}
	}
	return Settings;
}
