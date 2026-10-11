// Copyright Foundry Media. FoundryFSDK default UI kit: a party-member name plate.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryPartyNameplate.generated.h"

class UTextBlock;
class UWidget;

/**
 * A single floating/anchored label for one party member: name, leader crown + host pill (both mark
 * the LEADER, same as the social drawer's HostBadge), and a ready check mark. No context menu, no
 * presence dot - this is a lightweight WORLD/HUD tag (a lobby's floating name plates), not a drawer
 * row; see UFoundryFriendRow for the fuller roster-list presentation. Setup pushes every field in one
 * call so a caller never has to touch the bound widgets directly; OnPlateUpdated fires after every
 * Setup so a WBP can restyle (e.g. re-tween a pop) without new C++.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryPartyNameplate : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Fills the plate: Name shown as NameText (tinted via GetNameTint); bLeader shows CrownIcon AND
	 *  HostBadge (both mark the party LEADER); bReady shows ReadyIcon; bLocal selects the tint
	 *  GetNameTint resolves (the local player's own plate vs. everyone else's). */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void Setup(const FString& Name, bool bLeader, bool bReady, bool bLocal);

	/** NameText's colour: Palette.Primary for the local player's own plate, Palette.Text otherwise.
	 *  Override for a game's own hero tint (e.g. Goo Crew's HeroPink). */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI")
	FLinearColor GetNameTint(bool bLocal) const;
	virtual FLinearColor GetNameTint_Implementation(bool bLocal) const;

	/** Fires after every Setup, once every bound widget has been pushed. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnPlateUpdated();

protected:
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> NameText;
	/** Shown only for the party leader (alongside HostBadge). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> CrownIcon;
	/** Shown only for the party leader (alongside CrownIcon). */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> HostBadge;
	/** Shown only while the member is ready. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> ReadyIcon;
};
