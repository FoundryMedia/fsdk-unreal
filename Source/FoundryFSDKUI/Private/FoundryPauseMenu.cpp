#include "FoundryPauseMenu.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "FoundryConfirmDialog.h"
#include "FoundryCreditsView.h"
#include "FoundryFSDKUILog.h"
#include "FoundryMenuHost.h"
#include "FoundryUISettings.h"
#include "Kismet/KismetSystemLibrary.h"

UFoundryPauseMenu::UFoundryPauseMenu()
{
	bRetakeFocusOnClick = true;
}

void UFoundryPauseMenu::SetContext(EFoundryPauseContext InContext)
{
	PauseContext = InContext;
	bContextExplicit = true;
	ApplyContextLabels();
}

void UFoundryPauseMenu::ApplyContextLabels()
{
	SetText(SettingsText, TEXT("SETTINGS"));
	SetText(AboutText, TEXT("ABOUT")); // same in both contexts
	switch (PauseContext)
	{
	case EFoundryPauseContext::Match:
		SetText(TitleText, TEXT("PAUSED"));
		SetText(PrimaryText, TEXT("LEAVE MATCH"));
		SetText(ResumeText, TEXT("RESUME"));
		break;
	case EFoundryPauseContext::Lobby:
	default:
		SetText(TitleText, TEXT("MENU"));
		SetText(PrimaryText, TEXT("QUIT TO DESKTOP"));
		SetText(ResumeText, TEXT("CANCEL"));
		break;
	}
	OnContextApplied(PauseContext);
}

void UFoundryPauseMenu::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (SettingsButton) { SettingsButton->OnClicked.AddDynamic(this, &UFoundryPauseMenu::HandleSettingsClicked); }
	if (AboutButton) { AboutButton->OnClicked.AddDynamic(this, &UFoundryPauseMenu::HandleAboutClicked); }
	if (PrimaryButton) { PrimaryButton->OnClicked.AddDynamic(this, &UFoundryPauseMenu::HandlePrimaryClicked); }
	if (ResumeButton) { ResumeButton->OnClicked.AddDynamic(this, &UFoundryPauseMenu::HandleResumeClicked); }
	SetIsFocusable(true);
}

void UFoundryPauseMenu::NativeConstruct()
{
	Super::NativeConstruct();
	if (!bContextExplicit)
	{
		if (UObject* Host = FindMenuHost())
		{
			PauseContext = IFoundryMenuHost::Execute_GetPauseContext(Host);
		}
		else
		{
			PauseContext = EFoundryPauseContext::Lobby;
		}
	}
	ApplyContextLabels();
	SetKeyboardFocus();
	PlayOrTween(MenuAnim, true, 0.18f, Panel ? Panel : this);
}

FReply UFoundryPauseMenu::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		HandleResumeClicked();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UFoundryPauseMenu::HandleSettingsClicked()
{
	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestOpenSettings(Host);
	}
	OnSettingsRequested.Broadcast();
}

void UFoundryPauseMenu::HandleAboutClicked()
{
	OnAboutRequested.Broadcast();

	const TSubclassOf<UUserWidget> Loaded = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultCreditsView, TEXT("DefaultCreditsView"));
	UClass* ViewClass = Loaded.Get();
	if (ViewClass == nullptr || !ViewClass->IsChildOf(UFoundryCreditsView::StaticClass()))
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s: ABOUT has no usable credits view class (DefaultCreditsView) - nothing to show."), *GetClass()->GetName());
		return;
	}

	UFoundryCreditsView* View = CreateWidget<UFoundryCreditsView>(GetOwningPlayer(), TSubclassOf<UFoundryCreditsView>(ViewClass));
	if (View == nullptr)
	{
		return;
	}

	// Its own layer over this menu (like the confirm dialog) - this menu stays underneath.
	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestPushMenu(Host, View);
	}
	else
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s has no menu host - adding the credits view to the viewport directly."), *GetClass()->GetName());
		View->AddToViewport(30);
	}
}

void UFoundryPauseMenu::HandleResumeClicked()
{
	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestPopMenu(Host);
	}
	else
	{
		RemoveFromParent();
	}
	OnResumeRequested.Broadcast();
}

void UFoundryPauseMenu::HandlePrimaryClicked()
{
	if (ActiveConfirm)
	{
		return; // a confirm is already up - a second click is a no-op
	}

	UFoundryConfirmDialog* Dialog = (PauseContext == EFoundryPauseContext::Match)
		? OpenConfirmDialog(
			FText::FromString(TEXT("Leave this match?")),
			FText::FromString(TEXT("Your team plays on without you. You return to the lobby.")),
			FText::FromString(TEXT("LEAVE MATCH")),
			FText::FromString(TEXT("STAY")),
			LeaveMatchCooldownSeconds, ConfirmDialogClass)
		: OpenConfirmDialog(
			FText::FromString(TEXT("Quit to desktop?")),
			FText::FromString(TEXT("The game closes now.")),
			FText::FromString(TEXT("QUIT")),
			FText::FromString(TEXT("CANCEL")),
			0.0f, ConfirmDialogClass);
	if (Dialog == nullptr)
	{
		return; // OpenConfirmDialog already warned
	}

	ActiveConfirm = Dialog;
	Dialog->OnConfirmed.AddDynamic(this, &UFoundryPauseMenu::HandleConfirmedPrimary);
	Dialog->OnCancelled.AddDynamic(this, &UFoundryPauseMenu::HandleConfirmCancelled);
}

void UFoundryPauseMenu::HandleConfirmedPrimary()
{
	const EFoundryPauseContext ConfirmedContext = PauseContext;
	ActiveConfirm = nullptr;
	OnPrimaryConfirmed.Broadcast(ConfirmedContext);

	if (ConfirmedContext == EFoundryPauseContext::Match)
	{
		// Through the shared helper so a party leader's leave also takes the party (post-horn only).
		if (!LeaveMatchViaHost())
		{
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s confirmed LEAVE MATCH with no menu host - nothing to leave to."), *GetClass()->GetName());
		}
	}
	else
	{
		UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
	}
}

void UFoundryPauseMenu::HandleConfirmCancelled()
{
	ActiveConfirm = nullptr;
}
