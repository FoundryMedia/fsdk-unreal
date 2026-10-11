// Copyright Foundry Media. FoundryFSDK default UI kit: the pause menu.

#pragma once

#include "CoreMinimal.h"
#include "FoundryMenuHost.h" // EFoundryPauseContext
#include "FoundryUIWidgetBase.h"
#include "FoundryPauseMenu.generated.h"

class UButton;
class UFoundryConfirmDialog;
class UTextBlock;
class UWidget;
class UWidgetAnimation;

/**
 * The default PAUSE menu: relabels itself for Lobby ("MENU" / QUIT TO DESKTOP / CANCEL) or Match
 * ("PAUSED" / LEAVE MATCH / RESUME) via the host's IFoundryMenuHost::GetPauseContext, or an explicit
 * SetContext call made before Construct. The destructive action (quit to desktop / leave match)
 * always confirms first through a UFoundryConfirmDialog - LEAVE MATCH additionally cooldowns the
 * confirm button for LeaveMatchCooldownSeconds (a fat-finger guard for a mid-match click); QUIT TO
 * DESKTOP has none. Never adds/removes itself from the viewport - see
 * UFoundryUIWidgetBase::FindMenuHost.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryPauseMenu : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Turns on bRetakeFocusOnClick (UFoundryUIWidgetBase) - a click on the backdrop still lets
	 *  Escape close this menu afterward. */
	UFoundryPauseMenu();

	DECLARE_DYNAMIC_MULTICAST_DELEGATE(FFoundryPauseMenuEvent);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FFoundryPauseMenuPrimaryEvent, EFoundryPauseContext, InContext);

	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryPauseMenuEvent OnSettingsRequested;

	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryPauseMenuEvent OnResumeRequested;

	/** The destructive action's confirm dialog was confirmed (QUIT TO DESKTOP in Lobby, LEAVE MATCH
	 *  in Match) - broadcast right before this menu acts on it. */
	UPROPERTY(BlueprintAssignable, Category = "Foundry UI")
	FFoundryPauseMenuPrimaryEvent OnPrimaryConfirmed;

	/** Null -> UFoundryUISettings::DefaultConfirmDialog. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryConfirmDialog> ConfirmDialogClass;

	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI", meta = (ClampMin = "0"))
	float LeaveMatchCooldownSeconds = 5.0f;

	/** Relabels for InContext; wins over whatever NativeConstruct would otherwise resolve from the
	 *  host, as long as it is called before Construct (e.g. right after CreateWidget, before the
	 *  menu is pushed). */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void SetContext(EFoundryPauseContext InContext);

	UFUNCTION(BlueprintPure, Category = "Foundry UI")
	EFoundryPauseContext GetContext() const { return PauseContext; }

	/** Fired after the labels for Context have been applied (Construct, or an explicit SetContext). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnContextApplied(EFoundryPauseContext InContext);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Backdrop;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Panel;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> SettingsButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SettingsText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> PrimaryButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PrimaryText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ResumeButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ResumeText;

	/** Optional - an animation authored on this WBP's own Sequencer; unbound uses PlayOrTween's code
	 *  tween fallback (fade only, like UFoundryConfirmDialog's DialogAnim). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetAnimOptional))
	TObjectPtr<UWidgetAnimation> MenuAnim;

private:
	UFUNCTION() void HandleSettingsClicked();
	UFUNCTION() void HandleResumeClicked();
	UFUNCTION() void HandlePrimaryClicked();
	UFUNCTION() void HandleConfirmedPrimary();
	UFUNCTION() void HandleConfirmCancelled();

	void ApplyContextLabels();

	// Named PauseContext, not Context: UHT's generated thunks for OnContextApplied / the primary delegate
	// declare a parameter called Context, and a member of the same name trips C4458 (warning-as-error).
	EFoundryPauseContext PauseContext = EFoundryPauseContext::Lobby;
	// An explicit SetContext call before Construct wins over whatever the host would resolve.
	bool bContextExplicit = false;

	// GC-hold while the confirm dialog is open (also doubles as the "a confirm is already up"
	// guard a second Primary click checks against) - mirrors UFoundrySocialDrawer::ActiveMenu.
	UPROPERTY(Transient)
	TObjectPtr<UFoundryConfirmDialog> ActiveConfirm;
};
