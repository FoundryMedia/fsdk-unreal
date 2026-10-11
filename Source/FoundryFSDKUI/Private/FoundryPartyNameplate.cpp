#include "FoundryPartyNameplate.h"

#include "Components/TextBlock.h"
#include "FoundryUISettings.h"

void UFoundryPartyNameplate::Setup(const FString& Name, bool bLeader, bool bReady, bool bLocal)
{
	SetText(NameText, Name);
	if (NameText)
	{
		NameText->SetColorAndOpacity(FSlateColor(GetNameTint(bLocal)));
	}
	Show(CrownIcon, bLeader);
	Show(HostBadge, bLeader);
	Show(ReadyIcon, bReady);
	OnPlateUpdated();
}

FLinearColor UFoundryPartyNameplate::GetNameTint_Implementation(bool bLocal) const
{
	const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
	return bLocal ? Palette.Primary : Palette.Text;
}
