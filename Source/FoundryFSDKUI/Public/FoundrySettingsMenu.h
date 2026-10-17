// Copyright Foundry Media. FoundryFSDK default UI kit: the settings menu.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameUserSettings.h" // EWindowMode::Type (FFoundryPendingVideo)
#include "InputCoreTypes.h" // FKey
#include "Types/SlateEnums.h" // ESelectInfo::Type (the combo handlers' second parameter)
#include "UObject/SoftObjectPtr.h" // TSoftObjectPtr<UTexture2D> (Previews)
#include "FoundryUIWidgetBase.h"
#include "FoundrySettingsMenu.generated.h"

class UButton;
class UCheckBox;
class UComboBoxString;
class UFoundryConfirmDialog;
class UFoundryKeybindRow;
class UImage;
class UPanelWidget;
class USlider;
class UTextBlock;
class UTexture2D;
class UWidget;
class UWidgetAnimation;

/**
 * The default SETTINGS menu: a full-screen two-pane layout (left: Video / Audio / Interface /
 * Controls, scrolling; right: a description pane that shows what the hovered or focused row does -
 * Fortnite-style). Video (window mode / resolution / quality / fps cap / vsync) is STAGED - the
 * four combos + VSync write into a pending struct, and ApplyButton commits them all at once through
 * the engine's own UGameUserSettings. Audio + Interface (volumes, UI scale, reduced motion -
 * UFoundryGameUserSettings; the two sections hide themselves when the project's
 * GameUserSettingsClassName isn't a UFoundryGameUserSettings subclass) apply live and save on
 * release. Controls (Enhanced Input rebinding over UFoundryUISettings::RebindableContexts - hidden
 * behind an explanatory note when Enhanced Input user settings aren't enabled, or when no context
 * is configured) applies immediately, like every keybind change. Never adds/removes itself from
 * the viewport - see UFoundryUIWidgetBase::FindMenuHost.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundrySettingsMenu : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Turns on bRetakeFocusOnClick (UFoundryUIWidgetBase) - a click on the backdrop still lets
	 *  Escape close this menu afterward. */
	UFoundrySettingsMenu();

	/** Null -> UFoundryUISettings::DefaultKeybindRow. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryKeybindRow> KeybindRowClass;

	/** Optional preview art for the right-hand description pane, keyed by the hovered/focused row's
	 *  OWN bound name ("WindowModeRow", "ResolutionRow", ... - see the row UPROPERTYs below). An
	 *  absent entry leaves PreviewImage collapsed. Empty by default; a game's own WBP subclass fills
	 *  it in per the rows it cares to illustrate. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TMap<FName, TSoftObjectPtr<UTexture2D>> Previews;

	/** Fired whenever the description pane's selection changes - a different row hovered/focused, a
	 *  keybind row hovered, or back to the default "Settings" pane. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnDescriptionChanged(const FText& Title);

	/** Fired whenever a staged video change becomes pending (true), or Apply/re-populate clears it
	 *  (false). Drives ApplyButton's enabled state on its own; this is for a WBP that wants to
	 *  react too (e.g. a "you have unsaved changes" hint elsewhere on the pane). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnVideoPendingChanged(bool bPending);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	/** Escape, in the TUNNEL phase: a focused combo/slider child would otherwise swallow the bubble-
	 *  phase OnKeyDown before it ever reaches this widget. Skipped while a keybind row is capturing a
	 *  key - that row's own (bubble-phase) OnKeyDown handles Escape as a capture-cancel instead. */
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Backdrop;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Panel;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	/** Top-right X - same behavior as BackButton (discard pending video, pop through the host). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CloseButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ApplyButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> BackButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BackText;
	/** One line at the bottom - "Applied." once the staged video changes commit, or a rebind failure
	 *  message. Fades out on its own after a few seconds (NativeTick). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> NoteText;

	// -- the right-hand description pane --
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DescriptionTitleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> DescriptionBodyText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> PreviewImage;

	// -- Video (the engine's own UGameUserSettings, staged - see FFoundryPendingVideo) --
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> VideoSection;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UComboBoxString> WindowModeCombo;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UComboBoxString> ResolutionCombo;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UComboBoxString> QualityCombo;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UComboBoxString> FpsCapCombo;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UCheckBox> VSyncCheck;

	// -- rows, for the hover/focus-to-describe pass (NativeTick) - each is the row's own
	// HorizontalBox, named to match, so IsHovered()/HasFocusedDescendants() reads the whole row --
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> WindowModeRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ResolutionRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> QualityRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> FpsCapRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> VSyncRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> MasterRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> MusicRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> SFXRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> UIScaleRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ReducedMotionRow;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> DeveloperModeRow;
	/** The Controls SECTION HEADER's row (not an individual keybind row - those are hover-tested
	 *  separately, by walking KeybindList's own children). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ControlsRow;

	// -- Audio (UFoundryGameUserSettings) --
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> AudioSection;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<USlider> MasterSlider;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> MasterValueText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<USlider> MusicSlider;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> MusicValueText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<USlider> SFXSlider;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SFXValueText;

	// -- Interface (UFoundryGameUserSettings) --
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> InterfaceSection;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<USlider> UIScaleSlider;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> UIScaleValueText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UCheckBox> ReducedMotionCheck;
	/** Ticking it lets the console key (Tilde) open the Foundry console - Foundry and game commands only. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UCheckBox> DeveloperModeCheck;

	// -- Controls (Enhanced Input) --
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ControlsSection;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> KeybindList;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ControlsNoteText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ResetAllKeysButton;

	/** Optional - an animation authored on this WBP's own Sequencer; unbound uses PlayOrTween's code
	 *  tween fallback (fade only), like the pause menu's MenuAnim / the confirm dialog's DialogAnim. */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetAnimOptional))
	TObjectPtr<UWidgetAnimation> MenuAnim;

private:
	/** The four video combos + VSync write here instead of the live UGameUserSettings - ApplyButton
	 *  is what actually commits them (SetFullscreenMode/SetScreenResolution/SetOverallScalability
	 *  Level/SetFrameRateLimit/SetVSyncEnabled + ApplySettings(false) + SaveSettings()). A resolution
	 *  revert countdown (confirm-or-revert within N seconds) is v2 - today Apply is immediate and
	 *  permanent, like every other commit on this menu. */
	struct FFoundryPendingVideo
	{
		EWindowMode::Type WindowMode = EWindowMode::Windowed;
		FIntPoint Resolution = FIntPoint::ZeroValue;
		/** -1 mirrors GetOverallScalabilityLevel()'s own "custom mix" sentinel - never passed to
		 *  SetOverallScalabilityLevel (HandleApplyClicked guards it). */
		int32 Quality = -1;
		float FpsCap = 0.0f;
		bool bVSync = false;
	};
	FFoundryPendingVideo PendingVideo;
	bool bPendingVideoDirty = false;

	// -- video --
	UFUNCTION() void HandleWindowModeChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
	UFUNCTION() void HandleResolutionChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
	UFUNCTION() void HandleQualityChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
	UFUNCTION() void HandleFpsCapChanged(FString SelectedItem, ESelectInfo::Type SelectionType);
	UFUNCTION() void HandleVSyncChanged(bool bIsChecked);
	UFUNCTION() void HandleApplyClicked();
	/** Sets bPendingVideoDirty, (en/dis)ables ApplyButton and fires OnVideoPendingChanged - but only
	 *  on an actual change, so a flood of combo callbacks during Populate (guarded by bPopulating
	 *  anyway) or repeat no-op calls don't spam the event. */
	void SetPendingVideoDirty(bool bDirty);

	// -- audio / interface --
	UFUNCTION() void HandleMasterVolumeChanged(float Value);
	UFUNCTION() void HandleMusicVolumeChanged(float Value);
	UFUNCTION() void HandleSFXVolumeChanged(float Value);
	UFUNCTION() void HandleUIScaleChanged(float Value);
	UFUNCTION() void HandleReducedMotionChanged(bool bIsChecked);
	/** Set, SaveSettings, then ApplyDeveloperMode on the game instance - live, no restart. */
	UFUNCTION() void HandleDeveloperModeChanged(bool bIsChecked);
	/** Shared by Master/Music/SFX/UIScale - a drag-end is when the (debounced) save actually happens. */
	UFUNCTION() void HandleAudioSliderCaptureEnd();
	/** Per-tick: a hovered slider gets a lit-up bar/handle (FoundryUITheme::CompositeOver), same
	 *  formula as the generator's combo/checkbox hover brushes - a slider's thumb tint multiplies
	 *  SliderHandleColor, so there is no usable STATIC hover brush the way a Button/CheckBox has. */
	void UpdateSliderHover();

	// -- controls --
	UFUNCTION() void HandleKeyChosen(FName InMappingName, FKey Key);
	UFUNCTION() void HandleResetRequested(FName InMappingName);
	/** Opens the "Reset all controls?" confirm (OpenConfirmDialog); the actual reset happens in
	 *  HandleResetAllConfirmed. */
	UFUNCTION() void HandleResetAllKeysClicked();
	UFUNCTION() void HandleResetAllConfirmed();
	UFUNCTION() void HandleResetAllCancelled();
	/** The reset-every-mapping body HandleResetAllKeysClicked used to run directly, now gated behind
	 *  the confirm above. */
	void PerformResetAllKeys();

	// GC-hold while the RESET ALL confirm is open (mirrors UFoundryPauseMenu::ActiveConfirm).
	UPROPERTY(Transient)
	TObjectPtr<UFoundryConfirmDialog> ActiveResetConfirm;

	// Same behavior as Back (discard pending video, pop through the host) - CloseButton's
	// OnClicked binds straight to this, no separate handler needed.
	UFUNCTION() void HandleBackClicked();

	void PopulateVideo();
	void PopulateAudioAndInterface();
	void PopulateControls();
	/** Rebuilds KeybindList from the active Enhanced Input key profile's player-mapped rows. */
	void RebuildKeybindRows();
	void ShowNote(const FString& Message);

	// -- the right-hand description pane --

	/** One entry per registered row (built once, in NativeOnInitialized, from the Row UPROPERTYs
	 *  above - which are already bound by then). Key matches the row's own bound NAME, so
	 *  Previews.Find(Key) looks up its optional preview art. */
	struct FDescribedRow
	{
		TWeakObjectPtr<UWidget> Row;
		FName Key;
		FText Title;
		FText Body;
	};
	TArray<FDescribedRow> DescribedRows;
	/** ControlsRow's own Body text - reused as the body for a hovered KEYBIND row (the rebinding
	 *  instructions are the same for every mapping; only the title changes to that mapping's name). */
	FText ControlsBodyCached;
	/** The row (or keybind row) the pane currently describes; null = the default "Settings" pane.
	 *  Identity (pointer), not value - so returning to "nothing selected" after showing a row is
	 *  itself a detectable change. */
	TWeakObjectPtr<UWidget> SelectedDescriptionWidget;

	void BuildDescribedRows();
	/** The NativeTick selection rule: the first registered/keybind row with IsHovered() (keybind
	 *  rows checked first - a hovered one wins outright), else the first with
	 *  HasFocusedDescendants(), else the default pane. */
	void UpdateHoveredDescription();
	void ApplyDescription(UWidget* SelectedWidget, const FText& Title, const FText& Body, FName PreviewKey);
	/** Whether any UFoundryKeybindRow under KeybindList is mid-capture - NativeOnPreviewKeyDown
	 *  checks this before treating Escape as "close the menu". */
	bool IsAnyKeybindRowCapturing() const;

	float NoteTimer = 0.0f;

	/** True for the duration of any Populate* call. Populate itself drives every combo/slider/
	 *  checkbox via Set*, and every one of those fires its own OnSelectionChanged/OnValueChanged/
	 *  OnCheckStateChanged - without this guard, HandleQualityChanged's own PopulateVideo() call
	 *  (before this fix) recursed without end, and every open silently re-applied+saved all four
	 *  video combos from their OWN callbacks. Every handler below early-returns while this is set. */
	bool bPopulating = false;
};
