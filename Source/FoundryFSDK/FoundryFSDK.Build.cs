// Copyright Foundry Media. FoundryFSDK Unreal module build rules.
//
// Links the shared fsdk-core C ABI from ThirdParty/fsdk-core and exposes its
// public include directory. SCAFFOLD: the ThirdParty lib is a placeholder until
// fsdk-core is vendored/submoduled and built per platform (see
// ThirdParty/fsdk-core/README.md). Build.cs is written to be correct so that
// once the static lib is dropped in, the module compiles unchanged.

using System.IO;
using UnrealBuildTool;

public class FoundryFSDK : ModuleRules
{
	public FoundryFSDK(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsage.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
		});

		// --- fsdk-core (ThirdParty C ABI) ----------------------------------
		// ThirdParty/fsdk-core mirrors the fsdk-core repo: its public header
		// lives under include/foundry/fsdk.h and the built static lib under
		// lib/<platform>/. Vendor it as a git submodule pointing at ../fsdk-core
		// (see ThirdParty/fsdk-core/README.md).
		string FsdkRoot = Path.Combine(ModuleDirectory, "..", "..", "ThirdParty", "fsdk-core");
		string FsdkInclude = Path.Combine(FsdkRoot, "include");

		// Public so the include path is available to consumers of this module.
		PublicIncludePaths.Add(FsdkInclude);

		// Link the platform-appropriate static library. These paths are the
		// expected drop locations once fsdk-core is built; the files are absent
		// in the scaffold (placeholders only), so guard with File.Exists to keep
		// the scaffold parseable by UBT without the binaries present.
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			string Lib = Path.Combine(FsdkRoot, "lib", "Win64", "fsdk_core.lib");
			if (File.Exists(Lib))
			{
				PublicAdditionalLibraries.Add(Lib);
			}
		}
		else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
			string Lib = Path.Combine(FsdkRoot, "lib", "Linux", "libfsdk_core.a");
			if (File.Exists(Lib))
			{
				PublicAdditionalLibraries.Add(Lib);
			}
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			string Lib = Path.Combine(FsdkRoot, "lib", "Mac", "libfsdk_core.a");
			if (File.Exists(Lib))
			{
				PublicAdditionalLibraries.Add(Lib);
			}
		}
	}
}
