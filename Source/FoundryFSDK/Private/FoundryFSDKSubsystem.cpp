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
#include "FoundryFSDKLauncherHandoff.h"
#include "FoundryFSDKTransport.h"

#include "Async/Async.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTime.h"
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
	fsdk_chat* Chat = nullptr;
	FCriticalSection CS;

	// ── Chat WS handoff ──
	// The WS sink (game thread) appends here under this LIGHT lock (never held
	// across any network call - the driver tick must never stall behind a worker's
	// blocking join); the driver tick drains it into the chat handle.
	FCriticalSection ChatQueueCS;
	TArray<FString> ChatInbound;
	bool bChatClosed = false;

	/** Message copies staged by the C message callback. GAME-THREAD-ONLY: the
	 *  callback fires only inside the driver tick's fsdk_chat_on_ws_text drain,
	 *  and the tick broadcasts + clears these AFTER releasing the core lock
	 *  (broadcasting into Blueprint while holding it invites re-entry). */
	struct FChatMsgCopy
	{
		EFoundryChatChannel Channel = EFoundryChatChannel::Global;
		FString DisplayName;
		FString FoundryId;
		FString Body;
	};
	TArray<FChatMsgCopy> ChatMessages;

	~FFsdkCoreState()
	{
		if (Chat != nullptr)
		{
			fsdk_chat_destroy(Chat); // borrows Client - must go first
		}
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

extern "C"
{
	/** fsdk_chat_message_fn: stage a POD-copy for the driver tick to broadcast
	 *  once it releases the core lock. UserData is the owning FFsdkCoreState -
	 *  guaranteed alive: this only ever fires inside the tick's drain, which
	 *  holds a ref. */
	static void FoundryFSDKChatMessageThunk(const fsdk_chat_message* Message, void* UserData)
	{
		FFsdkCoreState* State = static_cast<FFsdkCoreState*>(UserData);
		if (State == nullptr || Message == nullptr)
		{
			return;
		}
		FFsdkCoreState::FChatMsgCopy Copy;
		Copy.Channel = Message->channel == FSDK_CHAT_CHANNEL_PARTY
			? EFoundryChatChannel::Party : EFoundryChatChannel::Global;
		Copy.DisplayName = UTF8_TO_TCHAR(Message->display_name);
		Copy.FoundryId = UTF8_TO_TCHAR(Message->from_foundry_id);
		Copy.Body = UTF8_TO_TCHAR(Message->body);
		State->ChatMessages.Add(MoveTemp(Copy));
	}
}

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
			case FSDK_ERR_UNAVAILABLE:       return EFoundryFsdkResult::Unavailable;
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
	// Chat first: stop the driver tick + detach the WS sink BEFORE the core state
	// can go away (the sink holds only a weak ref, but a live tick must not race
	// the teardown).
	if (ChatTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ChatTickHandle);
		ChatTickHandle.Reset();
	}
	FoundryFSDKClearWsSink();
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

// ── FRC chat ────────────────────────────────────────────────────────────────
//
// One chat session per game instance, driven from the GAME THREAD by a 0.25s
// FTSTicker: it drains the WS sink's inbound frames into the chat handle, runs
// the ~25s keepalive, watches fsdk_chat_ready for state flips, and re-joins
// with backoff after a drop. The core lock is only ever TRY-locked here — a
// worker mid-join (blocking room-resolve HTTP) must never stall the game
// thread; undrained frames simply carry to the next tick in order.

void UFoundryFSDKSubsystem::JoinGlobalChat(const FString& GameSlug)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr || GameSlug.IsEmpty())
	{
		OnChatStateChanged.Broadcast(false);
		return;
	}
	bChatDesired = true;
	ChatGameSlug = GameSlug;
	ChatRejoinStrikes = 0;
	NextChatRejoinTime = 0.0;
	if (!ChatTickHandle.IsValid())
	{
		ChatTickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UFoundryFSDKSubsystem::ChatDriverTick), 0.25f);
	}
	StartChatJoin();
}

void UFoundryFSDKSubsystem::StartChatJoin()
{
	if (bChatJoinInFlight)
	{
		return;
	}
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		return;
	}
	bChatJoinInFlight = true;

	// Route inbound frames/drops into the core state's queue BEFORE the join can
	// open the socket. Weak ref: a torn-down state just drops frames.
	TWeakPtr<FFsdkCoreState, ESPMode::ThreadSafe> WeakCore(CoreRef);
	FoundryFSDKSetWsSink(
		[WeakCore](const FString& Text)
		{
			if (TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> State = WeakCore.Pin())
			{
				FScopeLock QLock(&State->ChatQueueCS);
				State->ChatInbound.Add(Text);
			}
		},
		[WeakCore]()
		{
			if (TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> State = WeakCore.Pin())
			{
				FScopeLock QLock(&State->ChatQueueCS);
				State->bChatClosed = true;
			}
		});

	const FString Slug = ChatGameSlug;
	const bool bCanReHandoff = bLauncherSession;
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, Slug, bCanReHandoff, WeakThis]()
	{
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			if (CoreRef->Chat == nullptr)
			{
				Result = fsdk_chat_create(CoreRef->Client, &CoreRef->Chat);
				if (Result == FSDK_OK)
				{
					fsdk_chat_set_message_callback(CoreRef->Chat,
						&FoundryFSDKChatMessageThunk, CoreRef.Get());
				}
			}
			else
			{
				Result = FSDK_OK;
			}
			if (Result == FSDK_OK)
			{
				Result = fsdk_chat_join_global(CoreRef->Chat, TCHAR_TO_UTF8(*Slug));
			}
			if (Result == FSDK_ERR_UNAUTHORIZED && bCanReHandoff)
			{
				// The 15-min daemon token expired mid-session: transparently re-mint
				// from the launcher and retry the join once. Worker thread - the
				// pipe read is allowed to block here.
				FString Fresh;
				if (FoundryFSDKReadLauncherToken(Fresh) && !Fresh.IsEmpty())
				{
					if (fsdk_set_player_token(CoreRef->Client, TCHAR_TO_UTF8(*Fresh)) == FSDK_OK)
					{
						Result = fsdk_chat_join_global(CoreRef->Chat, TCHAR_TO_UTF8(*Slug));
					}
				}
			}
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result]()
		{
			UFoundryFSDKSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}
			Self->bChatJoinInFlight = false;
			if (Result != FSDK_OK)
			{
				// Ready never flipped — schedule the next attempt on the ladder.
				Self->ChatRejoinStrikes = FMath::Min(Self->ChatRejoinStrikes + 1, 5);
				Self->NextChatRejoinTime =
					FPlatformTime::Seconds() + static_cast<double>(1 << Self->ChatRejoinStrikes);
				UE_LOG(LogFoundryFSDK, Warning, TEXT("Chat join failed: %s (retrying)"),
					UTF8_TO_TCHAR(fsdk_result_str(Result)));
			}
		});
	});
}

void UFoundryFSDKSubsystem::JoinPartyChat(const FString& PartyId)
{
	// The party channel is a SUBSCRIPTION on the global chat's socket - the
	// join worker multiplexes it through the same handle (creating the handle
	// if the game joined party chat first). No second connection ever.
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr || PartyId.IsEmpty())
	{
		OnPartyChatStateChanged.Broadcast(false);
		return;
	}
	if (!ChatTickHandle.IsValid())
	{
		ChatTickHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UFoundryFSDKSubsystem::ChatDriverTick), 0.25f);
	}
	Async(EAsyncExecution::Thread, [CoreRef, PartyId]()
	{
		FScopeLock Lock(&CoreRef->CS);
		if (CoreRef->Chat == nullptr)
		{
			if (fsdk_chat_create(CoreRef->Client, &CoreRef->Chat) != FSDK_OK)
			{
				return;
			}
			fsdk_chat_set_message_callback(CoreRef->Chat,
				&FoundryFSDKChatMessageThunk, CoreRef.Get());
		}
		const fsdk_result Result = fsdk_chat_join_party(CoreRef->Chat, TCHAR_TO_UTF8(*PartyId));
		if (Result != FSDK_OK)
		{
			UE_LOG(LogFoundryFSDK, Warning, TEXT("Party chat join failed: %s"),
				UTF8_TO_TCHAR(fsdk_result_str(Result)));
		}
	});
}

void UFoundryFSDKSubsystem::LeavePartyChat()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid())
	{
		return;
	}
	Async(EAsyncExecution::Thread, [CoreRef]()
	{
		FScopeLock Lock(&CoreRef->CS);
		if (CoreRef->Chat != nullptr)
		{
			(void)fsdk_chat_leave_party(CoreRef->Chat);
		}
	});
}

void UFoundryFSDKSubsystem::SendChatToChannel(EFoundryChatChannel Channel, const FString& Body)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Chat == nullptr)
	{
		OnChatSendComplete.Broadcast(EFoundryFsdkResult::Unavailable);
		return;
	}
	const fsdk_chat_channel CoreChannel = Channel == EFoundryChatChannel::Party
		? FSDK_CHAT_CHANNEL_PARTY : FSDK_CHAT_CHANNEL_GLOBAL;
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, CoreChannel, Body, WeakThis]()
	{
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = CoreRef->Chat != nullptr
				? fsdk_chat_send_channel(CoreRef->Chat, CoreChannel, TCHAR_TO_UTF8(*Body))
				: FSDK_ERR_UNAVAILABLE;
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnChatSendComplete.Broadcast(ToBlueprintResult(Result));
			}
		});
	});
}

void UFoundryFSDKSubsystem::SendChat(const FString& Body)
{
	SendChatToChannel(EFoundryChatChannel::Global, Body);
}

void UFoundryFSDKSubsystem::LeaveChat()
{
	bChatDesired = false;
	bChatJoinInFlight = false;
	if (ChatTickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(ChatTickHandle);
		ChatTickHandle.Reset();
	}
	FoundryFSDKClearWsSink();
	if (bChatReady)
	{
		bChatReady = false;
		OnChatStateChanged.Broadcast(false);
	}
	if (bPartyChatReady)
	{
		bPartyChatReady = false;
		OnPartyChatStateChanged.Broadcast(false);
	}
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid())
	{
		return;
	}
	Async(EAsyncExecution::Thread, [CoreRef]()
	{
		FScopeLock Lock(&CoreRef->CS);
		if (CoreRef->Chat != nullptr)
		{
			fsdk_chat_destroy(CoreRef->Chat); // closes the socket via the WS seam
			CoreRef->Chat = nullptr;
		}
	});
}

// ── In-game social (redacted reads + whisper over fid GameScope) ────────────
// Same worker-thread shape as the matchmaking calls: the core ABI is blocking
// HTTP, so every refresh runs off-thread and broadcasts back on the game
// thread. The server enforces the redaction - these can only ever read the
// friends list / own party and use the friends-only whisper surface.

void UFoundryFSDKSubsystem::RefreshFriends()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnFriendsUpdated.Broadcast(EFoundryFsdkResult::NotAuthenticated, {});
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		TArray<fsdk_friend> Buf;
		Buf.SetNum(64); // server caps friends at 50
		size_t Count = 0;
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = fsdk_social_friends(CoreRef->Client, Buf.GetData(),
				static_cast<size_t>(Buf.Num()), &Count);
		}
		TArray<FFoundryFriend> Friends;
		Friends.Reserve(static_cast<int32>(Count));
		for (size_t i = 0; Result == FSDK_OK && i < Count; i++)
		{
			FFoundryFriend F;
			F.FoundryId = UTF8_TO_TCHAR(Buf[i].foundry_id);
			F.DisplayName = UTF8_TO_TCHAR(Buf[i].display_name);
			F.Username = UTF8_TO_TCHAR(Buf[i].username);
			F.Presence = UTF8_TO_TCHAR(Buf[i].presence);
			F.PresenceGame = UTF8_TO_TCHAR(Buf[i].presence_game);
			Friends.Add(MoveTemp(F));
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Friends = MoveTemp(Friends)]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnFriendsUpdated.Broadcast(ToBlueprintResult(Result), Friends);
			}
		});
	});
}

void UFoundryFSDKSubsystem::RefreshParty()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnPartyUpdated.Broadcast(EFoundryFsdkResult::NotAuthenticated, FFoundryParty());
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		fsdk_party Header;
		TArray<fsdk_party_member> Buf;
		Buf.SetNum(16);
		size_t Count = 0;
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = fsdk_social_party_info(CoreRef->Client, &Header, Buf.GetData(),
				static_cast<size_t>(Buf.Num()), &Count);
		}
		FFoundryParty Party;
		if (Result == FSDK_OK)
		{
			Party.PartyId = UTF8_TO_TCHAR(Header.id);
			Party.LeaderFoundryId = UTF8_TO_TCHAR(Header.leader_foundry_id);
			Party.MaxSize = Header.max_size;
			Party.Members.Reserve(static_cast<int32>(Count));
			for (size_t i = 0; i < Count; i++)
			{
				FFoundryPartyMember M;
				M.FoundryId = UTF8_TO_TCHAR(Buf[i].foundry_id);
				M.DisplayName = UTF8_TO_TCHAR(Buf[i].display_name);
				M.Username = UTF8_TO_TCHAR(Buf[i].username);
				M.State = UTF8_TO_TCHAR(Buf[i].state);
				Party.Members.Add(MoveTemp(M));
			}
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Party = MoveTemp(Party)]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnPartyUpdated.Broadcast(ToBlueprintResult(Result), Party);
			}
		});
	});
}

void UFoundryFSDKSubsystem::RefreshPendingRequests()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnPendingUpdated.Broadcast(EFoundryFsdkResult::NotAuthenticated, {});
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		TArray<fsdk_friend> Buf;
		Buf.SetNum(64);
		size_t Count = 0;
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = fsdk_social_pending(CoreRef->Client, Buf.GetData(),
				static_cast<size_t>(Buf.Num()), &Count);
		}
		TArray<FFoundryFriend> Pending;
		Pending.Reserve(static_cast<int32>(Count));
		for (size_t i = 0; Result == FSDK_OK && i < Count; i++)
		{
			FFoundryFriend F;
			F.FoundryId = UTF8_TO_TCHAR(Buf[i].foundry_id);
			F.DisplayName = UTF8_TO_TCHAR(Buf[i].display_name);
			F.Username = UTF8_TO_TCHAR(Buf[i].username);
			F.Presence = UTF8_TO_TCHAR(Buf[i].presence);
			Pending.Add(MoveTemp(F));
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Pending = MoveTemp(Pending)]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnPendingUpdated.Broadcast(ToBlueprintResult(Result), Pending);
			}
		});
	});
}

void UFoundryFSDKSubsystem::RunSocialAction(const FString& Action, bool bRefreshPartyOnOk,
	TFunction<int32(FFsdkCoreState&)> Call)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnSocialActionComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated, Action);
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis, Action, bRefreshPartyOnOk,
		Call = MoveTemp(Call)]()
	{
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = static_cast<fsdk_result>(Call(*CoreRef));
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, Action, bRefreshPartyOnOk]()
		{
			UFoundryFSDKSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}
			Self->OnSocialActionComplete.Broadcast(ToBlueprintResult(Result), Action);
			if (bRefreshPartyOnOk && Result == FSDK_OK)
			{
				Self->RefreshParty();
			}
		});
	});
}

void UFoundryFSDKSubsystem::SendFriendRequest(const FString& Username)
{
	RunSocialAction(TEXT("friend.request"), false, [Username](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_friend_request(State.Client, TCHAR_TO_UTF8(*Username));
	});
}

void UFoundryFSDKSubsystem::AcceptFriendRequest(const FString& RequesterFoundryId)
{
	RunSocialAction(TEXT("friend.accept"), false, [RequesterFoundryId](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_friend_accept(State.Client, TCHAR_TO_UTF8(*RequesterFoundryId));
	});
}

void UFoundryFSDKSubsystem::RemoveFriend(const FString& FriendFoundryId)
{
	RunSocialAction(TEXT("friend.remove"), false, [FriendFoundryId](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_friend_remove(State.Client, TCHAR_TO_UTF8(*FriendFoundryId));
	});
}

void UFoundryFSDKSubsystem::BlockPlayer(const FString& FoundryId)
{
	RunSocialAction(TEXT("friend.block"), false, [FoundryId](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_friend_block(State.Client, TCHAR_TO_UTF8(*FoundryId));
	});
}

void UFoundryFSDKSubsystem::UnblockPlayer(const FString& FoundryId)
{
	RunSocialAction(TEXT("friend.unblock"), false, [FoundryId](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_friend_unblock(State.Client, TCHAR_TO_UTF8(*FoundryId));
	});
}

void UFoundryFSDKSubsystem::GetMyFriendCode()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnFriendCodeReady.Broadcast(EFoundryFsdkResult::NotAuthenticated, FString());
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		char Code[24] = {0};
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = fsdk_social_friend_code(CoreRef->Client, Code, sizeof(Code));
		}
		const FString CodeStr = UTF8_TO_TCHAR(Code);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, CodeStr]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnFriendCodeReady.Broadcast(ToBlueprintResult(Result), CodeStr);
			}
		});
	});
}

void UFoundryFSDKSubsystem::RedeemFriendCode(const FString& Code)
{
	RunSocialAction(TEXT("code.redeem"), false, [Code](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_redeem_code(State.Client, TCHAR_TO_UTF8(*Code));
	});
}

void UFoundryFSDKSubsystem::CreateParty()
{
	RunSocialAction(TEXT("party.create"), true, [](FFsdkCoreState& State)
	{
		char PartyId[64];
		return (int32)fsdk_social_party_create(State.Client, PartyId, sizeof(PartyId));
	});
}

void UFoundryFSDKSubsystem::InviteToParty(const FString& PartyId, const FString& Username)
{
	RunSocialAction(TEXT("party.invite"), false, [PartyId, Username](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_party_invite(State.Client,
			TCHAR_TO_UTF8(*PartyId), TCHAR_TO_UTF8(*Username));
	});
}

void UFoundryFSDKSubsystem::AcceptPartyInvite(const FString& PartyId)
{
	RunSocialAction(TEXT("party.accept"), true, [PartyId](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_party_accept(State.Client, TCHAR_TO_UTF8(*PartyId));
	});
}

void UFoundryFSDKSubsystem::DeclinePartyInvite(const FString& PartyId)
{
	RunSocialAction(TEXT("party.decline"), true, [PartyId](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_party_decline(State.Client, TCHAR_TO_UTF8(*PartyId));
	});
}

void UFoundryFSDKSubsystem::LeaveParty(const FString& PartyId)
{
	RunSocialAction(TEXT("party.leave"), true, [PartyId](FFsdkCoreState& State)
	{
		return (int32)fsdk_social_party_leave(State.Client, TCHAR_TO_UTF8(*PartyId));
	});
}

void UFoundryFSDKSubsystem::RefreshConversations()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnConversationsUpdated.Broadcast(EFoundryFsdkResult::NotAuthenticated, {});
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		TArray<fsdk_dm_conversation> Buf;
		Buf.SetNum(64);
		size_t Count = 0;
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = fsdk_dm_conversations(CoreRef->Client, Buf.GetData(),
				static_cast<size_t>(Buf.Num()), &Count);
		}
		TArray<FFoundryDmConversation> Conversations;
		Conversations.Reserve(static_cast<int32>(Count));
		for (size_t i = 0; Result == FSDK_OK && i < Count; i++)
		{
			FFoundryDmConversation C;
			C.FoundryId = UTF8_TO_TCHAR(Buf[i].foundry_id);
			C.DisplayName = UTF8_TO_TCHAR(Buf[i].display_name);
			C.Presence = UTF8_TO_TCHAR(Buf[i].presence);
			C.LastBody = UTF8_TO_TCHAR(Buf[i].last_body);
			C.Unread = static_cast<int64>(Buf[i].unread);
			C.bLastFromMe = Buf[i].last_from_me != 0;
			Conversations.Add(MoveTemp(C));
		}
		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Result, Conversations = MoveTemp(Conversations)]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnConversationsUpdated.Broadcast(ToBlueprintResult(Result), Conversations);
			}
		});
	});
}

void UFoundryFSDKSubsystem::RefreshWhisperHistory(const FString& FriendFoundryId)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnWhisperHistoryUpdated.Broadcast(EFoundryFsdkResult::NotAuthenticated, FriendFoundryId, {});
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, FriendFoundryId, WeakThis]()
	{
		// fsdk_dm_message carries a 2KB body buffer - heap, never the stack.
		TArray<fsdk_dm_message> Buf;
		Buf.SetNum(64);
		size_t Count = 0;
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = fsdk_dm_history(CoreRef->Client, TCHAR_TO_UTF8(*FriendFoundryId),
				Buf.GetData(), static_cast<size_t>(Buf.Num()), &Count);
		}
		TArray<FFoundryDmMessage> Messages;
		Messages.Reserve(static_cast<int32>(Count));
		for (size_t i = 0; Result == FSDK_OK && i < Count; i++)
		{
			FFoundryDmMessage M;
			M.Id = static_cast<int64>(Buf[i].id);
			M.bFromMe = Buf[i].from_me != 0;
			M.bUnsent = Buf[i].unsent != 0;
			M.Body = UTF8_TO_TCHAR(Buf[i].body);
			M.CreatedAt = UTF8_TO_TCHAR(Buf[i].created_at);
			Messages.Add(MoveTemp(M));
		}
		AsyncTask(ENamedThreads::GameThread,
			[WeakThis, Result, FriendFoundryId, Messages = MoveTemp(Messages)]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnWhisperHistoryUpdated.Broadcast(
					ToBlueprintResult(Result), FriendFoundryId, Messages);
			}
		});
	});
}

void UFoundryFSDKSubsystem::SendWhisper(const FString& FriendFoundryId, const FString& Body)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnWhisperSendComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated);
		return;
	}
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, FriendFoundryId, Body, WeakThis]()
	{
		fsdk_result Result;
		{
			FScopeLock Lock(&CoreRef->CS);
			Result = fsdk_dm_send(CoreRef->Client,
				TCHAR_TO_UTF8(*FriendFoundryId), TCHAR_TO_UTF8(*Body));
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->OnWhisperSendComplete.Broadcast(ToBlueprintResult(Result));
			}
		});
	});
}

void UFoundryFSDKSubsystem::MarkWhisperRead(const FString& FriendFoundryId)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		return;
	}
	Async(EAsyncExecution::Thread, [CoreRef, FriendFoundryId]()
	{
		FScopeLock Lock(&CoreRef->CS);
		(void)fsdk_dm_mark_read(CoreRef->Client, TCHAR_TO_UTF8(*FriendFoundryId));
	});
}

bool UFoundryFSDKSubsystem::ChatDriverTick(float /*DeltaSeconds*/)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core;
	if (!CoreRef.IsValid())
	{
		return true;
	}
	TArray<FString> Frames;
	bool bDropped = false;
	{
		FScopeLock QLock(&CoreRef->ChatQueueCS);
		Frames = MoveTemp(CoreRef->ChatInbound);
		CoreRef->ChatInbound.Reset();
		bDropped = CoreRef->bChatClosed;
		CoreRef->bChatClosed = false;
	}

	bool bReadyNow = bChatReady;
	bool bPartyReadyNow = bPartyChatReady;
	if (CoreRef->CS.TryLock())
	{
		if (CoreRef->Chat != nullptr)
		{
			for (const FString& Frame : Frames)
			{
				fsdk_chat_on_ws_text(CoreRef->Chat, TCHAR_TO_UTF8(*Frame));
			}
			if (bDropped)
			{
				fsdk_chat_on_ws_closed(CoreRef->Chat);
			}
			fsdk_chat_tick(CoreRef->Chat,
				static_cast<long long>(FPlatformTime::Seconds() * 1000.0));
			bReadyNow = fsdk_chat_ready(CoreRef->Chat) != 0;
			bPartyReadyNow =
				fsdk_chat_channel_ready(CoreRef->Chat, FSDK_CHAT_CHANNEL_PARTY) != 0;
		}
		CoreRef->CS.Unlock();
	}
	else if (!Frames.IsEmpty() || bDropped)
	{
		// A worker holds the core (join in flight): requeue IN ORDER for next tick.
		FScopeLock QLock(&CoreRef->ChatQueueCS);
		Frames.Append(MoveTemp(CoreRef->ChatInbound));
		CoreRef->ChatInbound = MoveTemp(Frames);
		CoreRef->bChatClosed |= bDropped;
		return true;
	}

	// Broadcast staged messages AFTER the core lock is released.
	if (!CoreRef->ChatMessages.IsEmpty())
	{
		TArray<FFsdkCoreState::FChatMsgCopy> Msgs = MoveTemp(CoreRef->ChatMessages);
		CoreRef->ChatMessages.Reset();
		for (const FFsdkCoreState::FChatMsgCopy& Msg : Msgs)
		{
			OnChatMessage.Broadcast(Msg.Channel, Msg.DisplayName, Msg.FoundryId, Msg.Body);
		}
	}

	if (bPartyReadyNow != bPartyChatReady)
	{
		bPartyChatReady = bPartyReadyNow;
		OnPartyChatStateChanged.Broadcast(bPartyChatReady);
	}

	if (bReadyNow != bChatReady)
	{
		bChatReady = bReadyNow;
		OnChatStateChanged.Broadcast(bChatReady);
		if (bChatReady)
		{
			ChatRejoinStrikes = 0;
		}
		else if (bChatDesired)
		{
			ChatRejoinStrikes = FMath::Min(ChatRejoinStrikes + 1, 5);
			NextChatRejoinTime =
				FPlatformTime::Seconds() + static_cast<double>(1 << ChatRejoinStrikes);
		}
	}

	// Auto-rejoin while the game still wants the room.
	if (bChatDesired && !bChatReady && !bChatJoinInFlight
		&& FPlatformTime::Seconds() >= NextChatRejoinTime)
	{
		StartChatJoin();
	}
	return true; // keep ticking
}

// ── FID auth ────────────────────────────────────────────────────────────────

TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> UFoundryFSDKSubsystem::EnsureClient()
{
	if (!Core.IsValid() || Core->Client == nullptr)
	{
		InitializeClient(TEXT("https://api.foundryplatform.app"));
	}
	return Core;
}

void UFoundryFSDKSubsystem::ApplyLoginResult(EFoundryFsdkResult Result, const FString& DisplayName,
                                             const FString& FoundryId)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		bIsLoggedIn = true;
		CachedDisplayName = DisplayName;
		CachedFoundryId = FoundryId;
	}
	OnLoginComplete.Broadcast(Result, DisplayName);
}

#if FOUNDRY_FSDK_FID_AUTH

void UFoundryFSDKSubsystem::Login(const FString& EmailOrUsername, const FString& Password, bool bRememberMe)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnLoginComplete.Broadcast(EFoundryFsdkResult::Internal, FString());
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	const FString Eou = EmailOrUsername;     // deep copies for the worker
	const FString Pw = Password;             // player's own password; transient, never logged
	const int RememberInt = bRememberMe ? 1 : 0;

	Async(EAsyncExecution::Thread, [CoreRef, WeakThis, Eou, Pw, RememberInt]()
	{
		fsdk_result R;
		FString DisplayName, FoundryId;
		{
			FScopeLock Lock(&CoreRef->CS);
			const FTCHARToUTF8 EouUtf8(*Eou);
			const FTCHARToUTF8 PwUtf8(*Pw);
			R = fsdk_login(CoreRef->Client, EouUtf8.Get(), PwUtf8.Get(), RememberInt);
			if (R == FSDK_OK)
			{
				fsdk_session S;
				if (fsdk_current_session(CoreRef->Client, &S) == FSDK_OK)
				{
					DisplayName = UTF8_TO_TCHAR(S.display_name);
					FoundryId = UTF8_TO_TCHAR(S.foundry_id);
				}
			}
		}
		const EFoundryFsdkResult Result = ToBlueprintResult(R);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, DisplayName, FoundryId]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->ApplyLoginResult(Result, DisplayName, FoundryId);
			}
		});
	});
}

void UFoundryFSDKSubsystem::Refresh()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnLoginComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated, FString());
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		fsdk_result R;
		FString DisplayName, FoundryId;
		{
			FScopeLock Lock(&CoreRef->CS);
			R = fsdk_refresh(CoreRef->Client);
			if (R == FSDK_OK)
			{
				fsdk_session S;
				if (fsdk_current_session(CoreRef->Client, &S) == FSDK_OK)
				{
					DisplayName = UTF8_TO_TCHAR(S.display_name);
					FoundryId = UTF8_TO_TCHAR(S.foundry_id);
				}
			}
		}
		const EFoundryFsdkResult Result = ToBlueprintResult(R);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, DisplayName, FoundryId]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->ApplyLoginResult(Result, DisplayName, FoundryId);
			}
		});
	});
}

void UFoundryFSDKSubsystem::TryResumeSession()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnLoginComplete.Broadcast(EFoundryFsdkResult::NotAuthenticated, FString());
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		fsdk_result R;
		FString DisplayName, FoundryId;
		{
			FScopeLock Lock(&CoreRef->CS);
			R = fsdk_try_resume(CoreRef->Client);
			if (R == FSDK_OK)
			{
				fsdk_session S;
				if (fsdk_current_session(CoreRef->Client, &S) == FSDK_OK)
				{
					DisplayName = UTF8_TO_TCHAR(S.display_name);
					FoundryId = UTF8_TO_TCHAR(S.foundry_id);
				}
			}
		}
		const EFoundryFsdkResult Result = ToBlueprintResult(R);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Result, DisplayName, FoundryId]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->ApplyLoginResult(Result, DisplayName, FoundryId);
			}
		});
	});
}

void UFoundryFSDKSubsystem::Logout()
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = Core; // nothing to revoke if no client
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		bIsLoggedIn = false;
		CachedDisplayName.Empty();
		CachedFoundryId.Empty();
		OnLoggedOut.Broadcast();
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		{
			FScopeLock Lock(&CoreRef->CS);
			fsdk_logout(CoreRef->Client);
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->bIsLoggedIn = false;
				Self->CachedDisplayName.Empty();
				Self->CachedFoundryId.Empty();
				Self->OnLoggedOut.Broadcast();
			}
		});
	});
}

#else // !FOUNDRY_FSDK_FID_AUTH - default (launcher) build: inert stubs, no login machinery

void UFoundryFSDKSubsystem::Login(const FString& /*EmailOrUsername*/, const FString& /*Password*/, bool /*bRememberMe*/)
{
	// FID-embedded auth is not compiled in this build; the default path is the
	// launcher handoff (AutoLoginFromLauncher). No fsdk_login machinery shipped.
	ApplyLoginResult(EFoundryFsdkResult::NotImplemented, FString(), FString());
}

void UFoundryFSDKSubsystem::Refresh()
{
	ApplyLoginResult(EFoundryFsdkResult::NotImplemented, FString(), FString());
}

void UFoundryFSDKSubsystem::TryResumeSession()
{
	ApplyLoginResult(EFoundryFsdkResult::NotImplemented, FString(), FString());
}

void UFoundryFSDKSubsystem::Logout()
{
	OnLoggedOut.Broadcast();
}

#endif // FOUNDRY_FSDK_FID_AUTH

void UFoundryFSDKSubsystem::AutoLoginFromLauncher()
{
	// DEFAULT sign-in: read a scoped match token from the launcher session daemon
	// (FOUNDRY_IPC) on a worker, then authenticate on the game thread. No handoff ->
	// fail fast (NotAuthenticated); the menu surfaces "sign in through the launcher".
	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [WeakThis]()
	{
		FString Token;
		const bool bGot = FoundryFSDKReadLauncherToken(Token) && !Token.IsEmpty();
		AsyncTask(ENamedThreads::GameThread, [WeakThis, bGot, Token]()
		{
			UFoundryFSDKSubsystem* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}
			if (bGot)
			{
				// A daemon-minted token is short-lived AND re-mintable: remember the
				// source so long-lived surfaces (the chat socket) can transparently
				// re-handoff when it expires mid-session.
				Self->bLauncherSession = true;
				Self->SetPlayerToken(Token); // sets logged-in + broadcasts OnLoginComplete(Ok)
			}
			else
			{
				Self->ApplyLoginResult(EFoundryFsdkResult::NotAuthenticated, FString(), FString());
			}
		});
	});
}

void UFoundryFSDKSubsystem::SetPlayerToken(const FString& PlayerToken)
{
	// BYO identity: set the token + authenticated under the lock (no network), then
	// BEST-EFFORT fetch the platform identity snapshot on a worker so the login
	// broadcast carries the player's real display name (the launcher-handoff path
	// used to broadcast an empty name -> games showed a placeholder). A token FID
	// doesn't recognize (true BYO) just fails the probe and broadcasts Ok with an
	// empty name - exactly the old behavior.
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		OnLoginComplete.Broadcast(EFoundryFsdkResult::Internal, FString());
		return;
	}
	fsdk_result R;
	{
		FScopeLock Lock(&CoreRef->CS);
		const FTCHARToUTF8 TokenUtf8(*PlayerToken);
		R = fsdk_set_player_token(CoreRef->Client, TokenUtf8.Get());
	}
	if (R != FSDK_OK)
	{
		ApplyLoginResult(ToBlueprintResult(R), FString(), FString());
		return;
	}

	TWeakObjectPtr<UFoundryFSDKSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [CoreRef, WeakThis]()
	{
		FString DisplayName, FoundryId;
		{
			FScopeLock Lock(&CoreRef->CS);
			if (fsdk_refresh_session(CoreRef->Client) == FSDK_OK)
			{
				fsdk_session S;
				if (fsdk_current_session(CoreRef->Client, &S) == FSDK_OK)
				{
					DisplayName = UTF8_TO_TCHAR(S.display_name);
					FoundryId = UTF8_TO_TCHAR(S.foundry_id);
				}
			}
		}
		AsyncTask(ENamedThreads::GameThread, [WeakThis, DisplayName, FoundryId]()
		{
			if (UFoundryFSDKSubsystem* Self = WeakThis.Get())
			{
				Self->ApplyLoginResult(EFoundryFsdkResult::Ok, DisplayName, FoundryId);
			}
		});
	});
}

#if FOUNDRY_FSDK_FID_AUTH
void UFoundryFSDKSubsystem::SetAuthBaseUrl(const FString& AuthBaseUrl)
{
	TSharedPtr<FFsdkCoreState, ESPMode::ThreadSafe> CoreRef = EnsureClient();
	if (!CoreRef.IsValid() || CoreRef->Client == nullptr)
	{
		return;
	}
	FScopeLock Lock(&CoreRef->CS);
	const FTCHARToUTF8 BaseUtf8(*AuthBaseUrl);
	fsdk_set_auth_base(CoreRef->Client, BaseUtf8.Get());
}
#else // !FOUNDRY_FSDK_FID_AUTH
void UFoundryFSDKSubsystem::SetAuthBaseUrl(const FString& /*AuthBaseUrl*/) {}
#endif // FOUNDRY_FSDK_FID_AUTH
