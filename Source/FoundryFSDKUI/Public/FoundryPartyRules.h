// Copyright Foundry Media. FoundryFSDK default UI kit: the party rules the social drawer, the rows,
// the friend menu and the match cluster all share - pure functions, no UObject, no subsystem reads.
//
// Why a separate namespace: the same three questions are asked from four widgets (how long does this
// invite live, does this party fit, which entries does this row's menu offer). Answering them in each
// widget is how the launcher and the game drifted apart ("one consistent model" - party-system-
// consistency.md); here each answer lives once and is pinned by an automation test.

#pragma once

#include "CoreMinimal.h"
#include "FoundryFriendRow.h" // EFoundryFriendRowKind
#include "FoundryInviteClock.h" // FInviteClock - the skew-proof countdown ExpiryText / IsExpired read

class UFoundrySocialSubsystem;

namespace FoundryPartyRules
{
	// -- invite lifetime -----------------------------------------------------------------------------

	/** "expires in 3 min" / "expires in 40 s" while live, "Expired" at 0, "" when RemainingSeconds < 0
	 *  (no expiry known - an older fid, or a JOINED member). RemainingSeconds comes from
	 *  FInviteClock::RemainingNow: anchored on when the UI received the row, never on the local wall
	 *  clock against fid's ExpiresAt (a fast client clock would expire every live invite). */
	FOUNDRYFSDKUI_API FString ExpiryText(double RemainingSeconds);

	/** True only for a KNOWN expiry that has run out (an unknown expiry never expires). */
	FOUNDRYFSDKUI_API bool IsExpired(double RemainingSeconds);

	// -- party cap -----------------------------------------------------------------------------------

	/**
	 * The cap a party is measured against in game: the selected queue's party cap when it is known
	 * (QueueCap >= 1; PartyCapForQueue answers -1 until the read lands), else the platform cap
	 * (PlatformMax >= 1; 0 when not in a party), else FallbackCap. D4a (Justin): "the queue decides the
	 * party size so changing gamemodes needs to take party size into account".
	 */
	FOUNDRYFSDKUI_API int32 EffectiveCap(int32 QueueCap, int32 PlatformMax, int32 FallbackCap);

	/** EffectiveCap fed from the social subsystem: the SELECTED mode's cap (PartyCapForQueue of
	 *  SelectedQueueKey), else the party's platform cap, else PartyCapacity(). The one number the drawer
	 *  header, the invite gate and the match cluster all measure the party against. */
	FOUNDRYFSDKUI_API int32 CapFor(const UFoundrySocialSubsystem& Social);

	/** Joined members above a KNOWN cap (Cap >= 1). fid never kicks anyone for it - the party just
	 *  cannot grow, and cannot queue for a mode that holds fewer. */
	FOUNDRYFSDKUI_API bool IsTooBig(int32 Joined, int32 Cap);

	/** An invite or join would not fit: joined + live invites already fill a known cap. */
	FOUNDRYFSDKUI_API bool IsFull(int32 Joined, int32 LiveInvited, int32 Cap);

	/** The drawer header's count: "3 / 4", or "8 / 4 - too big for this mode" when over. */
	FOUNDRYFSDKUI_API FString HeaderText(int32 Joined, int32 Cap);

	/** The reason pill on a disabled Invite: "Party full (4 / 4)". */
	FOUNDRYFSDKUI_API FString FullReason(int32 Count, int32 Cap);

	/** A mode row / status line when the party exceeds the mode's cap: "Party too big (8 > 4)". */
	FOUNDRYFSDKUI_API FString TooBigText(int32 Joined, int32 Cap);

	/** A friend row's status line: "In a party 2 / 6" (with the game when they are playing one:
	 *  "Playing Conquest - party 2 / 6"); empty when the friend is in no party, so the caller keeps
	 *  its own presence label. */
	FOUNDRYFSDKUI_API FString FriendPartyLine(const FString& PresenceGame, bool bInParty, int32 Size, int32 MaxSize);

	// -- which entries a row's context menu offers -----------------------------------------------------

	struct FMenuItemState
	{
		bool bShown = false;
		bool bEnabled = false;
		/** Shown as the kit-pill tooltip when the item is shown but disabled. */
		FString Reason;
	};

	/** Everything PlanFriendMenu needs to know, gathered by the drawer from the subsystem. */
	struct FFriendMenuContext
	{
		EFoundryFriendRowKind Kind = EFoundryFriendRowKind::Online;
		/** Kind == Party and the member has not joined yet (State INVITED). */
		bool bPendingMember = false;
		bool bSignedIn = false;
		bool bHasParty = false;
		/** True when the local player leads the party, or has none (they would lead the one an
		 *  invite creates). */
		bool bLocalLeader = true;
		/** The row's player is a member (joined or invited) of the LOCAL party. */
		bool bTargetInLocalParty = false;
		/** Pending rows: the invite's expiry has passed (it no longer counts against the cap). */
		bool bTargetInviteExpired = false;
		/** FFoundryFriend::bPartyJoinable - the friend is in a party with room that is not yours. */
		bool bFriendPartyJoinable = false;
		/** Joined + live invited members of the local party (the number a new invite adds to). */
		int32 PartyCount = 0;
		/** EffectiveCap(...) - 0 means unknown (never reads as full). */
		int32 Cap = 0;
	};

	struct FFriendMenuPlan
	{
		FMenuItemState SendMessage;
		FMenuItemState Invite;
		FMenuItemState ReInvite;
		FMenuItemState CancelInvite;
		FMenuItemState AskToJoin;
		FMenuItemState Kick;
		FMenuItemState RemoveFriend;
		FMenuItemState Block;
		FMenuItemState Unblock;

		/** False -> the drawer opens no menu at all (a member row the local player has nothing to do with). */
		bool AnyShown() const
		{
			return SendMessage.bShown || Invite.bShown || ReInvite.bShown || CancelInvite.bShown || AskToJoin.bShown
				|| Kick.bShown || RemoveFriend.bShown || Block.bShown || Unblock.bShown;
		}
	};

	FOUNDRYFSDKUI_API FFriendMenuPlan PlanFriendMenu(const FFriendMenuContext& Context);
}
