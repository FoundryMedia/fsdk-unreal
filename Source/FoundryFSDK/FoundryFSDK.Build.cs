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
		// files include "fsdk_internal.h" from Private/FsdkCore. Only the CLIENT
		// translation units are vendored (fsdk.c / transport.c / client.c) - the
		// server/token-verify code is deliberately absent from the player binary.
		string FsdkCore = Path.Combine(ModuleDirectory, "Private", "FsdkCore");
		PrivateIncludePaths.Add(Path.Combine(FsdkCore, "include")); // <foundry/fsdk.h>
		PrivateIncludePaths.Add(FsdkCore);                          // "fsdk_internal.h"
	}
}
