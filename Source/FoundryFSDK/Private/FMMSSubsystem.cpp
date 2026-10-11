// Copyright Foundry Media. FMMS matchmaking orchestrator.

#include "FMMSSubsystem.h"

#include "FoundryMatchmakingCopy.h"
#include "foundry/fsdk.h" // fsdk_wait_reason_str (diagnostics only - never player-facing text)

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/DateTime.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogFMMS, Log, All);

void UFMMSSubsystem::Deinitialize()
{
	StopPolling();
	Super::Deinitialize();
}

UFoundryFSDKSubsystem* UFMMSSubsystem::Fsdk() const
{
	UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
}

void UFMMSSubsystem::BindOnce(UFoundryFSDKSubsystem* Sdk)
{
	if (bBound || Sdk == nullptr)
	{
		return;
	}
	Sdk->OnAuthenticateComplete.AddDynamic(this, &UFMMSSubsystem::HandleAuth);
	Sdk->OnRequestMatchComplete.AddDynamic(this, &UFMMSSubsystem::HandleRequest);
	Sdk->OnPollMatchComplete.AddDynamic(this, &UFMMSSubsystem::HandlePoll);
	Sdk->OnGetConnectionComplete.AddDynamic(this, &UFMMSSubsystem::HandleConnection);
	Sdk->OnMySessionComplete.AddDynamic(this, &UFMMSSubsystem::HandleMySession);
	bBound = true;
}

bool UFMMSSubsystem::ThrottleFindMatch()
{
	const double NowSec = FPlatformTime::Seconds();
	if (NowSec - LastFindMatchSeconds < FindMatchMinIntervalSeconds)
	{
		UE_LOG(LogFMMS, Warning, TEXT("FindMatch ignored - too soon after the last attempt (anti-spam)."));
		return false;
	}
	LastFindMatchSeconds = NowSec;
	return true;
}

void UFMMSSubsystem::FindMatch(const FString& Queue, const FString& AttributesJson,
                               const FString& PlayerToken, const FString& ApiBaseUrl)
{
	if (Phase != EFMMSPhase::Idle && Phase != EFMMSPhase::Failed && Phase != EFMMSPhase::Reconnectable)
	{
		UE_LOG(LogFMMS, Warning, TEXT("FindMatch ignored - flow already in progress (phase %d)"), (int32)Phase);
		return;
	}
	if (!ThrottleFindMatch())
	{
		return;
	}
	AbandonReconnectSeat(); // searching fresh from Reconnectable = declining the old seat
	CurrentMatchId.Reset(); // a fresh search invalidates any previously known match
	if (Queue.IsEmpty())
	{
		Fail(TEXT("No queue specified."));
		return;
	}
	if (PlayerToken.IsEmpty())
	{
		Fail(TEXT("No player token. Sign in (or pass -FoundryToken=)."));
		return;
	}
	UFoundryFSDKSubsystem* Sdk = Fsdk();
	if (Sdk == nullptr)
	{
		Fail(TEXT("FoundryFSDK subsystem unavailable."));
		return;
	}
	const FString Base = ApiBaseUrl.IsEmpty() ? TEXT("https://api.foundryplatform.app") : ApiBaseUrl;
	if (!Sdk->InitializeClient(Base))
	{
		Fail(TEXT("Failed to initialize the Foundry client."));
		return;
	}
	BindOnce(Sdk);
	PendingQueue = Queue;
	PendingAttributes = AttributesJson.IsEmpty() ? TEXT("{}") : AttributesJson;
	SetPhase(EFMMSPhase::Authenticating, TEXT("Authenticating..."));
	Sdk->Authenticate(PlayerToken);
}

void UFMMSSubsystem::FindMatchAuthenticated(const FString& Queue, const FString& AttributesJson)
{
	if (Phase != EFMMSPhase::Idle && Phase != EFMMSPhase::Failed && Phase != EFMMSPhase::Reconnectable)
	{
		UE_LOG(LogFMMS, Warning, TEXT("FindMatchAuthenticated ignored - flow already in progress (phase %d)"), (int32)Phase);
		return;
	}
	if (!ThrottleFindMatch())
	{
		return;
	}
	AbandonReconnectSeat(); // searching fresh from Reconnectable = declining the old seat
	CurrentMatchId.Reset(); // a fresh search invalidates any previously known match
	if (Queue.IsEmpty())
	{
		Fail(TEXT("No queue specified."));
		return;
	}
	UFoundryFSDKSubsystem* Sdk = Fsdk();
	if (Sdk == nullptr)
	{
		Fail(TEXT("FoundryFSDK subsystem unavailable."));
		return;
	}
	if (!Sdk->IsLoggedIn())
	{
		Fail(TEXT("Not signed in. Log in first (Login / TryResumeSession)."));
		return;
	}
	// The client already exists + is authenticated from login, so skip
	// InitializeClient + Authenticate and go straight to the match request.
	BindOnce(Sdk);
	PendingQueue = Queue;
	PendingAttributes = AttributesJson.IsEmpty() ? TEXT("{}") : AttributesJson;
	SetPhase(EFMMSPhase::Requesting, TEXT("Checking regions and requesting match..."));
	Sdk->RequestMatch(PendingQueue, PendingAttributes);
}

void UFMMSSubsystem::Cancel()
{
	StopPolling();
	if (Phase == EFMMSPhase::Requesting || Phase == EFMMSPhase::Searching
		|| Phase == EFMMSPhase::WaitingForCapacity || Phase == EFMMSPhase::Connecting)
	{
		if (UFoundryFSDKSubsystem* Sdk = Fsdk())
		{
			Sdk->CancelMatch();
		}
	}
	AbandonReconnectSeat(); // Cancel from Reconnectable = decline the live seat
	CurrentMatchId.Reset();
	SetPhase(EFMMSPhase::Idle, TEXT("Cancelled."));
}

/** Decline any pending reconnect seat: cancel its ticket server-side + forget it. */
void UFMMSSubsystem::AbandonReconnectSeat()
{
	if (ReconnectTicketId.IsEmpty())
	{
		return;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->AbandonMatch(ReconnectTicketId);
	}
	ReconnectTicketId.Reset();
	CurrentMatchId.Reset(); // the declined seat's match is no longer ours
}

void UFMMSSubsystem::CheckActiveSession()
{
	// Menu-entry recovery. After a server-initiated kick (operator stop, crash,
	// network loss) the local phase is STALE - Traveling/Connecting with no flow
	// actually running - so ask the PLATFORM, the only party that knows whether
	// the match is still live. Never clobber a genuinely in-flight search.
	if (Phase == EFMMSPhase::Requesting || Phase == EFMMSPhase::Searching || Phase == EFMMSPhase::WaitingForCapacity)
	{
		return;
	}
	UFoundryFSDKSubsystem* Sdk = Fsdk();
	if (Sdk == nullptr || !Sdk->IsLoggedIn())
	{
		return;
	}
	BindOnce(Sdk);
	Sdk->QueryMySession();
}

void UFMMSSubsystem::Reconnect()
{
	if (Phase != EFMMSPhase::Reconnectable || ReconnectTicketId.IsEmpty())
	{
		UE_LOG(LogFMMS, Warning, TEXT("Reconnect ignored - no reconnectable match."));
		return;
	}
	UFoundryFSDKSubsystem* Sdk = Fsdk();
	if (Sdk == nullptr)
	{
		Fail(TEXT("FoundryFSDK subsystem unavailable."));
		return;
	}
	BindOnce(Sdk);
	SetPhase(EFMMSPhase::Connecting, TEXT("Reconnecting to your match..."));
	Sdk->ReconnectMatch(ReconnectTicketId);
	ReconnectTicketId.Reset(); // consumed; the connection flow owns it from here
}

void UFMMSSubsystem::HandleMySession(EFoundryFsdkResult Result, FFoundrySessionSeat Seat)
{
	// Record the live match id REGARDLESS of the phase machine below - it is
	// informational (feeds GetCurrentMatchId for in-match match/team chat) and
	// my-session is the only client-side source of it (the connection payload
	// deliberately carries none).
	if (Result == EFoundryFsdkResult::Ok && Seat.bActive)
	{
		CurrentMatchId = Seat.MatchId;
	}
	if (Phase == EFMMSPhase::Requesting || Phase == EFMMSPhase::Searching
		|| Phase == EFMMSPhase::WaitingForCapacity || Phase == EFMMSPhase::Connecting)
	{
		return; // a live flow started meanwhile - its handlers own the phase
	}
	if (Result == EFoundryFsdkResult::Ok && Seat.bActive && !Seat.TicketId.IsEmpty())
	{
		ReconnectTicketId = Seat.TicketId;
		SetPhase(EFMMSPhase::Reconnectable, TEXT("You have a match in progress."));
		return;
	}
	// No live seat (incl. old-fid 404 / transient failure): the match is gone, so
	// any stale local state must not brick Find Match - reset to Idle.
	ReconnectTicketId.Reset();
	if (Result == EFoundryFsdkResult::Ok && !Seat.bActive)
	{
		CurrentMatchId.Reset(); // definitive platform answer: no live seat
	}
	if (Phase != EFMMSPhase::Idle && Phase != EFMMSPhase::Failed)
	{
		SetPhase(EFMMSPhase::Idle, TEXT("Ready."));
	}
}

void UFMMSSubsystem::HandleAuth(EFoundryFsdkResult Result)
{
	if (Phase != EFMMSPhase::Authenticating)
	{
		return;
	}
	if (Result != EFoundryFsdkResult::Ok)
	{
		FailWithResult(Result);
		return;
	}
	UFoundryFSDKSubsystem* Sdk = Fsdk();
	if (Sdk == nullptr)
	{
		Fail(TEXT("SDK unavailable."));
		return;
	}
	SetPhase(EFMMSPhase::Requesting, TEXT("Checking regions and requesting match..."));
	Sdk->RequestMatch(PendingQueue, PendingAttributes);
}

void UFMMSSubsystem::HandleRequest(EFoundryFsdkResult Result)
{
	if (Phase != EFMMSPhase::Requesting)
	{
		return;
	}
	if (Result != EFoundryFsdkResult::Ok)
	{
		// Covers the server rejecting the search up front too (503/Unavailable: no capacity for this
		// queue) - stop here with a human message rather than searching forever, a match could never
		// form right now. FoundryMatchmakingCopy::ForResult carries that exact sentence.
		FailWithResult(Result);
		return;
	}
	// A fresh ticket: reset the client-side wait ceiling's clock (S5) - HasExceededWaitCeiling falls
	// back to this when the server hasn't told us an expiresAt yet.
	SearchStartedSeconds = FPlatformTime::Seconds();
	TicketExpiresAtUnix = 0;
	SetPhase(EFMMSPhase::Searching, FoundryMatchmakingCopy::ForWait(EFoundryWaitReason::Searching, 0).ToString());
	Poll(); // immediate first poll - its OWN completion decides the next poll's cadence (S5)
}

void UFMMSSubsystem::HandlePoll(EFoundryFsdkResult Result, EFoundryMatchStatus Status, FFoundryTicketWait Wait)
{
	if (Phase != EFMMSPhase::Searching && Phase != EFMMSPhase::WaitingForCapacity)
	{
		return;
	}

	// Track the ticket's own TTL as soon as the server reports one (S5) - the wait ceiling below
	// prefers it over the flat NoExpiryWaitCeilingSeconds fallback.
	if (Wait.ExpiresAtUnix > 0)
	{
		TicketExpiresAtUnix = Wait.ExpiresAtUnix;
	}

	// Log level, every poll: the numeric result/status/wait-reason + its diagnostic string - NEVER
	// the player-facing text below, which comes from FoundryMatchmakingCopy alone.
	UE_LOG(LogFMMS, Log, TEXT("[FMMS] poll result=%d status=%d wait_reason=%s(%d) estimate=%ds retry=%ds"),
		(int32)Result, (int32)Status,
		UTF8_TO_TCHAR(fsdk_wait_reason_str((int32)Wait.Reason)), (int32)Wait.Reason,
		Wait.EstimateSeconds, Wait.RetryAfterSeconds);

	// The client-side ceiling (S5): never wait on a QUEUED ticket forever, even while the server
	// keeps answering QUEUED/RateLimited.
	if (HasExceededWaitCeiling())
	{
		Fail(FoundryMatchmakingCopy::Expired().ToString());
		return;
	}

	if (Result == EFoundryFsdkResult::RateLimited)
	{
		// The server's poll backstop (we arrived sooner than its own retryAfterSeconds) - keep
		// waiting, re-arm with ITS cadence (Wait.RetryAfterSeconds was just refreshed from the
		// response's Retry-After header). Never a failure.
		StartPolling(ClampPollInterval(Wait.RetryAfterSeconds));
		return;
	}
	if (Result == EFoundryFsdkResult::Network || Result == EFoundryFsdkResult::Timeout)
	{
		StartPolling(PollIntervalSeconds); // transient - keep polling at the default cadence
		return;
	}
	if (Result != EFoundryFsdkResult::Ok)
	{
		Fail(FoundryMatchmakingCopy::ForResult(Result).ToString());
		return;
	}

	switch (Status)
	{
	case EFoundryMatchStatus::Found:
	{
		StopPolling();
		UFoundryFSDKSubsystem* Sdk = Fsdk();
		if (Sdk == nullptr)
		{
			Fail(TEXT("SDK unavailable."));
			return;
		}
		SetPhase(EFMMSPhase::Connecting, TEXT("Match found - getting connection..."));
		Sdk->GetConnection();
		return;
	}
	case EFoundryMatchStatus::Searching:
	case EFoundryMatchStatus::Pending:
		break; // keep waiting - the wait-reason dispatch below picks the exact phase/message
	case EFoundryMatchStatus::Cancelled:
		Fail(TEXT("Match was cancelled."));
		return;
	case EFoundryMatchStatus::Expired:
		Fail(FoundryMatchmakingCopy::Expired().ToString());
		return;
	default:
		Fail(TEXT("Matchmaking failed."));
		return;
	}

	// Still QUEUED: the server-told wait reason decides whether this reads as a plain search, a
	// calmer "we're starting a server for you" wait, or an outright refusal (no fleet at all).
	switch (Wait.Reason)
	{
	case EFoundryWaitReason::NoFleet:
		Fail(FoundryMatchmakingCopy::ForWait(EFoundryWaitReason::NoFleet, 0).ToString());
		return;
	case EFoundryWaitReason::CapacityStarting:
	case EFoundryWaitReason::NoCapacity:
		SetPhase(EFMMSPhase::WaitingForCapacity, FoundryMatchmakingCopy::ForWait(Wait.Reason, Wait.EstimateSeconds).ToString());
		break;
	case EFoundryWaitReason::Searching:
	default:
		SetPhase(EFMMSPhase::Searching, FoundryMatchmakingCopy::ForWait(EFoundryWaitReason::Searching, 0).ToString());
		break;
	}

	StartPolling(ClampPollInterval(Wait.RetryAfterSeconds));
}

void UFMMSSubsystem::HandleConnection(EFoundryFsdkResult Result, FFoundryConnection Connection)
{
	if (Phase != EFMMSPhase::Connecting)
	{
		return;
	}
	if (Result == EFoundryFsdkResult::NoMatch)
	{
		// Endpoint not ready yet (match forming) - resume polling at the default cadence (we have no
		// fresh FFoundryTicketWait here; the next poll resumes server-driven cadence as normal).
		SetPhase(EFMMSPhase::Searching, TEXT("Match forming - waiting for server..."));
		StartPolling(PollIntervalSeconds);
		return;
	}
	if (Result != EFoundryFsdkResult::Ok || Connection.Ip.IsEmpty() || Connection.Port <= 0)
	{
		FailWithResult(Result);
		return;
	}
	UGameInstance* GI = GetGameInstance();
	APlayerController* PC = GI ? GI->GetFirstLocalPlayerController(GetWorld()) : nullptr;
	if (PC == nullptr)
	{
		Fail(TEXT("No local player controller to travel."));
		return;
	}
	// Deliberately NO endpoint in the user-facing message: the server IP must never
	// hit screens, streams, or captured logs (DDoS surface - see fsdk-security).
	SetPhase(EFMMSPhase::Traveling, TEXT("Match found - joining server..."));
	// Ride the player's display name as UE's native ?Name= option so PlayerState/nametags
	// never default to the OS machine name. COSMETIC layer only: the server overrides it
	// from the match token's SIGNED display_name claim when present. Sanitized: URL option
	// separators stripped, so it can never smuggle extra options (e.g. a second ?token=).
	FString NameOption;
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		FString SessionName = Sdk->GetDisplayName();
		SessionName.ReplaceInline(TEXT("?"), TEXT(""));
		SessionName.ReplaceInline(TEXT("="), TEXT(""));
		SessionName.ReplaceInline(TEXT("&"), TEXT(""));
		SessionName = SessionName.TrimStartAndEnd().Left(64);
		if (!SessionName.IsEmpty())
		{
			NameOption = FString::Printf(TEXT("?Name=%s"), *SessionName);
		}
	}
	// The URL carries the match token AND the endpoint - do NOT log it.
	const FString Url = FString::Printf(TEXT("%s:%d?token=%s%s"),
	                                    *Connection.Ip, Connection.Port, *Connection.MatchToken, *NameOption);
	PC->ClientTravel(Url, TRAVEL_Absolute);
}

void UFMMSSubsystem::Poll()
{
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->PollMatch();
	}
}

void UFMMSSubsystem::StartPolling(float IntervalSeconds)
{
	if (UGameInstance* GI = GetGameInstance())
	{
		// ONE-SHOT (bLoop=false): every poll re-arms its own next tick with the cadence IT decided
		// (S5 server-driven retry), so there is no fixed loop interval to fall back on.
		GI->GetTimerManager().SetTimer(PollTimer, this, &UFMMSSubsystem::Poll, IntervalSeconds, false);
	}
}

void UFMMSSubsystem::StopPolling()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(PollTimer);
	}
}

void UFMMSSubsystem::SetPhase(EFMMSPhase NewPhase, const FString& Message)
{
	Phase = NewPhase;
	UE_LOG(LogFMMS, Log, TEXT("[FMMS] phase=%d %s"), (int32)NewPhase, *Message);
	// The dev console mirrors these itself: it subscribes OnFMMSStatus at Initialize
	// (HandleFmmsStatus -> "[fmms] ..."), which survives Shipping (event-driven, not a
	// UE_LOG capture). Do NOT also Print() here - that double-writes every phase line
	// (the duplicate-console-lines bug, caught live 2026-08-02).
	OnFMMSStatus.Broadcast(NewPhase, Message);
}

void UFMMSSubsystem::Fail(const FString& Message)
{
	StopPolling();
	SetPhase(EFMMSPhase::Failed, Message);
}

void UFMMSSubsystem::FailWithResult(EFoundryFsdkResult Result)
{
	// The numeric code is diagnostics-only - it goes here, never into the player-facing text below.
	UE_LOG(LogFMMS, Log, TEXT("[FMMS] result=%d"), (int32)Result);
	Fail(FoundryMatchmakingCopy::ForResult(Result).ToString());
}

float UFMMSSubsystem::ClampPollInterval(int32 RetryAfterSeconds)
{
	return static_cast<float>(FMath::Clamp(RetryAfterSeconds, MinPollIntervalSeconds, MaxPollIntervalSeconds));
}

bool UFMMSSubsystem::HasExceededWaitCeiling() const
{
	if (TicketExpiresAtUnix > 0)
	{
		const int64 NowUnix = FDateTime::UtcNow().ToUnixTimestamp();
		return NowUnix >= TicketExpiresAtUnix;
	}
	return (FPlatformTime::Seconds() - SearchStartedSeconds) >= NoExpiryWaitCeilingSeconds;
}
