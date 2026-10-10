// Copyright Foundry Media. FoundryFSDK default UI kit: the social drawer.

#pragma once

#include "CoreMinimal.h"
#include "Engine/TimerHandle.h"
#include "FoundryFriendRow.h" // EFoundryFriendRowKind
#include "FoundryFriendMenu.h" // EFoundryFriendMenuItem
#include "FoundryUIWidgetBase.h"
#include "FoundrySocialDrawer.generated.h"

class IMenu;
class UButton;
class UEditableTextBox;
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

	/** Tells UFoundrySocialSubsystem::SetPanelOpen - call this whenever the HOST shows/hides the
	 *  drawer, so a host that wants one call doesn't have to reach the subsystem itself. */
	void SetOpen(bool bOpen);

	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryFriendRow> FriendRowClass;

	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryInviteRow> InviteRowClass;

	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI")
	TSubclassOf<UFoundryFriendMenu> FriendMenuClass;

	/** Played by the HOST (e.g. PlayOrTween(DrawerAnim, bOpen, ...)) around opening/closing the
	 *  drawer; the drawer itself never plays it. */
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

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CloseButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UEditableTextBox> SearchBox;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> AccountAvatar;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AccountNameText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AccountUsernameText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ManageAccountButton;
	/** "YOUR PARTY   3 / 8". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PartyHeaderText;
	/** Shown only when the server reports a party cap that differs from the subsystem's default. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CapConflictText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> PartyList;
	/** The "PARTY INVITES" label + list container; hidden when there are none. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> InvitesSection;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> InvitesList;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RequestsNote;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> OnlineToggleButton;
	/** "ONLINE   2". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> OnlineHeaderText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> OnlineList;
	/** "No friends online right now." / "Sign in to see your friends." */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> OnlineEmptyText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> OfflineToggleButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> OfflineHeaderText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> OfflineList;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> BlockedToggleButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> BlockedHeaderText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> BlockedList;
	/** The username field + Send button row; collapsed until ADD FRIEND opens it. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> AddFriendEntry;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UEditableTextBox> AddFriendInput;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> AddFriendSendButton;
	/** Why adding a friend is blocked, shown after ADD FRIEND is pressed. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> AddFriendNote;
	/** Shown for 6s after a non-friend-request action fails (OnActionMessage mirrors it to the host). */
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
	void HandleActionFailed(const FString& Action, const FString& Message);

	/** Sets bDirty; the actual rebuild runs at most once per frame, from NativeTick, and only while
	 *  the drawer is Visible (a collapsed drawer rebuilds exactly once, the frame it next opens). */
	void MarkDirty();
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
};
