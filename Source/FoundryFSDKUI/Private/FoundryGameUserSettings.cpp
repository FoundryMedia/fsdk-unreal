#include "FoundryGameUserSettings.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "FoundryFSDKUILog.h"
#include "FoundryUISettings.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundClass.h"
#include "Sound/SoundMix.h"

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
		UGameplayStatics::SetSoundMixClassOverride(World, Mix, MasterClass, MasterVolume, 1.0f, 0.1f, true);
	}
	if (USoundClass* MusicClass = Settings->MusicSoundClass.LoadSynchronous())
	{
		UGameplayStatics::SetSoundMixClassOverride(World, Mix, MusicClass, MusicVolume, 1.0f, 0.1f, false);
	}
	if (USoundClass* SFXClass = Settings->SFXSoundClass.LoadSynchronous())
	{
		UGameplayStatics::SetSoundMixClassOverride(World, Mix, SFXClass, SFXVolume, 1.0f, 0.1f, false);
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
