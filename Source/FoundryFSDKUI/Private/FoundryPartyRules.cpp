#include "FoundryPartyRules.h"

#include "FoundryQueueKey.h"
#include "FoundrySocialSubsystem.h"

namespace FoundryPartyRules
{
	FString ExpiryText(double RemainingSeconds)
	{
		if (RemainingSeconds < 0.0)
		{
			return FString();
		}
		if (RemainingSeconds <= 0.0)
		{
			return TEXT("Expired");
		}
		// Whole seconds rounded UP so a live invite never reads "0 s"; from a minute up it speaks in
		// minutes, also rounded up ("3 min" until the last 2 minutes tick over), like the launcher.
		const int32 Seconds = FMath::CeilToInt(RemainingSeconds);
		if (Seconds < 60)
		{
			return FString::Printf(TEXT("expires in %d s"), Seconds);
		}
		return FString::Printf(TEXT("expires in %d min"), FMath::CeilToInt(Seconds / 60.0));
	}

	bool IsExpired(double RemainingSeconds)
	{
		return RemainingSeconds >= 0.0 && RemainingSeconds <= 0.0;
	}

	int32 EffectiveCap(int32 QueueCap, int32 PlatformMax, int32 FallbackCap)
	{
		if (QueueCap >= 1)
		{
			return QueueCap;
		}
		if (PlatformMax >= 1)
		{
			return PlatformMax;
		}
		return FallbackCap;
	}

	int32 CapFor(const UFoundrySocialSubsystem& Social)
	{
		return EffectiveCap(Social.PartyCapForQueue(Social.SelectedQueueKey()), Social.Party().PlatformMaxSize, Social.PartyCapacity());
	}

	bool IsTooBig(int32 Joined, int32 Cap)
	{
		return Cap >= 1 && Joined > Cap;
	}

	bool IsFull(int32 Joined, int32 LiveInvited, int32 Cap)
	{
		return Cap >= 1 && Joined + LiveInvited >= Cap;
	}

	FString HeaderText(int32 Joined, int32 Cap)
	{
		return IsTooBig(Joined, Cap)
			? FString::Printf(TEXT("%d / %d - too big for this mode"), Joined, Cap)
			: FString::Printf(TEXT("%d / %d"), Joined, Cap);
	}

	FString FullReason(int32 Count, int32 Cap)
	{
		return FString::Printf(TEXT("Party full (%d / %d)"), Count, Cap);
	}

	FString TooBigText(int32 Joined, int32 Cap)
	{
		return FString::Printf(TEXT("Party too big (%d > %d)"), Joined, Cap);
	}

	FString FriendPartyLine(const FString& PresenceGame, bool bInParty, int32 Size, int32 MaxSize)
	{
		if (!bInParty)
		{
			return FString();
		}
		const FString Count = MaxSize > 0 ? FString::Printf(TEXT("%d / %d"), Size, MaxSize) : FString::FromInt(Size);
		return PresenceGame.IsEmpty()
			? FString::Printf(TEXT("In a party %s"), *Count)
			: FString::Printf(TEXT("Playing %s - party %s"), *PresenceGame, *Count);
	}

	FString GameSlugOfQueue(const FString& QueueKey)
	{
		// ONE parser for the core subsystems and this module (an FRN key's game is read from its queue/<game>/<mode>
		// tail; a bare id / UUID names no game) - the two can no longer disagree about which game a queue is.
		return FoundryQueueKey::GameSlug(QueueKey);
	}

	FString GameDisplayName(const FString& Slug)
	{
		FString Out;
		bool bWordStart = true;
		for (const TCHAR Ch : Slug)
		{
			if (Ch == TEXT('-') || Ch == TEXT('_') || Ch == TEXT(' '))
			{
				Out.AppendChar(TEXT(' '));
				bWordStart = true;
				continue;
			}
			Out.AppendChar(bWordStart ? FChar::ToUpper(Ch) : Ch);
			bWordStart = false;
		}
		return Out;
	}

	FString ReadyStatusText(bool bReady, const FString& ReadyGameSlug)
	{
		if (!bReady)
		{
			return TEXT("Not ready");
		}
		return ReadyGameSlug.IsEmpty() ? FString(TEXT("Ready")) : FString::Printf(TEXT("Ready in %s"), *GameDisplayName(ReadyGameSlug));
	}

	FString JoinNames(const TArray<FString>& Names)
	{
		if (Names.Num() == 0)
		{
			return FString();
		}
		if (Names.Num() == 1)
		{
			return Names[0];
		}
		FString Head;
		for (int32 i = 0; i < Names.Num() - 1; ++i)
		{
			Head += (i > 0 ? FString(TEXT(", ")) : FString()) + Names[i];
		}
		return FString::Printf(TEXT("%s and %s"), *Head, *Names.Last());
	}

	FFindMatchGate PlanFindMatch(const FFindMatchGateInput& In)
	{
		FFindMatchGate Gate;
		if (In.Joined < 2)
		{
			return Gate; // solo / a party of one: nothing to wait for
		}
		Gate.bGated = true;

		if (!In.bLocalLeader)
		{
			// Only the leader's Find Match queues the party (fid creates every member's ticket from it).
			Gate.bShowButton = false;
			Gate.bEnabled = false;
			Gate.Status = FString::Printf(TEXT("Waiting for %s to start"), In.LeaderName.IsEmpty() ? TEXT("the party leader") : *In.LeaderName);
			return Gate;
		}

		// The count comes from the same names the status line shows, so the two can never disagree; a blocked
		// gate with no names still means at least one member is not ready, so it never reads "2 / 2" disabled.
		const int32 Ready = In.bAllReady ? In.Joined : FMath::Clamp(In.Joined - FMath::Max(1, In.UnreadyNames.Num()), 0, In.Joined);
		Gate.Label = FString::Printf(TEXT("FIND MATCH (READY %d / %d)"), Ready, In.Joined);
		Gate.bEnabled = In.bAllReady;
		if (!In.bAllReady)
		{
			Gate.Status = In.UnreadyNames.Num() > 0
				? FString::Printf(TEXT("Waiting for %s to ready up"), *JoinNames(In.UnreadyNames))
				: FString(TEXT("Waiting for everyone to ready up"));
		}
		return Gate;
	}

	FFriendMenuPlan PlanFriendMenu(const FFriendMenuContext& C)
	{
		FFriendMenuPlan Plan;
		auto Show = [](FMenuItemState& Item, bool bEnabled, const FString& Reason)
		{
			Item.bShown = true;
			Item.bEnabled = bEnabled;
			Item.Reason = bEnabled ? FString() : Reason;
		};

		const bool bFriendRow = C.Kind == EFoundryFriendRowKind::Online || C.Kind == EFoundryFriendRowKind::Offline;
		const bool bOnline = C.Kind == EFoundryFriendRowKind::Online;
		const bool bFull = C.Cap >= 1 && C.PartyCount >= C.Cap;

		if (C.Kind == EFoundryFriendRowKind::Blocked)
		{
			Show(Plan.Unblock, true, FString());
			return Plan;
		}

		if (C.Kind == EFoundryFriendRowKind::Party)
		{
			// Only the leader has anything to do with another member's row; everyone else gets no menu
			// for an invitee and just a whisper for a joined member.
			const bool bLeaderActs = C.bHasParty && C.bLocalLeader;
			if (C.bPendingMember)
			{
				if (bLeaderActs)
				{
					// Re-inviting a LIVE invite never changes the count (the row is already in it); only an
					// expired one rejoins the count, so only then can a full party refuse it.
					const bool bRefusedFull = bFull && C.bTargetInviteExpired;
					Show(Plan.ReInvite, C.bSignedIn && !bRefusedFull,
						!C.bSignedIn ? FString(TEXT("Sign in to invite friends.")) : FullReason(C.PartyCount, C.Cap));
					Show(Plan.CancelInvite, true, FString());
				}
			}
			else
			{
				Show(Plan.SendMessage, true, FString());
				if (bLeaderActs)
				{
					Show(Plan.Kick, true, FString());
				}
			}
			return Plan;
		}

		if (bFriendRow)
		{
			Show(Plan.SendMessage, true, FString());

			// Same precedence the drawer always used, so the reason matches why it is actually disabled.
			const bool bCanInvite = bOnline && C.bSignedIn && !bFull && C.bLocalLeader && !C.bTargetInLocalParty;
			FString InviteReason;
			if (!bOnline) { InviteReason = TEXT("They're offline."); }
			else if (!C.bSignedIn) { InviteReason = TEXT("Sign in to invite friends."); }
			else if (C.bTargetInLocalParty) { InviteReason = TEXT("They're already in your party."); }
			else if (bFull) { InviteReason = FullReason(C.PartyCount, C.Cap); }
			else if (!C.bLocalLeader) { InviteReason = TEXT("Only the host can invite."); }
			Show(Plan.Invite, bCanInvite, InviteReason);

			// Ask to join exists only for a friend whose party can take you (fid's joinable flag) - never a
			// dead-end entry for a friend who is in no party. Being in THEIR party already disables it.
			if (C.bFriendPartyJoinable)
			{
				const bool bCanAsk = C.bSignedIn && !C.bTargetInLocalParty;
				Show(Plan.AskToJoin, bCanAsk,
					!C.bSignedIn ? FString(TEXT("Sign in to ask to join a party.")) : FString(TEXT("You're already in their party.")));
			}

			Show(Plan.RemoveFriend, true, FString());
			Show(Plan.Block, true, FString());
		}
		return Plan;
	}
}
