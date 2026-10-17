// Copyright Foundry Media. FoundryFSDK default UI kit: the lobby match cluster.

#pragma once

#include "CoreMinimal.h"
#include "FMMSSubsystem.h" // EFoundryMatchmakingState
#include "FoundryModeRow.h"
#include "FoundryUISettings.h" // FFoundryMatchmakingMode
#include "FoundryUIWidgetBase.h"
#include "FoundryMatchCluster.generated.h"

class UButton;
class UFMMSSubsystem;
class UFoundrySocialSubsystem;
class UPanelWidget;
class UTextBlock;
class UWidget;
class UWidgetAnimation;

/**
 * The default bottom-centre matchmaking cluster: the mode pill (opens ModeMenu, a list of
 * ModeRowClass entries above it), the Find Match button (and, when the WBP has one, a separate
 * Cancel Search button), the status line (spinner + text + Leave match) and "Party N / cap".
 *
 * Reads UFMMSSubsystem for everything about the search (state, message, local elapsed clock,
 * CanRequest / CanCancel) and acts through its RequestFindMatch / RequestCancel / RequestReconnect /
 * DeclineReconnectSeat - the single matchmaking state machine, so every cluster in the game (and a
 * chat that mirrors it) shows the same state. Reads UFoundrySocialSubsystem for sign-in and the
 * party (the party id rides the ticket as the "partyId" attribute; "Party N / cap" reads the
 * roster). The modes come from GetModes (default: UFoundryUISettings::MatchmakingModes).
 *
 * States: Idle / Pending / Searching / WaitingForCapacity / Found / Traveling / Failed /
 * CancelPending / Reconnectable. Code sets the button labels ("FIND MATCH", "CANCEL SEARCH",
 * "JOINING...", "RECONNECT", ...), the status text, what is shown and what is enabled; everything
 * else about the look (hover, press, a searching pulse) belongs in the WBP, driven by
 * OnMatchmakingStateChanged. While a search can still be cancelled the cancel IS offered: with a
 * CancelButton bound, FindMatchButton collapses and CancelButton shows; without one (the older
 * single-button WBPs) FindMatchButton relabels to CANCEL SEARCH. Past that point a busy state keeps
 * the button ENABLED (a disabled UButton dims its content) but hit-test invisible. LeaveMatchButton
 * (Reconnectable only) declines the live seat.
 *
 * The elapsed timer is the subsystem's LOCAL clock and is labelled "Searching for m:ss". The HOST
 * only places the widget; it wires nothing.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryMatchCluster : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Closes the mode menu if it is open; true when it was. */
	bool CloseModeMenu();

	/** The row widget class for the mode menu. Null -> UFoundryUISettings::DefaultModeRow. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryModeRow> ModeRowClass;

	/** When true and no SpinAnim is bound, the code rotates Spinner (360 deg/s) while a busy state shows
	 *  it. A WBP whose own graph animates the spinner sets this false. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	bool bCodeSpinner = true;

	/** The modes the menu offers. Default: UFoundryUISettings::MatchmakingModes; override to build the
	 *  list in code or Blueprint. Re-read on every refresh. */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI")
	TArray<FFoundryMatchmakingMode> GetModes() const;
	virtual TArray<FFoundryMatchmakingMode> GetModes_Implementation() const;

	/** The selected mode's index into GetModes (INDEX_NONE when the list is empty). */
	UFUNCTION(BlueprintPure, Category = "Foundry UI")
	int32 GetSelectedModeIndex() const { return SelectedModeIndex; }

	/** Selects a mode. Refused unless the state is Idle / Failed and the mode is available (has a queue). */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void SelectMode(int32 Index);

	/** Fired whenever the matchmaking state changes (drive animations / styling from it). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnMatchmakingStateChanged(EFoundryMatchmakingState NewState);

	/** Native hook, runs BEFORE the BlueprintImplementableEvent above. A game subclass overrides it
	 *  to mirror the state elsewhere. */
	virtual void NativeOnMatchmakingStateChanged(EFoundryMatchmakingState NewState) {}

	/** Fired after the menu is shown on open and before it hides on close (extra styling; the open /
	 *  close motion itself is ModeMenuAnim, else a code tween). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnModeMenuChanged(bool bOpen);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	/** Hides ModeMenu once the CLOSE transition finishes (bForward=false). */
	virtual void NativeOnTransitionFinished(bool bForward) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ModeButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ModeText;
	/** The popup above the pill; collapsed until ModeButton opens it. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ModeMenu;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> ModeList;
	/** Rotates 0<->180 (TweenAngle) as the menu opens / closes, when ModeMenuAnim is not bound. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ModeChevron;
	/** The WBP's open animation (ModeMenu fade-in + chevron half-turn), any length: played forward on
	 *  open, in reverse on close; the menu hides when the reverse finishes. Unbound -> a code fade. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Foundry UI", meta = (BindWidgetAnimOptional)) TObjectPtr<UWidgetAnimation> ModeMenuAnim;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> FindMatchButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FindMatchText;
	/** Optional. When bound it takes over from FindMatchButton while a search can be cancelled. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CancelButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CancelText;
	/** The row under the button: Spinner + StatusText + LeaveMatchButton. Hidden when empty. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> StatusRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Spinner;
	/** Optional looping spin authored in the WBP; when bound it replaces the code rotation. */
	UPROPERTY(BlueprintReadOnly, Transient, Category = "Foundry UI", meta = (BindWidgetAnimOptional)) TObjectPtr<UWidgetAnimation> SpinAnim;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StatusText;
	/** Decline a live seat (Reconnectable state only). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> LeaveMatchButton;
	/** "Party 3 / 8". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PartyText;

private:
	UFUNCTION() void HandleFindMatch();
	UFUNCTION() void HandleCancel();
	UFUNCTION() void HandleLeaveMatch();
	UFUNCTION() void HandleModeButton();

	/** The two-argument native delegate of UFMMSSubsystem::OnMatchmakingStateChanged. */
	void HandleFmmsState(EFoundryMatchmakingState State, const FString& Message);

	void Refresh();
	/** Tells the social subsystem which queue the picked mode is, so party caps follow the mode. */
	void SyncSelectedQueueKey();
	void RefreshSpinner(bool bShow);
	void SetModeMenuOpen(bool bOpen);
	void RebuildModeRows();
	/** Keeps SelectedModeIndex on a valid mode: first available, else 0, INDEX_NONE for an empty list. */
	void ResolveSelectedMode();

	UFMMSSubsystem* Fmms() const;
	UFoundrySocialSubsystem* Social() const;

	// Lazy lookup caches (the accessors above are const).
	mutable TWeakObjectPtr<UFMMSSubsystem> FmmsWeak;
	mutable TWeakObjectPtr<UFoundrySocialSubsystem> SocialWeak;

	/** GetModes() as of the last refresh. */
	TArray<FFoundryMatchmakingMode> Modes;
	int32 SelectedModeIndex = INDEX_NONE;

	FSlateColor StatusNormalColor;
	bool bStatusColorCaptured = false;
	bool bModeMenuOpen = false;
	bool bHaveLastState = false;
	EFoundryMatchmakingState LastState = EFoundryMatchmakingState::Idle;
	int32 LastElapsedShown = -2;
	bool bSpinnerBusy = false;
	float SpinAngle = 0.0f;
	/** True when the WBP wraps StatusText itself (Wrap Text At / Auto Wrap Text set in the Designer). */
	bool bDesignerWrapsStatus = false;
	/** The FindMatchButton width StatusText was last wrapped at (0 = not yet laid out). */
	float StatusWrapWidth = 0.0f;
};
