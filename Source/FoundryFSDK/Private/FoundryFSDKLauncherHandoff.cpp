// Copyright Foundry Media. Launcher -> game session handoff (the DEFAULT auth path).
//
// Reads FOUNDRY_IPC (set by the launcher when it spawns the game), connects to the
// session daemon's local named pipe, presents the per-launch nonce, and receives a
// short-lived matchmaking token. The blast radius is bounded server-side (the daemon
// serves only a 15-min aud=fsdk-fmms token, never the session/refresh token), the
// pipe DACL is current-user-only, and the daemon creates it with
// FILE_FLAG_FIRST_PIPE_INSTANCE (anti-squat) + is started first by the launcher.

#include "FoundryFSDKLauncherHandoff.h"

#include "Misc/Parse.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	/** Extract a JSON string field value (`"key":"value"`) with a minimal scan -
	 *  the response is a small, well-known shape from our own daemon, so no full
	 *  JSON parser (keeps the client lean - see fsdk-security.md). */
	bool ExtractJsonString(const FString& Json, const TCHAR* Key, FString& Out)
	{
		const FString Needle = FString::Printf(TEXT("\"%s\":\""), Key);
		const int32 KeyAt = Json.Find(Needle, ESearchCase::CaseSensitive);
		if (KeyAt == INDEX_NONE)
		{
			return false;
		}
		const int32 Start = KeyAt + Needle.Len();
		const int32 End = Json.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
		if (End == INDEX_NONE)
		{
			return false;
		}
		Out = Json.Mid(Start, End - Start);
		return !Out.IsEmpty();
	}
}

bool FoundryFSDKReadLauncherToken(FString& OutToken)
{
#if PLATFORM_WINDOWS
	const FString Ipc = FPlatformMisc::GetEnvironmentVariable(TEXT("FOUNDRY_IPC"));
	if (Ipc.IsEmpty())
	{
		return false; // no launcher handoff -> caller fails fast
	}
	FString PipeName, Nonce;
	if (!Ipc.Split(TEXT(";"), &PipeName, &Nonce) || PipeName.IsEmpty() || Nonce.IsEmpty())
	{
		return false;
	}

	// Connect to the daemon pipe; the daemon pre-creates the next instance, but a
	// connect can race ERROR_PIPE_BUSY - wait + retry briefly.
	HANDLE Pipe = INVALID_HANDLE_VALUE;
	for (int32 Attempt = 0; Attempt < 5; ++Attempt)
	{
		Pipe = ::CreateFileW(*PipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
		if (Pipe != INVALID_HANDLE_VALUE)
		{
			break;
		}
		if (::GetLastError() != ERROR_PIPE_BUSY)
		{
			return false;
		}
		::WaitNamedPipeW(*PipeName, 200);
	}
	if (Pipe == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	// SECURITY (hardening TODO): verify the pipe SERVER's owner SID == the current
	// user and that its binary is Authenticode-signed by Foundry (anti-impersonation).
	// The daemon's FILE_FLAG_FIRST_PIPE_INSTANCE + current-user-only DACL, plus the
	// launcher starting the daemon before any game, already block squatting; this
	// client-side check is defense-in-depth and is gated on confirming the launcher
	// exe is signed. (GetNamedPipeServerProcessId -> OpenProcess -> WinVerifyTrust.)

	const FTCHARToUTF8 Request(
		*FString::Printf(TEXT("{\"op\":\"get-match-token\",\"nonce\":\"%s\"}\n"), *Nonce));
	DWORD Written = 0;
	const bool bWrote = ::WriteFile(Pipe, Request.Get(), static_cast<DWORD>(Request.Length()), &Written, nullptr);

	FString Response;
	if (bWrote)
	{
		TArray<uint8> Acc;
		uint8 Buf[2048];
		DWORD Read = 0;
		while (::ReadFile(Pipe, Buf, sizeof(Buf), &Read, nullptr) && Read > 0)
		{
			Acc.Append(Buf, static_cast<int32>(Read));
			if (Acc.Contains(static_cast<uint8>('\n')) || Acc.Num() > 16 * 1024)
			{
				break;
			}
		}
		Acc.Add(0);
		Response = UTF8_TO_TCHAR(reinterpret_cast<const char*>(Acc.GetData()));
	}
	::CloseHandle(Pipe);

	if (Response.IsEmpty() || !Response.Contains(TEXT("\"ok\":true")))
	{
		return false;
	}
	return ExtractJsonString(Response, TEXT("playerToken"), OutToken);
#else
	(void)OutToken;
	return false;
#endif
}
