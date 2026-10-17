// Copyright Foundry Media. fsdk-core social rows -> the Blueprint-facing structs.
//
// One home for the C-row -> USTRUCT conversions so the bridge's six social readers and the automation
// test share the SAME code (the test pins ISO parsing and the party/friend fields without a network).
// Private header: it pulls in the C ABI, which no public header may.

#pragma once

#include "CoreMinimal.h"
#include "FoundryFSDKSubsystem.h"
#include "foundry/fsdk.h"

namespace FoundrySocialConvert
{
	/** fid's ISO-8601 timestamp (UTC, "2026-10-16T22:15:00Z") -> FDateTime. A zero FDateTime (0 ticks)
	 *  means "absent": an empty/NULL string (a JOINED member has no invite clock; an older fid sends none)
	 *  or text that is not a timestamp. Callers treat zero as "no clock", never as the year 0001. */
	inline FDateTime ParseIso8601Utc(const char* Utf8)
	{
		FDateTime Out(0);
		if (Utf8 != nullptr && Utf8[0] != '\0' && FDateTime::ParseIso8601(UTF8_TO_TCHAR(Utf8), Out))
		{
			return Out;
		}
		return FDateTime(0);
	}

	/** A friends-list / pending / blocked / join-request row. The party fields stay zero on the rows
	 *  that carry none (the core zeroes them). */
	inline FFoundryFriend Friend(const fsdk_friend& Row)
	{
		FFoundryFriend F;
		F.FoundryId = UTF8_TO_TCHAR(Row.foundry_id);
		F.DisplayName = UTF8_TO_TCHAR(Row.display_name);
		F.Username = UTF8_TO_TCHAR(Row.username);
		F.Presence = UTF8_TO_TCHAR(Row.presence);
		F.PresenceGame = UTF8_TO_TCHAR(Row.presence_game);
		F.bInParty = Row.in_party != 0;
		F.PartySize = Row.party_size;
		F.PartyMaxSize = Row.party_max_size;
		F.bPartyJoinable = Row.party_joinable != 0;
		return F;
	}

	/** A pending join request on the party I lead (the row fid returns is the friend-row shape). */
	inline FFoundryJoinRequest JoinRequest(const fsdk_friend& Row)
	{
		FFoundryJoinRequest R;
		R.FoundryId = UTF8_TO_TCHAR(Row.foundry_id);
		R.DisplayName = UTF8_TO_TCHAR(Row.display_name);
		R.Username = UTF8_TO_TCHAR(Row.username);
		return R;
	}

	inline FFoundryPartyMember Member(const fsdk_party_member& Row)
	{
		FFoundryPartyMember M;
		M.FoundryId = UTF8_TO_TCHAR(Row.foundry_id);
		M.DisplayName = UTF8_TO_TCHAR(Row.display_name);
		M.Username = UTF8_TO_TCHAR(Row.username);
		M.State = UTF8_TO_TCHAR(Row.state);
		M.bReady = Row.ready != 0;
		M.ReadyGameSlug = UTF8_TO_TCHAR(Row.ready_game_slug);
		M.bInMatch = Row.in_match != 0;
		M.InvitedAt = ParseIso8601Utc(Row.invited_at);
		M.ExpiresAt = ParseIso8601Utc(Row.expires_at);
		return M;
	}

	/** The party header (no members). On an /invites row the caller's own invite clock lands in
	 *  InvitedAt / InviteExpiresAt; on a /mine row both stay zero. */
	inline void PartyHeader(const fsdk_party& Row, FFoundryParty& Out)
	{
		Out.PartyId = UTF8_TO_TCHAR(Row.id);
		Out.LeaderFoundryId = UTF8_TO_TCHAR(Row.leader_foundry_id);
		Out.LeaderDisplayName = UTF8_TO_TCHAR(Row.leader_display_name);
		Out.MaxSize = Row.max_size;
		Out.PlatformMaxSize = Row.platform_max_size;
		Out.InvitedAt = ParseIso8601Utc(Row.my_invited_at);
		Out.InviteExpiresAt = ParseIso8601Utc(Row.my_expires_at);
	}
}
