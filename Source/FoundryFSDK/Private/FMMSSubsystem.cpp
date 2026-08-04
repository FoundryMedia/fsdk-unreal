// Copyright Foundry Media. FMMS matchmaking orchestrator.

#include "FMMSSubsystem.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
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
	if (Phase == EFMMSPhase::Requesting || Phase == EFMMSPhase::Searching || Phase == EFMMSPhase::Connecting)
	{
		if (UFoundryFSDKSubsystem* Sdk = Fsdk())
		{
			Sdk->CancelMatch();
		}
	}
	AbandonReconnectSeat(); // Cancel from Reconnectable = decline the live seat
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
}

void UFMMSSubsystem::CheckActiveSession()
{
	// Menu-entry recovery. After a server-initiated kick (operator stop, crash,
	// network loss) the local phase is STALE - Traveling/Connecting with no flow
	// actually running - so ask the PLATFORM, the only party that knows whether
	// the match is still live. Never clobber a genuinely in-flight search.
	if (Phase == EFMMSPhase::Requesting || Phase == EFMMSPhase::Searching)
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
	if (Phase == EFMMSPhase::Requesting || Phase == EFMMSPhase::Searching || Phase == EFMMSPhase::Connecting)
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
		Fail(FString::Printf(TEXT("Authentication failed (%d)."), (int32)Result));
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
	if (Result == EFoundryFsdkResult::Unavailable)
	{
		// The server rejected the search up front (503): no capacity for this queue. Stop here with a
		// human message rather than searching forever - a match could never form right now.
		Fail(TEXT("No game servers are available for this queue right now. Please try again shortly."));
		return;
	}
	if (Result != EFoundryFsdkResult::Ok)
	{
		Fail(FString::Printf(TEXT("Match request failed (%d)."), (int32)Result));
		return;
	}
	SetPhase(EFMMSPhase::Searching, TEXT("Searching for a match..."));
	StartPolling();
	Poll(); // immediate first poll
}

void UFMMSSubsystem::HandlePoll(EFoundryFsdkResult Result, EFoundryMatchStatus Status)
{
	if (Phase != EFMMSPhase::Searching)
	{
		return;
	}
	if (Result == EFoundryFsdkResult::Network || Result == EFoundryFsdkResult::Timeout)
	{
		return; // transient - keep polling
	}
	if (Result != EFoundryFsdkResult::Ok)
	{
		Fail(FString::Printf(TEXT("Polling failed (%d)."), (int32)Result));
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
		break;
	}
	case EFoundryMatchStatus::Searching:
	case EFoundryMatchStatus::Pending:
		break; // keep waiting
	case EFoundryMatchStatus::Cancelled:
		Fail(TEXT("Match was cancelled."));
		break;
	case EFoundryMatchStatus::Expired:
		Fail(TEXT("Matchmaking ticket expired."));
		break;
	default:
		Fail(TEXT("Matchmaking failed."));
		break;
	}
}

void UFMMSSubsystem::HandleConnection(EFoundryFsdkResult Result, FFoundryConnection Connection)
{
	if (Phase != EFMMSPhase::Connecting)
	{
		return;
	}
	if (Result == EFoundryFsdkResult::NoMatch)
	{
		// Endpoint not ready yet (match forming) - resume polling.
		SetPhase(EFMMSPhase::Searching, TEXT("Match forming - waiting for server..."));
		StartPolling();
		return;
	}
	if (Result != EFoundryFsdkResult::Ok || Connection.Ip.IsEmpty() || Connection.Port <= 0)
	{
		Fail(FString::Printf(TEXT("Could not get a connection (%d)."), (int32)Result));
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

void UFMMSSubsystem::StartPolling()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().SetTimer(PollTimer, this, &UFMMSSubsystem::Poll, PollIntervalSeconds, true);
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
