#pragma once

#include "CoreMinimal.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SPanel.h"

class SWindow;

/**
 * Presents one tooltip at the cursor, like STooltipPresenter, but fitted against the WINDOW the
 * presenter lives in rather than its own allotted box - so a presenter the size of a context menu
 * can still hang the pill outside the menu.
 */
class SFoundryTooltipPresenter : public SPanel
{
public:
	SLATE_BEGIN_ARGS(SFoundryTooltipPresenter)
	{
		_Visibility = EVisibility::HitTestInvisible;
	}
	SLATE_END_ARGS()

	SFoundryTooltipPresenter() : ChildSlot(this) { bCanSupportFocus = false; }
	void Construct(const FArguments& InArgs);
	void SetContent(TSharedPtr<SWidget> InWidget);

private:
	virtual bool ComputeVolatility() const override;
	virtual void OnArrangeChildren(const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren) const override;
	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual FChildren* GetChildren() override;

	TWeakChild<SWidget> ChildSlot;
};

/**
 * Wraps a kit menu's content before FSlateApplication::PushMenu. A pushed menu lives in the menu
 * stack's SMenuPanel, in the game WINDOW's overlay - outside the SGameLayerManager subtree - which
 * costs two things the viewport gives every other widget:
 *  - tooltips: nothing in that path answers OnVisualizeTooltip, so FSlateUser::UpdateTooltip falls
 *    back to a real tooltip WINDOW (SWindow::MakeToolTipWindow = per-WINDOW transparency, so the
 *    pill's rounded corners show the window's dark rectangle, plus an opacity fade that flickers
 *    while the cursor moves - 2026-10-13). This host visualizes the tooltip itself, in an overlay
 *    slot ABOVE its own content: a later sibling in the same subtree paints after the menu, whatever
 *    the invalidation state of the panels around it (a presenter slot appended to the window overlay
 *    and one appended to the SMenuPanel both painted UNDER the menu). The presenter fits the pill
 *    against the window, not the menu box, and Slate does not clip it (no ancestor clips).
 *  - the hardware cursor: in PIE, UGameViewportClient::MouseLeave (WITH_EDITOR) resets the platform
 *    cursor shapes the moment the mouse leaves the SViewport - i.e. the moment it reaches the menu -
 *    so the OS arrow shows over it (a packaged game keeps the shapes). This host re-applies them on
 *    mouse enter through the same public MouseEnter the viewport widget calls.
 */
class SFoundryPopupHost : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFoundryPopupHost) : _ViewportClient(nullptr) {}
		SLATE_DEFAULT_SLOT(FArguments, Content)
		SLATE_ARGUMENT(UGameViewportClient*, ViewportClient)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	virtual bool OnVisualizeTooltip(const TSharedPtr<SWidget>& TooltipContent) override;
	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

private:
	TWeakObjectPtr<UGameViewportClient> ViewportClient;
	TSharedPtr<SFoundryTooltipPresenter> Presenter;
};
