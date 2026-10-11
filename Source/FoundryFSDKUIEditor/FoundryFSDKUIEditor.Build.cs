// Copyright Foundry Media. FoundryFSDKUIEditor module build rules.
//
// Editor-only: home of the FoundryUIBuild commandlet, which generates the default UI kit's Widget
// Blueprints (WBP_FoundrySocialDrawer / WBP_FoundryFriendRow / WBP_FoundryInviteRow /
// WBP_FoundryFriendMenu) under /FoundryFSDK/UI/Social and imports its icon set
// (FoundryUIBuildCommandlet.cpp).

using UnrealBuildTool;

public class FoundryFSDKUIEditor : ModuleRules
{
	public FoundryFSDKUIEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"UMG",
			"UMGEditor",
			"UnrealEd",
			"Kismet",
			"KismetCompiler",
			"BlueprintGraph",
			"AssetTools",
			"AssetRegistry",
			"Projects",
			"EditorSubsystem",
			"FoundryFSDKUI",
			"Slate",
			"SlateCore",
			"Json", // -profile JSON parsing (FJsonObject / FJsonSerializer)
		});
	}
}
