// Copyright Foundry Media. FoundryFSDK default UI kit: the credits view.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryCreditsView.generated.h"

class UButton;
class UPanelWidget;
class UTextBlock;
class UWidget;
class UWidgetAnimation;

/**
 * The default CREDITS view: a title, a scrolling list of section headers + name/role rows built from
 * UFoundryUISettings (CreditsTitle / Credits / CreditsFooter), and a close button. Pushed by the
 * pause menu's ABOUT button as its OWN menu layer (the pause menu stays underneath); closes through
 * the menu host (IFoundryMenuHost::RequestPopMenu), host-less it removes itself from the viewport.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryCreditsView : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Turns on bRetakeFocusOnClick (UFoundryUIWidgetBase) - a click on the backdrop still lets
	 *  Escape close this view afterward. */
	UFoundryCreditsView();

	/** (Re)builds the title, footer and rows from UFoundryUISettings. Called from NativeConstruct;
	 *  call it again to refresh after the settings change at runtime. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void Populate();

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeConstruct() override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Backdrop;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UWidget> Panel;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> TitleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> FooterText;
	/** The rows' parent (the generator emits a ScrollBox); rows are added with UPanelWidget::AddChild. */
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UPanelWidget> EntriesBox;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UButton> CloseButton;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> CloseText;

	/** Optional - an animation authored on this WBP's own Sequencer; unbound uses PlayOrTween's code
	 *  tween fallback (fade only). */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetAnimOptional))
	TObjectPtr<UWidgetAnimation> FadeAnim;

private:
	UFUNCTION() void HandleCloseClicked();

	static bool IsCloseKey(const FKey& Key);

	/** FindMenuHost() ? Execute_RequestPopMenu(host) : RemoveFromParent() + a WARN. */
	void Dismiss();

	// Escape in the preview AND bubble passes, plus a fast double click, must pop exactly one layer.
	bool bDismissed = false;
};
