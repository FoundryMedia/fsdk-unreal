// Copyright Foundry Media. FoundryFSDK default UI kit: one incoming party-invite row.

#pragma once

#include "CoreMinimal.h"
#include "FoundryInviteClock.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryInviteRow.generated.h"

class UButton;
class UTextBlock;

/**
 * An incoming party invite - "<leader>'s party", "N / cap", a live "expires in 3 min" / "Expired"
 * line, Accept / Decline - or, reused by the leader's join-requests block, "<name> wants to join your
 * party" with the same two buttons (SetupJoinRequest). The expiry line re-renders once a second
 * from the invite's FInviteClock (anchored on when the UI received the row, never on the local wall
 * clock - see FoundryInviteClock.h; fid hides an expired invite on its next poll anyway, so the row
 * only has to be honest for the seconds in between); an expired row greys its Accept.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryInviteRow : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Id is the PartyId for an invite row, the requester's FoundryId for a join-request row. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FFoundryInviteRowAction, const FString& /*Id*/);
	FFoundryInviteRowAction OnAccept;
	FFoundryInviteRowAction OnDecline;

	/** InJoined / InCap <= 0 hide the size; an unset InClock (an older fid) hides the expiry and
	 *  never greys Accept. */
	void Setup(const FString& InLeaderName, const FString& InPartyId, int32 InJoined = 0, int32 InCap = 0,
		const FoundryPartyRules::FInviteClock& InClock = FoundryPartyRules::FInviteClock());

	/** A leader's incoming join request: no size, no expiry. */
	void SetupJoinRequest(const FString& InRequesterId, const FString& InDisplayName);

	/** True for an invite whose expiry has passed. */
	bool IsExpiredNow() const { return bExpired; }

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI")
	FString LeaderName;

	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnRowUpdated();

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> LeaderText;
	/** "3 / 6" - folded into LeaderText when the WBP has no SizeText. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SizeText;
	/** "expires in 3 min" / "Expired"; hidden when the invite carries no expiry. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> ExpiryText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> AcceptButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> DeclineButton;

private:
	UFUNCTION() void HandleAccept();
	UFUNCTION() void HandleDecline();

	void RefreshExpiry();

	FString PartyId;
	FoundryPartyRules::FInviteClock Clock;
	bool bExpired = false;
	float ExpiryAccumulator = 0.0f;
};
