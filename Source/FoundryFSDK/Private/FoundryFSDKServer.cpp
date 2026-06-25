// Copyright Foundry Media. Server-side FoundryFSDK facade (implementation).
//
// Server-only (UE_SERVER). Bridges the game's authoritative server code to
// fsdk-core's server ABI + the OpenSSL JWKS verifier. The fsdk-core server calls
// (ready/health/get_binding/shutdown) go out over the process-wide HTTP transport
// seam installed at module startup (FoundryFSDKTransport.cpp) to the Agones sidecar
// gateway; validate_player runs locally against the cached JWKS public keys.
//
// See ../../fsdk-core/SECURITY.md and .claude/rules/fsdk-security.md.

#include "FoundryFSDKServer.h"

#if UE_SERVER

#include "FoundryFSDKVerifier.h"

#include "foundry/fsdk.h"

#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoundryFSDKServer, Log, All);

// Opaque holder so the public header doesn't leak the C ABI.
struct FFsdkServerState
{
	fsdk_server* Server = nullptr;
};

TUniquePtr<FFoundryFSDKServer> FFoundryFSDKServer::Create(const FString& AgonesAddr, const FString& AuthBaseUrl)
{
	const FTCHARToUTF8 AddrUtf8(*AgonesAddr);
	const char* Addr = AgonesAddr.IsEmpty() ? nullptr : AddrUtf8.Get();

	fsdk_server* Server = nullptr;
	const fsdk_result R = fsdk_server_create(Addr, &Server);
	if (R != FSDK_OK || Server == nullptr)
	{
		UE_LOG(LogFoundryFSDKServer, Error, TEXT("fsdk_server_create failed: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(R)));
		return nullptr;
	}

	// Fill the verifier seam + pre-warm the JWKS so the first join doesn't block.
	FoundryFSDKInstallJwtVerifier(AuthBaseUrl);
	FoundryFSDKPrewarmJwks();

	TUniquePtr<FFoundryFSDKServer> Out(new FFoundryFSDKServer());
	Out->State = new FFsdkServerState();
	Out->State->Server = Server;
	UE_LOG(LogFoundryFSDKServer, Log, TEXT("FoundryFSDK server created (fsdk-core %s)"),
		UTF8_TO_TCHAR(fsdk_version()));
	return Out;
}

FFoundryFSDKServer::~FFoundryFSDKServer()
{
	if (State != nullptr)
	{
		if (State->Server != nullptr)
		{
			fsdk_server_destroy(State->Server);
			State->Server = nullptr;
		}
		delete State;
		State = nullptr;
	}
	FoundryFSDKShutdownJwtVerifier();
}

bool FFoundryFSDKServer::GetBindingAndReady()
{
	if (State == nullptr || State->Server == nullptr)
	{
		return false;
	}

	// Best-effort binding read (latches the bound match id for ValidatePlayer). The
	// match-id annotation may be absent until match_id is threaded through allocation
	// end to end - then validate_player simply skips the per-match binding check.
	char* Binding = nullptr;
	const fsdk_result BR = fsdk_server_get_binding(State->Server, &Binding);
	if (BR != FSDK_OK)
	{
		UE_LOG(LogFoundryFSDKServer, Warning, TEXT("get_binding failed (%s) - continuing to Ready"),
			UTF8_TO_TCHAR(fsdk_result_str(BR)));
	}
	if (Binding != nullptr)
	{
		fsdk_string_free(Binding);
	}

	const fsdk_result RR = fsdk_server_ready(State->Server);
	if (RR != FSDK_OK)
	{
		UE_LOG(LogFoundryFSDKServer, Error, TEXT("Agones Ready failed: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(RR)));
		return false;
	}
	return true;
}

void FFoundryFSDKServer::Health()
{
	if (State != nullptr && State->Server != nullptr)
	{
		fsdk_server_health(State->Server);
	}
}

bool FFoundryFSDKServer::ValidatePlayer(const FString& MatchToken, FString& OutFoundryId, FString& OutMatchId)
{
	OutFoundryId.Reset();
	OutMatchId.Reset();
	if (State == nullptr || State->Server == nullptr)
	{
		return false;
	}

	fsdk_player_info Info;
	FMemory::Memzero(&Info, sizeof(Info));
	const FTCHARToUTF8 TokenUtf8(*MatchToken);
	const fsdk_result R = fsdk_server_validate_player(State->Server, TokenUtf8.Get(), &Info);
	if (R != FSDK_OK)
	{
		// Descriptions only - never log the token itself.
		UE_LOG(LogFoundryFSDKServer, Warning, TEXT("ValidatePlayer rejected: %s"),
			UTF8_TO_TCHAR(fsdk_result_str(R)));
		return false;
	}
	OutFoundryId = UTF8_TO_TCHAR(Info.foundry_id);
	OutMatchId = UTF8_TO_TCHAR(Info.match_id);
	return true;
}

void FFoundryFSDKServer::Shutdown()
{
	if (State != nullptr && State->Server != nullptr)
	{
		fsdk_server_shutdown(State->Server);
	}
}

#endif // UE_SERVER
