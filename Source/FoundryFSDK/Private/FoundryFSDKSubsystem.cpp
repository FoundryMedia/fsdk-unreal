// Copyright Foundry Media. FoundryFSDK Unreal subsystem facade (implementation).
//
// Idiomatic-UE wrapper over the fsdk-core CLIENT C ABI (vendored + compiled
// in-module under Private/FsdkCore). The core performs the real player-scoped
// HTTPS conversation via the host transport installed at module startup (engine
// HTTP module - see FoundryFSDKModule.cpp / FoundryFSDKTransport.cpp).
//
// The core ABI is synchronous and blocks on the network, so each public method
// runs its core call on a WORKER THREAD and broadcasts an On...Complete delegate
// back on the GAME THREAD - the game thread never blocks. The core is not
// thread-safe, so access is serialized by a per-state critical section, and the
// core handles live behind a ref-counted FFsdkCoreState so an in-flight worker
// can never touch a freed client (e.g. if the subsystem is torn down mid-call).
//
// SECURITY: the player token is forwarded to the core and never persisted or
// logged here. See ../../fsdk-core/SECURITY.md.

#include "FoundryFSDKSubsystem.h"

#include "foundry/fsdk.h"

#include "Async/Async.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoundryFSDK, Log, All);

/**
 * Ref-counted owner of the fsdk-core handles + the lock that serializes core
 * access. Shared (by value) into every worker, so the handles outlive any
 * in-flight call even if the subsystem is destroyed. The destructor runs only
 * when the last owner releases - no other thread can be mid-call - so it frees
 * without locking.
 */
struct FFsdkCoreState
{
	fsdk_client* Client = nullptr;
	fsdk_ticket* ActiveTicket = nullptr;
	FCriticalSection CS;

	~FFsdkCoreState()
	{
		if (ActiveTicket != nullptr)
		{
			fsdk_ticket_destroy(ActiveTicket);
		}
		if (Client != nullptr)
		{
			fsdk_client_destroy(Client);
		}
	}
};

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

	/** Map an fsdk_result (C ABI) to the Blueprint result enum. */
	EFoundryFsdkResult ToBlueprintResult(fsdk_result Result)
	{
		switch (Result)
		{
			case FSDK_OK:                    return EFoundryFsdkResult::Ok;
			case FSDK_NOT_IMPLEMENTED:       return EFoundryFsdkResult::NotImplemented;
			case FSDK_ERR_INVALID_ARG:       return EFoundryFsdkResult::InvalidArg;
			case FSDK_ERR_NOT_AUTHENTICATED: return EFoundryFsdkResult::NotAuthenticated;
			case FSDK_ERR_UNAUTHORIZED:      return EFoundryFsdkResult::Unauthorized;
			case FSDK_ERR_NETWORK:           return EFoundryFsdkResult::Network;
			case FSDK_ERR_TIMEOUT:           return EFoundryFsdkResult::Timeout;
			case FSDK_ERR_PROTOCOL:          return EFoundryFsdkResult::Protocol;
			case FSDK_ERR_TOKEN_INVALID:     return EFoundryFsdkResult::Protocol;
			case FSDK_ERR_TOKEN_EXPIRED:     return EFoundryFsdkResult::Unauthorized;
			case FSDK_ERR_NO_MATCH:          return EFoundryFsdkResult::NoMatch;
			case FSDK_ERR_AGONES:            return EFoundryFsdkResult::Internal;
			case FSDK_ERR_INTERNAL:          return EFoundryFsdkResult::Internal;
			default:                         return EFoundryFsdkResult::Unknown;
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
	// Drop the subsystem's reference. Any worker mid-call holds its own ref, so
	// the handles stay alive until that call returns, then free on the last release.
	Core.Reset();
	Super::Deinitialize();
}

bool UFoundryFSDKSubsystem::InitializeClient(const FString& BaseUrl)
{
	TSharedRef<FFsdkCoreState, ESPMode::ThreadSafe> NewState =
		MakeShared<FFsdkCoreState, ESPMode::ThreadSafe>();

	fsdk_client* NewClient = nullptr;
	const fsdk_result Result = fsdk_client_create(TCHAR_TO_UTF8(*BaseUrl), &NewClient);
	if (Result != FSDK_OK)
	{
		UE_LOG(LogFoundryFSDK, Error, TEXT("fsdk_client_create failed: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(Result)));
		return false;
	}
	NewState->Client = NewClient;

	// Replace any prior state; its handles free once no worker still holds a ref.
	Core = NewState;
	return true;
}

void UFoundryFSDKSubsystem::Authenticate(const FString& PlayerToken)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		UE_LOG(LogFoundryFSDK, Error, TEXT("Authenticate called before InitializeClient"));
		OnAuthenticateComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated);
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	const FString Token = PlayerToken; // deep copy for the worker

	// SECURITY: the token is the player's own FID session token; forwarded to the
	// core (bearer) and never persisted or logged here.
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis, Token]()
	{
		fsdk_result R;
		{
			FScopeLock Lock(&CoreRef->CS);
			const FTCHARToUTF8 TokenUtf8(*Token);
			R = fsdk_authenticate(CoreRef->Client, TokenUtf8.Get());
		}
		const EFoundryFsdkResult Result = ToBlueprintResult(R);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnAuthenticateComplete.Broadcast(Result);
			}
		});
	});
}

void UFoundryFSDKSubsystem::RequestMatch(const FString& Queue, const FString& AttributesJson)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		UE_LOG(LogFoundryFSDK, Error, TEXT("RequestMatch called before InitializeClient"));
		OnRequestMatchComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated);
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	const FString QueueCopy = Queue;
	const FString AttrsCopy = AttributesJson;

	Async(EAsyncExecution::Thread, [CoreRef, WeakThis, QueueCopy, AttrsCopy]()
	{
		fsdk_result R;
		{
			FScopeLock Lock(&CoreRef->CS);
			// Drop any previous ticket.
			if (CoreRef->ActiveTicket != nullptr)
			{
				fsdk_ticket_destroy(CoreRef->ActiveTicket);
				CoreRef->ActiveTicket = nullptr;
			}
			const FTCHARToUTF8 QueueUtf8(*QueueCopy);
			const FTCHARToUTF8 AttrsUtf8(*AttrsCopy);
			const char* Attrs = AttrsCopy.IsEmpty() ? nullptr : AttrsUtf8.Get();
			fsdk_ticket* Ticket = nullptr;
			R = fsdk_request_match(CoreRef->Client, QueueUtf8.Get(), Attrs, &Ticket);
			if (R == FSDK_OK)
			{
				CoreRef->ActiveTicket = Ticket;
			}
		}
		const EFoundryFsdkResult Result = ToBlueprintResult(R);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnRequestMatchComplete.Broadcast(Result);
			}
		});
	});
}

void UFoundryFSDKSubsystem::PollMatch()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnPollMatchComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated, EFoundryMatchStatus::Unknown);
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		fsdk_result R;
		fsdk_match_status S = FSDK_MATCH_PENDING;
		{
			FScopeLock Lock(&CoreRef->CS);
			if (CoreRef->ActiveTicket == nullptr)
			{
				R = FSDK_ERR_NO_MATCH;
			}
			else
			{
				R = fsdk_poll_match(CoreRef->Client, CoreRef->ActiveTicket, &S);
			}
		}
		const EFoundryFsdkResult Result = ToBlueprintResult(R);
		const EFoundryMatchStatus Status =
			(R == FSDK_OK) ? ToBlueprintStatus(S) : EFoundryMatchStatus::Unknown;
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Status]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnPollMatchComplete.Broadcast(Result, Status);
			}
		});
	});
}

void UFoundryFSDKSubsystem::GetConnection()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnGetConnectionComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated, FFoundryConnection());
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		fsdk_result R;
		fsdk_connection Conn;
		FMemory::Memzero(&Conn, sizeof(Conn));
		{
			FScopeLock Lock(&CoreRef->CS);
			if (CoreRef->ActiveTicket == nullptr)
			{
				R = FSDK_ERR_NO_MATCH;
			}
			else
			{
				R = fsdk_get_connection(CoreRef->Client, CoreRef->ActiveTicket, &Conn);
			}
		}
		FFoundryConnection Out;
		if (R == FSDK_OK)
		{
			// ip:port is an opaque rendezvous (box today, relay later) - see SECURITY.md.
			Out.Ip = UTF8_TO_TCHAR(Conn.ip);
			Out.Port = static_cast<int32>(Conn.port);
			Out.MatchToken = UTF8_TO_TCHAR(Conn.match_token);
		}
		const EFoundryFsdkResult Result = ToBlueprintResult(R);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Out]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnGetConnectionComplete.Broadcast(Result, Out);
			}
		});
	});
}

void UFoundryFSDKSubsystem::CancelMatch()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		return;
	}

	// Fire-and-forget; cancel + drop the ticket under the serialization lock.
	Async(EAsyncExecution::Thread, [CoreRef]()
	{
		FScopeLock Lock(&CoreRef->CS);
		if (CoreRef->ActiveTicket != nullptr)
		{
			fsdk_cancel_match(CoreRef->Client, CoreRef->ActiveTicket);
			fsdk_ticket_destroy(CoreRef->ActiveTicket);
			CoreRef->ActiveTicket = nullptr;
		}
	});
}
