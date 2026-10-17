// Copyright Foundry Media. FoundryFSDK default UI kit: the social drawer.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "FoundryFriendRow.h" // EFoundryFriendRowKind
#include "FoundryFriendMenu.h" // EFoundryFriendMenuItem
#include "FoundryInviteClock.h"
#include "FoundryUIWidgetBase.h"
#include "FoundrySocialDrawer.generated.h"

class IMenu;
class UButton;
class UEditableTextBox;
class UFoundryConfirmDialog;
class UFoundryFriendMenu;
class UFoundryFriendRow;
class UFoundryInviteRow;
class UFoundrySocialSubsystem;
class UImage;
class UPanelWidget;
class UTextBlock;
class UWidget;
class UWidgetAnimation;

/**
 * The default FRIENDS drawer: the account block (avatar / name / @username / Manage account), your
 * current party, pending incoming party invites, an expandable ONLINE / OFFLINE / BLOCKED friends
 * list (search filters online + offline, never blocked), and the ADD FRIEND entry. Rows are spawned
 * from FriendRowClass / InviteRowClass into PartyList / OnlineList / OfflineList / BlockedList /
 * InvitesList; right-clicking a row opens FriendMenuClass as a Slate popup menu.
 *
 * Reads/acts entirely through UFoundrySocialSubsystem (never UFoundryFSDKSubsystem directly) - the
 * ONE poller every social surface shares. The HOST decides when the drawer is on screen and calls
 * SetOpen (or SetPanelOpen on the subsystem directly) to raise/lower the poll cadence; this widget
 * never calls SetPanelOpen on its own Construct/Destruct (a drawer can be constructed well before it
 * is actually shown).
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundrySocialDrawer : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	UFoundrySocialDrawer(const FObjectInitializer& ObjectInitializer);

	DECLARE_MULTICAST_DELEGATE(FFoundrySocialDrawerEvent);
	DECLARE_MULTICAST_DELEGATE_TwoParams(FFoundryWhisperRequested, const FString& /*FoundryId*/, const FString& /*DisplayName*/);

	DECLARE_MULTICAST_DELEGATE_OneParam(FFoundryActionMessage, const FString& /*Message*/);

	FFoundrySocialDrawerEvent OnCloseRequested;
	/** SendMessage from the context menu - the drawer never opens a whisper itself; the host wires
	 *  this to its own chat surface. */
	FFoundryWhisperRequested OnWhisperRequested;
	/** A non-friend-request social action failed (party invite, request-to-join, ...) - the host may
	 *  forward Message to its own chat/system-line surface. A friend-request failure goes to
	 *  AddFriendNote instead and is never broadcast here. */
	FFoundryActionMessage OnActionMessage;

	/** Opening focuses the first useful control (the search box). */
	void FocusFirstControl();

	/**
	 * Test support: opens the friend context menu over the first row in the ONLINE list, at that
	 * row's own screen centre - a headless fixture shot can prove the popup menu renders without a
	 * real mouse (no PushMenu call otherwise reachable without one). Returns false when the online
	 * list is empty or unbound.
	 */
	bool OpenFirstRowMenuForTesting();

	/**
	 * Tells UFoundrySocialSubsystem::SetPanelOpen - call this whenever the HOST shows/hides the
	 * drawer, so a host that wants one call doesn't have to reach the subsystem itself.
	 *
	 * The drawer ANIMATES ITSELF by default: bAnimate=true (the default) plays DrawerAnim when the
	 * drawer's OWN WBP binds one, else tweens its own RenderTranslation X (TweenOffset, default 448)
	 * and RenderOpacity over 0.22s via PlayOrTween (reversible mid-flight) - opening sets Visibility
	 * to SelfHitTestInvisible up front, closing collapses once the transition finishes
	 * (NativeOnTransitionFinished(false)). Pass bAnimate=false when a HOST plays its own motion on
	 * the drawer instead (e.g. its own DrawerAnim authored on a Drawer child in the HOST's own
	 * widget tree) - the drawer then only calls SetPanelOpen and snaps straight to the end state
	 * (visibility/opacity/translation), leaving any timed reveal/collapse to the host.
	 */
	void SetOpen(bool bOpen, bool bAnimate = true);

	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryFriendRow> FriendRowClass;

	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryInviteRow> InviteRowClass;

	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryFriendMenu> FriendMenuClass;

	/** Optional - an animation authored on the drawer's OWN WBP (its own Sequencer, not the host's).
	 *  When bound, SetOpen's PlayOrTween plays THIS instead of its code-tween fallback; the drawer
	 *  plays it itself, on open/close, never the host. Leave unbound to use the default code tween
	 *  (TweenOffset slide + opacity fade). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetAnimOptional))
	TObjectPtr<UWidgetAnimation> DrawerAnim;

	/** Tint for a roster index: the local player gets Palette.Primary; override for a game's own
	 *  cast colours. */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI")
	FLinearColor GetMemberTint(const FString& FoundryId, int32 RosterIndex, bool bLocal) const;
	virtual FLinearColor GetMemberTint_Implementation(const FString& FoundryId, int32 RosterIndex, bool bLocal) const;

	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnOnlineExpandedChanged(bool bExpanded);
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnOfflineExpandedChanged(bool bExpanded);
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnBlockedExpandedChanged(bool bExpanded);
	/** The ADD FRIEND entry opened / closed (e.g. relabel ADD FRIEND -> DONE). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnAddFriendEntryChanged(bool bOpen);

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void NativeDestruct() override;
	/** Collapses the drawer once the CLOSE transition finishes (bForward=false); the OPEN side is
	 *  handled up front by SetOpen itself (SelfHitTestInvisible before the tween/animation starts). */
	virtual void NativeOnTransitionFinished(bool bForward) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CloseButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UEditableTextBox> SearchBox;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> AccountAvatar;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AccountNameText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AccountUsernameText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ManageAccountButton;
	/** "YOUR PARTY   3 / 4" - the cap is the selected mode's in game (else the platform cap); over it
	 *  the text reads "8 / 4 - too big for this mode" in the Error palette colour. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PartyHeaderText;
	/** Toggles your own ready flag (SetReady); visible only while in a party. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ReadyToggleButton;
	/** "READY" / "UNREADY" - mirrors UFoundrySocialSubsystem::IsSelfReady(). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ReadyToggleText;
	/** Shown only while the party is bigger than its cap (names the cap and what to do about it). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CapConflictText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> PartyList;
	/** The "PARTY INVITES" label + list container; hidden when there are none. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> InvitesSection;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> InvitesList;
	/** The leader's "JOIN REQUESTS" label + list container; shown only while the local player leads a
	 *  party and somebody is asking to join it. Rows are InviteRowClass rows in join-request mode. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> JoinRequestsSection;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> JoinRequestsList;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RequestsNote;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> OnlineToggleButton;
	/** "ONLINE   2". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> OnlineHeaderText;
	/** Rotates 0<->180 (TweenAngle) as the ONLINE section expands/collapses. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> OnlineChevron;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> OnlineList;
	/** "No friends online right now." / "Sign in to see your friends." */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> OnlineEmptyText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> OfflineToggleButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> OfflineHeaderText;
	/** Rotates 0<->180 (TweenAngle) as the OFFLINE section expands/collapses. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> OfflineChevron;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> OfflineList;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> BlockedToggleButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BlockedHeaderText;
	/** Rotates 0<->180 (TweenAngle) as the BLOCKED section expands/collapses. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> BlockedChevron;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> BlockedList;
	/** The username field + Send button row; collapsed until ADD FRIEND opens it. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> AddFriendEntry;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UEditableTextBox> AddFriendInput;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> AddFriendSendButton;
	/** Why adding a friend is blocked, shown after ADD FRIEND is pressed. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AddFriendNote;
	/** Shown for 6s after a non-friend-request action fails (OnActionMessage mirrors it to the host), and
	 *  for 4s after one succeeds ("Asked Sam to join" - the platform's own sentence). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ActionNote;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> AddFriendButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> LeaveButton;

private:
	UFoundrySocialSubsystem* Subsystem();

	UFUNCTION() void HandleClose();
	UFUNCTION() void HandleManageAccount();
	UFUNCTION() void HandleSearchChanged(const FText& Text);
	UFUNCTION() void HandleToggleOnline();
	UFUNCTION() void HandleToggleOffline();
	UFUNCTION() void HandleToggleBlocked();
	UFUNCTION() void HandleAddFriendButton();
	UFUNCTION() void HandleAddFriendSend();
	UFUNCTION() void HandleAddFriendCommitted(const FText& Text, ETextCommit::Type Commit);
	UFUNCTION() void HandleLeaveParty();
	UFUNCTION() void HandleReadyToggle();
	void HandleActionFailed(const FString& Action, const FString& Message);
	/** OnActionSucceeded is a DYNAMIC multicast delegate (FName, FString): a UFUNCTION bound with AddDynamic. */
	UFUNCTION() void HandleActionSucceeded(FName Action, const FString& Message);
	/** The ActionNote line: red for a failure, green for a success; hides itself after Seconds. */
	void ShowActionNote(const FString& Message, bool bError, float Seconds);

	/** Accept on an invite row. Accepting while you LEAD a party with other joined members leaves it (fid's
	 *  accept auto-leaves; leadership transfers to the longest-standing member and your pending invites
	 *  end), so that case confirms first. A lone leader's party just ends, so it accepts at once. */
	void HandleAcceptInvite(const FString& PartyId);
	UFUNCTION() void HandleAcceptConfirmed();
	UFUNCTION() void HandleAcceptCancelled();

	/** Remove from party: destructive, so it confirms first (the kit's confirm dialog, pushed through the host). */
	void AskKickMember(const FString& FoundryId, const FString& DisplayName);
	UFUNCTION() void HandleKickConfirmed();
	UFUNCTION() void HandleKickCancelled();

	/** Sets bDirty; the actual rebuild runs at most once per frame, from NativeTick, and only while
	 *  the drawer is Visible (a collapsed drawer rebuilds exactly once, the frame it next opens). */
	void MarkDirty();
	/** OnChanged: records invite countdown anchors (below), then MarkDirty. */
	void HandleSocialChanged();
	/** First-sight anchor per pending invite: the countdown starts when the UI first RECEIVED the row (or
	 *  its ExpiresAt changed - a re-invite), never from the local wall clock vs fid's ExpiresAt (a fast
	 *  client clock would expire every live invite). Rows fid stops returning are pruned. */
	void NoteInviteClocks();
	FoundryPartyRules::FInviteClock ClockFor(const FString& Key) const;
	void Rebuild();
	void ApplyAddFriendEntry();
	UFoundryFriendRow* AddFriendRow(UPanelWidget* List);

	void OpenFriendMenu(UFoundryFriendRow* Row, const FVector2D& ScreenPosition);
	void CloseFriendMenu();
	void HandleMenuItem(EFoundryFriendMenuItem Item);
	void HandleMenuConfirm();
	void HandleMenuCancel();
	void HandleMenuDismissed(TSharedRef<IMenu> DismissedMenu);

	FString Filter;
	FString AddFriendBlockedReason;
	bool bOnlineExpanded = true;
	bool bOfflineExpanded = false;
	bool bBlockedExpanded = false;
	bool bAddFriendEntryOpen = false;
	bool bAddFriendNoteRequested = false;
	bool bWarnedNoRowClass = false;
	bool bDirty = true; // starts true: the first time the drawer becomes Visible it rebuilds once

	FTimerHandle ActionNoteTimerHandle;

	TWeakObjectPtr<UFoundrySocialSubsystem> SubsystemWeak;

	// The open "accepting disbands your party" confirm (GC-hold while open) + the invite it will accept.
	UPROPERTY(Transient) TObjectPtr<UFoundryConfirmDialog> ActiveConfirm;
	FString PendingAcceptPartyId;

	// The open "remove <name> from the party?" confirm (GC-hold while open) + who it will remove.
	UPROPERTY(Transient) TObjectPtr<UFoundryConfirmDialog> ActiveKickConfirm;
	FString PendingKickFoundryId;

	struct FInviteAnchor
	{
		FDateTime ExpiresAt;
		FoundryPartyRules::FInviteClock Clock;
	};
	/** Key "m:<foundryId>" for a pending roster member, "i:<partyId>" for an incoming invite. */
	TMap<FString, FInviteAnchor> InviteAnchors;

	// PartyHeaderText's own colour, captured on the first rebuild, so the over-size Error tint can be undone.
	FSlateColor PartyHeaderNormalColor;
	bool bPartyHeaderColorCaptured = false;

	// The open context menu widget (GC-hold - the UWidget isn't kept alive by its own Slate content
	// alone, so a strong ref is held here while PushMenu's IMenu is open) + its dismiss handle.
	UPROPERTY(Transient) TObjectPtr<UFoundryFriendMenu> ActiveMenu;
	TWeakPtr<IMenu> ActiveMenuHandle;

	// What a pending RemoveFriend/Block confirm will do when confirmed - also the Invite target: a
	// friend's USERNAME (falling back to display name only when they have none), stashed when the
	// menu opens and never re-read off the row afterwards (a refresh may rebuild rows while the menu
	// is open).
	FString PendingActionFoundryId;
	FString PendingActionUsername;
	EFoundryFriendMenuItem PendingAction = EFoundryFriendMenuItem::SendMessage;

	// Row reuse: Rebuild() updates an EXISTING row in place (Setup again + reorder) instead of
	// destroying and recreating every widget on every poll - keyed by a stable identity per list
	// (a FoundryId for friend rows, a party id for invite rows). See the free SyncRowList helper at
	// the top of FoundrySocialDrawer.cpp.
	TMap<FString, TObjectPtr<UFoundryFriendRow>> PartyRowsByFoundryId;
	TMap<FString, TObjectPtr<UFoundryFriendRow>> OnlineRowsByFoundryId;
	TMap<FString, TObjectPtr<UFoundryFriendRow>> OfflineRowsByFoundryId;
	TMap<FString, TObjectPtr<UFoundryFriendRow>> BlockedRowsByFoundryId;
	TMap<FString, TObjectPtr<UFoundryInviteRow>> InviteRowsByPartyId;
	/** Join-request rows, keyed by the requester's FoundryId. */
	TMap<FString, TObjectPtr<UFoundryInviteRow>> JoinRequestRowsByFoundryId;
};
