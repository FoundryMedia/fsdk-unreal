#include "FoundryCreditRow.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "FoundryFSDKUILog.h"
#include "FoundryUISettings.h"
#include "HAL/PlatformProcess.h"

void UFoundryCreditRow::SetSectionHeader(const FText& Section)
{
	SetText(SectionText, Section.ToString());
	Show(SectionText, true);
	Show(NameText, false);
	Show(RoleText, false);
}

void UFoundryCreditRow::SetEntry(const FText& Name, const FText& Role)
{
	Show(SectionText, false);
	SetText(NameText, Name.ToString());
	Show(NameText, true);
	SetText(RoleText, Role.ToString());
	Show(RoleText, !Role.IsEmpty());
}

void UFoundryCreditRow::SetLinks(const TArray<FFoundryCreditLink>& Links)
{
	if (LinksBox)
	{
		LinksBox->ClearChildren();
	}
	LinkButtons.Reset();
	LinkUrls.Reset();
	if (Links.Num() == 0 || WidgetTree == nullptr)
	{
		return;
	}
	if (LinksBox == nullptr)
	{
		// No LinksBox in the WBP: append one to the row that holds the name / role.
		UPanelWidget* RowPanel = RoleText ? RoleText->GetParent() : (NameText ? NameText->GetParent() : nullptr);
		if (RowPanel == nullptr)
		{
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s has no LinksBox and no row panel - credit links are left out"), *GetClass()->GetName());
			return;
		}
		LinksBox = WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass());
		if (UHorizontalBoxSlot* BoxSlot = Cast<UHorizontalBoxSlot>(RowPanel->AddChild(LinksBox)))
		{
			BoxSlot->SetPadding(FMargin(12.0f, 0.0f, 0.0f, 0.0f));
			BoxSlot->SetVerticalAlignment(VAlign_Center);
		}
	}

	const UFoundryUISettings* Settings = UFoundryUISettings::Get();
	const FLinearColor Tint = Settings ? Settings->Palette.TextSecondary : FLinearColor(0.7f, 0.75f, 0.85f);
	for (const FFoundryCreditLink& Link : Links)
	{
		if (!Link.Url.StartsWith(TEXT("https://")))
		{
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] credit link '%s' is not https:// - left out"), *Link.Url);
			continue;
		}
		UTexture2D* IconTexture = Link.Icon.LoadSynchronous();
		if (IconTexture == nullptr)
		{
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] credit link %s: icon %s did not load - left out"), *Link.Url, *Link.Icon.ToString());
			continue;
		}
		UButton* Button = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass());
		FButtonStyle Style; // fresh brushes: no engine texture names, nothing drawn behind the icon
		FSlateBrush Clear;
		Clear.DrawAs = ESlateBrushDrawType::NoDrawType;
		Style.SetNormal(Clear).SetHovered(Clear).SetPressed(Clear).SetDisabled(Clear);
		Style.SetNormalPadding(FMargin(0.0f)).SetPressedPadding(FMargin(0.0f));
		Button->SetStyle(Style);
		Button->SetCursor(EMouseCursor::Hand);
		UImage* IconImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass());
		FSlateBrush IconBrush;
		IconBrush.DrawAs = ESlateBrushDrawType::Image;
		IconBrush.SetResourceObject(IconTexture);
		IconBrush.SetImageSize(FVector2D(22.0, 22.0));
		IconImage->SetBrush(IconBrush);
		IconImage->SetColorAndOpacity(Tint);
		Button->AddChild(IconImage);
		Button->OnClicked.AddDynamic(this, &UFoundryCreditRow::HandleLinkClicked);
		if (UHorizontalBoxSlot* ButtonSlot = Cast<UHorizontalBoxSlot>(LinksBox->AddChild(Button)))
		{
			ButtonSlot->SetPadding(FMargin(LinkButtons.Num() > 0 ? 10.0f : 0.0f, 0.0f, 0.0f, 0.0f));
			ButtonSlot->SetVerticalAlignment(VAlign_Center);
		}
		if (!Link.Label.IsEmpty())
		{
			SetKitToolTip(Button, FText::FromString(Link.Label));
		}
		LinkButtons.Add(Button);
		LinkUrls.Add(Link.Url);
	}
	Show(LinksBox, LinkButtons.Num() > 0, true);
}

void UFoundryCreditRow::HandleLinkClicked()
{
	// One handler for every link (a dynamic delegate carries no payload): the clicked button is the hovered or
	// focused one.
	for (int32 i = 0; i < LinkButtons.Num(); ++i)
	{
		if (LinkButtons[i] && (LinkButtons[i]->IsHovered() || LinkButtons[i]->HasKeyboardFocus()))
		{
			UE_LOG(LogFoundryFSDKUI, Log, TEXT("[UI] opening credit link %s"), *LinkUrls[i]);
			FPlatformProcess::LaunchURL(*LinkUrls[i], nullptr, nullptr);
			return;
		}
	}
}
