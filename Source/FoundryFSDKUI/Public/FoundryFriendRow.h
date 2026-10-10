// Copyright Foundry Media. FoundryFSDK default UI kit: one social-drawer row.

#pragma once

#include "CoreMinimal.h"
#include "FoundryFSDKSubsystem.h" // FFoundryFriend
#include "FoundryUIWidgetBase.h"
#include "FoundryFriendRow.generated.h"

class UButton;
class UImage;
class UTextBlock;
class UWidget;

/** Which list a row renders in - drives the context menu's item set (UFoundryFriendMenu::Setup). */
UENUM(BlueprintType)
enum class EFoundryFriendRowKind : uint8
{
	/** A member of your current party. The local player's own row and a pending-invited row never
	 *  open a context menu (the drawer skips wiring OnContextMenuRequested for them). */
	Party,
	Online,
	Offline,
	Blocked
};

/**
 * One row in the social drawer: a party member (joined or pending), an online/offline friend, or a
 * blocked player. Setup fills the bound widgets and the mirrored BlueprintReadOnly properties, then
 * fires OnRowUpdated - so a WBP can restyle on any of them without new C++.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryFriendRow : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE_TwoParams(FFoundryFriendRowContextMenu, UFoundryFriendRow* /*Row*/, const FVector2D& /*ScreenPosition*/);
	/** Right-clicked; the drawer opens UFoundryFriendMenu at ScreenPosition. */
	FFoundryFriendRowContextMenu OnContextMenuRequested;

	void Setup(const FFoundryFriend& InFriend, EFoundryFriendRowKind InKind, bool bInHost, bool bInLocal,
		bool bInJoined, const FLinearColor& InTint, const FString& InStatusOverride);

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	FFoundryFriend Friend;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	EFoundryFriendRowKind Kind = EFoundryFriendRowKind::Online;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	bool bIsHost = false;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	bool bIsLocal = false;

	/** false = invited, not joined yet (never shown as present). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	bool bIsJoined = true;

	/** online | idle | away | dnd | offline | "" (unknown) - mirrors Friend.Presence at the
	 *  top level for a WBP binding that doesn't want to reach into the struct. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	FString Presence;

	/** The member/friend's own tint (same colour their avatar wears elsewhere in the game). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	FLinearColor Tint = FLinearColor::White;

	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnRowUpdated();

protected:
	virtual FReply NativeOnMouseButtonDown(const FGeometry& InGeometry, const FPointerEvent& InMouseEvent) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> Avatar;
	/** Hidden when not joined / blocked. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> PresenceDot;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> NameText;
	/** Hidden for a joined host (HostBadge shows instead). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> StatusText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> HostBadge;
	/** Unused by the kit's own drawer (Collapsed in Setup); kept so a game can give a row its own
	 *  inline action by overriding NativeOnInitialized and wiring it itself. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> ActionButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ActionText;
};
