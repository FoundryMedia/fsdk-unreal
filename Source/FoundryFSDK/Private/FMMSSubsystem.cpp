// Copyright Foundry Media. FMMS matchmaking orchestrator.

#include "FMMSSubsystem.h"

#include "FoundryMatchmakingCopy.h"
#include "foundry/fsdk.h" // fsdk_wait_reason_str (diagnostics only - never player-facing text)

#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/DateTime.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogFMMS, Log, All);

namespace
{
	/** How long a lobby-visible Idle notice ("Match over", a refused-join sentence) stays under Find Match. */
	constexpr double LobbyNoticeTtlSeconds = 8.0;
}

void UFMMSSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	if (GEngine != nullptr)
	{
		NetworkFailureHandle = GEngine->OnNetworkFailure().AddUObject(this, &UFMMSSubsystem::HandleNetworkFailure);
		TravelFailureHandle = GEngine->OnTravelFailure().AddUObject(this, &UFMMSSubsystem::HandleTravelFailure);
	}
}

void UFMMSSubsystem::Deinitialize()
{
	if (GEngine != nullptr)
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureHandle);
		GEngine->OnTravelFailure().Remove(TravelFailureHandle);
	}
	NetworkFailureHandle.Reset();
	TravelFailureHandle.Reset();
	StopPolling();
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(MessageTtlTimer);
	}
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
	bMatchEndedNotified = false; // a new search is accepted: the previous match's end no longer applies
	bSeatReleased = false;
	EndedTicketId.Reset();
	KnownTicketId.Reset();
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
	bMatchEndedNotified = false; // a new search is accepted: the previous match's end no longer applies
	bSeatReleased = false;
	EndedTicketId.Reset();
	KnownTicketId.Reset();
	PendingQueue = Queue;
	PendingAttributes = AttributesJson.IsEmpty() ? TEXT("{}") : AttributesJson;
	SetPhase(EFMMSPhase::Requesting, TEXT("Checking regions and requesting match..."));
	Sdk->RequestMatch(PendingQueue, PendingAttributes);
}

void UFMMSSubsystem::Cancel()
{
	StopPolling();
	// Release the ticket in EVERY phase. The phase stays Traveling for the whole match, so the old
	// "only while a search is live" scope never DELETEd the ticket of a player who left a match, and the
	// lobby then offered Reconnect to the dead seat (Justin, 2026-10-16). CancelMatch runs on a worker
	// thread and is a no-op when the SDK holds no ticket handle.
	// A SEAT exists in Found / Connecting / Traveling (and a pending Reconnect seat): remember it and arm the
	// ended-seat belt BEFORE the async cancel, so a my-session read that races the DELETE (the lobby's
	// CheckActiveSession at BeginPlay) declines the seat instead of offering Reconnect. A Searching ticket is
	// QUEUED, not seated - nothing to race. Deliberate leave RELEASES the seat (Justin, 2026-10-16).
	if (IsSeatHeld())
	{
		const FString Released = !ReconnectTicketId.IsEmpty() ? ReconnectTicketId : KnownTicketId; // copy before the resets below
		if (!Released.IsEmpty())
		{
			EndedTicketId = Released;
		}
		bSeatReleased = true;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->CancelMatch();
	}
	AbandonReconnectSeat(); // Cancel from Reconnectable = decline the live seat
	CurrentMatchId.Reset();
	KnownTicketId.Reset();
	// RETURN TO LOBBY after NotifyMatchEnded lands here with the phase already Idle: keep the lobby's
	// "Match over" message instead of overwriting it with "Cancelled.".
	if (!(bMatchEndedNotified && Phase == EFMMSPhase::Idle))
	{
		SetPhase(EFMMSPhase::Idle, TEXT("Cancelled."));
	}
}

bool UFMMSSubsystem::IsSeatHeld() const
{
	return Phase == EFMMSPhase::Connecting || Phase == EFMMSPhase::Traveling
		|| MatchmakingState == EFoundryMatchmakingState::Found || MatchmakingState == EFoundryMatchmakingState::Traveling
		|| !ReconnectTicketId.IsEmpty();
}

void UFMMSSubsystem::HandleMatchJoinFailure(const FString& Reason)
{
	if (!IsSeatHeld())
	{
		return; // not our join (or already released): nothing to do
	}
	const FString Message = Reason.IsEmpty() ? FString(TEXT("Could not join the match.")) : Reason; // copy before any broadcast
	UE_LOG(LogFMMS, Log, TEXT("[FMMS] join failed or refused - releasing the seat."));
	StopPolling();
	const FString Released = !ReconnectTicketId.IsEmpty() ? ReconnectTicketId : KnownTicketId;
	if (!Released.IsEmpty())
	{
		EndedTicketId = Released;
	}
	bSeatReleased = true; // arms the ended-seat belt: a later my-session read of this seat is declined
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		Sdk->CancelMatch(); // worker thread, no-op without a ticket handle
	}
	AbandonReconnectSeat();
	CurrentMatchId.Reset();
	KnownTicketId.Reset();
	bCancelPending = false;
	SetPhase(EFMMSPhase::Idle, Message, LobbyNoticeTtlSeconds); // transient: the refusal must not sit under Find Match
}

void UFMMSSubsystem::HandleNetworkFailure(UWorld* /*World*/, UNetDriver* /*NetDriver*/,
                                          ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	// Only a refused / failed JOIN releases the seat. ConnectionLost / ConnectionTimeout / etc. are unplanned
	// disconnects from a live match - exactly what Reconnect is for - so they are left alone.
	if (FailureType == ENetworkFailure::PendingConnectionFailure || FailureType == ENetworkFailure::FailureReceived)
	{
		HandleMatchJoinFailure(ErrorString);
	}
}

void UFMMSSubsystem::HandleTravelFailure(UWorld* /*World*/, ETravelFailure::Type /*FailureType*/, const FString& ErrorString)
{
	HandleMatchJoinFailure(ErrorString);
}

void UFMMSSubsystem::NotifyMatchEnded()
{
	if (bMatchEndedNotified)
	{
		return; // idempotent: the game may call this from a replication callback AND the results screen
	}
	bMatchEndedNotified = true;
	StopPolling();
	// Remember WHICH seat ended (the pending reconnect seat wins, else the one my-session last reported).
	// Copy before AbandonReconnectSeat() clears ReconnectTicketId.
	// (A plain Cancel() may already have recorded it: never overwrite a known id with an empty one.)
	const FString Ended = !ReconnectTicketId.IsEmpty() ? ReconnectTicketId : KnownTicketId;
	if (!Ended.IsEmpty())
	{
		EndedTicketId = Ended;
	}
	if (UFoundryFSDKSubsystem* Sdk = Fsdk())
	{
		// Tell matchmaking the match is over BEFORE the ticket is cancelled: fid then stops backfilling into
		// the dead match and my-session stops offering the seat. Keyed on the SDK's active TICKET, which every
		// search holds - CurrentMatchId is empty on a fresh search (only my-session sets it), so it cannot gate
		// this. CancelMatch waits for the report to finish.
		Sdk->ReportMatchEnded();
		Sdk->CancelMatch(); // worker thread, fire-and-forget; a failure is WARN-logged there and changes nothing here
	}
	else
	{
		UE_LOG(LogFMMS, Log, TEXT("[FMMS] NotifyMatchEnded without an SDK - local state cleared only."));
	}
	AbandonReconnectSeat();
	CurrentMatchId.Reset();
	KnownTicketId.Reset();
	bCancelPending = false; // the Idle below must read "Match over", not "Search cancelled."
	SetPhase(EFMMSPhase::Idle, TEXT("Match over"), LobbyNoticeTtlSeconds); // transient: it must not linger under Find Match
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
	ApplyMySession(Result, Seat);
}

void UFMMSSubsystem::ApplyMySession(EFoundryFsdkResult Result, const FFoundrySessionSeat& Seat)
{
	const FString SeatTicketId = Seat.TicketId; // copy first: Abandon/SetPhase below re-enter other listeners
	// Belt for NotifyMatchEnded / a seat-releasing Cancel: the match is over (or left), so a live seat that is (or may be) ours is the dead
	// match's - the reaper lags up to ~2 min after the server exits and the cancel may have failed. Decline
	// it (idempotent server-side) and stay Idle; never offer Reconnect to a finished match.
	if ((bMatchEndedNotified || bSeatReleased) && Result == EFoundryFsdkResult::Ok && Seat.bActive && !SeatTicketId.IsEmpty()
		&& (EndedTicketId.IsEmpty() || SeatTicketId == EndedTicketId))
	{
		UE_LOG(LogFMMS, Log, TEXT("[FMMS] my-session still reports the seat of a match that already ended - declining it."));
		EndedTicketId = SeatTicketId;
		if (UFoundryFSDKSubsystem* Sdk = Fsdk())
		{
			Sdk->AbandonMatch(SeatTicketId);
		}
		ReconnectTicketId.Reset();
		CurrentMatchId.Reset();
		KnownTicketId.Reset();
		if (Phase != EFMMSPhase::Idle && Phase != EFMMSPhase::Failed)
		{
			SetPhase(EFMMSPhase::Idle, TEXT("Ready."));
		}
		return;
	}
	// Record the live match id REGARDLESS of the phase machine below - it is
	// informational (feeds GetCurrentMatchId for in-match match/team chat) and
	// my-session is the only client-side source of it (the connection payload
	// deliberately carries none).
	if (Result == EFoundryFsdkResult::Ok && Seat.bActive)
	{
		CurrentMatchId = Seat.MatchId;
		if (!SeatTicketId.IsEmpty())
		{
			KnownTicketId = SeatTicketId;
		}
	}
	if (Phase == EFMMSPhase::Requesting || Phase == EFMMSPhase::Searching
		|| Phase == EFMMSPhase::WaitingForCapacity || Phase == EFMMSPhase::Connecting)
	{
		return; // a live flow started meanwhile - its handlers own the phase
	}
	if (Result == EFoundryFsdkResult::Ok && Seat.bActive && !SeatTicketId.IsEmpty())
	{
		ReconnectTicketId = SeatTicketId;
		SetPhase(EFMMSPhase::Reconnectable, TEXT("You have a match in progress."));
		return;
	}
	// No live seat (incl. old-fid 404 / transient failure): the match is gone, so
	// any stale local state must not brick Find Match - reset to Idle.
	ReconnectTicketId.Reset();
	if (Result == EFoundryFsdkResult::Ok && !Seat.bActive)
	{
		CurrentMatchId.Reset(); // definitive platform answer: no live seat
		KnownTicketId.Reset();
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

void UFMMSSubsystem::SetPhase(EFMMSPhase NewPhase, const FString& Message, double MessageTtlSeconds)
{
	Phase = NewPhase;
	UE_LOG(LogFMMS, Log, TEXT("[FMMS] phase=%d %s"), (int32)NewPhase, *Message);
	// The dev console mirrors these itself: it subscribes OnFMMSStatus at Initialize
	// (HandleFmmsStatus -> "[fmms] ..."), which survives Shipping (event-driven, not a
	// UE_LOG capture). Do NOT also Print() here - that double-writes every phase line
	// (the duplicate-console-lines bug, caught live 2026-08-02).
	OnFMMSStatus.Broadcast(NewPhase, Message);
	// The lobby-facing state follows the phase AFTER the status event, so a listener that reads
	// GetMatchmakingState() from OnFMMSStatus sees the previous state, and one bound to
	// OnMatchmakingStateChanged sees the new one. A dev fixture freezes it (checked inside).
	ApplyMatchmakingPhase(NewPhase, Message, MessageTtlSeconds);
}

// -- lobby-facing matchmaking state ---------------------------------------------------------------
// A port of Goo Crew's UGooLobbyModel match half (ApplyPhase / RequestFindMatch / RequestCancel /
// SetState), moved into core so every game's lobby UI reads ONE state machine. It hangs off SetPhase
// only; nothing in the FMMS flow above reads it. No locks: this is game-thread state, and the FMMS
// calls it makes (FindMatchAuthenticated / Cancel / Reconnect) are the existing async ones.

namespace
{
	double MatchmakingClockNow()
	{
		return FPlatformTime::Seconds();
	}
}

bool UFMMSSubsystem::CanCancel() const
{
	return MatchmakingState == EFoundryMatchmakingState::Pending
		|| MatchmakingState == EFoundryMatchmakingState::Searching
		|| MatchmakingState == EFoundryMatchmakingState::WaitingForCapacity
		|| MatchmakingState == EFoundryMatchmakingState::Found;
}

bool UFMMSSubsystem::CanRequest() const
{
	if (bMatchmakingFixture)
	{
		return false;
	}
	if (MatchmakingState != EFoundryMatchmakingState::Idle && MatchmakingState != EFoundryMatchmakingState::Failed)
	{
		return false;
	}
	const UFoundryFSDKSubsystem* Sdk = Fsdk();
	return Sdk != nullptr && Sdk->IsLoggedIn();
}

float UFMMSSubsystem::GetElapsedSearchSeconds() const
{
	if (!bSearchClockRunning)
	{
		return -1.0f;
	}
	switch (MatchmakingState)
	{
	case EFoundryMatchmakingState::Pending:
	case EFoundryMatchmakingState::Searching:
	case EFoundryMatchmakingState::WaitingForCapacity:
	case EFoundryMatchmakingState::Found:
	case EFoundryMatchmakingState::CancelPending:
		return static_cast<float>(FMath::Max(0.0, MatchmakingClockNow() - SearchClockStartedAt));
	default:
		return -1.0f;
	}
}

void UFMMSSubsystem::SetMatchmakingState(EFoundryMatchmakingState NewState, const FString& Message,
                                         double MessageTtlSeconds, bool bForceBroadcast)
{
	const bool bChanged = bForceBroadcast || NewState != MatchmakingState || Message != MatchmakingMessage;
	MatchmakingState = NewState;
	MatchmakingMessage = Message;
	// Every call re-decides the TTL: a new state/message must never inherit the previous one's expiry.
	if (UGameInstance* GI = GetGameInstance())
	{
		FTimerManager& Timers = GI->GetTimerManager();
		Timers.ClearTimer(MessageTtlTimer);
		if (MessageTtlSeconds > 0.0 && !Message.IsEmpty())
		{
			Timers.SetTimer(MessageTtlTimer, this, &UFMMSSubsystem::OnMatchmakingMessageExpired,
			                static_cast<float>(MessageTtlSeconds), false);
		}
	}
	if (bChanged)
	{
		OnMatchmakingStateChanged.Broadcast(MatchmakingState, MatchmakingMessage);
	}
}

void UFMMSSubsystem::OnMatchmakingMessageExpired()
{
	// A transient message ran out: blank it, and let a Failed state fall back to Idle (the failure text was
	// the only thing keeping the UI on Failed - both Idle and Failed accept a new request).
	if (MatchmakingState == EFoundryMatchmakingState::Failed)
	{
		MatchmakingState = EFoundryMatchmakingState::Idle;
	}
	MatchmakingMessage.Reset();
	OnMatchmakingStateChanged.Broadcast(MatchmakingState, MatchmakingMessage);
}

bool UFMMSSubsystem::RequestFindMatch(const FString& Queue, const FString& AttributesJson)
{
	if (!CanRequest() || Queue.IsEmpty())
	{
		return false; // a second click while a request is live is a no-op, never a second ticket
	}
	// COPY the arguments before anything is broadcast: a caller passes references into its own state
	// (the match cluster hands over Modes[i].QueueName), and the Pending broadcast below re-enters
	// that caller, whose Refresh() reassigns Modes - the reference then points into freed memory.
	// Shipping client 0.1.1 crashed on the first Find Match exactly there (EXCEPTION_ACCESS_VIOLATION in
	// FindMatchAuthenticated's PendingQueue = Queue; 2026-10-15). Never trust a reference parameter across
	// a delegate this object fires.
	const FString QueueCopy = Queue;
	const FString AttributesCopy = AttributesJson;
	const EFoundryMatchmakingState Before = MatchmakingState;
	SearchClockStartedAt = MatchmakingClockNow();
	bSearchClockRunning = true;
	bCancelPending = false;
	SetMatchmakingState(EFoundryMatchmakingState::Pending, TEXT("Requesting a match..."), 0.0);
	FindMatchAuthenticated(QueueCopy, AttributesCopy);
	// FMMS sets its phase synchronously when it takes the request. A request it refused (its 2 s
	// anti-spam window, a flow already live) leaves the phase where it was - and would leave us stuck
	// on Pending with nothing ever answering (Goo Crew, 2026-10-08: the second click after a failure).
	if (Phase == EFMMSPhase::Idle || Phase == EFMMSPhase::Failed || Phase == EFMMSPhase::Reconnectable)
	{
		bSearchClockRunning = false;
		SetMatchmakingState(Before, TEXT("Give it a second, then try again."), 3.0);
		return false;
	}
	return true;
}

void UFMMSSubsystem::RequestCancel()
{
	if (bMatchmakingFixture || !CanCancel())
	{
		return;
	}
	bCancelPending = true;
	SetMatchmakingState(EFoundryMatchmakingState::CancelPending, TEXT("Cancelling..."), 0.0);
	// Cancel() flips the phase to Idle synchronously and drops any late connection through its phase
	// guard; ApplyMatchmakingPhase resolves CancelPending on that Idle ("Search cancelled.").
	Cancel();
}

void UFMMSSubsystem::RequestReconnect()
{
	if (bMatchmakingFixture || MatchmakingState != EFoundryMatchmakingState::Reconnectable)
	{
		return;
	}
	Reconnect();
}

void UFMMSSubsystem::DeclineReconnectSeat()
{
	if (bMatchmakingFixture || MatchmakingState != EFoundryMatchmakingState::Reconnectable)
	{
		return;
	}
	Cancel(); // from Reconnectable this abandons the seat's ticket server-side and resets the phase to Idle
}

void UFMMSSubsystem::ApplyMatchmakingPhase(EFMMSPhase InPhase, const FString& Message, double MessageTtlSeconds)
{
	if (bMatchmakingFixture)
	{
		return; // a dev fixture owns the displayed state; real phase changes are ignored
	}
	switch (InPhase)
	{
	case EFMMSPhase::Idle:
		if (bCancelPending)
		{
			bCancelPending = false;
			bSearchClockRunning = false;
			SetMatchmakingState(EFoundryMatchmakingState::Idle, TEXT("Search cancelled."), 4.0);
		}
		else
		{
			bSearchClockRunning = false;
			SetMatchmakingState(EFoundryMatchmakingState::Idle, Message, MessageTtlSeconds);
		}
		break;
	case EFMMSPhase::Authenticating:
	case EFMMSPhase::Requesting:
		if (!bSearchClockRunning)
		{
			SearchClockStartedAt = MatchmakingClockNow();
			bSearchClockRunning = true;
		}
		if (!bCancelPending)
		{
			SetMatchmakingState(EFoundryMatchmakingState::Pending, Message);
		}
		break;
	case EFMMSPhase::Searching:
		if (!bSearchClockRunning)
		{
			SearchClockStartedAt = MatchmakingClockNow();
			bSearchClockRunning = true;
		}
		if (!bCancelPending)
		{
			SetMatchmakingState(EFoundryMatchmakingState::Searching, Message);
		}
		break;
	case EFMMSPhase::WaitingForCapacity:
		// Still a live search (the search clock keeps running) - Message is the server-composed
		// sentence (incl. any "~N min left"), rendered verbatim by the status line.
		if (!bSearchClockRunning)
		{
			SearchClockStartedAt = MatchmakingClockNow();
			bSearchClockRunning = true;
		}
		if (!bCancelPending)
		{
			SetMatchmakingState(EFoundryMatchmakingState::WaitingForCapacity, Message);
		}
		break;
	case EFMMSPhase::Connecting:
		// A match arrived. If a cancel is pending, FMMS itself decides: its Cancel() during
		// Connecting cancels the ticket and flips to Idle, and a late connection callback is
		// dropped by its phase guard. We keep showing "Cancelling" until that Idle lands.
		if (!bCancelPending)
		{
			SetMatchmakingState(EFoundryMatchmakingState::Found, TEXT("Match found - connecting"));
		}
		break;
	case EFMMSPhase::Traveling:
		// Past the point of no return: the client is travelling. Cancel is off.
		bCancelPending = false;
		SetMatchmakingState(EFoundryMatchmakingState::Traveling, TEXT("Joining the server..."));
		break;
	case EFMMSPhase::Failed:
		bCancelPending = false;
		bSearchClockRunning = false;
		SetMatchmakingState(EFoundryMatchmakingState::Failed, Message.IsEmpty() ? TEXT("try again.") : Message, 6.0);
		break;
	case EFMMSPhase::Reconnectable:
		bCancelPending = false;
		bSearchClockRunning = false;
		SetMatchmakingState(EFoundryMatchmakingState::Reconnectable,
		                    Message.IsEmpty() ? TEXT("You are seated in a live match.") : Message);
		break;
	default:
		break;
	}
}

bool UFMMSSubsystem::SetMatchmakingFixture(EFoundryMatchmakingState State, const FString& Message, float ElapsedSeconds)
{
#if !UE_BUILD_SHIPPING
	bMatchmakingFixture = true;
	bCancelPending = false;
	if (ElapsedSeconds >= 0.0f)
	{
		SearchClockStartedAt = MatchmakingClockNow() - static_cast<double>(ElapsedSeconds);
		bSearchClockRunning = true;
	}
	else
	{
		bSearchClockRunning = false;
	}
	// Forced: the fixture flag itself changed what the UI shows ("Dev fixture - Find Match disabled."),
	// even when the state and message happen to equal the current ones.
	SetMatchmakingState(State, Message, 0.0, true);
	return true;
#else
	(void)State;
	(void)Message;
	(void)ElapsedSeconds;
	return false;
#endif
}

void UFMMSSubsystem::ClearMatchmakingFixture()
{
#if !UE_BUILD_SHIPPING
	if (!bMatchmakingFixture)
	{
		return;
	}
	bMatchmakingFixture = false;
	bCancelPending = false;
	bSearchClockRunning = false;
	// Re-derive from the real phase (its message is gone, so the defaults in the table apply).
	ApplyMatchmakingPhase(Phase, FString());
	OnMatchmakingStateChanged.Broadcast(MatchmakingState, MatchmakingMessage);
#endif
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
