// Copyright Foundry Media. FoundryFSDK Unreal subsystem facade (implementation).
//
// Thin idiomatic-UE wrapper over the fsdk-core CLIENT C ABI. SCAFFOLD: forwards
// to the C ABI stubs; real behavior arrives when fsdk-core is implemented and
// the ThirdParty static lib is linked (see ../../ThirdParty/fsdk-core/README.md).

#include "FoundryFSDKSubsystem.h"

#include "foundry/fsdk.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoundryFSDK, Log, All);

namespace
{
	/** Map an fsdk_match_status (C ABI) to the Blueprint enum. */
	EFoundryMatchStatus ToBlueprintStatus(fsdk_match_status Status)
	{
		switch (Status)
		{
			case FSDK_MATCH_PENDING:   return EFoundryMatchStatus::Pending;
			case FSDK_MATCH_SEARCHING: return EFoundryMatchStatus::Searching;
			case FSDK_MATCH_FOUND:     return EFoundryMatchStatus::Found;
			case FSDK_MATCH_CANCELLED: return EFoundryMatchStatus::Cancelled;
			case FSDK_MATCH_FAILED:    return EFoundryMatchStatus::Failed;
			case FSDK_MATCH_EXPIRED:   return EFoundryMatchStatus::Expired;
			default:                   return EFoundryMatchStatus::Unknown;
		}
	}
}

void UFoundryFSDKSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogFoundryFSDK, Log, TEXT("FoundryFSDK subsystem initialized (fsdk-core %s)"),
		UTF8_TO_TCHAR(fsdk_version()));
}

void UFoundryFSDKSubsystem::Deinitialize()
{
	if (ActiveTicket != nullptr)
	{
		fsdk_ticket_destroy(static_cast<fsdk_ticket*>(ActiveTicket));
		ActiveTicket = nullptr;
	}
	if (Client != nullptr)
	{
		fsdk_client_destroy(Client);
		Client = nullptr;
	}
	Super::Deinitialize();
}

bool UFoundryFSDKSubsystem::InitializeClient(const FString& BaseUrl)
{
	if (Client != nullptr)
	{
		fsdk_client_destroy(Client);
		Client = nullptr;
	}

	const fsdk_result Result = fsdk_client_create(TCHAR_TO_UTF8(*BaseUrl), &Client);
	if (Result != FSDK_OK)
	{
		UE_LOG(LogFoundryFSDK, Error, TEXT("fsdk_client_create failed: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(Result)));
		return false;
	}
	return true;
}

bool UFoundryFSDKSubsystem::Authenticate(const FString& PlayerToken)
{
	if (Client == nullptr)
	{
		UE_LOG(LogFoundryFSDK, Error, TEXT("Authenticate called before InitializeClient"));
		return false;
	}

	// SECURITY: PlayerToken is the player's own FID session token, passed in by
	// game code. We forward it to the C ABI and do not log or persist it.
	const fsdk_result Result = fsdk_authenticate(Client, TCHAR_TO_UTF8(*PlayerToken));
	if (Result != FSDK_OK)
	{
		UE_LOG(LogFoundryFSDK, Warning, TEXT("fsdk_authenticate failed: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(Result)));
		return false;
	}
	return true;
}

bool UFoundryFSDKSubsystem::RequestMatch(const FString& Queue, const FString& AttributesJson)
{
	if (Client == nullptr)
	{
		UE_LOG(LogFoundryFSDK, Error, TEXT("RequestMatch called before InitializeClient"));
		return false;
	}

	// Drop any previous ticket.
	if (ActiveTicket != nullptr)
	{
		fsdk_ticket_destroy(static_cast<fsdk_ticket*>(ActiveTicket));
		ActiveTicket = nullptr;
	}

	const char* Attrs = AttributesJson.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*AttributesJson);
	fsdk_ticket* Ticket = nullptr;
	const fsdk_result Result = fsdk_request_match(Client, TCHAR_TO_UTF8(*Queue), Attrs, &Ticket);
	if (Result != FSDK_OK)
	{
		UE_LOG(LogFoundryFSDK, Warning, TEXT("fsdk_request_match failed: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(Result)));
		return false;
	}
	ActiveTicket = Ticket;
	return true;
}

EFoundryMatchStatus UFoundryFSDKSubsystem::PollMatch()
{
	if (Client == nullptr || ActiveTicket == nullptr)
	{
		return EFoundryMatchStatus::Unknown;
	}

	fsdk_match_status Status = FSDK_MATCH_PENDING;
	const fsdk_result Result =
		fsdk_poll_match(Client, static_cast<fsdk_ticket*>(ActiveTicket), &Status);
	if (Result != FSDK_OK)
	{
		UE_LOG(LogFoundryFSDK, Verbose, TEXT("fsdk_poll_match failed: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(Result)));
		return EFoundryMatchStatus::Unknown;
	}
	return ToBlueprintStatus(Status);
}

bool UFoundryFSDKSubsystem::GetConnection(FFoundryConnection& OutConnection)
{
	OutConnection = FFoundryConnection();
	if (Client == nullptr || ActiveTicket == nullptr)
	{
		return false;
	}

	fsdk_connection Conn;
	const fsdk_result Result =
		fsdk_get_connection(Client, static_cast<fsdk_ticket*>(ActiveTicket), &Conn);
	if (Result != FSDK_OK)
	{
		// FSDK_ERR_NO_MATCH / FSDK_NOT_IMPLEMENTED (scaffold) land here.
		UE_LOG(LogFoundryFSDK, Verbose, TEXT("fsdk_get_connection not ready: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(Result)));
		return false;
	}

	// ip:port is an opaque rendezvous (box today, relay later) - see SECURITY.md.
	OutConnection.Ip = UTF8_TO_TCHAR(Conn.ip);
	OutConnection.Port = static_cast<int32>(Conn.port);
	OutConnection.MatchToken = UTF8_TO_TCHAR(Conn.match_token);
	return true;
}

void UFoundryFSDKSubsystem::CancelMatch()
{
	if (Client == nullptr || ActiveTicket == nullptr)
	{
		return;
	}
	fsdk_cancel_match(Client, static_cast<fsdk_ticket*>(ActiveTicket));
	fsdk_ticket_destroy(static_cast<fsdk_ticket*>(ActiveTicket));
	ActiveTicket = nullptr;
}
