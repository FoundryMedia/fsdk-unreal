// Copyright Foundry Media. FoundryFSDK Unreal module build rules.
//
// The shared fsdk-core C ABI is VENDORED IN-MODULE (Private/FsdkCore) and compiled
// by UBT as C - no CMake, no prebuilt static lib. fsdk-core bakes in no network
// stack: the host (this module) provides HTTP via fsdk_set_http_transport, backed
// by the engine's HTTP module (which owns TLS). See FoundryFSDKTransport.cpp and
// ../../fsdk-core/SECURITY.md.

using System.IO;
using UnrealBuildTool;

public class FoundryFSDK : ModuleRules
{
	public FoundryFSDK(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Silence MSVC's CRT "secure" deprecation warnings for the portable C in
		// fsdk-core (snprintf/strtol/etc.) so a strict editor build doesn't fail.
		PrivateDefinitions.Add("_CRT_SECURE_NO_WARNINGS=1");

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"HTTP", // engine HTTP stack backs the fsdk-core transport seam (TLS)
		});

		// --- fsdk-core (vendored C ABI, compiled in-module) -----------------
		// Sources live under Private/FsdkCore and are compiled by UBT as C. The
		// public ABI header is Private/FsdkCore/include/foundry/fsdk.h; the .c
		// files include "fsdk_internal.h" from Private/FsdkCore. The CLIENT TUs
		// (fsdk.c / transport.c / client.c) compile for every target; the SERVER
		// TUs (server.c / token.c) are present in the tree but their bodies are
		// wrapped in #if FOUNDRY_FSDK_SERVER, so on any non-server target they
		// compile to EMPTY translation units - no server/token-verify code (and no
		// OpenSSL) ever enters the shipped player binary. See the gate below and
		// .claude/rules/fsdk-security.md.
		string FsdkCore = Path.Combine(ModuleDirectory, "Private", "FsdkCore");
		PrivateIncludePaths.Add(Path.Combine(FsdkCore, "include")); // <foundry/fsdk.h>
		PrivateIncludePaths.Add(FsdkCore);                          // "fsdk_internal.h"

		// --- SERVER-ONLY gate (the load-bearing security boundary) ----------
		// On a dedicated-server target only: activate the vendored server TUs
		// (server.c/token.c), the OpenSSL-backed JWT verifier, and the server
		// lifecycle wrapper, and link UE's bundled OpenSSL to fill the verifier
		// seam (RS256 / JWKS). FOUNDRY_FSDK_SERVER is a private define so the gate
		// is owned + greppable here, never leaking into the player binary.
		if (Target.Type == TargetType.Server)
		{
			PrivateDefinitions.Add("FOUNDRY_FSDK_SERVER=1");
			PrivateDependencyModuleNames.Add("OpenSSL"); // fills the RS256 verifier seam
			PrivateDependencyModuleNames.Add("Json");     // parse the auth-efga JWKS document
		}
	}
}
