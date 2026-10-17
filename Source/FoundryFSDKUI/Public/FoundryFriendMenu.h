// Copyright Foundry Media. FoundryFSDK default UI kit: the friend right-click context menu.

#pragma once

#include "CoreMinimal.h"
#include "FoundryFriendRow.h" // EFoundryFriendRowKind
#include "FoundryPartyRules.h" // FFriendMenuPlan
#include "FoundryUIWidgetBase.h"
#include "FoundryFriendMenu.generated.h"

class UButton;
class UFoundrySocialSubsystem;
class UTextBlock;
class UWidget;

/** One action in the friend context menu. */
UENUM(BlueprintType)
enum class EFoundryFriendMenuItem : uint8
{
	SendMessage,
	Invite,
	/** "Ask to join" (kept as RequestToJoin: the enum value is part of the kit's Blueprint surface). */
	RequestToJoin,
	RemoveFriend,
	Block,
	Unblock,
	/** A pending member: send the invite again (resets its 15-minute clock, re-notifies). */
	ReInvite,
	/** A pending member: withdraw the invite. */
	CancelInvite,
	/** A joined member: remove them from the party (leader only). */
	RemoveFromParty
};

/**
 * The right-click context menu a social drawer opens over a friend/party row (as a Slate popup via
 * FSlateApplication::PushMenu). Which items show and which are enabled come from a
 * FoundryPartyRules::FFriendMenuPlan the drawer computes (Setup); RemoveFriend and Block swap
 * ItemsPanel for ConfirmPanel (ShowConfirm) instead of firing OnItem directly, so the caller still
 * gets a chance to confirm before acting.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryFriendMenu : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FFoundryFriendMenuItemEvent, EFoundryFriendMenuItem /*Item*/);
	DECLARE_MULTICAST_DELEGATE(FFoundryFriendMenuEvent);

	/** A menu item was clicked. RemoveFriend/Block fire this too (the caller decides whether to
	 *  call ShowConfirm before acting on them - the kit's own drawer does). */
	FFoundryFriendMenuItemEvent OnItem;
	/** The confirm step's own button was clicked (ShowConfirm). */
	FFoundryFriendMenuEvent OnConfirm;
	/** Cancel in the confirm step. */
	FFoundryFriendMenuEvent OnCancel;

	/** Shows each item the plan shows, enables each the plan enables, and sets each disabled item's
	 *  kit-pill tooltip to the plan's reason (a disabled UButton still shows its own tooltip). */
	void Setup(EFoundryFriendRowKind InKind, const FString& InName, const FoundryPartyRules::FFriendMenuPlan& Plan);

	/** Collapses ItemsPanel, shows ConfirmPanel with Text + a ButtonLabel'd ConfirmButton. */
	void ShowConfirm(const FString& Text, const FString& ButtonLabel);

	/** Back to the items panel from a confirm step (Cancel) - leaves every item's enabled state as
	 *  Setup last left it. */
	void ShowItems();

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	EFoundryFriendRowKind Kind = EFoundryFriendRowKind::Online;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	FString TargetName;

protected:
	virtual void NativeOnInitialized() override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ItemsPanel;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> SendMessageButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> InviteButton;
	/** "Ask to join" - the bound name stays RequestJoinButton so existing WBPs keep working. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> RequestJoinButton;
	/** Pending member rows (leader): "Re-invite". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ReInviteButton;
	/** Pending member rows (leader): "Cancel invite". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CancelInviteButton;
	/** Joined member rows (leader, never the local player): "Remove from party". */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> KickButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> RemoveFriendButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> BlockButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> UnblockButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ConfirmPanel;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ConfirmText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ConfirmButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ConfirmButtonText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CancelButton;

private:
	/** Applies one plan entry to its button: shown, enabled, and the reason as the kit-pill tooltip. */
	void ApplyItem(UButton* Button, const FoundryPartyRules::FMenuItemState& State);

	UFUNCTION() void HandleSendMessage();
	UFUNCTION() void HandleInvite();
	UFUNCTION() void HandleRequestJoin();
	UFUNCTION() void HandleReInvite();
	UFUNCTION() void HandleCancelInvite();
	UFUNCTION() void HandleKick();
	UFUNCTION() void HandleRemoveFriend();
	UFUNCTION() void HandleBlock();
	UFUNCTION() void HandleUnblock();
	UFUNCTION() void HandleConfirm();
	UFUNCTION() void HandleCancel();
};
