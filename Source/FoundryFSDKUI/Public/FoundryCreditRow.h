// Copyright Foundry Media. FoundryFSDK default UI kit: one line of the credits view.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryCreditRow.generated.h"

class UTextBlock;

/**
 * One row of the credits view: either a SECTION HEADER (SectionText shown, Name/Role collapsed) or
 * an ENTRY (Name + Role shown, SectionText collapsed; an empty Role collapses RoleText). Pure
 * display - binds three optional text blocks by name and owns no look.
 */
UCLASS(Abstract)
class FOUNDRYFSDKUI_API UFoundryCreditRow : public UFoundryUIWidgetBase
{
	GENERATED_BODY()

public:
	/** Shows SectionText with Section; collapses NameText and RoleText. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void SetSectionHeader(const FText& Section);

	/** Shows NameText with Name and RoleText with Role (collapsed when Role is empty); collapses SectionText. */
	UFUNCTION(BlueprintCallable, Category = "Foundry UI")
	void SetEntry(const FText& Name, const FText& Role);

protected:
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SectionText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> NameText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RoleText;
};
