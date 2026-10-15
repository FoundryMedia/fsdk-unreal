#include "FoundryCreditRow.h"

#include "Components/TextBlock.h"

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
