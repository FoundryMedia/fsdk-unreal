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

#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "Logging/LogMacros.h"

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
	fsdk_set_log_sink(nullptr, nullptr);
}
