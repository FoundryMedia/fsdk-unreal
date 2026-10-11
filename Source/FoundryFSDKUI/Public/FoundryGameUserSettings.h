// Copyright Foundry Media. FoundryFSDK default UI kit: the persistence base.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h"
#include "FoundryGameUserSettings.generated.h"

/**
 * The kit's persistence base: the engine's own video settings (UGameUserSettings, unchanged -
 * resolution/window mode/scalability/vsync/frame cap, all saved to the same GameUserSettings.ini)
 * plus the volumes, UI scale and reduced-motion flag UFoundrySettingsMenu reads and writes. A game
 * subclasses this - set Project Settings -> Engine -> General -> Default Classes ->
 * GameUserSettingsClassName to the subclass - to add its own fields (developer mode, keybind
 * defaults, ...); the kit's settings menu and ApplyAudio always resolve THIS class (Get()), never a
 * game-specific one, so a subclass's extra fields never have to be duplicated here.
 */
UCLASS(Config = GameUserSettings)
class FOUNDRYFSDKUI_API UFoundryGameUserSettings : public UGameUserSettings
{
	GENERATED_BODY()

public:
	UPROPERTY(Config)
	float MasterVolume = 1.0f;

	UPROPERTY(Config)
	float MusicVolume = 1.0f;

	UPROPERTY(Config)
	float SFXVolume = 1.0f;

	/** Multiplier on the UI's on-screen sizes (1 = the reference metrics). Clamped 0.75..1.5. */
	UPROPERTY(Config)
	float UIScale = 1.0f;

	/** Removes hover scaling, join pops, camera drift and decorative motion; short fades and state
	 *  changes stay (mirrors GooCrewUserSettings' original field - see Goo Crew's migration note). */
	UPROPERTY(Config)
	bool bReducedMotion = false;

	UFUNCTION(BlueprintPure, Category = "Foundry UI|Settings")
	float GetMasterVolume() const { return MasterVolume; }

	UFUNCTION(BlueprintPure, Category = "Foundry UI|Settings")
	float GetMusicVolume() const { return MusicVolume; }

	UFUNCTION(BlueprintPure, Category = "Foundry UI|Settings")
	float GetSFXVolume() const { return SFXVolume; }

	UFUNCTION(BlueprintPure, Category = "Foundry UI|Settings")
	float GetUIScale() const { return UIScale; }

	UFUNCTION(BlueprintPure, Category = "Foundry UI|Settings")
	bool IsReducedMotion() const { return bReducedMotion; }

	/** Clamps 0..1. Does not save or re-apply audio on its own - the settings menu/subsystem batches
	 *  that (a slider mid-drag calling SetMasterVolume on every tick must not hit disk every tick). */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI|Settings")
	void SetMasterVolume(float Volume);

	UFUNCTION(BlueprintCallable, Category = "Foundry UI|Settings")
	void SetMusicVolume(float Volume);

	UFUNCTION(BlueprintCallable, Category = "Foundry UI|Settings")
	void SetSFXVolume(float Volume);

	/** Clamps 0.75..1.5. Broadcasts OnInterfaceSettingsChanged. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI|Settings")
	void SetUIScale(float Scale);

	/** Broadcasts OnInterfaceSettingsChanged. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI|Settings")
	void SetReducedMotion(bool bReduced);

	/** Fired whenever UIScale or bReducedMotion changes - the two settings a screen's LAYOUT
	 *  depends on (volumes never trigger this; nothing visual depends on them). */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFoundryInterfaceSettingsChanged);
	UPROPERTY(BlueprintAssignable, Category = "Foundry UI|Settings")
	FFoundryInterfaceSettingsChanged OnInterfaceSettingsChanged;

	/**
	 * (Re)applies the saved volumes to WorldContextObject's world: a sound-mix push (once per world -
	 * tracked, never repeated) via UFoundryUISettings::SoundMix, then a per-class volume override via
	 * MasterSoundClass/MusicSoundClass/SFXSoundClass. Unset refs are a silent no-op - the volumes
	 * still persist, there is just nothing to apply them to (mirrors GooCrewSettingsSubsystem's
	 * original "missing = volume APPLICATION no-ops; get/set/save keep working" posture).
	 */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI|Settings")
	void ApplyAudio(UObject* WorldContextObject);

	/** Super, then ApplyAudio for every live Game/PIE world (GEngine->GetWorldContexts()) - so a
	 *  volume change made through the engine's own ApplySettings path reaches the audio system too. */
	virtual void ApplyNonResolutionSettings() override;

	virtual void SetToDefaults() override;

	/** GEngine->GetGameUserSettings() cast to this class; null (+ one WARN per process) when
	 *  GameUserSettingsClassName is not set to a UFoundryGameUserSettings subclass. */
	static UFoundryGameUserSettings* Get();

private:
	/** Every world ApplyAudio has pushed the sound mix onto - the push is per-world, never repeated.
	 *  A set (not a single pointer) because two PIE worlds can alternate (each is a distinct UWorld);
	 *  stale entries are pruned opportunistically inside ApplyAudio. */
	TSet<TWeakObjectPtr<UWorld>> AudioMixPushedWorlds;
};
