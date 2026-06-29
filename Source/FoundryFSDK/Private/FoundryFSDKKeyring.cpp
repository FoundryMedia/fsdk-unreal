// Copyright Foundry Media. fsdk-core <-> OS keyring bridge (refresh-token persistence).
//
// Backs fsdk-core's secret-store seam with the OS keyring so the rotated FID refresh
// token survives between runs. Only the refresh token is persisted (the core never
// hands the access token or password to this seam). On Windows the store is the
// Credential Manager; on other platforms it is a session-only no-op (login works,
// no resume). See ../../fsdk-core/SECURITY.md and .claude/rules/desktop-launcher-auth.md.

#include "FoundryFSDKKeyring.h"

#include "CoreMinimal.h"
#include "foundry/fsdk.h"

#include <cstdlib>
#include <cstring>

#if PLATFORM_WINDOWS && FOUNDRY_FSDK_FID_AUTH
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include <wincred.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// The core's seam typedefs have C language linkage; define the callbacks with
// matching linkage so the function-pointer types agree across compilers.
extern "C"
{
#if PLATFORM_WINDOWS && FOUNDRY_FSDK_FID_AUTH
	/** fsdk_secret_save_fn: persist value (UTF-8) under key in the Credential Manager. */
	static int FoundryFSDKSecretSave(const char* Key, const char* Value, void* /*UserData*/)
	{
		if (Key == nullptr || Value == nullptr)
		{
			return 1;
		}
		const FString TargetName = UTF8_TO_TCHAR(Key);
		const TCHAR* UserName = TEXT("Foundry");

		CREDENTIALW Cred;
		FMemory::Memzero(&Cred, sizeof(Cred));
		Cred.Type = CRED_TYPE_GENERIC;
		Cred.TargetName = const_cast<LPWSTR>(*TargetName);
		Cred.CredentialBlobSize = static_cast<DWORD>(strlen(Value)); // UTF-8 bytes
		Cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(Value));
		Cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
		Cred.UserName = const_cast<LPWSTR>(UserName);

		return CredWriteW(&Cred, 0) ? 0 : 1;
	}

	/** fsdk_secret_load_fn: read the value back as a malloc()'d UTF-8 NUL-terminated string. */
	static int FoundryFSDKSecretLoad(const char* Key, char** OutValue, void* /*UserData*/)
	{
		if (Key == nullptr || OutValue == nullptr)
		{
			return 1;
		}
		*OutValue = nullptr;
		const FString TargetName = UTF8_TO_TCHAR(Key);

		PCREDENTIALW Cred = nullptr;
		if (!CredReadW(*TargetName, CRED_TYPE_GENERIC, 0, &Cred) || Cred == nullptr)
		{
			return 1; // absent / unreadable
		}
		const DWORD Size = Cred->CredentialBlobSize;
		// MUST pair with the core's free() (fsdk_secret_load contract) - CRT malloc.
		char* Buffer = static_cast<char*>(::malloc(static_cast<size_t>(Size) + 1));
		if (Buffer != nullptr)
		{
			if (Size > 0 && Cred->CredentialBlob != nullptr)
			{
				FMemory::Memcpy(Buffer, Cred->CredentialBlob, Size);
			}
			Buffer[Size] = '\0';
			*OutValue = Buffer;
		}
		CredFree(Cred);
		return (*OutValue != nullptr) ? 0 : 1;
	}

	/** fsdk_secret_delete_fn: delete the entry (absent == success). */
	static int FoundryFSDKSecretDelete(const char* Key, void* /*UserData*/)
	{
		if (Key == nullptr)
		{
			return 1;
		}
		const FString TargetName = UTF8_TO_TCHAR(Key);
		if (CredDeleteW(*TargetName, CRED_TYPE_GENERIC, 0))
		{
			return 0;
		}
		return (GetLastError() == ERROR_NOT_FOUND) ? 0 : 1;
	}
#endif // PLATFORM_WINDOWS
} // extern "C"

void FoundryFSDKInstallSecretStore()
{
#if PLATFORM_WINDOWS && FOUNDRY_FSDK_FID_AUTH
	fsdk_set_secret_store(&FoundryFSDKSecretSave, &FoundryFSDKSecretLoad, &FoundryFSDKSecretDelete, nullptr);
#else
	// No keyring backend on this platform -> the core falls back to session-only
	// (login works; nothing persists, so no cross-launch resume).
#endif
}

void FoundryFSDKShutdownSecretStore()
{
	fsdk_set_secret_store(nullptr, nullptr, nullptr, nullptr);
}
