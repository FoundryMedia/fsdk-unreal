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

		// FID-embedded in-game auth gate. ON for every NON-SHIPPING build (dev
		// workflow: PIE/standalone runs without the launcher use the dev console's
		// masked `foundry login`); OFF in Shipping, so the SHIPPED client binary
		// carries NO credential-login path - auth comes only from the launcher
		// session-daemon handoff (UFoundryFSDKSubsystem::AutoLoginFromLauncher), and a
		// missing launcher session fails fast. A game distributed OUTSIDE the launcher
		// opts its Shipping build in with the env var FOUNDRY_FSDK_FID_AUTH=1. PUBLIC
		// (not Private): it gates the PUBLIC subsystem header, so consumers (Conquest)
		// must see the same value or UHT/link would disagree - see
		// unreal-plugin-conventions.
		bool bFidAuth = System.Environment.GetEnvironmentVariable("FOUNDRY_FSDK_FID_AUTH") == "1"
			|| Target.Configuration != UnrealTargetConfiguration.Shipping;
		PublicDefinitions.Add("FOUNDRY_FSDK_FID_AUTH=" + (bFidAuth ? "1" : "0"));

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"HTTP", // engine HTTP stack backs the fsdk-core transport seam (TLS)
			"WebSockets", // engine WS stack backs the fsdk-core WS seam (FRC chat)
			"InputCore", // FKey/EKeys for the developer-console keybinds
			"Slate",     // the curated console/overlay/netgraph widgets
			"SlateCore",
		});

		// Windows Credential Manager (CredWrite/Read/Delete) backs the secret-store
		// seam for refresh-token persistence (FoundryFSDKKeyring.cpp).
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicSystemLibraries.Add("Advapi32.lib");
		}

		// --- fsdk-core (vendored C ABI, compiled in-module) -----------------
		// Sources live under Private/FsdkCore and are compiled by UBT as C. The
		// public ABI header is Private/FsdkCore/include/foundry/fsdk.h; the .c
		// files include "fsdk_internal.h" from Private/FsdkCore. The CLIENT TUs
		// (fsdk.c / transport.c / client.c / auth.c) compile for every target; the SERVER
		// TUs (server.c / token.c) are present in the tree but their bodies are
		// wrapped in #if FOUNDRY_FSDK_SERVER, so on any non-server target they
		// compile to EMPTY translation units - no server/token-verify code (and no
		// OpenSSL) ever enters the shipped player binary. See the gate below and
		// .claude/rules/fsdk-security.md.
		// NOTE (vendoring divergence): that #if in server.c/token.c is applied to
		// this VENDORED copy only - upstream fsdk-core/src/{server,token}.c do NOT
		// carry it (their standalone CMake lib + CTest suite need the code always
		// compiled). After any re-vendor from fsdk-core, RE-APPLY the gate to
		// server.c + token.c, or ship them ungated and the boundary silently regresses
		// to fail-closed-at-runtime (server/token code back in the player binary).
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
