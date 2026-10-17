// Copyright Foundry Media. See FoundryQueueKey.h.

#include "FoundryQueueKey.h"

FString FoundryQueueKey::GameSlug(const FString& QueueKey)
{
	const FString Key = QueueKey.TrimStartAndEnd();
	if (Key.StartsWith(TEXT("frn:"), ESearchCase::IgnoreCase))
	{
		// frn:fmms:<org>:queue/<game>/<mode>: the game is the segment right after "queue/". The org and
		// service segments never contain '/', so the first "queue/" is the resource tail.
		const int32 Tail = Key.Find(TEXT("queue/"), ESearchCase::IgnoreCase);
		if (Tail == INDEX_NONE)
		{
			return FString();
		}
		const FString After = Key.Mid(Tail + 6);
		int32 Slash = INDEX_NONE;
		return After.FindChar(TEXT('/'), Slash) ? After.Left(Slash).TrimStartAndEnd() : FString();
	}
	int32 Slash = INDEX_NONE;
	return Key.FindChar(TEXT('/'), Slash) ? Key.Left(Slash).TrimStartAndEnd() : FString();
}
