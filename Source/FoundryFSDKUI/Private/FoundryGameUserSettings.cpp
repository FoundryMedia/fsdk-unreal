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
#include "Engine/GameViewportClient.h"
#include "GenericPlatform/GenericWindow.h"
#include "Misc/App.h"
#include "Widgets/SWindow.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	/**
	 * Loudness is perceived roughly logarithmically, so the slider maps LINEARLY IN DECIBELS: 100 % = 0 dB,
	 * every step down the same audible amount, VolumeRangeDb below full at the bottom (50 % = -18 dB). The
	 * last TaperBelow of travel ramps linearly to true silence at 0, so 0 is off with no jump. (Goo Crew,
	 * 2026-10-24: raw amplitude crowded every useful level into the bottom 20 %; slider^3.32 then made
	 * 10 % inaudible. dB-linear keeps 10 % quiet but present.) Saved values stay SLIDER positions.
	 */
	constexpr float VolumeRangeDb = 36.0f;
	constexpr float TaperBelow = 0.05f;

	float SliderToGain(float Slider)
	{
		const float S = FMath::Clamp(Slider, 0.0f, 1.0f);
		if (S <= 0.0f)
		{
			return 0.0f;
		}
		const float Gain = FMath::Pow(10.0f, -VolumeRangeDb * (1.0f - S) / 20.0f);
		return S < TaperBelow ? Gain * (S / TaperBelow) : Gain;
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
	// Process-wide, before any early return: the engine scales every sound by this while the window is unfocused
	// (BaseEngine.ini [Audio] UnfocusedVolumeMultiplier is 0 = silent when alt-tabbed).
	FApp::SetUnfocusedVolumeMultiplier(bPlayAudioInBackground ? 1.0f : 0.0f);

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
	bPlayAudioInBackground = true;
	LobbyFrameRateLimit = 60.0f;
	MatchFrameRateLimit = -1.0f;
	ConsoleKey = TEXT("Tilde");
	DebugOverlayKey = TEXT("F9");
}

int32 UFoundryGameUserSettings::GetMonitorRefreshRate()
{
#if PLATFORM_WINDOWS
	HWND Hwnd = nullptr;
	if (GEngine && GEngine->GameViewport)
	{
		const TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow();
		if (Window.IsValid() && Window->GetNativeWindow().IsValid())
		{
			Hwnd = static_cast<HWND>(Window->GetNativeWindow()->GetOSWindowHandle());
		}
	}
	HMONITOR Monitor = Hwnd ? MonitorFromWindow(Hwnd, MONITOR_DEFAULTTOPRIMARY) : MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
	MONITORINFOEXW Info;
	Info.cbSize = sizeof(Info);
	if (Monitor && GetMonitorInfoW(Monitor, &Info))
	{
		DEVMODEW Mode = {};
		Mode.dmSize = sizeof(Mode);
		// 0 and 1 mean "the hardware default" - unknown.
		if (EnumDisplaySettingsW(Info.szDevice, ENUM_CURRENT_SETTINGS, &Mode) && Mode.dmDisplayFrequency > 1)
		{
			return static_cast<int32>(Mode.dmDisplayFrequency);
		}
	}
#endif
	return 0;
}

float UFoundryGameUserSettings::ResolveFrameRateLimit(EFoundryPauseContext Context) const
{
	if (Context == EFoundryPauseContext::Lobby)
	{
		return FMath::Max(0.0f, LobbyFrameRateLimit);
	}
	if (MatchFrameRateLimit >= 0.0f)
	{
		return MatchFrameRateLimit;
	}
	const int32 Hz = GetMonitorRefreshRate();
	return Hz > 0 ? static_cast<float>(Hz) : 144.0f;
}

void UFoundryGameUserSettings::ApplyFrameRateForContext(EFoundryPauseContext Context)
{
	const float Limit = ResolveFrameRateLimit(Context);
	SetFrameRateLimit(Limit); // the engine's own field, so its ApplySettings path keeps this cap
	if (GEngine)
	{
		GEngine->SetMaxFPS(Limit);
	}
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[Settings] frame rate cap %s for the %s"),
		Limit > 0.0f ? *FString::Printf(TEXT("%.0f"), Limit) : TEXT("unlimited"),
		Context == EFoundryPauseContext::Lobby ? TEXT("lobby") : TEXT("match"));
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
