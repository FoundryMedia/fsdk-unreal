// Copyright Foundry Media. FoundryFSDK default UI kit: plays the boot splash once per GameInstance.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Engine/EngineTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "TimerManager.h"
#include "FoundryBootSplashSubsystem.generated.h"

class APlayerController;
class UFoundryBootSplash;

/**
 * Owns the once-per-process boot splash. The "played" flag lives on the GameInstance (not a static),
 * so it survives a match -> lobby travel but is fresh for every PIE session. The host's menu
 * controller calls TryPlay and, on false, continues straight to its menu - a missing splash asset
 * NEVER strands the player behind a black screen.
 */
UCLASS()
class FOUNDRYFSDKUI_API UFoundryBootSplashSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Shows the configured boot splash ONCE per GameInstance. Returns false (and the caller continues straight to its menu)
	 *  when: UFoundryUISettings::bShowBootSplash is false; already played; the world is a dedicated server or this is a
	 *  commandlet; a non-Shipping build has -NoBootSplash on the command line; DefaultBootSplash is unset / fails to load /
	 *  is not a UFoundryBootSplash; CreateWidget returns null. Every false path that is not "played already" or "switched off"
	 *  logs a WARN naming the consequence ("continuing to the menu without a boot splash"). OnDone runs (once) after the splash
	 *  has finished and been removed; it is NOT run when TryPlay returns false. */
	bool TryPlay(APlayerController* PC, FSimpleDelegate OnDone);

	/** As above, plus OnFadeOut: runs once when the splash starts fading out (bring the scene and music in
	 *  under it - the crossfade). Also run before OnDone if the splash is force-finished without fading. */
	bool TryPlay(APlayerController* PC, FSimpleDelegate OnDone, FSimpleDelegate OnFadeOut);

	/** True once a splash has been shown (or is showing) on this GameInstance. */
	bool HasPlayed() const;

	/** True while the splash is on screen. */
	bool IsActive() const;

	/** Test hook (fixtures, headless shots): the hold length the NEXT TryPlay uses instead of the setting;
	 *  negative = off. Never read from config - a shot passes it, a player never sees it. */
	void SetHoldOverrideSeconds(float Seconds);

	virtual void Deinitialize() override;

private:
	UFUNCTION()
	void HandleSplashFinished();

	/** The failsafe timer's target: the sequence should have ended by now, so end it. */
	void HandleFailsafe();

	bool bPlayed = false;
	float HoldOverrideSeconds = -1.0f;
	FSimpleDelegate PendingDone;
	FSimpleDelegate PendingFadeOut;

	UFUNCTION()
	void HandleSplashFadeOutStarted();
	FTimerHandle FailsafeTimer;

	UPROPERTY(Transient)
	TObjectPtr<UFoundryBootSplash> Splash;
};
