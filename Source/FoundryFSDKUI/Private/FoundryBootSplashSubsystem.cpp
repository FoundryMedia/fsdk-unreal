#include "FoundryBootSplashSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Engine/GameInstance.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "FoundryBootSplash.h"
#include "FoundryFSDKUILog.h"
#include "FoundryUISettings.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "MediaSource.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"

namespace
{
	/** Slack past the configured sequence before the failsafe forces the finish. */
	constexpr float FailsafeSlackSeconds = 2.0f;

	/** Movie mode's failsafe before the movie's length is known (the widget falls back to the logos after 3 s without an open). */
	constexpr float MovieFailsafeSeconds = 15.0f;

	/** One WARN naming the consequence, shared by every refusal that is not "played already" / "switched off". */
	void WarnSkipped(const TCHAR* Reason)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] %s - continuing to the menu without a boot splash"), Reason);
	}
}

bool UFoundryBootSplashSubsystem::HasPlayed() const
{
	return bPlayed;
}

bool UFoundryBootSplashSubsystem::IsActive() const
{
	return Splash != nullptr && !Splash->IsFinished();
}

void UFoundryBootSplashSubsystem::Deinitialize()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(FailsafeTimer);
	}
	if (Splash)
	{
		Splash->OnFinished.RemoveDynamic(this, &UFoundryBootSplashSubsystem::HandleSplashFinished);
		Splash->RemoveFromParent();
		Splash = nullptr;
	}
	PendingDone.Unbind();
	Super::Deinitialize();
}

bool UFoundryBootSplashSubsystem::TryPlay(APlayerController* PC, FSimpleDelegate OnDone)
{
	return TryPlay(PC, MoveTemp(OnDone), FSimpleDelegate());
}

void UFoundryBootSplashSubsystem::HandleSplashFadeOutStarted()
{
	FSimpleDelegate FadeOut = MoveTemp(PendingFadeOut);
	PendingFadeOut.Unbind();
	FadeOut.ExecuteIfBound();
}

bool UFoundryBootSplashSubsystem::TryPlay(APlayerController* PC, FSimpleDelegate OnDone, FSimpleDelegate OnFadeOut)
{
	const UFoundryUISettings* Settings = UFoundryUISettings::Get();
	if (Settings == nullptr || !Settings->bShowBootSplash)
	{
		return false; // switched off: not worth a warning
	}
	if (bPlayed)
	{
		return false; // already played on this GameInstance
	}
	if (IsRunningCommandlet())
	{
		WarnSkipped(TEXT("running as a commandlet"));
		return false;
	}
	if (PC == nullptr)
	{
		WarnSkipped(TEXT("no player controller"));
		return false;
	}
	UWorld* World = PC->GetWorld();
	if (IsRunningDedicatedServer() || (World != nullptr && World->GetNetMode() == NM_DedicatedServer))
	{
		WarnSkipped(TEXT("dedicated server"));
		return false;
	}
	if (!PC->IsLocalController())
	{
		WarnSkipped(TEXT("player controller is not local"));
		return false;
	}
#if !UE_BUILD_SHIPPING
	if (FParse::Param(FCommandLine::Get(), TEXT("NoBootSplash")))
	{
		WarnSkipped(TEXT("-NoBootSplash on the command line"));
		return false;
	}
#endif

	// Resolve the widget class: UFoundryUISettings::LoadWidgetClass logs the unset / failed-to-load reason.
	const TSubclassOf<UUserWidget> Loaded = UFoundryUISettings::LoadWidgetClass(Settings->DefaultBootSplash, TEXT("DefaultBootSplash"));
	if (!Loaded)
	{
		WarnSkipped(TEXT("DefaultBootSplash is unset or failed to load"));
		return false;
	}
	if (!Loaded->IsChildOf(UFoundryBootSplash::StaticClass()))
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] DefaultBootSplash '%s' is not a UFoundryBootSplash"), *Loaded->GetName());
		WarnSkipped(TEXT("DefaultBootSplash has the wrong parent class"));
		return false;
	}
	const TSubclassOf<UFoundryBootSplash> SplashClass = Loaded.Get();

	UFoundryBootSplash* NewSplash = CreateWidget<UFoundryBootSplash>(PC, SplashClass);
	if (NewSplash == nullptr)
	{
		WarnSkipped(TEXT("CreateWidget returned null"));
		return false;
	}

	bPlayed = true;

	// A failed load only warns and passes null: the splash then shows what it has (or collapses the logo).
	UTexture2D* StudioTexture = Settings->StudioLogo.LoadSynchronous();
	if (StudioTexture == nullptr && !Settings->StudioLogo.IsNull())
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] StudioLogo %s did not load - the studio logo is left out"), *Settings->StudioLogo.ToString());
	}
	UTexture2D* PlatformTexture = Settings->PlatformLogo.LoadSynchronous();
	if (PlatformTexture == nullptr && !Settings->PlatformLogo.IsNull())
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] PlatformLogo %s did not load - the platform logo is left out"), *Settings->PlatformLogo.ToString());
	}
	USoundBase* Stinger = Settings->StingerSound.LoadSynchronous();
	if (Stinger == nullptr && !Settings->StingerSound.IsNull())
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] StingerSound %s did not load - the splash plays silent"), *Settings->StingerSound.ToString());
	}

	const float FadeIn = Settings->BootSplashFadeInSeconds;
	const float FadeOut = Settings->BootSplashFadeOutSeconds;
	float Hold = Settings->BootSplashHoldSeconds;
	// The splash lasts as long as the stinger: hold = sound - fades (a looping sound reports "indefinite";
	// Duration is the clip length either way).
	if (Settings->bBootSplashMatchesStinger && Stinger && Stinger->Duration > 0.0f)
	{
		Hold = FMath::Max(0.0f, Stinger->Duration - FMath::Max(0.0f, FadeIn) - FMath::Max(0.0f, FadeOut));
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] matching stinger %s: %.2f s (hold %.2f s)"),
			*GetNameSafe(Stinger), Stinger->Duration, Hold);
	}
	if (HoldOverrideSeconds >= 0.0f)
	{
		Hold = HoldOverrideSeconds; // dev fixtures (photographing the splash) still win
	}
	NewSplash->Setup(StudioTexture, PlatformTexture, Stinger, FadeIn, Hold, FadeOut);

	// MOVIE MODE: the movie replaces the logos + stinger (which stay as the widget's fallback). A failed load only warns.
	bool bMovie = false;
	if (!Settings->BootSplashMovie.IsNull())
	{
		UMediaSource* Movie = Settings->BootSplashMovie.LoadSynchronous();
		if (Movie == nullptr)
		{
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] BootSplashMovie %s did not load - the logo sequence plays instead"), *Settings->BootSplashMovie.ToString());
		}
		else
		{
			UMaterialInterface* MovieMaterial = Settings->BootSplashMovieMaterial.LoadSynchronous();
			if (MovieMaterial == nullptr && !Settings->BootSplashMovieMaterial.IsNull())
			{
				UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] BootSplashMovieMaterial %s did not load - the movie shows as a plain image (no exit reveal)"), *Settings->BootSplashMovieMaterial.ToString());
			}
			USoundClass* MovieSoundClass = Settings->BootSplashMovieSoundClass.LoadSynchronous();
			if (MovieSoundClass == nullptr && !Settings->BootSplashMovieSoundClass.IsNull())
			{
				UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] BootSplashMovieSoundClass %s did not load - the movie audio uses the engine's media default class"), *Settings->BootSplashMovieSoundClass.ToString());
			}
			NewSplash->SetupMovie(Movie, MovieMaterial, MovieSoundClass, Settings->BootSplashMovieRevealSeconds);
			bMovie = NewSplash->IsMovieMode();
		}
	}

	Splash = NewSplash;
	PendingDone = MoveTemp(OnDone);
	PendingFadeOut = MoveTemp(OnFadeOut);
	Splash->OnFadeOutStarted.AddDynamic(this, &UFoundryBootSplashSubsystem::HandleSplashFadeOutStarted);
	Splash->OnFinished.AddDynamic(this, &UFoundryBootSplashSubsystem::HandleSplashFinished);
	Splash->AddToViewport(1000);

	// UI-only so nothing reaches the game behind it; the caller's own menu path re-applies its input mode.
	FInputModeUIOnly InputMode;
	InputMode.SetWidgetToFocus(Splash->TakeWidget());
	PC->SetInputMode(InputMode);
	PC->SetShowMouseCursor(false);

	// Failsafe: whatever goes wrong inside the sequence, the player is never stranded behind it.
	if (UGameInstance* GI = GetGameInstance())
	{
		// The widget's hold clock advances at most 0.1 s per frame (its first-frame hitch guard) while this timer
		// runs on the world's DeltaSeconds (up to ~0.4 s on a hitch frame): a slow first launch would trip a 1:1
		// failsafe mid-hold with no fade-out. Size it for the worst-case clock ratio (4x) plus the slack.
		const float Sequence = FMath::Max(0.0f, FadeIn) + FMath::Max(0.0f, Hold) + FMath::Max(0.0f, FadeOut);
		// Movie mode: the movie is real-time (media clock), so the failsafe is its (unknown-yet) length plus slack - but never less
		// than the logo fallback it can drop into (3 s open timeout + that sequence at the worst-case clock ratio).
		const float Total = bMovie
			? FMath::Max(MovieFailsafeSeconds + FailsafeSlackSeconds, 3.0f + Sequence * 4.0f + FailsafeSlackSeconds)
			: Sequence * 4.0f + FailsafeSlackSeconds;
		GI->GetTimerManager().SetTimer(FailsafeTimer, this, &UFoundryBootSplashSubsystem::HandleFailsafe, Total, false);
	}
	return true;
}

void UFoundryBootSplashSubsystem::HandleFailsafe()
{
	if (Splash && !Splash->IsFinished())
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] failsafe timer fired before the sequence ended - forcing the finish"));
		Splash->ForceFinish();
	}
}

void UFoundryBootSplashSubsystem::HandleSplashFinished()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(FailsafeTimer);
	}
	if (Splash)
	{
		Splash->OnFinished.RemoveDynamic(this, &UFoundryBootSplashSubsystem::HandleSplashFinished);
		Splash->RemoveFromParent();
		Splash = nullptr;
	}
	HandleSplashFadeOutStarted(); // a force-finish that never faded still hands over (no-op if it already ran)
	FSimpleDelegate Done = MoveTemp(PendingDone);
	PendingDone.Unbind();
	Done.ExecuteIfBound();
}

void UFoundryBootSplashSubsystem::SetHoldOverrideSeconds(float Seconds)
{
	HoldOverrideSeconds = Seconds;
}
