#include "FoundryCreditsView.h"

#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "FoundryCreditRow.h"
#include "FoundryFSDKUILog.h"
#include "FoundryMenuHost.h"
#include "FoundryUISettings.h"
#include "InputCoreTypes.h"

UFoundryCreditsView::UFoundryCreditsView()
{
	bRetakeFocusOnClick = true;
}

void UFoundryCreditsView::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (CloseButton) { CloseButton->OnClicked.AddDynamic(this, &UFoundryCreditsView::HandleCloseClicked); }
	SetText(CloseText, TEXT("CLOSE"));
	SetIsFocusable(true);
}

void UFoundryCreditsView::NativeConstruct()
{
	Super::NativeConstruct();
	bDismissed = false;
	Populate();
	SetKeyboardFocus();
	PlayOrTween(FadeAnim, true, 0.18f, Panel ? Panel : this);
}

void UFoundryCreditsView::Populate()
{
	const UFoundryUISettings* Settings = UFoundryUISettings::Get();
	if (Settings == nullptr)
	{
		return;
	}

	SetText(TitleText, Settings->CreditsTitle.ToString());
	SetText(FooterText, Settings->CreditsFooter.ToString());

	if (EntriesBox == nullptr)
	{
		return;
	}
	EntriesBox->ClearChildren();

	UClass* LoadedClass = UFoundryUISettings::LoadWidgetClass(Settings->DefaultCreditRow, TEXT("DefaultCreditRow")).Get();
	if (LoadedClass == nullptr || !LoadedClass->IsChildOf(UFoundryCreditRow::StaticClass()))
	{
		if (LoadedClass != nullptr)
		{
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s: DefaultCreditRow (%s) is not a UFoundryCreditRow - the credits list stays empty."), *GetClass()->GetName(), *LoadedClass->GetName());
		}
		else
		{
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s has no credit row class - the credits list stays empty."), *GetClass()->GetName());
		}
		return;
	}
	const TSubclassOf<UFoundryCreditRow> RowClass(LoadedClass);

	// Sections in order of first appearance; each entry keeps its position inside its section.
	TArray<FString> SectionOrder;
	for (const FFoundryCreditEntry& Entry : Settings->Credits)
	{
		SectionOrder.AddUnique(Entry.Section);
	}

	for (const FString& Section : SectionOrder)
	{
		if (!Section.IsEmpty())
		{
			if (UFoundryCreditRow* Header = CreateWidget<UFoundryCreditRow>(this, RowClass))
			{
				Header->SetSectionHeader(FText::FromString(Section));
				EntriesBox->AddChild(Header);
			}
		}
		for (const FFoundryCreditEntry& Entry : Settings->Credits)
		{
			if (Entry.Section != Section)
			{
				continue;
			}
			if (UFoundryCreditRow* Row = CreateWidget<UFoundryCreditRow>(this, RowClass))
			{
				Row->SetEntry(FText::FromString(Entry.Name), FText::FromString(Entry.Role));
				Row->SetLinks(Entry.Links);
				EntriesBox->AddChild(Row);
			}
		}
	}
}

bool UFoundryCreditsView::IsCloseKey(const FKey& Key)
{
	return Key == EKeys::Escape || Key == EKeys::Gamepad_Special_Right || Key == EKeys::Gamepad_Special_Left;
}

FReply UFoundryCreditsView::NativeOnPreviewKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	// Tunnels before the bubble phase, so a focused descendant (the scroll box) cannot swallow it.
	if (IsCloseKey(InKeyEvent.GetKey()))
	{
		HandleCloseClicked();
		return FReply::Handled();
	}
	return Super::NativeOnPreviewKeyDown(InGeometry, InKeyEvent);
}

FReply UFoundryCreditsView::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (IsCloseKey(InKeyEvent.GetKey()))
	{
		HandleCloseClicked();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UFoundryCreditsView::Dismiss()
{
	if (UObject* Host = FindMenuHost())
	{
		IFoundryMenuHost::Execute_RequestPopMenu(Host);
	}
	else
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s closed with no menu host - removing itself from the viewport."), *GetClass()->GetName());
		RemoveFromParent();
	}
}

void UFoundryCreditsView::HandleCloseClicked()
{
	if (bDismissed)
	{
		return;
	}
	bDismissed = true;
	Dismiss();
}
