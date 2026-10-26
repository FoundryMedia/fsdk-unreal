#include "Modules/ModuleManager.h"

#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateDynamicImageBrush.h"
#include "IImageWrapperModule.h"
#include "Misc/CoreMisc.h"
#include "HAL/FileManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "MoviePlayer.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScaleBox.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoundryStartupScreen, Log, All);

/**
 * The startup screen: UFoundryUISettings::StartupScreenImage (a PNG under the project's Content directory,
 * staged NonUFS) shown cover-fit over black from the window's first frame until the first map is loaded.
 * The boot splash then shows the SAME file on its first frame (UFoundryBootSplash), so the hand-over is
 * invisible. Built at PreLoadingScreen: no UObject, no config object - the key is read straight from the ini.
 * Off in the editor, commandlets, dedicated servers, and wherever the movie player is disabled.
 */
class FFoundryFSDKStartupScreenModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (GIsEditor || IsRunningCommandlet() || IsRunningDedicatedServer() || !IsMoviePlayerEnabled() || GConfig == nullptr)
		{
			return;
		}
		FString Relative;
		GConfig->GetString(TEXT("/Script/FoundryFSDKUI.FoundryUISettings"), TEXT("StartupScreenImage"), Relative, GGameIni);
		Relative.TrimStartAndEndInline();
		if (Relative.IsEmpty())
		{
			return;
		}
		const FString Path = FPaths::IsRelative(Relative) ? FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir() / Relative) : Relative;
		FVector2D Size;
		if (!ReadPngSize(Path, Size))
		{
			UE_LOG(LogFoundryStartupScreen, Warning, TEXT("startup screen image %s is missing or not a PNG - the engine's default (black) startup shows"), *Path);
			return;
		}

		// A DYNAMIC brush: Slate decodes the file by name. A plain FSlateImageBrush on an unregistered path
		// resolves to no texture and draws a solid white quad (the resource map only holds style-set brushes).
		// The decode can run off the game thread, where ImageWrapper cannot be loaded - load it now.
		FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		ImageBrush = MakeShared<FSlateDynamicImageBrush>(FName(*Path), Size);
		FLoadingScreenAttributes Screen;
		Screen.bAutoCompleteWhenLoadingCompletes = true;
		Screen.bMoviesAreSkippable = false;
		Screen.WidgetLoadingScreen =
			SNew(SBorder)
			.BorderImage(&BlackBrush)
			.Padding(0.0f)
			[
				SNew(SScaleBox)
				.Stretch(EStretch::ScaleToFill)
				.HAlign(HAlign_Center)
				.VAlign(VAlign_Center)
				[
					SNew(SImage).Image(ImageBrush.Get())
				]
			];
		GetMoviePlayer()->SetupLoadingScreen(Screen);
		UE_LOG(LogFoundryStartupScreen, Log, TEXT("startup screen: %s (%.0f x %.0f)"), *Relative, Size.X, Size.Y);
	}

	virtual bool IsGameModule() const override
	{
		return false;
	}

private:
	/** Width and height from a PNG's IHDR chunk (bytes 16..23, big-endian). */
	static bool ReadPngSize(const FString& Path, FVector2D& OutSize)
	{
		TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*Path));
		if (!Reader || Reader->TotalSize() < 24)
		{
			return false;
		}
		uint8 Header[24];
		Reader->Serialize(Header, sizeof(Header));
		static const uint8 Signature[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		if (FMemory::Memcmp(Header, Signature, 8) != 0)
		{
			return false;
		}
		const uint32 W = (uint32(Header[16]) << 24) | (uint32(Header[17]) << 16) | (uint32(Header[18]) << 8) | uint32(Header[19]);
		const uint32 H = (uint32(Header[20]) << 24) | (uint32(Header[21]) << 16) | (uint32(Header[22]) << 8) | uint32(Header[23]);
		if (W == 0 || H == 0)
		{
			return false;
		}
		OutSize = FVector2D(double(W), double(H));
		return true;
	}

	FSlateColorBrush BlackBrush = FSlateColorBrush(FLinearColor::Black);
	TSharedPtr<FSlateDynamicImageBrush> ImageBrush;
};

IMPLEMENT_MODULE(FFoundryFSDKStartupScreenModule, FoundryFSDKStartupScreen)
