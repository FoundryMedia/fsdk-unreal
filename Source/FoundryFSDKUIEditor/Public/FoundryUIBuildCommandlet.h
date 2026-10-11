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
 * -run=FoundryUIBuild [-dump] [-out=/FoundryFSDK/UI] [-verifyicons] [-profile=<path to a JSON
 * file>] : builds the five default social-drawer Widget Blueprints (WBP_FoundrySocialDrawer /
 * WBP_FoundryFriendRow / WBP_FoundryInviteRow / WBP_FoundryFriendMenu / WBP_FoundryPartyNameplate)
 * under <out>/Social, and the five default menu Widget Blueprints (WBP_FoundryPauseMenu /
 * WBP_FoundryConfirmDialog / WBP_FoundrySettingsMenu / WBP_FoundryKeybindRow / WBP_FoundryTooltip)
 * under <out>/Menus, from the C++ spec trees in this module's .cpp, and imports the plugin's icon
 * set (Content/UI/Icons/src/*.png) into <out>/Icons (Texture Group UI, uncompressed TC_EditorIcon,
 * Mip Gen SimpleAverage, NeverStream, Trilinear filtering).
 *
 * Re-runnable: CreatePackage replaces an existing package's object of the same name cleanly; all
 * ten Blueprints and every icon texture are rebuilt from the spec / source PNGs each run, nothing
 * outside <out>/Social, <out>/Menus and <out>/Icons is touched. No animations are generated (the
 * WBPs' Sequencer tracks are left for the Designer pass); the parent classes' PlayOrTween
 * code-tween fallback covers their absence.
 *
 * -profile points at a JSON file ({out, prefix, drawerParent, nameplateParent, pauseParent,
 * confirmParent, settingsParent, keybindRowParent, menus{out,prefix}, palette{},
 * fonts{regular,bold,heading}, radii{panel,input,button}}, every field optional) that builds a
 * GAME's own skinned copies instead of the plugin's navy defaults: colours fall back to the plugin
 * palette per-key, fonts fall back to the engine Roboto per-face, radii fall back to 24/16/16 (the
 * navy kit's own values); "menus" (out/prefix for the pause menu, confirm dialog, settings menu,
 * keybind row and tooltip - the tooltip is never reparentable, like the friend row/invite
 * row/friend menu) falls back to the social out/prefix when absent, so a profile with no "menus"
 * object still gets all five built alongside the drawer. Prints the
 * [/Script/FoundryFSDKUI.FoundryUISettings] ini block (DefaultSocialDrawer through DefaultTooltip +
 * the resolved Palette) to paste into the project's DefaultGame.ini. With no -profile, output is
 * byte-identical to before -profile existed.
 *
 * -verifyicons loads every imported icon's MipGenSettings/LODGroup/NumMips/NeverStream/Filter/SRGB
 * and logs them, so a texture-import regression is provable without opening each asset by hand.
 *
 * With -dump, every tree is also printed (type, **bold** bound names, props, ASCII-indented - never
 * box-drawing glyphs) to the log, and written to TREES.md: <out>/Social/TREES.md gets ONLY the
 * social trees, <out>/Menus/TREES.md gets ONLY the menus trees (each resolved to its mounted disk
 * path when -profile names a game's own "out") - unless the two land in the SAME folder, in which
 * case that one file carries both sets, still a complete dump either way - so the plugin README
 * can paste it verbatim.
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
