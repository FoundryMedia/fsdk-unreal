// Copyright Foundry Media. FoundryFSDK default UI kit: the friend right-click context menu.

#pragma once

#include "CoreMinimal.h"
#include "FoundryFriendRow.h" // EFoundryFriendRowKind
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
	RequestToJoin,
	RemoveFriend,
	Block,
	Unblock
};

/**
 * The right-click context menu a social drawer opens over a friend/party row (as a Slate popup via
 * FSlateApplication::PushMenu). Which items show and which are enabled follow the row's kind
 * (Setup); RemoveFriend and Block swap ItemsPanel for ConfirmPanel (ShowConfirm) instead of firing
 * OnItem directly, so the caller still gets a chance to confirm before acting.
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

	/** InInviteDisabledReason is shown as InviteButton's tooltip whenever Invite ends up disabled
	 *  (offline target, or !bCanInvite) - the caller (the drawer) computes it from whichever of
	 *  party-full / host-only / already-in-party / not-signed-in applies; empty when enabled.
	 *  RequestJoinButton's own reason is still computed in here, same as before. */
	void Setup(EFoundryFriendRowKind InKind, const FString& InName, bool bCanInvite, const FString& InInviteDisabledReason, bool bCanRequestJoin);

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
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> RequestJoinButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> RemoveFriendButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> BlockButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> UnblockButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ConfirmPanel;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ConfirmText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ConfirmButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ConfirmButtonText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CancelButton;

private:
	UFoundrySocialSubsystem* Subsystem() const;

	UFUNCTION() void HandleSendMessage();
	UFUNCTION() void HandleInvite();
	UFUNCTION() void HandleRequestJoin();
	UFUNCTION() void HandleRemoveFriend();
	UFUNCTION() void HandleBlock();
	UFUNCTION() void HandleUnblock();
	UFUNCTION() void HandleConfirm();
	UFUNCTION() void HandleCancel();
};
