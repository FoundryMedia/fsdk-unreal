// Copyright Foundry Media. FoundryFSDK default UI kit: the lobby app bar.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryAppBar.generated.h"

class UButton;
class UFoundrySocialSubsystem;
class UImage;
class UTextBlock;
class UWidget;

/**
 * The default lobby top bar: the title, the player chip (avatar tint + name), the social button with
 * its badge (real incoming party invites + friend requests, never party size) and the settings
 * button.
 *
 * Reads UFoundrySocialSubsystem only (IsSignedIn, LocalDisplayName, Invites, PendingRequests, OnChanged;
 * it registers as a social consumer for as long as it is constructed, so the badge keeps polling).
 * The HOST wires the two buttons' consequences: OnSocialClicked (toggle its social drawer, then call
 * SetSocialOpen so the button can show its selected state) and, optionally, OnSettingsRequested. The
 * Settings click itself already works: it asks the owning controller's IFoundryMenuHost to open the
 * game's settings, else opens UFoundryUISettings::DefaultSettingsMenu over the viewport; the button
 * is disabled (with a tooltip) when neither exists. A game widens the bar by subclassing it (Goo
 * Crew adds its nav tabs) and styles it in the WBP; the code only pushes text, visibility, enabled
 * and the avatar tint.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryAppBar : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	DECLARE_MULTICAST_DELEGATE(FFoundryAppBarEvent);

	/** The social button was pressed - the host toggles its drawer. */
	FFoundryAppBarEvent OnSocialClicked;

	/** The Settings button was pressed (after the host / fallback was asked to open settings). */
	FFoundryAppBarEvent OnSettingsRequested;

	/** Focus returns here when the drawer closes. */
	UButton* GetSocialButton() const { return SocialButton; }

	/** The host's drawer opened / closed - forwards to OnSocialOpenChanged. */
	void SetSocialOpen(bool bOpen);

	/** The drawer opened / closed: style the social button's selected state. */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnSocialOpenChanged(bool bOpen);

	/** Sign-in state or name changed (e.g. swap the avatar for a placeholder when signed out). */
	UFUNCTION(BlueprintImplementableEvent, Category = "Foundry UI")
	void OnIdentityUpdated(bool bSignedIn);

	/** PlayerAvatar's tint: Palette.Primary signed in, Palette.TextSecondary signed out. A game
	 *  overrides it for its own hero tint (Goo Crew: HeroPink). */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI")
	FLinearColor GetAvatarTint(bool bSignedIn) const;
	virtual FLinearColor GetAvatarTint_Implementation(bool bSignedIn) const;

	/** The text shown in TitleText: UFoundryUISettings::AppBarTitle, else the project name. */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI")
	FText GetTitle() const;
	virtual FText GetTitle_Implementation() const;

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual void NativeDestruct() override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UImage> PlayerAvatar;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> PlayerNameText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> SocialButton;
	/** Shown only when the count is > 0. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> SocialBadge;
	/** The badge count; "9+" above nine. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SocialBadgeText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> SettingsButton;

private:
	UFUNCTION() void HandleSocial();
	UFUNCTION() void HandleSettings();

	void Refresh();
	/** The ONE social poller (UFoundrySocialSubsystem) - identity and the badge count read it. */
	UFoundrySocialSubsystem* Social() const;
};
