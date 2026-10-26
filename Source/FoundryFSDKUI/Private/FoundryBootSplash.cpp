#include "FoundryBootSplash.h"

#include "Animation/WidgetAnimation.h"
#include "Blueprint/WidgetTree.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/OverlaySlot.h"
#include "Components/PanelWidget.h"
#include "Components/ScaleBox.h"
#include "ContentStreaming.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "FoundryFSDKUILog.h"
#include "GameFramework/Actor.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MediaPlayer.h"
#include "MediaSoundComponent.h"
#include "MediaSource.h"
#include "MediaTexture.h"
#include "Engine/GameViewportClient.h"
#include "FoundryUISettings.h"
#include "Sound/SoundBase.h"
#include "Sound/SoundClass.h"
#include "Styling/SlateBrush.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "IImageWrapperModule.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

namespace
{
	/** A hitch on the first frame(s) must not eat the hold. */
	constexpr float MaxHoldTickSeconds = 0.1f;

	/** Movie mode: no open (or no advancing clock) within this long falls back to the logos + stinger. */
	constexpr float MovieOpenTimeoutSeconds = 3.0f;

	/** The cover image's native size: the 16:9 frame the movie is authored at (the ScaleBox only needs the aspect). */
	const FVector2D MovieNativeSize(1920.0, 1080.0);
}

void UFoundryBootSplash::Setup(UTexture2D* StudioTexture, UTexture2D* PlatformTexture, USoundBase* Stinger, float FadeInSeconds, float HoldSeconds, float FadeOutSeconds)
{
	FadeInDuration = FMath::Max(0.0f, FadeInSeconds);
	HoldDuration = FMath::Max(0.0f, HoldSeconds);
	FadeOutDuration = FMath::Max(0.0f, FadeOutSeconds);
	StingerSound = Stinger;
	bRealTimeHold = Stinger != nullptr;
	ApplyLogo(StudioLogo, StudioTexture);
	ApplyLogo(PlatformLogo, PlatformTexture);

	// The generated default carries the logo gap as PlatformLogo's own slot padding; with no studio
	// logo that gap would park the lone logo half a gap right of centre. Drop it when the studio side collapsed.
	if (StudioTexture == nullptr && PlatformLogo != nullptr)
	{
		if (UHorizontalBoxSlot* GapSlot = Cast<UHorizontalBoxSlot>(PlatformLogo->Slot))
		{
			GapSlot->SetPadding(FMargin(0.0f));
		}
	}
}

void UFoundryBootSplash::ApplyLogo(UImage* Image, UTexture2D* Texture) const
{
	if (Image == nullptr)
	{
		return;
	}
	if (Texture == nullptr)
	{
		Image->SetVisibility(ESlateVisibility::Collapsed);
		return;
	}

	// Aspect-fit inside LogoMaxSize. A degenerate texture or box falls back to the box itself.
	FVector2D Size = LogoMaxSize;
	const float TexW = static_cast<float>(Texture->GetSizeX());
	const float TexH = static_cast<float>(Texture->GetSizeY());
	if (TexW > 0.0f && TexH > 0.0f && LogoMaxSize.X > 0.0 && LogoMaxSize.Y > 0.0)
	{
		const float Scale = FMath::Min(static_cast<float>(LogoMaxSize.X) / TexW, static_cast<float>(LogoMaxSize.Y) / TexH);
		Size = FVector2D(TexW * Scale, TexH * Scale);
	}

	FSlateBrush Brush; // fresh: no engine texture name inherited from a stock style
	Brush.DrawAs = ESlateBrushDrawType::Image;
	Brush.SetResourceObject(Texture);
	Brush.SetImageSize(Size);
	Brush.TintColor = FSlateColor(FLinearColor::White);
	Image->SetBrush(Brush);
	Image->SetVisibility(ESlateVisibility::HitTestInvisible);
}

void UFoundryBootSplash::ForceFinish()
{
	Finish();
}

bool UFoundryBootSplash::IsFinished() const
{
	return Phase == EPhase::Finished;
}

void UFoundryBootSplash::Finish()
{
	if (Phase == EPhase::Finished)
	{
		return;
	}
	Phase = EPhase::Finished;
	CleanupMovie();
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] finished"));
	OnFinished.Broadcast();
}

void UFoundryBootSplash::SetupMovie(UMediaSource* Movie, UMaterialInterface* Material, USoundClass* SoundClass, float RevealSeconds)
{
	if (Movie == nullptr || bMovieMode)
	{
		return;
	}
	MovieSource = Movie;
	MovieMaterial = Material;
	MovieSoundClass = SoundClass;
	MovieRevealSeconds = FMath::Max(0.0f, RevealSeconds);
	if (!BuildMovie())
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] movie mode could not be built for %s - the logo sequence plays instead"), *GetNameSafe(Movie));
		CleanupMovie();
		MovieSource = nullptr;
		return;
	}
	bMovieMode = true;
}

bool UFoundryBootSplash::IsMovieMode() const
{
	return bMovieMode;
}

bool UFoundryBootSplash::BuildMovie()
{
	UPanelWidget* Root = WidgetTree ? Cast<UPanelWidget>(WidgetTree->RootWidget) : nullptr;
	UWorld* World = GetWorld();
	if (Root == nullptr || World == nullptr || MovieSource == nullptr)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] movie mode needs a panel root and a world (root %s, world %s)"), *GetNameSafe(Root), *GetNameSafe(World));
		return false;
	}

	MoviePlayer = NewObject<UMediaPlayer>(this);
	MoviePlayer->PlayOnOpen = false; // played by the tick, once the world behind has drawn a couple of frames
	MoviePlayer->SetLooping(false);
	MoviePlayer->OnMediaOpened.AddDynamic(this, &UFoundryBootSplash::HandleMovieOpened);
	MoviePlayer->OnMediaOpenFailed.AddDynamic(this, &UFoundryBootSplash::HandleMovieOpenFailed);
	MoviePlayer->OnEndReached.AddDynamic(this, &UFoundryBootSplash::HandleMovieEnded);

	MovieTexture = NewObject<UMediaTexture>(this);
	MovieTexture->AutoClear = true;
	MovieTexture->ClearColor = FLinearColor::Black;
	MovieTexture->SetMediaPlayer(MoviePlayer);
	MovieTexture->UpdateResource();

	MovieImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	if (MovieMaterial)
	{
		MovieMID = UMaterialInstanceDynamic::Create(MovieMaterial, this);
		MovieMID->SetTextureParameterValue(TEXT("Movie"), MovieTexture);
		MovieMID->SetScalarParameterValue(TEXT("MovieTime"), 0.0f);
		MovieImage->SetBrushFromMaterial(MovieMID);
	}
	else
	{
		MovieImage->SetBrushResourceObject(MovieTexture);
	}
	// The brush's own size is the movie's 16:9 frame: a material brush defaults to 32 x 32, which the cover fit
	// scaled as a SQUARE - the video was stretched 16/9 vertically (2026-10-25).
	{
		FSlateBrush Brush = MovieImage->GetBrush();
		Brush.ImageSize = MovieNativeSize;
		Brush.DrawAs = ESlateBrushDrawType::Image;
		MovieImage->SetBrush(Brush);
	}
	MovieImage->SetDesiredSizeOverride(MovieNativeSize);
	MovieImage->SetVisibility(ESlateVisibility::HitTestInvisible);

	// A cover fit: the image keeps the video's aspect and fills the screen (a 16:9 mask in the material stays aligned with the video).
	MovieScaleBox = WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass());
	MovieScaleBox->SetStretch(EStretch::ScaleToFill);
	MovieScaleBox->SetVisibility(ESlateVisibility::HitTestInvisible);
	MovieScaleBox->AddChild(MovieImage);
	UPanelSlot* BoxSlot = Root->AddChild(MovieScaleBox); // last child of the Overlay = above everything
	if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(BoxSlot))
	{
		OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
		OverlaySlot->SetVerticalAlignment(VAlign_Fill);
	}

	// Audio: the movie's own track, through a transient actor's media sound component (2D, UI sound).
	FActorSpawnParameters Params;
	Params.ObjectFlags |= RF_Transient;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	MovieSoundActor = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Params);
	if (MovieSoundActor)
	{
		MovieSound = NewObject<UMediaSoundComponent>(MovieSoundActor, TEXT("BootSplashMovieSound"));
		MovieSound->bIsUISound = true;
		MovieSound->bAllowSpatialization = false;
		MovieSound->SoundClass = MovieSoundClass;
		MovieSound->SetMediaPlayer(MoviePlayer);
		MovieSoundActor->SetRootComponent(MovieSound);
		MovieSound->RegisterComponent();
		// After RegisterComponent: the synth forwards the multiplier to its audio component, created in OnRegister.
		if (const UFoundryUISettings* Settings = UFoundryUISettings::Get())
		{
			MovieSound->SetVolumeMultiplier(FMath::Clamp(Settings->BootSplashMovieVolume, 0.0f, 1.0f));
		}
		if (!MovieSound->IsActive())
		{
			MovieSound->Activate(true);
		}
	}
	else
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] could not spawn the movie's sound actor - the movie plays silent"));
	}

	// The live world shows through wherever the material is transparent (and the video is opaque black until then).
	if (Backdrop)
	{
		Backdrop->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (LogoRow)
	{
		LogoRow->SetVisibility(ESlateVisibility::Collapsed);
	}

	BuildKeyArt(Root); // above the movie: the key art shows from the first frame, then fades to black

	// Pause the world NOW, before the first frame: that frame is otherwise a full Lumen/RT render of the lobby
	// behind an opaque splash (0.39 s on Justin's box, 2026-10-25), long enough to show as a black flash.
	if (const UFoundryUISettings* Settings = UFoundryUISettings::Get())
	{
		MovieWorldWarmUpSeconds = Settings->BootSplashMovieWorldWarmUpSeconds;
	}
	if (MovieWorldWarmUpSeconds >= 0.0f)
	{
		SetWorldRenderingPaused(true);
	}

	if (!MoviePlayer->OpenSource(MovieSource))
	{
		bMovieOpenFailed = true; // an immediate refusal: the first tick falls back to the logos
	}
	return true;
}

bool UFoundryBootSplash::BuildKeyArt(UPanelWidget* Root)
{
	const UFoundryUISettings* Settings = UFoundryUISettings::Get();
	FString Relative = Settings ? Settings->StartupScreenImage : FString();
	Relative.TrimStartAndEndInline();
	if (Relative.IsEmpty() || Root == nullptr)
	{
		return false;
	}
	const FString Path = FPaths::IsRelative(Relative) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Relative) : Relative;
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
	uint8 Header[24] = {};
	if (!Reader || Reader->TotalSize() < 24)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] key art %s is missing - the splash starts on the movie"), *Path);
		return false;
	}
	Reader->Serialize(Header, sizeof(Header));
	Reader.Reset();
	const uint32 W = (uint32(Header[16]) << 24) | (uint32(Header[17]) << 16) | (uint32(Header[18]) << 8) | uint32(Header[19]);
	const uint32 H = (uint32(Header[20]) << 24) | (uint32(Header[21]) << 16) | (uint32(Header[22]) << 8) | uint32(Header[23]);
	if (Header[1] != 'P' || Header[2] != 'N' || Header[3] != 'G' || W == 0 || H == 0)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] key art %s is not a PNG - the splash starts on the movie"), *Path);
		return false;
	}

	// The SAME file the startup screen drew, decoded NOW into a texture: the very first game frame must already
	// hold the picture (the engine drops its loading screen the moment the map has loaded, so any frame without it
	// is a black flash between the two). A by-name Slate brush decodes lazily at first paint.
	const FVector2D Size{ double(W), double(H) };
	KeyArtTexture = FImageUtils::ImportFileAsTexture2D(Path);
	if (KeyArtTexture == nullptr)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] key art %s could not be decoded - the splash starts on the movie"), *Path);
		return false;
	}
	UImage* KeyArt = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.SetResourceObject(KeyArtTexture);
		Brush.SetImageSize(Size);
		KeyArt->SetBrush(Brush);
	}
	KeyArt->SetDesiredSizeOverride(Size);
	KeyArt->SetVisibility(ESlateVisibility::HitTestInvisible);
	KeyArtScaleBox = WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass());
	KeyArtScaleBox->SetStretch(EStretch::ScaleToFill);
	KeyArtScaleBox->SetVisibility(ESlateVisibility::HitTestInvisible);
	KeyArtScaleBox->AddChild(KeyArt);
	if (UOverlaySlot* OverlaySlot = Cast<UOverlaySlot>(Root->AddChild(KeyArtScaleBox)))
	{
		OverlaySlot->SetHorizontalAlignment(HAlign_Fill);
		OverlaySlot->SetVerticalAlignment(VAlign_Fill);
	}
	KeyArtHoldSeconds = Settings ? FMath::Max(0.0f, Settings->StartupScreenHoldSeconds) : 1.0f;
	KeyArtFadeSeconds = Settings ? FMath::Max(0.0f, Settings->StartupScreenFadeSeconds) : 0.8f;
	KeyArtElapsed = 0.0f;
	return true;
}

bool UFoundryBootSplash::TickKeyArt(float InDeltaTime)
{
	if (KeyArtScaleBox == nullptr)
	{
		return true;
	}
	KeyArtElapsed += FMath::Min(InDeltaTime, MaxHoldTickSeconds); // a first-frame hitch must not eat the hold
	const float IntoFade = KeyArtElapsed - KeyArtHoldSeconds;
	const float Alpha = IntoFade <= 0.0f ? 1.0f : (KeyArtFadeSeconds > 0.0f ? 1.0f - FMath::Clamp(IntoFade / KeyArtFadeSeconds, 0.0f, 1.0f) : 0.0f);
	KeyArtScaleBox->SetRenderOpacity(FMath::InterpEaseInOut(0.0f, 1.0f, Alpha, 2.0f));
	if (Alpha > 0.0f)
	{
		return false;
	}
	ReleaseKeyArt();
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] key art faded to black"));
	return true;
}

void UFoundryBootSplash::ReleaseKeyArt()
{
	if (KeyArtScaleBox)
	{
		KeyArtScaleBox->RemoveFromParent();
		KeyArtScaleBox = nullptr;
	}
	KeyArtTexture = nullptr;
}

void UFoundryBootSplash::HandleMovieOpened(FString OpenedUrl)
{
	bMovieOpened = true;
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] movie opened: %s (duration %.2f s, player %s)"), *OpenedUrl,
		MoviePlayer ? MoviePlayer->GetDuration().GetTotalSeconds() : -1.0, MoviePlayer ? *MoviePlayer->GetPlayerName().ToString() : TEXT("?"));
}

void UFoundryBootSplash::HandleMovieOpenFailed(FString FailedUrl)
{
	bMovieOpenFailed = true;
	UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] movie failed to open: %s"), *FailedUrl);
}

void UFoundryBootSplash::HandleMovieEnded()
{
	bMovieEnded = true;
}

void UFoundryBootSplash::FireReveal(float MovieSeconds)
{
	if (bRevealFired)
	{
		return;
	}
	bRevealFired = true;
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] movie reveal at %.2f s of movie time (hand-over to the host)"), MovieSeconds);
	const int32 Wanting = IStreamingManager::Get().GetNumWantingResources();
	if (Wanting > 0)
	{
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] reveal: %d streaming resources still wanted behind the splash"), Wanting);
	}
	OnFadeOutStarted.Broadcast();
}

void UFoundryBootSplash::SetWorldRenderingPaused(bool bPaused)
{
	if (bPaused == bWorldRenderingPaused)
	{
		return;
	}
	UWorld* World = GetWorld();
	UGameViewportClient* Viewport = World ? World->GetGameViewport() : nullptr;
	if (Viewport == nullptr)
	{
		return;
	}
	Viewport->bDisableWorldRendering = bPaused;
	bWorldRenderingPaused = bPaused;
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] world rendering %s"), bPaused ? TEXT("paused under the opaque movie") : TEXT("resumed"));
}

void UFoundryBootSplash::CleanupMovie()
{
	SetWorldRenderingPaused(false); // never leave the world unrendered, whatever ended the movie
	if (MoviePlayer)
	{
		MoviePlayer->OnMediaOpened.RemoveDynamic(this, &UFoundryBootSplash::HandleMovieOpened);
		MoviePlayer->OnMediaOpenFailed.RemoveDynamic(this, &UFoundryBootSplash::HandleMovieOpenFailed);
		MoviePlayer->OnEndReached.RemoveDynamic(this, &UFoundryBootSplash::HandleMovieEnded);
	}
	if (MovieSound)
	{
		MovieSound->SetMediaPlayer(nullptr);
		MovieSound = nullptr;
	}
	if (MovieSoundActor)
	{
		MovieSoundActor->Destroy();
		MovieSoundActor = nullptr;
	}
	if (MovieTexture)
	{
		MovieTexture->SetMediaPlayer(nullptr);
		MovieTexture = nullptr;
	}
	if (MoviePlayer)
	{
		MoviePlayer->Close();
		MoviePlayer = nullptr;
	}
	if (MovieScaleBox)
	{
		MovieScaleBox->RemoveFromParent();
		MovieScaleBox = nullptr;
	}
	ReleaseKeyArt();
	MovieImage = nullptr;
	MovieMID = nullptr;
}

void UFoundryBootSplash::FallBackToLogos(const TCHAR* Reason)
{
	UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[BootSplash] movie mode failed (%s) - falling back to the logos + stinger"), Reason);
	CleanupMovie();
	bMovieMode = false;
	if (bRevealFired)
	{
		Finish(); // the host already took over: nothing left to play
		return;
	}
	if (Backdrop)
	{
		Backdrop->SetVisibility(ESlateVisibility::Visible);
	}
	if (LogoRow)
	{
		LogoRow->SetVisibility(ESlateVisibility::Visible);
		if (!FadeAnim)
		{
			LogoRow->SetRenderOpacity(0.0f);
		}
	}
	Phase = EPhase::NotStarted;
	SinceStart = 0.0f;
}

void UFoundryBootSplash::TickMovie(float InDeltaTime)
{
	if (Phase == EPhase::NotStarted)
	{
		// First tick, not Construct: the world is up and the audio device is ready.
		Phase = EPhase::MovieOpening;
		MoviePhaseSeconds = 0.0f;
		MovieFrames = 0;
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] movie opening %s%s"), *GetNameSafe(MovieSource), KeyArtScaleBox ? TEXT(" behind the key art") : TEXT(""));
		if (const UFoundryUISettings* Settings = UFoundryUISettings::Get())
		{
			MovieWorldWarmUpSeconds = Settings->BootSplashMovieWorldWarmUpSeconds;
		}
		if (MovieWorldWarmUpSeconds >= 0.0f)
		{
			SetWorldRenderingPaused(true); // covered by the key art / the opaque movie from here
		}
		return;
	}

	MoviePhaseSeconds += InDeltaTime; // real time, never capped
	++MovieFrames;

	if (Phase == EPhase::MovieOpening)
	{
		const bool bKeyArtDone = TickKeyArt(InDeltaTime);
		if (bMovieOpenFailed)
		{
			FallBackToLogos(TEXT("the movie failed to open"));
		}
		else if (bMovieOpened)
		{
			// Play only once the world behind has drawn a couple of frames, so the reveal never lands on a cold scene.
			if (MovieFrames >= 2 && bKeyArtDone && MoviePlayer)
			{
				MoviePlayer->Play();
				Phase = EPhase::MoviePlaying;
				MoviePhaseSeconds = 0.0f;
				UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] movie playing (duration %.2f s, reveal at %.2f s)"),
					MoviePlayer->GetDuration().GetTotalSeconds(), MovieRevealSeconds);
			}
		}
		else if (MoviePhaseSeconds > MovieOpenTimeoutSeconds + KeyArtHoldSeconds + KeyArtFadeSeconds)
		{
			FallBackToLogos(TEXT("the movie did not open in time"));
		}
		return;
	}

	// MoviePlaying
	if (MoviePlayer == nullptr)
	{
		Finish();
		return;
	}
	const float Duration = static_cast<float>(MoviePlayer->GetDuration().GetTotalSeconds());
	const float T = static_cast<float>(MoviePlayer->GetTime().GetTotalSeconds());
	if (MovieMID)
	{
		MovieMID->SetScalarParameterValue(TEXT("MovieTime"), T);
	}
	// The world renders again a little before the reveal (warm-up behind the still-opaque frame).
	if (bWorldRenderingPaused)
	{
		const float RevealAt = MovieRevealSeconds > 0.0f ? MovieRevealSeconds : Duration;
		if (RevealAt <= 0.0f || T >= RevealAt - MovieWorldWarmUpSeconds)
		{
			SetWorldRenderingPaused(false);
		}
	}
	if (!bRevealFired && MovieRevealSeconds > 0.0f && T >= MovieRevealSeconds)
	{
		FireReveal(T);
	}
	if (bMovieEnded || (Duration > 0.0f && T >= Duration))
	{
		FireReveal(FMath::Max(T, Duration)); // reveal at 0 = at the end (no-op when it already ran)
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] movie ended at %.2f s"), T);
		Finish();
		return;
	}
	if (T <= 0.0f && MoviePhaseSeconds > MovieOpenTimeoutSeconds)
	{
		FallBackToLogos(TEXT("the movie clock never advanced"));
	}
}

void UFoundryBootSplash::NativeDestruct()
{
	CleanupMovie();
	Super::NativeDestruct();
}

void UFoundryBootSplash::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true);
}

void UFoundryBootSplash::NativeConstruct()
{
	Super::NativeConstruct();
	SetKeyboardFocus();
	SetCursor(EMouseCursor::None); // a short unskippable cutscene: no pointer over it
	if (LogoRow && !FadeAnim)
	{
		LogoRow->SetRenderOpacity(0.0f); // the first value of the fade, so the logos never flash before the first tick
	}
}

void UFoundryBootSplash::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (Phase == EPhase::Finished)
	{
		return;
	}

	// Non-skippable: if anything stole focus, take it back.
	if (!HasKeyboardFocus())
	{
		SetKeyboardFocus();
	}

	if (bMovieMode)
	{
		TickMovie(InDeltaTime); // a failed movie drops bMovieMode and the logo sequence below takes over on the next tick
		return;
	}

	if (Phase != EPhase::NotStarted)
	{
		SinceStart += InDeltaTime; // real time since the stinger started - never capped
	}
	if (Phase == EPhase::NotStarted)
	{
		// First tick, not Construct: the world is up and the audio device is ready.
		Phase = EPhase::FadeIn;
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] in"));
		if (StingerSound)
		{
			UGameplayStatics::PlaySound2D(this, StingerSound);
		}
		PlayOrTween(FadeAnim, true, FadeInDuration, LogoRow);
		return;
	}

	if (Phase == EPhase::Hold)
	{
		HoldElapsed += FMath::Min(InDeltaTime, MaxHoldTickSeconds);
		// With a stinger the fade-out is scheduled from the SOUND's start (fade-in + hold), so a slow first frame
		// that stretched the fade-in never pushes the logos past the end of the sound.
		const bool bFadeOutDue = bRealTimeHold ? SinceStart >= FadeInDuration + HoldDuration : HoldElapsed >= HoldDuration;
		if (bFadeOutDue)
		{
			Phase = EPhase::FadeOut;
			FadeOutElapsed = 0.0f;
			UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] out"));
			PlayOrTween(FadeAnim, false, FadeOutDuration, LogoRow);
			OnFadeOutStarted.Broadcast();
		}
		return;
	}
	if (Phase == EPhase::FadeOut && bRevealSceneOnFadeOut && Backdrop)
	{
		FadeOutElapsed += InDeltaTime;
		const float T = FadeOutDuration > 0.0f ? FMath::Clamp(FadeOutElapsed / FadeOutDuration, 0.0f, 1.0f) : 1.0f;
		Backdrop->SetRenderOpacity(1.0f - T);
	}
}

void UFoundryBootSplash::NativeOnTransitionFinished(bool bForward)
{
	Super::NativeOnTransitionFinished(bForward);
	if (bForward)
	{
		if (Phase == EPhase::FadeIn)
		{
			Phase = EPhase::Hold;
			HoldElapsed = 0.0f;
			UE_LOG(LogFoundryFSDKUI, Log, TEXT("[BootSplash] hold"));
		}
		return;
	}
	if (Phase == EPhase::FadeOut)
	{
		Finish();
	}
}

FReply UFoundryBootSplash::NativeOnPreviewKeyDown(const FGeometry&, const FKeyEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnKeyDown(const FGeometry&, const FKeyEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnKeyUp(const FGeometry&, const FKeyEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnKeyChar(const FGeometry&, const FCharacterEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnAnalogValueChanged(const FGeometry&, const FAnalogInputEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnPreviewMouseButtonDown(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseButtonDown(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseButtonUp(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseButtonDoubleClick(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseMove(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnMouseWheel(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnTouchStarted(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnTouchMoved(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
FReply UFoundryBootSplash::NativeOnTouchEnded(const FGeometry&, const FPointerEvent&) { return FReply::Handled(); }
