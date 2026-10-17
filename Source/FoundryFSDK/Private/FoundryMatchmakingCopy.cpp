// Copyright Foundry Media. Player-facing matchmaking copy (S5 "matchmaking wait").

#include "FoundryMatchmakingCopy.h"

TMap<EFoundryFsdkResult, FText> FoundryMatchmakingCopy::ResultOverrides;
TMap<EFoundryWaitReason, FText> FoundryMatchmakingCopy::WaitOverrides;

namespace
{
	/** Default English copy for a terminal result. Every entry here is a complete, numeric-code-free
	 *  sentence - callers log the code separately. */
	FText DefaultForResult(EFoundryFsdkResult Result)
	{
		switch (Result)
		{
		case EFoundryFsdkResult::Network:
		case EFoundryFsdkResult::Timeout:
			return FText::FromString(TEXT("Can't reach matchmaking. Check your connection and try again."));
		case EFoundryFsdkResult::NotAuthenticated:
		case EFoundryFsdkResult::Unauthorized:
			return FText::FromString(TEXT("Sign in again to play online."));
		case EFoundryFsdkResult::RateLimited:
			return FText::FromString(TEXT("Slow down - try again in a moment."));
		case EFoundryFsdkResult::Unavailable:
			// The server rejected the search up front (503, before any ticket/wait existed) - distinct
			// from the QUEUED-ticket NoCapacity wait reason below, which keeps searching instead.
			return FText::FromString(TEXT("No game servers are available for this queue right now. Please try again shortly."));
		case EFoundryFsdkResult::Ok:
		case EFoundryFsdkResult::InvalidArg:
		case EFoundryFsdkResult::Protocol:
		case EFoundryFsdkResult::NoMatch:
		case EFoundryFsdkResult::NotImplemented:
		case EFoundryFsdkResult::Internal:
		case EFoundryFsdkResult::Unknown:
		default:
			return FText::FromString(TEXT("Matchmaking hit a snag. Try again."));
		}
	}

	/** Default English copy for a wait reason's BASE sentence (no estimate suffix - ForWait appends
	 *  that separately for CapacityStarting). */
	FText DefaultForWaitBase(EFoundryWaitReason Reason)
	{
		switch (Reason)
		{
		case EFoundryWaitReason::CapacityStarting:
			// "usually a few minutes": fid's estimate suffix ("(~N min left)") is now measured per fleet (the
			// scaler's own boot samples; us-east-2 booted in 3m23s on 2026-10-16) - the old "up to 15 minutes"
			// read the 612 s cold-start ceiling as a promise and sat on screen while the match was already placed.
			return FText::FromString(TEXT("Low traffic right now - we're starting a server for you. This usually takes a few minutes. Keep this open."));
		case EFoundryWaitReason::NoCapacity:
			return FText::FromString(TEXT("All servers are full right now. We'll keep trying - or check back in a few minutes."));
		case EFoundryWaitReason::NoFleet:
			return FText::FromString(TEXT("This game has no servers in your region yet."));
		case EFoundryWaitReason::Searching:
		default:
			return FText::FromString(TEXT("Searching for players..."));
		}
	}
}

FText FoundryMatchmakingCopy::ForResult(EFoundryFsdkResult Result)
{
	if (const FText* Override = ResultOverrides.Find(Result))
	{
		if (!Override->IsEmpty())
		{
			return *Override;
		}
	}
	return DefaultForResult(Result);
}

FText FoundryMatchmakingCopy::ForWait(EFoundryWaitReason Reason, int32 EstimateSeconds)
{
	FText Base;
	if (const FText* Override = WaitOverrides.Find(Reason))
	{
		Base = Override->IsEmpty() ? DefaultForWaitBase(Reason) : *Override;
	}
	else
	{
		Base = DefaultForWaitBase(Reason);
	}

	if (Reason == EFoundryWaitReason::CapacityStarting && EstimateSeconds > 0)
	{
		const int32 Minutes = FMath::Max(1, (EstimateSeconds + 59) / 60);
		return FText::FromString(FString::Printf(TEXT("%s (~%d min left)"), *Base.ToString(), Minutes));
	}
	return Base;
}

FText FoundryMatchmakingCopy::Expired()
{
	return FText::FromString(TEXT("No match found in time. Try again."));
}

void FoundryMatchmakingCopy::SetResultOverride(EFoundryFsdkResult Result, const FText& Text)
{
	ResultOverrides.Add(Result, Text);
}

void FoundryMatchmakingCopy::SetWaitOverride(EFoundryWaitReason Reason, const FText& Text)
{
	WaitOverrides.Add(Reason, Text);
}
