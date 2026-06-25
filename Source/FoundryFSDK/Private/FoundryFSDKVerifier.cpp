// Copyright Foundry Media. Server-only OpenSSL-backed JWT verifier (seam fill).
//
// Fills fsdk-core's fsdk_set_jwt_verifier seam: given the JWT header `kid`, the
// exact "<header>.<payload>" signing input, and the decoded signature bytes,
// verify RS256 (RSASSA-PKCS1-v1_5 over SHA-256) against the matching auth-efga
// JWKS key. The signing key is resolved by `kid` from a cache populated from
// <AuthBaseUrl>/.well-known/jwks.json (fetched via the engine HTTP module). Fails
// CLOSED: unknown kid (after a refresh) or bad signature -> rejected token.
//
// SERVER-ONLY. UE 5.7 bundles OpenSSL 1.1.1, so the active key-construction path
// is RSA_set0_key (NOT deprecated on 1.1.x -> no warnings-as-errors issue); a 3.x
// branch is kept for future engine upgrades. See .claude/rules/fsdk-security.md.

#include "FoundryFSDKVerifier.h"

#if defined(FOUNDRY_FSDK_SERVER) && FOUNDRY_FSDK_SERVER

#include "foundry/fsdk.h"

#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/ThreadSafeBool.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Logging/LogMacros.h"

#include <cstring>

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif
// OpenSSL's <openssl/ossl_typ.h> declares `typedef struct ui_st UI;`, which collides
// with UE's `namespace UI` (a UInterface-specifier namespace from ObjectMacros.h,
// pulled in through this module's shared PCH because the module depends on Engine).
// We never use OpenSSL's UI type, so rename its token across the OpenSSL include only,
// then restore - this avoids the C2365 'UI' redefinition without touching UE's UI.
#pragma push_macro("UI")
#define UI FoundryOpenSSL_UI
THIRD_PARTY_INCLUDES_START
#include <openssl/opensslv.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
#include <openssl/param_build.h>
#include <openssl/core_names.h>
#endif
THIRD_PARTY_INCLUDES_END
#undef UI
#pragma pop_macro("UI")
#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogFoundryFSDKVerifier, Log, All);

namespace
{
	FCriticalSection            GVerifierCS;
	TMap<FString, EVP_PKEY*>    GKeyCache;          // kid -> public key (owned)
	FString                     GJwksUrl;           // full .well-known/jwks.json URL
	double                      GLastRefreshSeconds = 0.0;
	bool                        GInstalled = false;

	/** base64url (no padding) -> bytes, via UE's standard base64 after normalizing. */
	bool Base64UrlDecode(const FString& In, TArray<uint8>& Out)
	{
		FString S = In;
		S.ReplaceInline(TEXT("-"), TEXT("+"));
		S.ReplaceInline(TEXT("_"), TEXT("/"));
		while (S.Len() % 4 != 0)
		{
			S.AppendChar(TEXT('='));
		}
		return FBase64::Decode(S, Out);
	}

	/** Build an RSA public key from JWKS modulus/exponent byte strings. Caller owns
	 *  the returned EVP_PKEY (EVP_PKEY_free). Returns nullptr on any failure. */
	EVP_PKEY* MakeRsaPubKey(const TArray<uint8>& N, const TArray<uint8>& E)
	{
		if (N.Num() == 0 || E.Num() == 0)
		{
			return nullptr;
		}
		BIGNUM* n = BN_bin2bn(N.GetData(), N.Num(), nullptr);
		BIGNUM* e = BN_bin2bn(E.GetData(), E.Num(), nullptr);
		if (n == nullptr || e == nullptr)
		{
			BN_free(n);
			BN_free(e);
			return nullptr;
		}

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
		EVP_PKEY* pkey = nullptr;
		OSSL_PARAM_BLD* bld = OSSL_PARAM_BLD_new();
		if (bld != nullptr
			&& OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_N, n)
			&& OSSL_PARAM_BLD_push_BN(bld, OSSL_PKEY_PARAM_RSA_E, e))
		{
			OSSL_PARAM* params = OSSL_PARAM_BLD_to_param(bld);
			EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
			if (ctx != nullptr && params != nullptr)
			{
				if (EVP_PKEY_fromdata_init(ctx) <= 0
					|| EVP_PKEY_fromdata(ctx, &pkey, EVP_PKEY_PUBLIC_KEY, params) <= 0)
				{
					pkey = nullptr;
				}
			}
			EVP_PKEY_CTX_free(ctx);
			OSSL_PARAM_free(params);
		}
		OSSL_PARAM_BLD_free(bld);
		BN_free(n); // fromdata copies; we still own n/e here
		BN_free(e);
		return pkey;
#else
		RSA* rsa = RSA_new();
		if (rsa == nullptr)
		{
			BN_free(n);
			BN_free(e);
			return nullptr;
		}
		// On success RSA takes ownership of n and e.
		if (RSA_set0_key(rsa, n, e, nullptr) != 1)
		{
			RSA_free(rsa);
			BN_free(n);
			BN_free(e);
			return nullptr;
		}
		EVP_PKEY* pkey = EVP_PKEY_new();
		if (pkey == nullptr)
		{
			RSA_free(rsa); // frees n/e too
			return nullptr;
		}
		if (EVP_PKEY_assign_RSA(pkey, rsa) != 1)
		{
			EVP_PKEY_free(pkey);
			RSA_free(rsa);
			return nullptr;
		}
		return pkey; // owns rsa -> owns n/e
#endif
	}

	bool VerifyRs256(EVP_PKEY* pkey, const char* signing_input,
	                 const unsigned char* sig, size_t siglen)
	{
		if (pkey == nullptr || signing_input == nullptr || sig == nullptr)
		{
			return false;
		}
		EVP_MD_CTX* ctx = EVP_MD_CTX_new();
		if (ctx == nullptr)
		{
			return false;
		}
		bool bOk = false;
		if (EVP_DigestVerifyInit(ctx, nullptr, EVP_sha256(), nullptr, pkey) == 1
			&& EVP_DigestVerifyUpdate(ctx, signing_input, strlen(signing_input)) == 1)
		{
			bOk = (EVP_DigestVerifyFinal(ctx, sig, siglen) == 1);
		}
		EVP_MD_CTX_free(ctx);
		return bOk;
	}

	void FreeAllKeys(TMap<FString, EVP_PKEY*>& Keys)
	{
		for (TPair<FString, EVP_PKEY*>& Pair : Keys)
		{
			if (Pair.Value != nullptr)
			{
				EVP_PKEY_free(Pair.Value);
			}
		}
		Keys.Empty();
	}

	/** Synchronous HTTPS GET via the engine HTTP module. Blocks until complete,
	 *  pumping the manager only on the game thread (PreLogin / startup run there). */
	bool SynchronousHttpGet(const FString& Url, FString& OutBody)
	{
		if (!FHttpModule::Get().IsHttpEnabled())
		{
			return false;
		}
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		Request->SetTimeout(20.0f);

		FThreadSafeBool bDone(false);
		bool bOk = false;
		int32 Code = 0;
		FString Body;
		Request->OnProcessRequestComplete().BindLambda(
			[&bDone, &bOk, &Code, &Body](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
			{
				bOk = bSucceeded && Response.IsValid();
				if (bOk)
				{
					Code = Response->GetResponseCode();
					Body = Response->GetContentAsString();
				}
				bDone = true;
			});

		if (!Request->ProcessRequest())
		{
			return false;
		}

		const bool bGameThread = IsInGameThread();
		FHttpManager& Manager = FHttpModule::Get().GetHttpManager();
		const double Deadline = FPlatformTime::Seconds() + 25.0;
		while (!bDone && FPlatformTime::Seconds() < Deadline)
		{
			if (bGameThread)
			{
				Manager.Tick(0.0f);
			}
			FPlatformProcess::Sleep(0.005f);
		}
		if (!bDone)
		{
			Request->CancelRequest();
			return false;
		}
		if (!bOk || Code < 200 || Code >= 300)
		{
			return false;
		}
		OutBody = Body;
		return true;
	}

	/** Parse a JWKS document into a fresh kid -> EVP_PKEY map (caller owns keys). */
	void ParseJwks(const FString& Body, TMap<FString, EVP_PKEY*>& OutKeys)
	{
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Keys = nullptr;
		if (!Root->TryGetArrayField(TEXT("keys"), Keys) || Keys == nullptr)
		{
			return;
		}
		for (const TSharedPtr<FJsonValue>& Val : *Keys)
		{
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			if (!Val.IsValid() || !Val->TryGetObject(Obj) || Obj == nullptr)
			{
				continue;
			}
			FString Kty, Kid, NB64, EB64;
			(*Obj)->TryGetStringField(TEXT("kty"), Kty);
			if (!Kty.IsEmpty() && Kty != TEXT("RSA"))
			{
				continue;
			}
			(*Obj)->TryGetStringField(TEXT("kid"), Kid);
			if (!(*Obj)->TryGetStringField(TEXT("n"), NB64)
				|| !(*Obj)->TryGetStringField(TEXT("e"), EB64))
			{
				continue;
			}
			TArray<uint8> N, E;
			if (!Base64UrlDecode(NB64, N) || !Base64UrlDecode(EB64, E))
			{
				continue;
			}
			EVP_PKEY* Key = MakeRsaPubKey(N, E);
			if (Key != nullptr)
			{
				if (EVP_PKEY** Prior = OutKeys.Find(Kid))
				{
					EVP_PKEY_free(*Prior);
				}
				OutKeys.Add(Kid, Key);
			}
		}
	}

	/** Refresh the key cache from the JWKS URL. Throttled to once / 60s unless
	 *  bForce. Does the HTTP fetch OUTSIDE the cache lock, then swaps under it. */
	void RefreshJwks(bool bForce)
	{
		FString Url;
		{
			FScopeLock Lock(&GVerifierCS);
			const double Now = FPlatformTime::Seconds();
			if (!bForce && GKeyCache.Num() > 0 && (Now - GLastRefreshSeconds) < 60.0)
			{
				return;
			}
			GLastRefreshSeconds = Now;
			Url = GJwksUrl;
		}
		if (Url.IsEmpty())
		{
			return;
		}
		FString Body;
		if (!SynchronousHttpGet(Url, Body))
		{
			UE_LOG(LogFoundryFSDKVerifier, Warning, TEXT("JWKS fetch failed: %s"), *Url);
			return;
		}
		TMap<FString, EVP_PKEY*> NewKeys;
		ParseJwks(Body, NewKeys);
		if (NewKeys.Num() == 0)
		{
			UE_LOG(LogFoundryFSDKVerifier, Warning, TEXT("JWKS parsed 0 usable RSA keys"));
			return; // keep whatever we had
		}
		{
			FScopeLock Lock(&GVerifierCS);
			FreeAllKeys(GKeyCache);
			GKeyCache = MoveTemp(NewKeys);
		}
		UE_LOG(LogFoundryFSDKVerifier, Log, TEXT("JWKS refreshed (%d key(s))"), GKeyCache.Num());
	}
}

// The verifier callback has C language linkage to match fsdk_jwt_verify_fn.
extern "C"
{
	static fsdk_result FoundryFSDKJwtVerify(const char* kid,
	                                        const char* signing_input,
	                                        const unsigned char* signature,
	                                        size_t signature_len,
	                                        void* /*user_data*/)
	{
		if (signing_input == nullptr || signature == nullptr)
		{
			return FSDK_ERR_TOKEN_INVALID;
		}
		const FString Kid = (kid != nullptr) ? FString(UTF8_TO_TCHAR(kid)) : FString();

		// 1) Fast path: key already cached. Verify under the lock (no nested locking).
		{
			FScopeLock Lock(&GVerifierCS);
			if (EVP_PKEY** Found = GKeyCache.Find(Kid))
			{
				return VerifyRs256(*Found, signing_input, signature, signature_len)
					? FSDK_OK : FSDK_ERR_TOKEN_INVALID;
			}
		}
		// 2) Unknown kid: refresh (throttled, fetches outside the lock) and retry.
		RefreshJwks(false);
		{
			FScopeLock Lock(&GVerifierCS);
			if (EVP_PKEY** Found = GKeyCache.Find(Kid))
			{
				return VerifyRs256(*Found, signing_input, signature, signature_len)
					? FSDK_OK : FSDK_ERR_TOKEN_INVALID;
			}
		}
		// Fail closed: no key for this kid.
		UE_LOG(LogFoundryFSDKVerifier, Warning, TEXT("no JWKS key for kid '%s' - rejecting token"), *Kid);
		return FSDK_ERR_TOKEN_INVALID;
	}
} // extern "C"

void FoundryFSDKInstallJwtVerifier(const FString& AuthBaseUrl)
{
	FString Base = AuthBaseUrl;
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	{
		FScopeLock Lock(&GVerifierCS);
		GJwksUrl = Base + TEXT("/.well-known/jwks.json");
	}
	fsdk_set_jwt_verifier(&FoundryFSDKJwtVerify, nullptr);
	GInstalled = true;
	UE_LOG(LogFoundryFSDKVerifier, Log, TEXT("RS256 verifier installed (JWKS: %s)"), *GJwksUrl);
}

void FoundryFSDKPrewarmJwks()
{
	RefreshJwks(true);
}

void FoundryFSDKShutdownJwtVerifier()
{
	if (GInstalled)
	{
		fsdk_set_jwt_verifier(nullptr, nullptr);
		GInstalled = false;
	}
	FScopeLock Lock(&GVerifierCS);
	FreeAllKeys(GKeyCache);
	GJwksUrl.Reset();
}

#endif // FOUNDRY_FSDK_SERVER
