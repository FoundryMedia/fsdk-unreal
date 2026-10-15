// Copyright Foundry Media. FoundryFSDK default UI kit project settings.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "FoundryFSDKSubsystem.h" // EFoundryFsdkResult / EFoundryWaitReason (Matchmaking copy overrides)
#include "Templates/SubclassOf.h"
#include "UObject/SoftObjectPtr.h"
#include "FoundryUISettings.generated.h"

class UInputMappingContext;
class USoundClass;
class USoundBase;
class USoundMix;
class UTexture2D;
class UUserWidget;

/**
 * One colour token per DESIGN.md's Deep Sapphire palette (sRGB hex, converted to linear once by
 * FFoundryUIPalette's constructor via FLinearColor::FromSRGBColor(FColor::FromHex(...))). Both the
 * FoundryUIBuild generator commandlet and the parent widgets (FoundryUITheme.h) read the SAME
 * struct - change a value here to restyle every default WBP and the parents' runtime fallbacks
 * (presence dots, member tints) at once.
 */
USTRUCT(BlueprintType)
struct FOUNDRYFSDKUI_API FFoundryUIPalette
{
	GENERATED_BODY()

	FFoundryUIPalette();

	/** #0c1324 - drawer / panel fill. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Surface;

	/** #151b2d - section rows, input frame. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor SurfaceLow;

	/** #23293c - hovered row, pills. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor SurfaceHigh;

	/** #2e3447 - menu box. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor SurfaceHighest;

	/** #00daf3 - primary action, your own avatar. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Primary;

	/** #008e9f - pressed primary. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor PrimaryContainer;

	/** #94ccff - compact actions (Send, Accept). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Secondary;

	/** #b6c4ff - host pill fill. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Tertiary;

	/** #dce1fb - primary text. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Text;

	/** #9aa3b8 - helper, offline names. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor TextSecondary;

	/** #45464d - ghost border (15% alpha where used). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Outline;

	/** #4edea3 - presence online. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Online;

	/** #f0c674 - presence idle / away. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Idle;

	/** #ff6b7f - errors, Block / Remove confirm button, dnd. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Error;

	/** #0c1324 (same value as Surface by default) - the HostBadge label text, read against the
	 *  Tertiary fill. A skin whose Tertiary isn't a light pill (e.g. Goo Crew's #F7DAD2 against a
	 *  #A94D40 label) overrides this independently of Text/TextSecondary. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor HostText;

	// Party member tints by roster index: #94ccff #b6c4ff #4edea3 #f0c674 #ff9ecb #ffb27a.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Member0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Member1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Member2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Member3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Member4;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FLinearColor Member5;
};

/** One selectable matchmaking mode the kit's match cluster offers (Project Settings -> Game -> Foundry UI). */
USTRUCT(BlueprintType)
struct FOUNDRYFSDKUI_API FFoundryMatchmakingMode
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FText Detail;

	/** FMMS queue NAME ("<game-slug>/<mode>"). Empty = shown disabled, "(not queueable yet)". */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FString QueueName;

	/** Extra ticket attributes as a JSON object; the cluster merges "partyId" into it when the player is in a party. Empty = {}. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foundry UI")
	FString AttributesJson;
};

/**
 * Project Settings -> Game -> Foundry UI: the account-page URL ManageAccountButton opens, the
 * default widget classes the social drawer resolves its row/menu classes from when a host leaves
 * them null, and the Deep Sapphire colour palette every default WBP (and the parents' code
 * fallbacks - presence dots, member tints) is built from.
 */
UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "Foundry UI"))
class FOUNDRYFSDKUI_API UFoundryUISettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UFoundryUISettings();

	/** Opened by the social drawer's Manage Account button (FPlatformProcess::LaunchURL). */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI")
	FString AccountUrl;

	/** WBP_FoundrySocialDrawer (generated by the FoundryUIBuild commandlet). */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultSocialDrawer;

	/** WBP_FoundryFriendRow. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultFriendRow;

	/** WBP_FoundryInviteRow. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultInviteRow;

	/** WBP_FoundryFriendMenu. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultFriendMenu;

	/** WBP_FoundryPartyNameplate. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultPartyNameplate;

	/** WBP_FoundryPauseMenu. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultPauseMenu;

	/** WBP_FoundryConfirmDialog. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultConfirmDialog;

	/** WBP_FoundrySettingsMenu. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultSettingsMenu;

	/** WBP_FoundryKeybindRow. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultKeybindRow;

	/** WBP_FoundryTooltip - the kit-styled pill UFoundryUIWidgetBase::ApplyKitTooltips / SetKitToolTip
	 *  wrap plain tooltip text in, in place of the engine's own default tooltip style. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultTooltip;

	/** WBP_FoundryAppBar - the lobby's top bar (title, player chip, social + settings buttons). */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultAppBar;

	/** WBP_FoundryMatchCluster - the mode pill, Find Match / Cancel Search, status line and party count. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultMatchCluster;

	/** WBP_FoundryModeRow - one row of the match cluster's mode menu. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Default Widgets")
	TSoftClassPtr<UUserWidget> DefaultModeRow;

	/** The app bar's title text. Empty -> the project name (FApp::GetProjectName). */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Matchmaking")
	FText AppBarTitle;

	/** The modes the match cluster's mode menu offers, in order. A mode with an empty QueueName is
	 *  listed disabled ("not queueable yet"). A game overrides UFoundryMatchCluster::GetModes to build
	 *  the list in code instead. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Matchmaking")
	TArray<FFoundryMatchmakingMode> MatchmakingModes;

	/** The sound mix UFoundryGameUserSettings::ApplyAudio pushes. Unset = the volumes persist but
	 *  are never applied - the same silent no-op posture as an unset default widget class. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Audio")
	TSoftObjectPtr<USoundMix> SoundMix;

	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Audio")
	TSoftObjectPtr<USoundClass> MasterSoundClass;

	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Audio")
	TSoftObjectPtr<USoundClass> MusicSoundClass;

	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Audio")
	TSoftObjectPtr<USoundClass> SFXSoundClass;

	/** The mapping contexts whose player-mappable rows UFoundrySettingsMenu's Controls section
	 *  lists. Empty -> the section shows "This game has no rebindable controls." */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Input")
	TArray<TSoftObjectPtr<UInputMappingContext>> RebindableContexts;

	/** Master switch for the boot splash (UFoundryBootSplashSubsystem::TryPlay). False = the game goes straight to its menu. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	bool bShowBootSplash = true;

	/** WBP_FoundryBootSplash - a UFoundryBootSplash widget blueprint (generated by the FoundryUIBuild commandlet). */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	TSoftClassPtr<UUserWidget> DefaultBootSplash;

	/** The studio logo shown on the splash (white on transparent works best). Unset = no studio logo. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	TSoftObjectPtr<UTexture2D> StudioLogo;

	/** The platform / engine logo shown beside the studio logo. Defaults to the Foundry logo ("made with Foundry"). */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	TSoftObjectPtr<UTexture2D> PlatformLogo;

	/** Sound played as the splash starts. Unset = silent. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	TSoftObjectPtr<USoundBase> StingerSound;

	/** Seconds the logos take to fade in. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	float BootSplashFadeInSeconds = 0.4f;

	/** Seconds the logos stay fully visible. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	float BootSplashHoldSeconds = 2.6f;

	/** Seconds the logos take to fade out. */
	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI|Boot Splash")
	float BootSplashFadeOutSeconds = 0.6f;

	UPROPERTY(Config, EditAnywhere, Category = "Foundry UI")
	FFoundryUIPalette Palette;

	/**
	 * S5 "matchmaking wait": player-facing copy overrides, keyed by the server-told wait reason
	 * (while a ticket is QUEUED) - replaces FoundryMatchmakingCopy::ForWait's base sentence for that
	 * reason (the "~N min left" suffix is still appended by code when EstimateSeconds is known). An
	 * empty entry (or no entry) falls back to the kit's default English copy. Pushed into
	 * FoundryMatchmakingCopy once at FoundryFSDKUI module startup
	 * (ApplyMatchmakingCopyOverrides, called from FFoundryFSDKUIModule::StartupModule) - the core
	 * FoundryFSDK module never references UFoundryUISettings directly (see FoundryFSDKUI.Build.cs:
	 * UI modules depend on the core, never the reverse).
	 */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI|Matchmaking")
	TMap<EFoundryWaitReason, FText> MatchmakingWaitCopyOverrides;

	/** Same, keyed by the terminal fsdk result a matchmaking Fail() reports. */
	UPROPERTY(EditDefaultsOnly, Category = "Foundry UI|Matchmaking")
	TMap<EFoundryFsdkResult, FText> MatchmakingResultCopyOverrides;

	static const UFoundryUISettings* Get() { return GetDefault<UFoundryUISettings>(); }

	/** Push MatchmakingWaitCopyOverrides / MatchmakingResultCopyOverrides into
	 *  FoundryMatchmakingCopy. Call once, at startup (see FFoundryFSDKUIModule::StartupModule) -
	 *  before any matchmaking flow runs (FoundryMatchmakingCopy's override hooks are "set once",
	 *  not thread-safe for repeated calls). */
	static void ApplyMatchmakingCopyOverrides();

	/** Synchronous load of a soft widget class. Null (and a log line naming the setting) when unset
	 *  or missing. */
	static TSubclassOf<UUserWidget> LoadWidgetClass(const TSoftClassPtr<UUserWidget>& Soft, const TCHAR* SettingName);

	virtual FName GetCategoryName() const override { return TEXT("Game"); }
};
