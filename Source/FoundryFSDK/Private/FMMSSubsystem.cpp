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
	bBound = true;
}

void UFMMSSubsystem::FindMatch(const FString& Queue, const FString& AttributesJson,
                               const FString& PlayerToken, const FString& ApiBaseUrl)
{
	if (Phase != EFMMSPhase::Idle && Phase != EFMMSPhase::Failed)
	{
		UE_LOG(LogFMMS, Warning, TEXT("FindMatch ignored - flow already in progress (phase %d)"), (int32)Phase);
		return;
	}
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
	if (Phase != EFMMSPhase::Idle && Phase != EFMMSPhase::Failed)
	{
		UE_LOG(LogFMMS, Warning, TEXT("FindMatchAuthenticated ignored - flow already in progress (phase %d)"), (int32)Phase);
		return;
	}
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
	SetPhase(EFMMSPhase::Requesting, TEXT("Requesting match..."));
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
	SetPhase(EFMMSPhase::Idle, TEXT("Cancelled."));
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
	SetPhase(EFMMSPhase::Requesting, TEXT("Requesting match..."));
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
	SetPhase(EFMMSPhase::Traveling,
	         FString::Printf(TEXT("Joining %s:%d..."), *Connection.Ip, Connection.Port));
	// The URL carries the match token - do NOT log it.
	const FString Url = FString::Printf(TEXT("%s:%d?token=%s"),
	                                    *Connection.Ip, Connection.Port, *Connection.MatchToken);
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
	OnFMMSStatus.Broadcast(NewPhase, Message);
}

void UFMMSSubsystem::Fail(const FString& Message)
{
	StopPolling();
	SetPhase(EFMMSPhase::Failed, Message);
}
