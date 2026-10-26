// Copyright Foundry Media. FoundryFSDK default UI kit: one line of the credits view.

#pragma once

#include "CoreMinimal.h"
#include "FoundryUIWidgetBase.h"
#include "FoundryCreditRow.generated.h"

class UTextBlock;
class UButton;
class UHorizontalBox;
struct FFoundryCreditLink;

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

	/** Icon buttons after the role, one per link (opens the URL in the browser). Built into LinksBox when the WBP has
	 *  one, else into a box appended to RoleText's parent row. Replaces any previous links. */
	void SetLinks(const TArray<FFoundryCreditLink>& Links);

protected:
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> SectionText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> NameText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UTextBlock> RoleText;
	UPROPERTY(BlueprintReadOnly, Category = "Foundry UI", meta = (BindWidgetOptional)) TObjectPtr<UHorizontalBox> LinksBox;

private:
	UFUNCTION()
	void HandleLinkClicked();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UButton>> LinkButtons;
	TArray<FString> LinkUrls;
};
