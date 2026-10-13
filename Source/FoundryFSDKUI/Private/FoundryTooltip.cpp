#include "FoundryTooltip.h"

#include "Components/TextBlock.h"
#include "FoundryFSDKUILog.h"

void UFoundryTooltip::SetLabel(const FText& Label)
{
	SetText(LabelText, Label.ToString());
}

void UFoundryTooltip::NativeConstruct()
{
	Super::NativeConstruct();
	UE_LOG(LogFoundryFSDKUI, Verbose, TEXT("[Tooltip] pill %s construct"), *GetName());
}

void UFoundryTooltip::NativeDestruct()
{
	UE_LOG(LogFoundryFSDKUI, Verbose, TEXT("[Tooltip] pill %s destruct"), *GetName());
	Super::NativeDestruct();
}
