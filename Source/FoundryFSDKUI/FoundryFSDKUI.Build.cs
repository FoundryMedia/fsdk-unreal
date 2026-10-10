// Copyright Foundry Media. FoundryFSDKUI module build rules.
//
// The default UI kit for the Foundry SDK: UMG parent classes (the social drawer, friend/invite
// rows, the friend context menu) and UFoundryUISettings (the Deep Sapphire palette + default widget
// classes). Opt-in: the core FoundryFSDK module gains NO UMG/Slate dependency from this existing - a
// game that never uses the default UI never pulls UMG through FoundryFSDK itself.

using UnrealBuildTool;

public class FoundryFSDKUI : ModuleRules
{
	public FoundryFSDKUI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"Slate",
			"SlateCore",
			"FoundryFSDK",
			"DeveloperSettings",
		});

		// Slate/SlateCore only expose InputCore's PUBLIC headers transitively (it is their own
		// PRIVATE dependency) - that is enough to COMPILE against EKeys (UFoundryFriendRow's
		// right-click detection), but LINKING needs InputCore listed here too, or the build fails
		// LNK2019 on EKeys::RightMouseButton.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
		});
	}
}
