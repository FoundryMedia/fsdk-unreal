// Copyright Foundry Media. FoundryFSDK default UI kit: a skew-proof countdown for a party invite.
//
// Why not compare fid's ExpiresAt with the local clock: a client clock that runs fast marks every live
// invite expired (and undercounts live invites against the cap); a slow one keeps a dead invite
// "live". fid only returns an invite while it is live, so the clock is anchored on the moment the UI
// first RECEIVED the row: remaining = (ExpiresAt - InvitedAt) - (seconds elapsed since then), floor 0.
// ExpiresAt and InvitedAt are both fid's own timestamps, so their difference (the lifetime) is exact
// whatever the client clock says; only a MONOTONIC elapsed time is taken from the client.
//
// Known limit: the anchor is first-sight, so an invite that first reaches the client late (a game
// that was closed when it was sent) reads a little longer than it has left. fid still refuses a dead
// accept (410) and stops returning the row; an exact figure needs a server-now stamp on the wire.

#pragma once

#include "CoreMinimal.h"

namespace FoundryPartyRules
{
	struct FInviteClock
	{
		/** Seconds left at AnchorSeconds; < 0 = no expiry known (unset). */
		double RemainingAtAnchor = -1.0;
		/** FPlatformTime::Seconds() when the UI first received this row (monotonic, not wall time). */
		double AnchorSeconds = 0.0;

		bool IsSet() const { return RemainingAtAnchor >= 0.0; }

		/** Seconds left at NowSeconds (same monotonic clock as AnchorSeconds); floor 0; -1 when unset. */
		double RemainingNow(double NowSeconds) const
		{
			if (!IsSet())
			{
				return -1.0;
			}
			return FMath::Max(0.0, RemainingAtAnchor - (NowSeconds - AnchorSeconds));
		}

		/** Unset when either timestamp is absent (an older fid, or a JOINED member): the lifetime is
		 *  then unknown, so the row shows no expiry rather than guessing. */
		static FInviteClock From(const FDateTime& ExpiresAt, const FDateTime& InvitedAt, double AnchorSeconds)
		{
			FInviteClock Clock;
			if (ExpiresAt.GetTicks() == 0 || InvitedAt.GetTicks() == 0)
			{
				return Clock;
			}
			Clock.RemainingAtAnchor = FMath::Max(0.0, (ExpiresAt - InvitedAt).GetTotalSeconds());
			Clock.AnchorSeconds = AnchorSeconds;
			return Clock;
		}
	};
}
