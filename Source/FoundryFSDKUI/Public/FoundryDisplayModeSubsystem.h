// Copyright Foundry Media. FoundryFSDK default UI kit: keep-or-revert for display mode changes.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "GenericPlatform/GenericWindow.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FoundryDisplayModeSubsystem.generated.h"

class UFoundryConfirmDialog;

/**
 * Owns a pending display change (window mode / resolution): applies it at once, remembers the mode it
 * replaced, and reverts to THAT snapshot unless Keep() runs before the deadline. The deadline runs on the
 * core ticker (real time - it keeps counting while the window is minimized, which is exactly when a broken
 * fullscreen mode cannot be answered) and survives the prompt widget being removed by anything (a menu
 * stack clear, a map travel, gamepad Start). The engine's own LastConfirmed* fields are NOT trusted:
 * UGameEngine confirms the video mode on every Windowed viewport resize, so RevertVideoMode would be a
 * no-op after switching into Windowed. A pending change is also reverted at shutdown.
 */
UCLASS()
class FOUNDRYFSDKUI_API UFoundryDisplayModeSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Applies Mode + Resolution now. The first call of a pending change snapshots the current mode; every
	 *  call (re)arms the deadline. */
	void ApplyPending(EWindowMode::Type Mode, FIntPoint Resolution, float RevertSeconds);

	/** The prompt showing the countdown (weak): closed for the player when the deadline reverts. */
	void SetPrompt(UFoundryConfirmDialog* Dialog);
	/** The prompt if it is still on screen and unanswered, else null. */
	UFoundryConfirmDialog* GetLivePrompt() const;

	UFUNCTION() void Keep();
	UFUNCTION() void Revert();

	bool IsPending() const { return bPending; }

	/** After a keep or a revert (the settings menu repopulates its combos). */
	FSimpleMulticastDelegate OnResolved;

	virtual void Deinitialize() override;

private:
	bool Tick(float DeltaTime);
	void Resolve(bool bKeep);

	bool bPending = false;
	EWindowMode::Type OldMode = EWindowMode::Windowed;
	FIntPoint OldResolution = FIntPoint::ZeroValue;
	double DeadlineSeconds = 0.0;
	FTSTicker::FDelegateHandle TickHandle;
	TWeakObjectPtr<UFoundryConfirmDialog> Prompt;
};
