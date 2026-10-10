// Copyright Foundry Media. FoundryUIBuild commandlet: generates the default UI kit's Widget
// Blueprints under /FoundryFSDK/UI/Social and imports its icon set.

#pragma once

#include "Commandlets/Commandlet.h"
#include "CoreMinimal.h"
#include "FoundryUIBuildCommandlet.generated.h"

/**
 * One node of a generated Widget Blueprint's tree: a UMG class name (Type - "Border", "TextBlock",
 * "Button", "Image", "VerticalBox", "HorizontalBox", "Overlay", "SizeBox", "ScrollBox", "Spacer",
 * "EditableTextBox"), an optional bound variable name (Name; bIsVariable follows from whether a
 * name was given), a flat bag of string props interpreted by BuildWidget/the child-attach loop in
 * FoundryUIBuildCommandlet.cpp (see the key table at the top of that file), and its children in
 * build order. A node's own HAlign / VAlign / SlotPad / Size / FillValue / Collapsed props describe
 * how IT sits in ITS PARENT (read by the parent when attaching it); everything else describes the
 * node's own rendering. A content-widget's padding toward its single child (Border, Button,
 * SizeBox) is therefore expressed as that CHILD's own SlotPad - there is no separate "own padding"
 * key.
 */
struct FFoundryWidgetSpec
{
	FName Type;
	FName Name = NAME_None;
	bool bIsVariable = false;
	TMap<FName, FString> Props;
	TArray<FFoundryWidgetSpec> Children;

	FFoundryWidgetSpec& P(const TCHAR* Key, const FString& Value) { Props.Add(Key, Value); return *this; }
	FFoundryWidgetSpec& P(const TCHAR* Key, float Value) { Props.Add(Key, FString::SanitizeFloat(Value)); return *this; }
	FFoundryWidgetSpec& C(FFoundryWidgetSpec Child) { Children.Add(MoveTemp(Child)); return *this; }
};

/**
 * -run=FoundryUIBuild [-dump] [-out=/FoundryFSDK/UI] : builds the four default social-drawer Widget
 * Blueprints (WBP_FoundrySocialDrawer / WBP_FoundryFriendRow / WBP_FoundryInviteRow /
 * WBP_FoundryFriendMenu) under <out>/Social from the C++ spec trees in this module's .cpp, and
 * imports the plugin's icon set (Content/UI/Icons/src/*.png) into <out>/Icons (Texture Group UI,
 * uncompressed TC_EditorIcon, Mip Gen FromTextureGroup).
 *
 * Re-runnable: CreatePackage replaces an existing package's object of the same name cleanly; the
 * four Blueprints and every icon texture are rebuilt from the spec / source PNGs each run, nothing
 * outside <out>/Social and <out>/Icons is touched. No animations are generated (the WBPs' Sequencer
 * tracks are left for the Designer pass); the parent classes' PlayOrTween code-tween fallback covers
 * their absence.
 *
 * With -dump, every tree is also printed (type, **bold** bound names, props, ASCII-indented - never
 * box-drawing glyphs) to the log and to Content/UI/Social/TREES.md, so the plugin README can paste
 * it verbatim.
 *
 * Must not run while the editor (UnrealEditor.exe) has the project open - see
 * .claude/rules/unreal-plugin-conventions.md "Code-built materials in a commandlet".
 */
UCLASS()
class UFoundryUIBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UFoundryUIBuildCommandlet();
	virtual int32 Main(const FString& Params) override;
};
