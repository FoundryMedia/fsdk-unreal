// Copyright Foundry Media. FoundryFSDK default UI kit: one incoming party-invite row.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryInviteRow.generated.h"

class UButton;
class UTextBlock;

/** An incoming party invite: "<leader>'s party" + Accept / Decline. */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryInviteRow : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FFoundryInviteRowAction, const FString& /*PartyId*/);
	FFoundryInviteRowAction OnAccept;
	FFoundryInviteRowAction OnDecline;

	void Setup(const FString& InLeaderName, const FString& InPartyId);

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	FString LeaderName;

	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnRowUpdated();

protected:
	virtual void NativeOnInitialized() override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> LeaderText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> AcceptButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> DeclineButton;

private:
	UFUNCTION() void HandleAccept();
	UFUNCTION() void HandleDecline();

	FString PartyId;
};
