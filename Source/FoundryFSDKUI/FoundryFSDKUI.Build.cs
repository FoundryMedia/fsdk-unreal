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
		//
		// EnhancedInput is private too: UFoundrySettingsMenu's Controls section is the only
		// consumer (UEnhancedInputLocalPlayerSubsystem / UEnhancedInputUserSettings), and no PUBLIC
		// kit header exposes a complete Enhanced Input type (UFoundryUISettings::RebindableContexts
		// only needs the UInputMappingContext forward declaration already in that header). GameplayTags
		// is EnhancedInput's own PRIVATE dependency (FMapPlayerKeyArgs/FGameplayTagContainer are only
		// reachable through its public headers) - listed explicitly here too, same InputCore lesson
		// above: a transitively-visible header is not the same as a linkable/always-resolvable one.
		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"InputCore",
			"Json", // UFoundryMatchCluster merges the party id into the mode's ticket-attributes JSON
			"EnhancedInput",
			"GameplayTags",
			"MediaAssets", // UFoundryBootSplash's movie mode: UMediaPlayer / UMediaTexture / UMediaSoundComponent
			"ApplicationCore", // FGenericWindow::GetOSWindowHandle (the monitor the game window is on)
			"AudioMixer", // USynthComponent::SetVolumeMultiplier (the movie sound component's base)
			"ImageWrapper", // the boot splash's key art is a PNG decoded by file name (a dynamic Slate brush)
		});
	}
}
