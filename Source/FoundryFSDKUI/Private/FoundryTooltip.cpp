#include "FoundryTooltip.h"

#include "Components/TextBlock.h"

void UFoundryTooltip::SetLabel(const FText& Label)
{
	SetText(LabelText, Label.ToString());
}
