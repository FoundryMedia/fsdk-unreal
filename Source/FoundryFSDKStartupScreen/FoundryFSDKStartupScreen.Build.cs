// Copyright Foundry Media. FoundryFSDKStartupScreen module build rules.
//
// The startup screen: a picture shown from the window's first frame until the first map is ready (the
// engine's movie player, set up at the PreLoadingScreen phase). Plain Slate - no UObjects exist yet when it
// is built, so the picture is a loose file (UFoundryUISettings::StartupScreenImage), not a texture asset.

using UnrealBuildTool;

public class FoundryFSDKStartupScreen : ModuleRules
{
	public FoundryFSDKStartupScreen(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine", // UGameEngine::SwitchGameWindowToUseGameViewport at the hand-over
			"MoviePlayer",
			"Slate",
			"SlateCore",
			"ApplicationCore",
			"ImageWrapper", // the PNG decode runs on the movie player's Slate thread - the module must be loaded first
		});
	}
}
