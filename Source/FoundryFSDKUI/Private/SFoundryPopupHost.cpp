#include "SFoundryPopupHost.h"

#include "Framework/Application/SlateApplication.h"
#include "Layout/ArrangedChildren.h"
#include "Layout/LayoutUtils.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"

// ---------------------------------------------------------------- SFoundryTooltipPresenter

void SFoundryTooltipPresenter::Construct(const FArguments& InArgs)
{
	SetCanTick(false);
}

void SFoundryTooltipPresenter::SetContent(TSharedPtr<SWidget> InWidget)
{
	if (InWidget.IsValid())
	{
		ChildSlot.AttachWidget(InWidget.ToSharedRef());
	}
	else
	{
		ChildSlot.DetachWidget();
	}
	Invalidate(EInvalidateWidgetReason::Volatility);
}

bool SFoundryTooltipPresenter::ComputeVolatility() const
{
	return SPanel::ComputeVolatility() || ChildSlot.Num() > 0;
}

void SFoundryTooltipPresenter::OnArrangeChildren(const FGeometry& AllottedGeometry, FArrangedChildren& ArrangedChildren) const
{
	// STooltipPresenter::OnArrangeChildren, with the fit rect = the window this presenter is in
	// (its own allotted box is only the menu's).
	static const FVector2D CursorSize = FVector2D(12, 12);
	if (ChildSlot.Num() == 0)
	{
		return;
	}
	const TSharedRef<SWidget> TooltipWidget = ChildSlot.GetWidget();
	const FGeometry& Cached = GetCachedGeometry();
	const FVector2D LocalCursorPosition = Cached.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
	const FSlateRect CursorAnchorRect(LocalCursorPosition, LocalCursorPosition + CursorSize);
	const FSlateRect TooltipPopup(LocalCursorPosition + CursorSize, LocalCursorPosition + CursorSize + TooltipWidget->GetDesiredSize());

	FSlateRect FitRect(FVector2D::ZeroVector, AllottedGeometry.GetLocalSize());
	if (const TSharedPtr<SWindow> Window = FSlateApplication::Get().FindWidgetWindow(SharedThis(this)))
	{
		const FSlateRect WindowRect = Window->GetClientRectInScreen(); // desktop space, like the cursor
		FitRect = FSlateRect(Cached.AbsoluteToLocal(WindowRect.GetTopLeft()), Cached.AbsoluteToLocal(WindowRect.GetBottomRight()));
	}
	const FVector2D TooltipPosition = ComputePopupFitInRect(CursorAnchorRect, TooltipPopup, EOrientation::Orient_Vertical, FitRect);
	const FVector2D TooltipPositionRounded = AllottedGeometry.LocalToRoundedLocal(TooltipPosition);
	ArrangedChildren.AddWidget(AllottedGeometry.MakeChild(TooltipWidget, TooltipWidget->GetDesiredSize(), FSlateLayoutTransform(TooltipPositionRounded)));
}

FVector2D SFoundryTooltipPresenter::ComputeDesiredSize(float) const
{
	return FVector2D::ZeroVector; // never grows the menu it sits over
}

FChildren* SFoundryTooltipPresenter::GetChildren()
{
	return &ChildSlot;
}

// ---------------------------------------------------------------- SFoundryPopupHost

void SFoundryPopupHost::Construct(const FArguments& InArgs)
{
	ViewportClient = InArgs._ViewportClient;
	ChildSlot
	[
		SNew(SOverlay)
		+ SOverlay::Slot()
		[
			InArgs._Content.Widget
		]
		+ SOverlay::Slot()
		[
			SAssignNew(Presenter, SFoundryTooltipPresenter)
		]
	];
}

bool SFoundryPopupHost::OnVisualizeTooltip(const TSharedPtr<SWidget>& TooltipContent)
{
	if (Presenter.IsValid())
	{
		Presenter->SetContent(TooltipContent); // null = Slate says "nothing" (the hovered widget changed)
		return true;
	}
	return false;
}

void SFoundryPopupHost::OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	SCompoundWidget::OnMouseEnter(MyGeometry, MouseEvent);
	if (UGameViewportClient* GVC = ViewportClient.Get())
	{
		// The viewport widget's own enter path: re-applies the project's hardware cursor shapes that
		// PIE reset on the SViewport's MouseLeave (which fired when the cursor reached this menu).
		const FVector2D Pos = MouseEvent.GetScreenSpacePosition();
		GVC->MouseEnter(GVC->Viewport, FMath::RoundToInt(Pos.X), FMath::RoundToInt(Pos.Y));
	}
}
