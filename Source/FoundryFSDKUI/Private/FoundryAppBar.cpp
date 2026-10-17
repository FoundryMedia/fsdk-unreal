#include "FoundryAppBar.h"

#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "FoundryFSDKUILog.h"
#include "FoundryMenuHost.h"
#include "FoundrySocialSubsystem.h"
#include "FoundryUISettings.h"
#include "Misc/App.h"

UFoundrySocialSubsystem* UFoundryAppBar::Social() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetSubsystem<UFoundrySocialSubsystem>() : nullptr;
}

void UFoundryAppBar::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (SocialButton)
	{
		SocialButton->OnClicked.AddDynamic(this, &UFoundryAppBar::HandleSocial);
	}
	if (SettingsButton)
	{
		// Enabled when the game has a menu host (its own settings) or the kit has a settings menu to open.
		const bool bHasSettings = FindMenuHost() != nullptr || !UFoundryUISettings::Get()->DefaultSettingsMenu.IsNull();
		SettingsButton->SetIsEnabled(bHasSettings);
		SetKitToolTip(SettingsButton, FText::FromString(bHasSettings
			? TEXT("Settings")
			: TEXT("Settings menu is not configured (Project Settings -> Game -> Foundry UI).")));
		SettingsButton->OnClicked.AddDynamic(this, &UFoundryAppBar::HandleSettings);
	}
	// The base already ran its tooltip pass BEFORE the tooltip above existed - wrap it in the kit pill now.
	ApplyKitTooltips(this);
}

void UFoundryAppBar::NativeConstruct()
{
	Super::NativeConstruct();
	if (UFoundrySocialSubsystem* S = Social())
	{
		S->RegisterConsumer(); // the badge reads invites / pending requests - keep the poller alive for it
		S->OnChanged.AddUObject(this, &UFoundryAppBar::Refresh);
	}
	Refresh();
}

void UFoundryAppBar::NativeDestruct()
{
	if (UFoundrySocialSubsystem* S = Social())
	{
		S->OnChanged.RemoveAll(this);
		S->UnregisterConsumer();
	}
	Super::NativeDestruct();
}

void UFoundryAppBar::Refresh()
{
	const UFoundrySocialSubsystem* S = Social();
	const bool bIn = S && S->IsSignedIn();
	SetText(PlayerNameText, bIn ? Truncate(S->LocalDisplayName(), 22) : FString(TEXT("Not signed in")));
	if (PlayerAvatar)
	{
		PlayerAvatar->SetColorAndOpacity(GetAvatarTint(bIn));
	}
	SetText(TitleText, GetTitle().ToString());

	// Party invites + friend requests + (leader only - the list is empty for everyone else) the players
	// asking to join your party: all three wait on the player, so all three light the badge.
	const int32 N = S ? (S->Invites().Num() + S->PendingRequests() + S->JoinRequests().Num()) : 0;
	Show(SocialBadge, N > 0);
	SetText(SocialBadgeText, N > 9 ? FString(TEXT("9+")) : FString::FromInt(N));
	if (SocialButton)
	{
		SetKitToolTip(SocialButton, FText::FromString(N > 0
			? FString::Printf(TEXT("Social - %d new"), N)
			: FString(TEXT("Social"))));
	}
	OnIdentityUpdated(bIn);
}

void UFoundryAppBar::SetSocialOpen(bool bOpen)
{
	OnSocialOpenChanged(bOpen);
}

FLinearColor UFoundryAppBar::GetAvatarTint_Implementation(bool bSignedIn) const
{
	const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
	return bSignedIn ? Palette.Primary : Palette.TextSecondary;
}

FText UFoundryAppBar::GetTitle_Implementation() const
{
	const FText& Configured = UFoundryUISettings::Get()->AppBarTitle;
	return Configured.IsEmpty() ? FText::FromString(FApp::GetProjectName()) : Configured;
}

void UFoundryAppBar::HandleSocial()
{
	OnSocialClicked.Broadcast();
}

void UFoundryAppBar::HandleSettings()
{
	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestOpenSettings(Host);
	}
	else
	{
		// No host: open the kit's own settings menu over the viewport (what the pause menu's host-less
		// fallback does for its dialogs - AddToViewport above the menu layers).
		const TSubclassOf<UUserWidget> SettingsClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultSettingsMenu, TEXT("DefaultSettingsMenu"));
		if (SettingsClass)
		{
			if (UUserWidget* Menu = CreateWidget<UUserWidget>(GetOwningPlayer(), SettingsClass))
			{
				Menu->AddToViewport(20);
			}
		}
	}
	OnSettingsRequested.Broadcast();
}
