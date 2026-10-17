// Copyright Foundry Media. ONE parser for "which game does this queue key belong to".

#pragma once

#include "CoreMinimal.h"

namespace FoundryQueueKey
{
	/**
	 * The game slug a queue key names - the slug fid checks every party member's ready flag against, and the
	 * game a followed ticket must belong to. Shared by the core subsystems and the UI kit so they cannot drift:
	 *   "goo-crew/solo"                          -> "goo-crew"   ({game}/{mode}; only the first '/' splits)
	 *   "frn:fmms:<org>:queue/goo-crew/solo"     -> "goo-crew"   (an FRN: the game of its queue/<game>/<mode> tail)
	 *   "3f2a9c..." (a bare id / UUID / name)    -> ""           (the client cannot name the game: defer to fid)
	 * Whitespace is trimmed; an FRN that is not a queue FRN is "" too.
	 */
	FOUNDRYFSDK_API FString GameSlug(const FString& QueueKey);
}
