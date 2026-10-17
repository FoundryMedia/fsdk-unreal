// Copyright Foundry Media. FoundryFSDK default UI kit widget base.

#pragma once

#include "Blueprint/UserWidget.h"
#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.generated.h"

class IMenu;
class UComboBoxString;
class UFoundryConfirmDialog;
class UFoundryTooltip;
class UTextBlock;
class UWidget;
class UWidgetAnimation;

/**
 * Base for the default UI kit's C++ parent classes. The LAYOUT is a Widget Blueprint (WBP_Foundry*)
 * whose parent is one of these; the C++ side only binds the named widgets it needs
 * (meta=(BindWidgetOptional), so a WBP can be laid out a piece at a time - a missing name is a
 * compiler note, never a crash) and pushes state into them: text, visibility, enabled, tint.
 * Anything that is a LOOK (hover, selected styling) is left to the WBP; the code never styles a
 * brush.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryUIWidgetBase : public UUserWidget
{
	GENERATED_BODY()

public:
	/**
	 * Every Button, ComboBox, CheckBox and Slider in the widget's tree takes the Hand cursor. Called
	 * from NativeOnInitialized here; a plain (non-UFoundryUIWidgetBase) row widget calls it itself.
	 */
	static void UseHandCursor(UUserWidget* Widget);

	static void SetText(UTextBlock* Text, const FString& Value);

	/** Collapsed when hidden; Visible (hit-testable) or SelfHitTestInvisible (display only) when
	 *  shown. */
	static void Show(UWidget* Widget, bool bVisible, bool bHitTestable = false);

	static FString Truncate(const FString& S, int32 MaxChars);

	/**
	 * Plays Anim forward / reverse (reversing mid-flight when it is already playing) when it is
	 * bound; otherwise tweens TweenTarget's RenderOpacity between 0 and 1 (plus a RenderTranslation
	 * X slide from TweenOffset, when non-zero) over FallbackSeconds, ease-out cubic, driven from
	 * NativeTick - reversible mid-flight the same way. Either path ends in
	 * OnTransitionFinished(bForward); bind the WBP's own styling to it, or override
	 * NativeOnTransitionFinished in a C++ subclass.
	 */
	void PlayOrTween(UWidgetAnimation* Anim, bool bForward, float FallbackSeconds, UWidget* TweenTarget);

	/** Extra distance (slate units) TweenTarget's RenderTranslation X eases from/to during the code
	 *  fallback tween; 0 = opacity only. Ignored when Anim plays instead. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	float TweenOffset = 0.0f;

	/**
	 * Eases Target's RenderTransformAngle from FromDeg to ToDeg over Seconds (ease-out cubic),
	 * ticked alongside PlayOrTween's own fallback tween but tracked independently of it - several
	 * widgets (e.g. a drawer's three section chevrons) can each be mid-rotation at once, and none of
	 * them share state with the opacity/translation tween PlayOrTween drives. Calling it again for
	 * the SAME Target restarts the tween from wherever it currently sits (so a rapid toggle reverses
	 * smoothly instead of snapping). Target's RenderTransformPivot should be set to (0.5, 0.5) once
	 * (e.g. in NativeOnInitialized) so the rotation spins in place rather than swinging an edge.
	 */
	void TweenAngle(UWidget* Target, float FromDeg, float ToDeg, float Seconds);

	/**
	 * Collapses the engine chrome FSlateApplication::PushMenu wraps around EVERY pushed menu:
	 * FMenuStack::PrePush -> WrapContent(bShouldShowBackground = true, no flag on PushMenu) builds
	 * SMenuContentWrapper -> SBox -> SOverlay [ SImage "Menu.Background", SImage "Menu.Outline",
	 * SBorder(content) ] (MenuStack.cpp). The dark box brush shows at the corners of any rounded
	 * content panel. Only the IMenuHost path (SMenuAnchor / combos) can switch it off, so a cursor-
	 * placed menu hides the two images after the push instead. Structure-checked: a different
	 * engine layout leaves the chrome alone (dark corners, nothing broken). Returns how many
	 * images were collapsed (2 on UE 5.7).
	 */
	static int32 HideEngineMenuChrome(const TSharedPtr<IMenu>& Menu);

	/**
	 * Clears the dark background/outline frame a UComboBoxString's OWN internal popup draws by
	 * default (SMenuAnchor/SComboButton's inherited IMenuHost::bShowMenuBackground, true unless
	 * cleared) - the same family of "engine chrome around a hosted popup" problem
	 * HideEngineMenuChrome solves above, but a combo's list is its own SMenuAnchor, never a
	 * FSlateApplication::PushMenu, so it needs its own seam (see Source/GooCrew/UI/Lobby/
	 * GooLobbyChatPanel.cpp ScrubChannelSelectBrushes/NativeConstruct for the same pattern against a
	 * Designer-built combo). Call once per combo after Construct - TakeWidget() realizes the live
	 * Slate tree the first time, which is when bShowMenuBackground becomes reachable. Does nothing
	 * to the combo's OWN placement (below/above the anchor) - that stays the caller's call, when one
	 * is needed.
	 */
	static void ScrubComboChrome(UComboBoxString* Combo);

	/**
	 * Walks Widget's tree and, for every descendant whose GetToolTipText() is non-empty and that
	 * doesn't already carry a custom tooltip widget, wraps that text in a UFoundryTooltip (from
	 * UFoundryUISettings::DefaultTooltip) via UWidget::SetToolTip - replacing the engine's own
	 * tooltip bubble with the kit's pill for every tooltip the WBP or its parent class set BEFORE
	 * this runs. Call once, after any such setup (e.g. right after UseHandCursor in
	 * NativeOnInitialized, which every UFoundryUIWidgetBase does automatically - a game widget that
	 * does NOT derive from this base, like Goo Crew's lobby, calls it explicitly instead). A later
	 * runtime text change goes through SetKitToolTip, not a fresh ApplyKitTooltips pass.
	 */
	static void ApplyKitTooltips(UUserWidget* Widget);

	/**
	 * Sets or clears Target's tooltip, in the kit's own pill (never the engine's default tooltip
	 * bubble) - the RUNTIME counterpart to ApplyKitTooltips, for a tooltip whose text changes after
	 * construction (e.g. a status line re-set every poll). Empty Text clears the tooltip entirely
	 * (SetToolTip(nullptr) + ToolTipText reset); otherwise an existing UFoundryTooltip is relabeled
	 * in place, or a fresh one is created from UFoundryUISettings::DefaultTooltip (falling back to a
	 * plain SetToolTipText when the class is unset, so the text is never silently lost).
	 */
	static void SetKitToolTip(UWidget* Target, const FText& Text);

	/**
	 * The owning player controller when it implements IFoundryMenuHost, else null (one WARN per
	 * widget CLASS naming the controller class - the kit's menus then fall back to AddToViewport /
	 * RemoveFromParent instead of asking the host).
	 */
	UObject* FindMenuHost() const;

	/**
	 * Tells the party the LOCAL player is leaving a match: when they lead a party and the match is already
	 * over, UFoundryPartyFollowComponent (on the owning controller) fans "the leader left" out so their
	 * party leaves too - the platform default, switchable with UFoundryUISettings::
	 * bPartyFollowsLeaderOnLeave. Call BEFORE the leave itself so the message is queued while the
	 * connection is up. A no-op for everyone else and for a game that never added the component.
	 */
	void NotifyPartyOfLeave() const;

	/**
	 * The ONE kit path out of a match through the host: NotifyPartyOfLeave, then IFoundryMenuHost::
	 * RequestLeaveMatch. False (and nothing done) when there is no host - the caller logs its own warning.
	 * Every kit leave button goes through this so none of them can skip the party notice.
	 */
	bool LeaveMatchViaHost() const;

	/**
	 * Creates + pushes a UFoundryConfirmDialog (ClassOverride, else UFoundryUISettings::
	 * DefaultConfirmDialog) through the host when one is available, else AddToViewport(30) - the
	 * shared "confirm before a destructive action" seam the pause menu's QUIT TO DESKTOP/LEAVE MATCH
	 * and the settings menu's RESET ALL all go through. Null (+ a WARN naming this widget's class)
	 * when no class resolves - the caller should refuse the action rather than skip confirmation.
	 * The CALLER owns binding OnConfirmed/OnCancelled and holding a GC reference (UPROPERTY) to the
	 * result while it is open, same as before this was factored out.
	 */
	UFoundryConfirmDialog* OpenConfirmDialog(const FText& Title, const FText& Body, const FText& ConfirmLabel,
		const FText& CancelLabel, float CooldownSeconds, TSubclassOf<UFoundryConfirmDialog> ClassOverride = nullptr);

	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnTransitionFinished(bool bForward);

	/** Native hook for OnTransitionFinished; override in a C++ subclass. The WBP's own
	 *  BlueprintImplementableEvent runs after it. */
	virtual void NativeOnTransitionFinished(bool bForward) {}

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void OnAnimationFinished_Implementation(const UWidgetAnimation* Animation) override;

	/**
	 * When true, a mouse-down that lands on this widget while NEITHER it nor any descendant already
	 * has keyboard focus (e.g. a click on a non-focusable backdrop) retakes keyboard focus for THIS
	 * widget, so a later Escape still reaches it - a click that lands on an already-focusable
	 * descendant (a combo, a slider) is left alone; that descendant takes focus on its own. Off by
	 * default; the kit's modal menus (pause menu, confirm dialog, settings menu) turn it on in their
	 * constructors.
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	bool bRetakeFocusOnClick = false;

	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

private:
	// The animation (if any) the last PlayOrTween call drove, and which direction - so
	// OnAnimationFinished_Implementation knows which one just finished and what bForward to report.
	TWeakObjectPtr<UWidgetAnimation> LastAnim;
	bool LastAnimForward = false;

	// Code-tween fallback state (PlayOrTween with a null Anim). TweenProgress is persistent (0 =
	// hidden, 1 = shown) so a toggle mid-flight reverses smoothly instead of snapping.
	TWeakObjectPtr<UWidget> TweenWidget;
	bool bTweenActive = false;
	bool bTweenForward = false;
	float TweenProgress = 0.0f;
	float TweenDuration = 0.3f;

	// TweenAngle state - one entry per widget currently mid-rotation. A repeat TweenAngle call for a
	// widget already in this list replaces its entry (restarts Elapsed, keeps nothing else around)
	// instead of stacking a second tween on the same widget.
	struct FFoundryAngleTween
	{
		TWeakObjectPtr<UWidget> Widget;
		float FromDeg = 0.0f;
		float ToDeg = 0.0f;
		float Duration = 0.15f;
		float Elapsed = 0.0f;
	};
	TArray<FFoundryAngleTween> AngleTweens;
};
