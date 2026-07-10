// Copyright Foundry Media. fsdk-core <-> Unreal host bridges (HTTP + logging).
//
// fsdk-core is engine-agnostic and bakes in NO network stack: it builds each
// request and hands it to a host transport (fsdk_set_http_transport). This file
// backs that seam with the engine's HTTP module (the engine owns TLS), and routes
// fsdk-core's log sink to UE_LOG.
//
// SECURITY: the player's FID token flows only as the bearer credential; it is
// NEVER logged or persisted here, and the core never logs secrets through the
// sink. The client surface touches only player-scoped endpoints (the core builds
// the paths) - see ../../fsdk-core/SECURITY.md.
//
// THREADING: the fsdk-core client ABI is synchronous, so the transport BLOCKS
// until the request completes, pumping the HTTP manager. It MUST be called on the
// game thread (the only thread that may tick the manager). For a proof / dogfood
// this brief block is acceptable; a production binding would run the core calls on
// a worker and surface results asynchronously (the ABI/Blueprint surface wouldn't
// change - only where the synchronous core call is driven from).

#include "FoundryFSDKTransport.h"

#include "foundry/fsdk.h"

#include "Async/Async.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "IWebSocket.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "Logging/LogMacros.h"
#include "Misc/ScopeLock.h"
#include "WebSocketsModule.h"

#include <cstdlib>

DEFINE_LOG_CATEGORY_STATIC(LogFoundryFSDKCore, Log, All);

namespace
{
	/** Result captured by the completion lambda; heap-owned (shared) so it safely
	 *  outlives a timed-out request that completes later. bDone is the atomic
	 *  release/acquire barrier: the completing thread writes Code/Body, THEN sets
	 *  bDone; the waiting thread observes bDone, THEN reads Code/Body. */
	struct FFsdkHttpResult
	{
		FThreadSafeBool bDone{ false };
		bool            bOk = false;
		int32           Code = 0;
		FString         Body;
	};
}

// The core's callback typedefs have C language linkage; define the callbacks with
// matching linkage so the function-pointer types agree across compilers.
extern "C"
{
	/** fsdk_log_fn: route core log messages to UE_LOG (descriptions only - the
	 *  core contract forbids passing secrets through the sink). */
	static void FoundryFSDKLogSink(fsdk_log_level Level, const char* Message, void* /*UserData*/)
	{
		if (Message == nullptr)
		{
			return;
		}
		const FString Msg = UTF8_TO_TCHAR(Message);
		switch (Level)
		{
			case FSDK_LOG_ERROR: UE_LOG(LogFoundryFSDKCore, Error,   TEXT("%s"), *Msg); break;
			case FSDK_LOG_WARN:  UE_LOG(LogFoundryFSDKCore, Warning, TEXT("%s"), *Msg); break;
			case FSDK_LOG_INFO:  UE_LOG(LogFoundryFSDKCore, Log,     TEXT("%s"), *Msg); break;
			default:             UE_LOG(LogFoundryFSDKCore, Verbose, TEXT("%s"), *Msg); break;
		}
	}

	/** fsdk_http_fn: perform one request via the engine HTTP module, blocking
	 *  until it completes (see THREADING note above). */
	static fsdk_result FoundryFSDKHttpTransport(fsdk_http_method Method,
	                                            const char* Url,
	                                            const char* BearerToken,
	                                            const char* BodyJson,
	                                            char** OutBody,
	                                            long* OutStatus,
	                                            void* /*UserData*/)
	{
		if (OutBody != nullptr)   { *OutBody = nullptr; }
		if (OutStatus != nullptr) { *OutStatus = 0; }
		if (Url == nullptr)       { return FSDK_ERR_INVALID_ARG; }

		if (!FHttpModule::Get().IsHttpEnabled())
		{
			return FSDK_ERR_NETWORK;
		}

		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(UTF8_TO_TCHAR(Url));
		switch (Method)
		{
			case FSDK_HTTP_POST:   Request->SetVerb(TEXT("POST"));   break;
			case FSDK_HTTP_DELETE: Request->SetVerb(TEXT("DELETE")); break;
			case FSDK_HTTP_GET:
			default:               Request->SetVerb(TEXT("GET"));    break;
		}
		if (BearerToken != nullptr && BearerToken[0] != '\0')
		{
			Request->SetHeader(TEXT("Authorization"),
				FString(TEXT("Bearer ")) + UTF8_TO_TCHAR(BearerToken));
		}
		if (BodyJson != nullptr && BodyJson[0] != '\0')
		{
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			Request->SetContentAsString(UTF8_TO_TCHAR(BodyJson));
		}
		Request->SetTimeout(20.0f);

		TSharedRef<FFsdkHttpResult, ESPMode::ThreadSafe> State =
			MakeShared<FFsdkHttpResult, ESPMode::ThreadSafe>();
		Request->OnProcessRequestComplete().BindLambda(
			[State](FHttpRequestPtr /*Req*/, FHttpResponsePtr Response, bool bSucceeded)
			{
				State->bOk = bSucceeded && Response.IsValid();
				if (State->bOk)
				{
					State->Code = Response->GetResponseCode();
					State->Body = Response->GetContentAsString();
				}
				State->bDone = true; // release: set LAST, after Code/Body
			});

		if (!Request->ProcessRequest())
		{
			return FSDK_ERR_NETWORK;
		}

		// Synchronous bridge: block until completion. Only the GAME THREAD may tick
		// the HTTP manager, so pump it here when we're on it (the sync/proof path);
		// on a worker thread (the async subsystem path) the game thread ticks for
		// us and we just poll. Either way no game-thread hitch in the async path.
		const bool bGameThread = IsInGameThread();
		FHttpManager& Manager = FHttpModule::Get().GetHttpManager();
		const double Deadline = FPlatformTime::Seconds() + 25.0;
		while (!State->bDone && FPlatformTime::Seconds() < Deadline)
		{
			if (bGameThread)
			{
				Manager.Tick(0.0f);
			}
			FPlatformProcess::Sleep(0.005f);
		}

		if (!State->bDone)
		{
			Request->CancelRequest();
			return FSDK_ERR_TIMEOUT;
		}
		if (!State->bOk)
		{
			return FSDK_ERR_NETWORK;
		}

		if (OutStatus != nullptr)
		{
			*OutStatus = static_cast<long>(State->Code);
		}
		if (OutBody != nullptr)
		{
			// MUST pair with the core's free() (fsdk_string_free) - allocate with
			// CRT malloc, NOT FMemory, so the heaps match.
			const FTCHARToUTF8 Utf8(*State->Body);
			const int32 Len = Utf8.Length();
			char* Buffer = static_cast<char*>(::malloc(static_cast<size_t>(Len) + 1));
			if (Buffer == nullptr)
			{
				return FSDK_ERR_INTERNAL;
			}
			FMemory::Memcpy(Buffer, Utf8.Get(), static_cast<SIZE_T>(Len));
			Buffer[Len] = '\0';
			*OutBody = Buffer;
		}
		return FSDK_OK;
	}
} // extern "C"

// ── WS bridge (fsdk_set_ws_transport - FRC chat) ────────────────────────────
//
// The core opens/uses the socket through three C callbacks that may fire on a
// WORKER thread (fsdk_chat_join_global runs its blocking room-resolve there).
// ALL IWebSocket interaction is marshaled to the GAME thread (UE's WebSockets
// events fire there anyway); outbound frames sent before OnConnected queue in
// order. Inbound frames go to the registered SINK (the subsystem's queue) on
// the game thread — the sink must be cheap and non-reentrant; frames arriving
// before a sink exists buffer in order. SINGLE active session by design (a new
// connect supersedes the old socket's delivery). Frame payloads are NEVER
// logged (the auth frame carries the session token).

namespace
{
	/** One host-owned socket (the core's opaque WS handle). Socket + queues are
	 *  game-thread-only; the wrapper is ref-counted so late game-thread tasks
	 *  can't touch a freed wrapper after close. */
	struct FFsdkWsSocket
	{
		FString Url;
		TSharedPtr<IWebSocket> Socket; // game thread only
		TArray<FString> PendingSends;  // queued until OnConnected (game thread)
		bool bOpen = false;            // game thread only
		bool bClosedDelivered = false; // game thread only (once-only close signal)
	};
	using FFsdkWsSocketRef = TSharedPtr<FFsdkWsSocket, ESPMode::ThreadSafe>;

	FCriticalSection GFsdkWsLock;
	FFsdkWsSocketRef GFsdkWsActive;                  // the single live session
	TFunction<void(const FString&)> GFsdkWsOnText;   // sink (subsystem-registered)
	TFunction<void()> GFsdkWsOnClosed;
	TArray<FString> GFsdkWsPreSinkBuffer;            // inbound before a sink exists
	bool GFsdkWsPendingClosed = false;

	/** Game thread: route one inbound frame to the sink (or buffer it). Ignores
	 *  frames from a superseded socket. */
	void FsdkWsDeliverText(const FFsdkWsSocketRef& Wrapper, const FString& Text)
	{
		FScopeLock Lock(&GFsdkWsLock);
		if (GFsdkWsActive != Wrapper)
		{
			return; // stale socket (superseded by a newer connect)
		}
		if (GFsdkWsOnText)
		{
			GFsdkWsOnText(Text);
		}
		else
		{
			GFsdkWsPreSinkBuffer.Add(Text);
		}
	}

	/** Game thread: signal the drop once (skipped entirely for a deliberate
	 *  core-initiated close — the core already knows). */
	void FsdkWsDeliverClosed(const FFsdkWsSocketRef& Wrapper)
	{
		if (Wrapper->bClosedDelivered)
		{
			return;
		}
		Wrapper->bClosedDelivered = true;
		FScopeLock Lock(&GFsdkWsLock);
		if (GFsdkWsActive != Wrapper)
		{
			return;
		}
		if (GFsdkWsOnClosed)
		{
			GFsdkWsOnClosed();
		}
		else
		{
			GFsdkWsPendingClosed = true;
		}
	}
}

extern "C"
{
	/** fsdk_ws_connect_fn: hand back a wrapper immediately; the real socket is
	 *  created + connected on the game thread (connection may still be in-flight
	 *  per the seam contract — the core learns outcomes via the feed calls). */
	static fsdk_result FoundryFSDKWsConnect(const char* Url, void** OutHandle, void* /*UserData*/)
	{
		if (Url == nullptr || OutHandle == nullptr)
		{
			return FSDK_ERR_INVALID_ARG;
		}
		*OutHandle = nullptr;

		FFsdkWsSocketRef Wrapper = MakeShared<FFsdkWsSocket, ESPMode::ThreadSafe>();
		Wrapper->Url = UTF8_TO_TCHAR(Url);
		{
			FScopeLock Lock(&GFsdkWsLock);
			GFsdkWsActive = Wrapper; // supersede: older sockets stop delivering
			GFsdkWsPreSinkBuffer.Reset();
			GFsdkWsPendingClosed = false;
		}

		AsyncTask(ENamedThreads::GameThread, [Wrapper]()
		{
			TSharedPtr<IWebSocket> Socket =
				FWebSocketsModule::Get().CreateWebSocket(Wrapper->Url, FString());
			if (!Socket.IsValid())
			{
				FsdkWsDeliverClosed(Wrapper);
				return;
			}
			Wrapper->Socket = Socket;
			Socket->OnConnected().AddLambda([Wrapper]()
			{
				Wrapper->bOpen = true;
				for (const FString& Frame : Wrapper->PendingSends)
				{
					Wrapper->Socket->Send(Frame);
				}
				Wrapper->PendingSends.Reset();
			});
			Socket->OnConnectionError().AddLambda([Wrapper](const FString& /*Error*/)
			{
				FsdkWsDeliverClosed(Wrapper);
			});
			Socket->OnClosed().AddLambda([Wrapper](int32 /*Code*/, const FString& /*Reason*/, bool /*bClean*/)
			{
				FsdkWsDeliverClosed(Wrapper);
			});
			Socket->OnMessage().AddLambda([Wrapper](const FString& Message)
			{
				FsdkWsDeliverText(Wrapper, Message);
			});
			Socket->Connect();
		});

		// The core's opaque handle: a heap holder of the shared ref (released in close).
		*OutHandle = new FFsdkWsSocketRef(Wrapper);
		return FSDK_OK;
	}

	/** fsdk_ws_send_fn: marshal one TEXT frame to the game thread; frames sent
	 *  before the socket opens queue in order. Transport failures surface as a
	 *  close (the seam has no sync failure channel once connect returned). */
	static fsdk_result FoundryFSDKWsSend(void* Handle, const char* Text, void* /*UserData*/)
	{
		if (Handle == nullptr || Text == nullptr)
		{
			return FSDK_ERR_INVALID_ARG;
		}
		FFsdkWsSocketRef Wrapper = *static_cast<FFsdkWsSocketRef*>(Handle);
		FString Frame = UTF8_TO_TCHAR(Text);
		AsyncTask(ENamedThreads::GameThread, [Wrapper, Frame = MoveTemp(Frame)]()
		{
			if (Wrapper->bOpen && Wrapper->Socket.IsValid())
			{
				Wrapper->Socket->Send(Frame);
			}
			else
			{
				Wrapper->PendingSends.Add(Frame);
			}
		});
		return FSDK_OK;
	}

	/** fsdk_ws_close_fn: deliberate close — release the core's handle now, close
	 *  the socket on the game thread, and never echo the drop back to the sink. */
	static void FoundryFSDKWsClose(void* Handle, void* /*UserData*/)
	{
		if (Handle == nullptr)
		{
			return;
		}
		FFsdkWsSocketRef* Holder = static_cast<FFsdkWsSocketRef*>(Handle);
		FFsdkWsSocketRef Wrapper = *Holder;
		delete Holder;
		AsyncTask(ENamedThreads::GameThread, [Wrapper]()
		{
			Wrapper->bClosedDelivered = true; // core asked — no closed echo
			if (Wrapper->Socket.IsValid())
			{
				Wrapper->Socket->Close();
				Wrapper->Socket.Reset();
			}
			FScopeLock Lock(&GFsdkWsLock);
			if (GFsdkWsActive == Wrapper)
			{
				GFsdkWsActive.Reset();
			}
		});
	}
} // extern "C"

void FoundryFSDKInstallWsTransport()
{
	fsdk_set_ws_transport(&FoundryFSDKWsConnect, &FoundryFSDKWsSend, &FoundryFSDKWsClose, nullptr);
}

void FoundryFSDKSetWsSink(TFunction<void(const FString&)> OnText, TFunction<void()> OnClosed)
{
	FScopeLock Lock(&GFsdkWsLock);
	GFsdkWsOnText = MoveTemp(OnText);
	GFsdkWsOnClosed = MoveTemp(OnClosed);
	// Flush anything that raced ahead of registration, in arrival order.
	if (GFsdkWsOnText)
	{
		for (const FString& Frame : GFsdkWsPreSinkBuffer)
		{
			GFsdkWsOnText(Frame);
		}
	}
	GFsdkWsPreSinkBuffer.Reset();
	if (GFsdkWsPendingClosed && GFsdkWsOnClosed)
	{
		GFsdkWsPendingClosed = false;
		GFsdkWsOnClosed();
	}
}

void FoundryFSDKClearWsSink()
{
	FScopeLock Lock(&GFsdkWsLock);
	GFsdkWsOnText = nullptr;
	GFsdkWsOnClosed = nullptr;
	GFsdkWsPreSinkBuffer.Reset();
	GFsdkWsPendingClosed = false;
}

void FoundryFSDKInstallHttpTransport()
{
	fsdk_set_http_transport(&FoundryFSDKHttpTransport, nullptr);
}

void FoundryFSDKInstallLogSink()
{
	fsdk_set_log_sink(&FoundryFSDKLogSink, nullptr);
}

void FoundryFSDKShutdownBridges()
{
	fsdk_set_http_transport(nullptr, nullptr);
	fsdk_set_ws_transport(nullptr, nullptr, nullptr, nullptr);
	fsdk_set_log_sink(nullptr, nullptr);
}
