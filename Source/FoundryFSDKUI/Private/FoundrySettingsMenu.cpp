#include "FoundrySettingsMenu.h"

#include "Components/Button.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/Slider.h"
#include "Components/TextBlock.h"
#include "Engine/Engine.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Texture2D.h" // TSoftObjectPtr<UTexture2D>::LoadSynchronous() needs the full type
#include "EnhancedInputSubsystems.h"
#include "FoundryConfirmDialog.h"
#include "FoundryGameUserSettings.h"
#include "FoundryKeybindRow.h"
#include "FoundryMenuHost.h"
#include "FoundryUISettings.h"
#include "FoundryUITheme.h"
#include "GameFramework/GameUserSettings.h"
#include "InputMappingContext.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UserSettings/EnhancedInputUserSettings.h"

namespace
{
	const TCHAR* WindowModeLabel(EWindowMode::Type Mode)
	{
		switch (Mode)
		{
		case EWindowMode::Fullscreen: return TEXT("Fullscreen");
		case EWindowMode::WindowedFullscreen: return TEXT("Windowed Fullscreen");
		case EWindowMode::Windowed:
		default: return TEXT("Windowed");
		}
	}

	EWindowMode::Type ParseWindowMode(const FString& Label)
	{
		if (Label == TEXT("Fullscreen")) { return EWindowMode::Fullscreen; }
		if (Label == TEXT("Windowed Fullscreen")) { return EWindowMode::WindowedFullscreen; }
		return EWindowMode::Windowed;
	}

	// Key table for both the combo's options and FpsCapCombo's parse-back.
	constexpr float FpsCaps[] = { 0.0f, 30.0f, 60.0f, 120.0f, 144.0f, 240.0f };
	const TCHAR* const FpsCapLabels[] = { TEXT("Off"), TEXT("30"), TEXT("60"), TEXT("120"), TEXT("144"), TEXT("240") };

	const TCHAR* const QualityLevelLabels[] = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Cinematic") };

	/** Sets bPopulating true for its lifetime, false again on every exit path (including an early
	 *  return) - a Populate function drives SetSelectedOption / ClearOptions / AddOption calls that fire their own
	 *  OnSelectionChanged/OnValueChanged/OnCheckStateChanged, and every handler checks this flag
	 *  before doing anything (see the comment on UFoundrySettingsMenu::bPopulating). */
	struct FScopedPopulating
	{
		bool& Flag;
		explicit FScopedPopulating(bool& InFlag) : Flag(InFlag) { Flag = true; }
		~FScopedPopulating() { Flag = false; }
	};
}

UFoundrySettingsMenu::UFoundrySettingsMenu()
{
	bRetakeFocusOnClick = true;
}

void UFoundrySettingsMenu::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	if (BackButton) { BackButton->OnClicked.AddDynamic(this, &UFoundrySettingsMenu::HandleBackClicked); }
	if (CloseButton) { CloseButton->OnClicked.AddDynamic(this, &UFoundrySettingsMenu::HandleBackClicked); }
	if (ApplyButton) { ApplyButton->OnClicked.AddDynamic(this, &UFoundrySettingsMenu::HandleApplyClicked); }

	if (WindowModeCombo) { WindowModeCombo->OnSelectionChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleWindowModeChanged); }
	if (ResolutionCombo) { ResolutionCombo->OnSelectionChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleResolutionChanged); }
	if (QualityCombo) { QualityCombo->OnSelectionChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleQualityChanged); }
	if (FpsCapCombo) { FpsCapCombo->OnSelectionChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleFpsCapChanged); }
	if (VSyncCheck) { VSyncCheck->OnCheckStateChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleVSyncChanged); }

	if (MasterSlider)
	{
		MasterSlider->OnValueChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleMasterVolumeChanged);
		MasterSlider->OnMouseCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
		MasterSlider->OnControllerCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
	}
	if (MusicSlider)
	{
		MusicSlider->OnValueChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleMusicVolumeChanged);
		MusicSlider->OnMouseCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
		MusicSlider->OnControllerCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
	}
	if (SFXSlider)
	{
		SFXSlider->OnValueChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleSFXVolumeChanged);
		SFXSlider->OnMouseCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
		SFXSlider->OnControllerCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
	}
	if (UIScaleSlider)
	{
		UIScaleSlider->OnValueChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleUIScaleChanged);
		UIScaleSlider->OnMouseCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
		UIScaleSlider->OnControllerCaptureEnd.AddDynamic(this, &UFoundrySettingsMenu::HandleAudioSliderCaptureEnd);
	}
	if (ReducedMotionCheck) { ReducedMotionCheck->OnCheckStateChanged.AddDynamic(this, &UFoundrySettingsMenu::HandleReducedMotionChanged); }
	if (ResetAllKeysButton) { ResetAllKeysButton->OnClicked.AddDynamic(this, &UFoundrySettingsMenu::HandleResetAllKeysClicked); }

	SetIsFocusable(true);
	BuildDescribedRows(); // the Row UPROPERTYs above are already bound by this point
}

void UFoundrySettingsMenu::NativeConstruct()
{
	Super::NativeConstruct();
	PopulateVideo();
	// One-time per combo: TakeWidget() (inside ScrubComboChrome) realizes the live Slate tree, which
	// is when the popup's own chrome becomes settable - must run after PopulateVideo binds the combo
	// into the tree, but is otherwise independent of its content.
	ScrubComboChrome(WindowModeCombo);
	ScrubComboChrome(ResolutionCombo);
	ScrubComboChrome(QualityCombo);
	ScrubComboChrome(FpsCapCombo);
	PopulateAudioAndInterface();
	PopulateControls();
	Show(NoteText, false);
	SetKeyboardFocus();
	PlayOrTween(MenuAnim, true, 0.18f, Panel ? Panel : this);
}

void UFoundrySettingsMenu::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (NoteTimer > 0.0f)
	{
		NoteTimer -= InDeltaTime;
		if (NoteTimer <= 0.0f)
		{
			Show(NoteText, false);
		}
	}
	UpdateHoveredDescription();
	UpdateSliderHover();
}

FReply UFoundrySettingsMenu::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		HandleBackClicked();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

FReply UFoundrySettingsMenu::NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// Tunnels top-down through the whole focus path BEFORE the bubble phase - a focused combo or
	// slider would otherwise consume Escape itself (closing its own popup, say) and never let the
	// bubble-phase NativeOnKeyDown above see it. A capturing keybind row needs the OPPOSITE: let its
	// own (bubble-phase) OnKeyDown treat Escape as a capture-cancel, not a menu-close.
	if (InKeyEvent.GetKey() == EKeys::Escape && !IsAnyKeybindRowCapturing())
	{
		HandleBackClicked();
		return FReply::Handled();
	}
	return Super::NativeOnPreviewKeyDown(InGeometry, InKeyEvent);
}

bool UFoundrySettingsMenu::IsAnyKeybindRowCapturing() const
{
	if (KeybindList == nullptr)
	{
		return false;
	}
	for (UWidget* Child : KeybindList->GetAllChildren())
	{
		if (const UFoundryKeybindRow* Row = Cast<UFoundryKeybindRow>(Child))
		{
			if (Row->IsCapturing())
			{
				return true;
			}
		}
	}
	return false;
}

void UFoundrySettingsMenu::HandleBackClicked()
{
	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestPopMenu(Host);
	}
	else
	{
		RemoveFromParent();
	}
}

void UFoundrySettingsMenu::ShowNote(const FString& Message)
{
	SetText(NoteText, Message);
	Show(NoteText, true);
	NoteTimer = 3.0f;
}

// -- Video: STAGED through PendingVideo, committed to the engine's own UGameUserSettings only by
// ApplyButton. A resolution-revert countdown (ConfirmVideoMode / RevertVideoMode) is v2 - today
// Apply is immediate and permanent, like every other commit on this menu. ------------------------

void UFoundrySettingsMenu::PopulateVideo()
{
	FScopedPopulating Guard(bPopulating);
	UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (GUS == nullptr)
	{
		Show(VideoSection, false);
		return;
	}
	Show(VideoSection, true);

	// Re-seed the pending struct from the LIVE settings - this is also the "Back/Escape discards
	// pending changes" behavior: nothing was ever written to GUS, so the next open just reads the
	// same live values again.
	PendingVideo.WindowMode = GUS->GetFullscreenMode();
	PendingVideo.Resolution = GUS->GetScreenResolution();
	PendingVideo.Quality = GUS->GetOverallScalabilityLevel();
	PendingVideo.FpsCap = GUS->GetFrameRateLimit();
	PendingVideo.bVSync = GUS->IsVSyncEnabled();

	if (WindowModeCombo)
	{
		WindowModeCombo->ClearOptions();
		WindowModeCombo->AddOption(TEXT("Fullscreen"));
		WindowModeCombo->AddOption(TEXT("Windowed Fullscreen"));
		WindowModeCombo->AddOption(TEXT("Windowed"));
		WindowModeCombo->SetSelectedOption(WindowModeLabel(GUS->GetFullscreenMode()));
	}

	if (ResolutionCombo)
	{
		ResolutionCombo->ClearOptions();
		TArray<FIntPoint> Resolutions;
		UKismetSystemLibrary::GetSupportedFullscreenResolutions(Resolutions);
		Resolutions.Sort([](const FIntPoint& A, const FIntPoint& B)
		{
			return A.X != B.X ? A.X < B.X : A.Y < B.Y;
		});

		const FIntPoint Current = GUS->GetScreenResolution();
		TSet<FString> Seen;
		bool bHasCurrent = false;
		for (const FIntPoint& Res : Resolutions)
		{
			const FString Label = FString::Printf(TEXT("%dx%d"), Res.X, Res.Y);
			if (Seen.Contains(Label))
			{
				continue; // dedupe
			}
			Seen.Add(Label);
			ResolutionCombo->AddOption(Label);
			bHasCurrent |= (Res == Current);
		}
		const FString CurrentLabel = FString::Printf(TEXT("%dx%d"), Current.X, Current.Y);
		if (!bHasCurrent)
		{
			ResolutionCombo->AddOption(CurrentLabel);
		}
		ResolutionCombo->SetSelectedOption(CurrentLabel);
	}

	if (QualityCombo)
	{
		QualityCombo->ClearOptions();
		for (const TCHAR* Label : QualityLevelLabels)
		{
			QualityCombo->AddOption(Label);
		}
		const int32 Current = GUS->GetOverallScalabilityLevel();
		if (Current >= 0 && Current < static_cast<int32>(UE_ARRAY_COUNT(QualityLevelLabels)))
		{
			QualityCombo->SetSelectedOption(QualityLevelLabels[Current]);
		}
		else
		{
			// -1 (or anything out of range) means the scalability settings are a custom mix the
			// named levels don't describe - add a placeholder rather than silently picking one.
			QualityCombo->AddOption(TEXT("Custom"));
			QualityCombo->SetSelectedOption(TEXT("Custom"));
		}
	}

	if (FpsCapCombo)
	{
		FpsCapCombo->ClearOptions();
		for (const TCHAR* Label : FpsCapLabels)
		{
			FpsCapCombo->AddOption(Label);
		}
		const float CurrentLimit = GUS->GetFrameRateLimit();
		int32 MatchIndex = INDEX_NONE;
		for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(FpsCaps)); ++i)
		{
			if (FMath::IsNearlyEqual(FpsCaps[i], CurrentLimit, 0.5f))
			{
				MatchIndex = i;
				break;
			}
		}
		if (MatchIndex != INDEX_NONE)
		{
			FpsCapCombo->SetSelectedOption(FpsCapLabels[MatchIndex]);
		}
		else
		{
			const FString CustomLabel = FString::Printf(TEXT("%.0f"), CurrentLimit);
			FpsCapCombo->AddOption(CustomLabel);
			FpsCapCombo->SetSelectedOption(CustomLabel);
		}
	}

	if (VSyncCheck)
	{
		VSyncCheck->SetIsChecked(GUS->IsVSyncEnabled());
	}

	SetPendingVideoDirty(false); // re-seeded from live settings - nothing pending any more
}

void UFoundrySettingsMenu::SetPendingVideoDirty(bool bDirty)
{
	if (bPendingVideoDirty == bDirty)
	{
		return;
	}
	bPendingVideoDirty = bDirty;
	if (ApplyButton)
	{
		ApplyButton->SetIsEnabled(bDirty);
	}
	OnVideoPendingChanged(bDirty);
}

void UFoundrySettingsMenu::HandleWindowModeChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (bPopulating || SelectedItem.IsEmpty() || SelectionType == ESelectInfo::Direct)
	{
		return; // PopulateVideo's own SetSelectedOption / ClearOptions calls, not a user pick
	}
	PendingVideo.WindowMode = ParseWindowMode(SelectedItem);
	SetPendingVideoDirty(true);
}

void UFoundrySettingsMenu::HandleResolutionChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (bPopulating || SelectedItem.IsEmpty() || SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	FString XStr, YStr;
	if (!SelectedItem.Split(TEXT("x"), &XStr, &YStr))
	{
		return;
	}
	PendingVideo.Resolution = FIntPoint(FCString::Atoi(*XStr), FCString::Atoi(*YStr));
	SetPendingVideoDirty(true);
}

void UFoundrySettingsMenu::HandleQualityChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (bPopulating || SelectedItem.IsEmpty() || SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	if (SelectedItem == TEXT("Custom"))
	{
		return; // the placeholder for "none of the named levels" - nothing to set
	}
	for (int32 i = 0; i < static_cast<int32>(UE_ARRAY_COUNT(QualityLevelLabels)); ++i)
	{
		if (SelectedItem == QualityLevelLabels[i])
		{
			PendingVideo.Quality = i;
			SetPendingVideoDirty(true);
			// The "Custom" placeholder only belongs while scalability IS custom - drop it directly
			// (a no-op RemoveOption::ClearSelection only fires if "Custom" were still selected, which
			// it never is here) rather than re-running PopulateVideo, which would recurse through
			// this same handler via SetSelectedOption's Direct callback.
			if (QualityCombo)
			{
				QualityCombo->RemoveOption(TEXT("Custom"));
			}
			return;
		}
	}
}

void UFoundrySettingsMenu::HandleFpsCapChanged(FString SelectedItem, ESelectInfo::Type SelectionType)
{
	if (bPopulating || SelectedItem.IsEmpty() || SelectionType == ESelectInfo::Direct)
	{
		return;
	}
	PendingVideo.FpsCap = (SelectedItem == TEXT("Off")) ? 0.0f : FCString::Atof(*SelectedItem);
	SetPendingVideoDirty(true);
}

void UFoundrySettingsMenu::HandleVSyncChanged(bool bIsChecked)
{
	if (bPopulating)
	{
		return; // PopulateVideo's own SetIsChecked call
	}
	PendingVideo.bVSync = bIsChecked;
	SetPendingVideoDirty(true);
}

void UFoundrySettingsMenu::HandleApplyClicked()
{
	UGameUserSettings* GUS = GEngine ? GEngine->GetGameUserSettings() : nullptr;
	if (GUS == nullptr)
	{
		return;
	}
	GUS->SetFullscreenMode(PendingVideo.WindowMode);
	GUS->SetScreenResolution(PendingVideo.Resolution);
	if (PendingVideo.Quality >= 0)
	{
		GUS->SetOverallScalabilityLevel(PendingVideo.Quality);
	}
	GUS->SetFrameRateLimit(PendingVideo.FpsCap);
	GUS->SetVSyncEnabled(PendingVideo.bVSync);
	GUS->ApplySettings(false);
	GUS->SaveSettings();
	PopulateVideo(); // re-seeds from the now-live settings and clears the pending flag
	ShowNote(TEXT("Applied."));
}

// -- Audio + Interface: UFoundryGameUserSettings. Both sections hide together when the project's
// GameUserSettingsClassName isn't one. ------------------------------------------------------------

void UFoundrySettingsMenu::PopulateAudioAndInterface()
{
	FScopedPopulating Guard(bPopulating);
	UFoundryGameUserSettings* Settings = UFoundryGameUserSettings::Get();
	if (Settings == nullptr)
	{
		Show(AudioSection, false);
		Show(InterfaceSection, false);
		return;
	}
	Show(AudioSection, true);
	Show(InterfaceSection, true);

	if (MasterSlider) { MasterSlider->SetValue(Settings->GetMasterVolume()); }
	SetText(MasterValueText, FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Settings->GetMasterVolume() * 100.0f)));
	if (MusicSlider) { MusicSlider->SetValue(Settings->GetMusicVolume()); }
	SetText(MusicValueText, FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Settings->GetMusicVolume() * 100.0f)));
	if (SFXSlider) { SFXSlider->SetValue(Settings->GetSFXVolume()); }
	SetText(SFXValueText, FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Settings->GetSFXVolume() * 100.0f)));

	if (UIScaleSlider) { UIScaleSlider->SetValue(Settings->GetUIScale()); }
	SetText(UIScaleValueText, FString::Printf(TEXT("%.2fx"), Settings->GetUIScale()));
	if (ReducedMotionCheck) { ReducedMotionCheck->SetIsChecked(Settings->IsReducedMotion()); }
}

void UFoundrySettingsMenu::HandleMasterVolumeChanged(float Value)
{
	if (bPopulating)
	{
		return;
	}
	UFoundryGameUserSettings* Settings = UFoundryGameUserSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}
	Settings->SetMasterVolume(Value);
	SetText(MasterValueText, FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Settings->GetMasterVolume() * 100.0f)));
	Settings->ApplyAudio(this);
}

void UFoundrySettingsMenu::HandleMusicVolumeChanged(float Value)
{
	if (bPopulating)
	{
		return;
	}
	UFoundryGameUserSettings* Settings = UFoundryGameUserSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}
	Settings->SetMusicVolume(Value);
	SetText(MusicValueText, FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Settings->GetMusicVolume() * 100.0f)));
	Settings->ApplyAudio(this);
}

void UFoundrySettingsMenu::HandleSFXVolumeChanged(float Value)
{
	if (bPopulating)
	{
		return;
	}
	UFoundryGameUserSettings* Settings = UFoundryGameUserSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}
	Settings->SetSFXVolume(Value);
	SetText(SFXValueText, FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Settings->GetSFXVolume() * 100.0f)));
	Settings->ApplyAudio(this);
}

void UFoundrySettingsMenu::HandleUIScaleChanged(float Value)
{
	if (bPopulating)
	{
		return;
	}
	UFoundryGameUserSettings* Settings = UFoundryGameUserSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}
	Settings->SetUIScale(Value);
	SetText(UIScaleValueText, FString::Printf(TEXT("%.2fx"), Settings->GetUIScale()));
}

void UFoundrySettingsMenu::HandleReducedMotionChanged(bool bIsChecked)
{
	if (bPopulating)
	{
		return;
	}
	UFoundryGameUserSettings* Settings = UFoundryGameUserSettings::Get();
	if (Settings == nullptr)
	{
		return;
	}
	Settings->SetReducedMotion(bIsChecked);
	Settings->SaveSettings();
}

void UFoundrySettingsMenu::HandleAudioSliderCaptureEnd()
{
	if (UFoundryGameUserSettings* Settings = UFoundryGameUserSettings::Get())
	{
		Settings->SaveSettings();
	}
}

void UFoundrySettingsMenu::UpdateSliderHover()
{
	const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
	auto ApplyHover = [&Palette](USlider* Slider)
	{
		if (Slider == nullptr)
		{
			return;
		}
		if (Slider->IsHovered())
		{
			Slider->SetSliderHandleColor(FoundryUITheme::CompositeOver(Palette.Primary, Palette.Text, 0.25f));
			Slider->SetSliderBarColor(FoundryUITheme::CompositeOver(Palette.SurfaceHigh, Palette.Primary, 0.35f));
		}
		else
		{
			Slider->SetSliderHandleColor(Palette.Primary);
			Slider->SetSliderBarColor(Palette.SurfaceHigh);
		}
	};
	ApplyHover(MasterSlider);
	ApplyHover(MusicSlider);
	ApplyHover(SFXSlider);
	ApplyHover(UIScaleSlider);
}

// -- Controls: Enhanced Input rebinding over UFoundryUISettings::RebindableContexts. --------------

void UFoundrySettingsMenu::PopulateControls()
{
	FScopedPopulating Guard(bPopulating);
	Show(ControlsSection, true);
	Show(ControlsNoteText, false);

	ULocalPlayer* LP = GetOwningLocalPlayer();
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	UEnhancedInputUserSettings* InputSettings = InputSubsystem ? InputSubsystem->GetUserSettings() : nullptr;
	const TArray<TSoftObjectPtr<UInputMappingContext>>& Contexts = UFoundryUISettings::Get()->RebindableContexts;

	if (InputSettings == nullptr || Contexts.Num() == 0)
	{
		SetText(ControlsNoteText, InputSettings == nullptr
			? TEXT("Enhanced Input user settings are disabled for this project.")
			: TEXT("This game has no rebindable controls."));
		Show(ControlsNoteText, true);
		Show(KeybindList, false);
		Show(ResetAllKeysButton, false);
		return;
	}

	for (const TSoftObjectPtr<UInputMappingContext>& SoftContext : Contexts)
	{
		if (UInputMappingContext* Context = SoftContext.LoadSynchronous())
		{
			InputSettings->RegisterInputMappingContext(Context); // idempotent - safe every open
		}
	}

	Show(KeybindList, true);
	Show(ResetAllKeysButton, true, true); // a button must be hit-testable, not just visible
	RebuildKeybindRows();
}

void UFoundrySettingsMenu::RebuildKeybindRows()
{
	if (KeybindList == nullptr)
	{
		return;
	}
	KeybindList->ClearChildren();

	ULocalPlayer* LP = GetOwningLocalPlayer();
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	UEnhancedInputUserSettings* InputSettings = InputSubsystem ? InputSubsystem->GetUserSettings() : nullptr;
	UEnhancedPlayerMappableKeyProfile* Profile = InputSettings ? InputSettings->GetActiveKeyProfile() : nullptr;
	if (Profile == nullptr)
	{
		return;
	}

	TSubclassOf<UFoundryKeybindRow> RowClass = KeybindRowClass;
	if (!RowClass)
	{
		RowClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultKeybindRow, TEXT("DefaultKeybindRow"));
	}
	if (!RowClass)
	{
		return;
	}

	const TMap<FName, FKeyMappingRow>& Rows = Profile->GetPlayerMappingRows();
	TArray<FName> SortedNames;
	Rows.GetKeys(SortedNames);
	SortedNames.Sort([&Rows](const FName& A, const FName& B)
	{
		auto DisplayOf = [&Rows](const FName& Name) -> FText
		{
			const FKeyMappingRow& Row = Rows[Name];
			return (Row.Mappings.Num() > 0) ? Row.Mappings.CreateConstIterator()->GetDisplayName() : FText::FromName(Name);
		};
		return DisplayOf(A).CompareTo(DisplayOf(B)) < 0;
	});

	for (const FName& MappingName : SortedNames)
	{
		const FKeyMappingRow& Row = Rows[MappingName];
		for (const FPlayerKeyMapping& Mapping : Row.Mappings)
		{
			if (Mapping.GetSlot() != EPlayerMappableKeySlot::First)
			{
				continue; // gamepad / other slots are v2
			}

			UFoundryKeybindRow* RowWidget = CreateWidget<UFoundryKeybindRow>(this, RowClass);
			if (RowWidget == nullptr)
			{
				continue;
			}
			FText DisplayName = Mapping.GetDisplayName();
			if (DisplayName.IsEmpty())
			{
				DisplayName = FText::FromString(FName::NameToDisplayString(MappingName.ToString(), false));
			}
			RowWidget->Setup(MappingName, DisplayName, Mapping.GetCurrentKey(), Mapping.IsCustomized());
			RowWidget->OnKeyChosen.AddDynamic(this, &UFoundrySettingsMenu::HandleKeyChosen);
			RowWidget->OnResetRequested.AddDynamic(this, &UFoundrySettingsMenu::HandleResetRequested);
			KeybindList->AddChild(RowWidget);
		}
	}

	// ClearChildren() above destroys whatever row the player was just capturing a key in (it called
	// us from inside its OWN key-down handler) - reclaim focus on the menu itself so Escape (and
	// anything else) still goes somewhere sane instead of nowhere.
	SetKeyboardFocus();
}

void UFoundrySettingsMenu::HandleKeyChosen(FName InMappingName, FKey Key)
{
	ULocalPlayer* LP = GetOwningLocalPlayer();
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	UEnhancedInputUserSettings* InputSettings = InputSubsystem ? InputSubsystem->GetUserSettings() : nullptr;
	if (InputSettings == nullptr)
	{
		return;
	}

	// Conflict check: refuse (rather than silently stomp) a key already mapped to a DIFFERENT
	// mapping. EnhancedInputUserSettings.h itself has no such guard in MapPlayerKey.
	if (UEnhancedPlayerMappableKeyProfile* Profile = InputSettings->GetActiveKeyProfile())
	{
		TArray<FName> ConflictingNames;
		Profile->GetMappingNamesForKey(Key, ConflictingNames);
		for (const FName& Conflict : ConflictingNames)
		{
			if (Conflict != InMappingName)
			{
				const FKeyMappingRow* ConflictRow = Profile->FindKeyMappingRow(Conflict);
				const FText ConflictDisplay = (ConflictRow && ConflictRow->Mappings.Num() > 0)
					? ConflictRow->Mappings.CreateConstIterator()->GetDisplayName()
					: FText::FromName(Conflict);
				ShowNote(FString::Printf(TEXT("Already used by %s"), *ConflictDisplay.ToString()));
				RebuildKeybindRows(); // restores the row's previous key
				return;
			}
		}
	}

	FMapPlayerKeyArgs Args;
	Args.MappingName = InMappingName;
	Args.Slot = EPlayerMappableKeySlot::First;
	Args.NewKey = Key;

	FGameplayTagContainer Failure;
	InputSettings->MapPlayerKey(Args, Failure);
	if (!Failure.IsEmpty())
	{
		ShowNote(TEXT("That key can't be used here."));
		RebuildKeybindRows(); // restores the row's previous key
		return;
	}

	InputSettings->ApplySettings();
	InputSettings->SaveSettings();
	RebuildKeybindRows();
}

void UFoundrySettingsMenu::HandleResetRequested(FName InMappingName)
{
	ULocalPlayer* LP = GetOwningLocalPlayer();
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	UEnhancedInputUserSettings* InputSettings = InputSubsystem ? InputSubsystem->GetUserSettings() : nullptr;
	if (InputSettings == nullptr)
	{
		return;
	}

	FMapPlayerKeyArgs Args;
	Args.MappingName = InMappingName;
	Args.Slot = EPlayerMappableKeySlot::First;
	FGameplayTagContainer Failure;
	InputSettings->ResetAllPlayerKeysInRow(Args, Failure);
	InputSettings->ApplySettings();
	InputSettings->SaveSettings();
	RebuildKeybindRows();
}

void UFoundrySettingsMenu::HandleResetAllKeysClicked()
{
	if (ActiveResetConfirm)
	{
		return; // a confirm is already up - a second click is a no-op
	}

	UFoundryConfirmDialog* Dialog = OpenConfirmDialog(
		FText::FromString(TEXT("Reset all controls?")),
		FText::FromString(TEXT("Every key goes back to its default.")),
		FText::FromString(TEXT("RESET")),
		FText::FromString(TEXT("CANCEL")),
		0.0f);
	if (Dialog == nullptr)
	{
		return; // OpenConfirmDialog already warned
	}

	ActiveResetConfirm = Dialog;
	Dialog->OnConfirmed.AddDynamic(this, &UFoundrySettingsMenu::HandleResetAllConfirmed);
	Dialog->OnCancelled.AddDynamic(this, &UFoundrySettingsMenu::HandleResetAllCancelled);
}

void UFoundrySettingsMenu::HandleResetAllConfirmed()
{
	ActiveResetConfirm = nullptr;
	PerformResetAllKeys();
}

void UFoundrySettingsMenu::HandleResetAllCancelled()
{
	ActiveResetConfirm = nullptr;
}

void UFoundrySettingsMenu::PerformResetAllKeys()
{
	ULocalPlayer* LP = GetOwningLocalPlayer();
	UEnhancedInputLocalPlayerSubsystem* InputSubsystem = LP ? LP->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>() : nullptr;
	UEnhancedInputUserSettings* InputSettings = InputSubsystem ? InputSubsystem->GetUserSettings() : nullptr;
	UEnhancedPlayerMappableKeyProfile* Profile = InputSettings ? InputSettings->GetActiveKeyProfile() : nullptr;
	if (InputSettings == nullptr || Profile == nullptr)
	{
		return;
	}

	for (const TPair<FName, FKeyMappingRow>& Pair : Profile->GetPlayerMappingRows())
	{
		FMapPlayerKeyArgs Args;
		Args.MappingName = Pair.Key;
		Args.Slot = EPlayerMappableKeySlot::First;
		FGameplayTagContainer Failure;
		InputSettings->ResetAllPlayerKeysInRow(Args, Failure);
	}
	InputSettings->ApplySettings();
	InputSettings->SaveSettings();
	RebuildKeybindRows();
}

// -- the right-hand description pane: Fortnite-style "hover a row, read about it here". -----------

void UFoundrySettingsMenu::BuildDescribedRows()
{
	auto Add = [this](UWidget* Row, const TCHAR* Key, const TCHAR* Title, const TCHAR* Body)
	{
		if (Row == nullptr)
		{
			return;
		}
		FDescribedRow Entry;
		Entry.Row = Row;
		Entry.Key = Key;
		Entry.Title = FText::FromString(Title);
		Entry.Body = FText::FromString(Body);
		if (Entry.Key == TEXT("ControlsRow"))
		{
			ControlsBodyCached = Entry.Body;
		}
		DescribedRows.Add(MoveTemp(Entry));
	};

	Add(WindowModeRow, TEXT("WindowModeRow"), TEXT("Window mode"),
		TEXT("Fullscreen gives the best performance. Windowed Fullscreen switches to other apps instantly."));
	Add(ResolutionRow, TEXT("ResolutionRow"), TEXT("Resolution"),
		TEXT("Your monitor's native resolution is sharpest. Lower it for more frames."));
	Add(QualityRow, TEXT("QualityRow"), TEXT("Quality"),
		TEXT("One preset for every scalability group. Custom shows when the groups differ."));
	Add(FpsCapRow, TEXT("FpsCapRow"), TEXT("FPS cap"),
		TEXT("Caps the frame rate so the GPU does not run flat out. Off = uncapped."));
	Add(VSyncRow, TEXT("VSyncRow"), TEXT("V-Sync"),
		TEXT("Removes tearing by syncing to your monitor; adds a little input delay."));
	Add(MasterRow, TEXT("MasterRow"), TEXT("Master volume"),
		TEXT("Controls every sound the game makes."));
	Add(MusicRow, TEXT("MusicRow"), TEXT("Music volume"),
		TEXT("Controls music only - sound effects are unaffected."));
	Add(SFXRow, TEXT("SFXRow"), TEXT("SFX volume"),
		TEXT("Controls sound effects only - music is unaffected."));
	Add(UIScaleRow, TEXT("UIScaleRow"), TEXT("UI scale"),
		TEXT("Makes every menu and HUD element bigger or smaller."));
	Add(ReducedMotionRow, TEXT("ReducedMotionRow"), TEXT("Reduced motion"),
		TEXT("Shortens or removes animated transitions."));
	Add(ControlsRow, TEXT("ControlsRow"), TEXT("Controls"),
		TEXT("Click a key to rebind it, then press the new key. Escape cancels. Reset puts a key back to its default."));
}

void UFoundrySettingsMenu::UpdateHoveredDescription()
{
	// A hovered KEYBIND row wins outright, with its own action name as the title - the body is
	// shared (the Controls row's own description): rebinding instructions are the same for every
	// mapping.
	if (KeybindList)
	{
		for (UWidget* Child : KeybindList->GetAllChildren())
		{
			if (UFoundryKeybindRow* Row = Cast<UFoundryKeybindRow>(Child))
			{
				if (Row->IsHovered())
				{
					ApplyDescription(Row, Row->GetDisplayName(), ControlsBodyCached, NAME_None);
					return;
				}
			}
		}
	}

	const FDescribedRow* HoveredEntry = nullptr;
	const FDescribedRow* FocusedEntry = nullptr;
	for (const FDescribedRow& Entry : DescribedRows)
	{
		UWidget* Row = Entry.Row.Get();
		if (Row == nullptr)
		{
			continue;
		}
		if (HoveredEntry == nullptr && Row->IsHovered())
		{
			HoveredEntry = &Entry;
		}
		if (FocusedEntry == nullptr && Row->HasFocusedDescendants())
		{
			FocusedEntry = &Entry;
		}
	}

	const FDescribedRow* Selected = HoveredEntry ? HoveredEntry : FocusedEntry;
	if (Selected)
	{
		ApplyDescription(Selected->Row.Get(), Selected->Title, Selected->Body, Selected->Key);
	}
	else
	{
		ApplyDescription(nullptr, FText::FromString(TEXT("Settings")),
			FText::FromString(TEXT("Hover a setting to read what it does.")), NAME_None);
	}
}

void UFoundrySettingsMenu::ApplyDescription(UWidget* SelectedWidget, const FText& Title, const FText& Body, FName PreviewKey)
{
	if (SelectedWidget == SelectedDescriptionWidget.Get())
	{
		return; // no change since last tick - the baked default pane already matches at first open
	}
	SelectedDescriptionWidget = SelectedWidget;
	SetText(DescriptionTitleText, Title.ToString());
	SetText(DescriptionBodyText, Body.ToString());

	if (PreviewImage)
	{
		const TSoftObjectPtr<UTexture2D>* Found = PreviewKey.IsNone() ? nullptr : Previews.Find(PreviewKey);
		UTexture2D* Tex = (Found && !Found->IsNull()) ? Found->LoadSynchronous() : nullptr;
		if (Tex)
		{
			PreviewImage->SetBrushFromTexture(Tex);
		}
		Show(PreviewImage, Tex != nullptr);
	}

	OnDescriptionChanged(Title);
}
