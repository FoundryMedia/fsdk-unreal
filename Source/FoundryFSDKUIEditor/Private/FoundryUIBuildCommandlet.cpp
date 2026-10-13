// Copyright Foundry Media. FoundryUIBuild commandlet implementation.
//
// Prop key table (interpreted by BuildWidget / the child-attach loop below):
//   Slot props (read by the PARENT when attaching this node):
//     HAlign       Fill|Left|Center|Right   (default: the parent's ChildHAlign, else Fill)
//     VAlign       Fill|Top|Center|Bottom   (default: the parent's ChildVAlign, else Fill)
//     SlotPad      "L,T,R,B" or a single "N" (all sides)
//     Size         Fill|Auto   (VerticalBox/HorizontalBox slots only; default Auto)
//     FillValue    float (Size=Fill only; default 1)
//     Collapsed    "1" -> initial Visibility Collapsed, else Visible
//     Clipping     "1" -> SetClipping(ClipToBounds) (any widget type; default Inherit)
//     Pivot        "X,Y" -> SetRenderTransformPivot (any widget type; default unset, engine 0.5,0.5).
//                   Set this on anything a runtime TweenAngle/SetRenderTransformAngle call will spin
//                   in place (e.g. a section chevron) - the engine default already happens to be
//                   0.5,0.5, so this is belt-and-suspenders, not load-bearing.
//   Container props (read for EACH child that doesn't give its own HAlign/VAlign):
//     ChildHAlign, ChildVAlign
//   Content props (per node Type):
//     SizeBox:          W, H (float overrides; axis omitted = no override), MaxH
//                        (SetMaxDesiredHeight - caps height while letting content size smaller)
//     Border:           Fill (palette token), FillAlpha (float override of the Fill token's own
//                        alpha; default: the token's own alpha, usually 1 - e.g. a modal backdrop),
//                        Radius (float) or HalfHeight="1", OutlineColor, OutlineWidth (default:
//                        OutlineColor=Fill at alpha 0, OutlineWidth=0), RadiusRole "Panel"|"Input"
//                        (default: Panel) - which -profile radii.* value a node with NO explicit
//                        Radius falls back to
//     Button:            Normal, Hovered, Pressed, Disabled (palette token or "Transparent";
//                        unspecified state defaults to the previous one, so the style is still
//                        explicitly assigned on all four fields), Radius/HalfHeight as Border (an
//                        absent Radius falls back to -profile radii.button)
//     Image (icon):      Icon (basename under Content/UI/Icons/src, no extension), IconW, IconH
//                        (default 24 each), Tint (default: no override, stays white - the runtime
//                        tints it, e.g. Avatar)
//     Image (shape):      Fill, Radius/HalfHeight, OutlineColor, OutlineWidth, W, H - same as
//                        Border's brush, used when no Icon is given (e.g. PresenceDot)
//     TextBlock:         Text (literal; empty for a field the runtime fills), FontSize, Bold="1",
//                        Color, Wrap="1", FontRole="Heading" (default: Bold/Regular per Bold) -
//                        which -profile fonts.* face this node resolves to (ResolveFont)
//     EditableTextBox:   Hint, FontSize, Color (backgrounds are always fully transparent; always
//                        resolves to -profile fonts.regular)
//     ComboBoxString:    Fill (default SurfaceHigh), Radius (default radii.input), MenuFill (the
//                        open-list panel, default SurfaceHighest) - item text is Color (default
//                        Text), FontSize (default 14); items are runtime-filled (no Items prop).
//                        Hovered/Pressed are Primary at 0.35 alpha composited over Fill (a second
//                        brush, never a flat colour) so hover reads as "the same control, lit up".
//                        ContentPadding is always (18, 9) (SetContentPadding - the only runtime
//                        setter the engine gives it). The popup's border/arrow/row-hover brushes are
//                        always rebuilt nameless (no stock engine texture survives) - see BuildWidget.
//     CheckBox:          W, H (default 22 each), Radius (default 6) - unchecked/checked images are
//                        rounded boxes (palette SurfaceHigh / Primary); no engine texture. Hovered
//                        is each resting colour composited with Primary (unchecked, 0.35) / Text
//                        (checked, 0.2) via FoundryUITheme::CompositeOver.
//     Slider:            BarColor, HandleColor (palette tokens, default SurfaceHigh / Primary),
//                        MinValue, MaxValue (default 0/1), StepSize (default 0.01). The thumb
//                        images (stock "Common/Button" by default) are always rebuilt as a plain
//                        white rounded shape (16x16, HalfHeight) - HandleColor tints it at runtime.
//     ScrollBox:         BarThickness (float)
//     Spacer:            SpacerSize (float, square)

#include "FoundryUIBuildCommandlet.h"

#include "AssetImportTask.h"
#include "Engine/Font.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CheckBox.h"
#include "Components/ComboBoxString.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Image.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/ScrollBox.h"
#include "Components/ScrollBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/Slider.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Factories/TextureFactory.h"
#include "FoundryConfirmDialog.h"
#include "FoundryFriendMenu.h"
#include "FoundryFriendRow.h"
#include "FoundryInviteRow.h"
#include "FoundryKeybindRow.h"
#include "FoundryPartyNameplate.h"
#include "FoundryPauseMenu.h"
#include "FoundrySettingsMenu.h"
#include "FoundrySocialDrawer.h"
#include "FoundryTooltip.h"
#include "FoundryUISettings.h"
#include "FoundryUITheme.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "TextureCompiler.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "WidgetBlueprint.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoundryUIBuild, Log, All);

namespace
{
	// -- small parse helpers --------------------------------------------------------------------

	FMargin ParseMargin(const FString& S)
	{
		if (S.IsEmpty())
		{
			return FMargin(0.0f);
		}
		TArray<FString> Parts;
		S.ParseIntoArray(Parts, TEXT(","), true);
		if (Parts.Num() == 1)
		{
			return FMargin(FCString::Atof(*Parts[0]));
		}
		if (Parts.Num() == 4)
		{
			return FMargin(FCString::Atof(*Parts[0]), FCString::Atof(*Parts[1]), FCString::Atof(*Parts[2]), FCString::Atof(*Parts[3]));
		}
		UE_LOG(LogFoundryUIBuild, Warning, TEXT("bad margin '%s' - using 0"), *S);
		return FMargin(0.0f);
	}

	EHorizontalAlignment ParseHAlign(const FString& S)
	{
		if (S == TEXT("Left")) { return HAlign_Left; }
		if (S == TEXT("Center")) { return HAlign_Center; }
		if (S == TEXT("Right")) { return HAlign_Right; }
		return HAlign_Fill;
	}

	EVerticalAlignment ParseVAlign(const FString& S)
	{
		if (S == TEXT("Top")) { return VAlign_Top; }
		if (S == TEXT("Center")) { return VAlign_Center; }
		if (S == TEXT("Bottom")) { return VAlign_Bottom; }
		return VAlign_Fill;
	}

	FLinearColor TokenColor(const FFoundryUIPalette& P, const FString& Token)
	{
		if (Token == TEXT("White")) { return FLinearColor::White; }
		if (Token == TEXT("Transparent")) { return FLinearColor(0.0f, 0.0f, 0.0f, 0.0f); }
		if (Token == TEXT("Surface")) { return P.Surface; }
		if (Token == TEXT("SurfaceLow")) { return P.SurfaceLow; }
		if (Token == TEXT("SurfaceHigh")) { return P.SurfaceHigh; }
		if (Token == TEXT("SurfaceHighest")) { return P.SurfaceHighest; }
		if (Token == TEXT("Primary")) { return P.Primary; }
		if (Token == TEXT("PrimaryContainer")) { return P.PrimaryContainer; }
		if (Token == TEXT("Secondary")) { return P.Secondary; }
		if (Token == TEXT("Tertiary")) { return P.Tertiary; }
		if (Token == TEXT("Text")) { return P.Text; }
		if (Token == TEXT("TextSecondary")) { return P.TextSecondary; }
		if (Token == TEXT("Outline")) { return P.Outline; }
		if (Token == TEXT("Online")) { return P.Online; }
		if (Token == TEXT("Idle")) { return P.Idle; }
		if (Token == TEXT("Error")) { return P.Error; }
		if (Token == TEXT("HostText")) { return P.HostText; }
		UE_LOG(LogFoundryUIBuild, Warning, TEXT("unknown palette token '%s' - defaulting to Text"), *Token);
		return P.Text;
	}

	// CompositeOver (the kit's hover-tint formula) lives in FoundryUITheme.h/.cpp now - shared with
	// runtime code (the settings menu's slider hover) so there is exactly one formula, not two.

	FSlateBrush MakeRoundedBrush(const FLinearColor& Fill, bool bHalfHeight, float Radius, const FLinearColor& OutlineColor, float OutlineWidth)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Fill);
		Brush.OutlineSettings.RoundingType = bHalfHeight ? ESlateBrushRoundingType::HalfHeightRadius : ESlateBrushRoundingType::FixedRadius;
		Brush.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
		Brush.OutlineSettings.Color = FSlateColor(OutlineColor);
		Brush.OutlineSettings.Width = OutlineWidth;
		return Brush;
	}

	FSlateBrush MakeIconBrush(UTexture2D* Tex, float W, float H, const FLinearColor& Tint)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::Image;
		Brush.ImageSize = FVector2D(W, H);
		Brush.TintColor = FSlateColor(Tint);
		if (Tex)
		{
			Brush.SetResourceObject(Tex);
		}
		return Brush;
	}

	// The engine's Roboto UFont ASSET, never FCoreStyle::GetDefaultFontStyle: that one carries a transient
	// composite font that does not survive SavePackage, so every TextBlock loads with no font and renders
	// tofu (the 2026-10-09 first run). UTextBlock's own default is this same asset + a typeface name.
	FSlateFontInfo RobotoFont(float Size, bool bBold)
	{
		static UFont* Roboto = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto"));
		if (!Roboto)
		{
			UE_LOG(LogTemp, Error, TEXT("[FoundryUIBuild] /Engine/EngineFonts/Roboto failed to load - text will render tofu"));
		}
		return FSlateFontInfo(Roboto, FMath::RoundToInt(Size), bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")));
	}

	// -- -profile support: a game gets its OWN skinned copies without forking the generator --------
	//
	// Every field is OPTIONAL and additive over the plugin's own defaults (navy Deep Sapphire,
	// engine Roboto, the kit's own 16/16/24 radii) - an absent key in the profile JSON changes
	// nothing, so a project only has to say what it wants to differ.

	/** A radius a project can re-dial without touching individual node specs: the outer drawer
	 *  shell ("panel"), the search-box frame ("input"), and any Button node that doesn't set its
	 *  own explicit Radius/HalfHeight ("button"). Defaults match the navy kit's current literals
	 *  exactly, so building with NO profile is byte-identical to before this existed. */
	struct FFoundryUIRadii
	{
		float Panel = 24.0f;
		float Input = 16.0f;
		float Button = 16.0f;
	};

	/** One named UFont asset + the typeface name to pull from its font family (e.g.
	 *  "/Game/UI/Fonts/F_NunitoSans", "Bold"). An empty AssetPath means "use the engine Roboto" -
	 *  ResolveFont below falls back to it field-by-field, so a profile can override just the
	 *  heading face and leave body/bold on Roboto if it wants. */
	struct FFoundryUIFontRef
	{
		FString AssetPath;
		FString Face = TEXT("Regular");
	};

	struct FFoundryUIFonts
	{
		FFoundryUIFontRef Regular;
		FFoundryUIFontRef Bold;
		/** Section headers / the drawer's own title row ("FRIENDS") / "YOUR PARTY" / "PARTY
		 *  INVITES" - anything a TextBlock node tags .P("FontRole","Heading"). Falls back to Bold,
		 *  then Regular, then Roboto when unset. */
		FFoundryUIFontRef Heading;
	};

	/** Resolves a TextBlock/EditableTextBox font: Heading (when bHeading and set) -> Bold (when
	 *  bBold/bHeading and set) -> Regular (when set) -> the engine Roboto, exactly as before any
	 *  profile existed. A font asset that fails to load logs and falls through the same chain. */
	FSlateFontInfo ResolveFont(const FFoundryUIFonts& Fonts, float Size, bool bBold, bool bHeading)
	{
		const FFoundryUIFontRef* Ref = nullptr;
		if (bHeading && !Fonts.Heading.AssetPath.IsEmpty())
		{
			Ref = &Fonts.Heading;
		}
		else if ((bBold || bHeading) && !Fonts.Bold.AssetPath.IsEmpty())
		{
			Ref = &Fonts.Bold;
		}
		else if (!bBold && !Fonts.Regular.AssetPath.IsEmpty())
		{
			Ref = &Fonts.Regular;
		}

		if (Ref != nullptr)
		{
			UFont* Loaded = LoadObject<UFont>(nullptr, *Ref->AssetPath);
			if (Loaded != nullptr)
			{
				return FSlateFontInfo(Loaded, FMath::RoundToInt(Size), FName(*Ref->Face));
			}
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile font '%s' failed to load - falling back to Roboto"), *Ref->AssetPath);
		}
		return RobotoFont(Size, bBold || bHeading);
	}

	FLinearColor ParseHexColor(const FString& HexStr)
	{
		return FLinearColor::FromSRGBColor(FColor::FromHex(HexStr));
	}

	struct FFoundryUIProfile
	{
		/** The Social WBPs' destination, e.g. "/Game/UI/Lobby" - used AS the social path directly
		 *  (unlike the legacy -out switch, which appends "/Social" to whatever it's given). Empty
		 *  means "use the default (/FoundryFSDK/UI/Social)". */
		FString Out;
		/** e.g. "WBP_Lobby" -> WBP_LobbyDrawer / WBP_LobbyFriendRow / WBP_LobbyInviteRow /
		 *  WBP_LobbyFriendMenu. Empty means "use the plugin's own WBP_Foundry* names". */
		FString Prefix;
		/** A native class path ("/Script/Module.ClassName") the generated drawer WBP reparents to,
		 *  e.g. a game's own UFoundrySocialDrawer subclass. Empty means UFoundrySocialDrawer
		 *  itself (friend row / invite row / friend menu always stay the plugin's own parents). */
		FString DrawerParent;
		/** Resolved exactly like DrawerParent, for the generated party-nameplate WBP. Empty means
		 *  UFoundryPartyNameplate itself. */
		FString NameplateParent;
		/** Resolved exactly like DrawerParent, for the generated pause menu / confirm dialog WBPs.
		 *  Empty means UFoundryPauseMenu / UFoundryConfirmDialog themselves. */
		FString PauseParent;
		FString ConfirmParent;
		/** Resolved exactly like DrawerParent, for the generated settings menu / keybind row WBPs.
		 *  Empty means UFoundrySettingsMenu / UFoundryKeybindRow themselves. */
		FString SettingsParent;
		FString KeybindRowParent;
		/** The menus WBPs' own destination/prefix ("menus": {out, prefix}); either left empty means
		 *  "use Out/Prefix above" (the social values) for THAT field. */
		FString MenusOut;
		FString MenusPrefix;
		FFoundryUIPalette Palette;
		FFoundryUIFonts Fonts;
		FFoundryUIRadii Radii;
	};

	/** Loads a -profile JSON file. Returns false (logging why) on anything that stops the build -
	 *  a missing/unreadable file or invalid JSON; a bad VALUE inside a present field (an unknown
	 *  hex, a font that won't load) is logged and skipped field-by-field instead, since those are
	 *  recoverable with the plugin/Roboto fallback. */
	bool LoadProfile(const FString& Path, const FFoundryUIPalette& PluginPalette, FFoundryUIProfile& OutProfile)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Path))
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile '%s' could not be read"), *Path);
			return false;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile '%s' is not valid JSON"), *Path);
			return false;
		}

		OutProfile.Palette = PluginPalette; // seed with the plugin's own defaults; overlay below
		Root->TryGetStringField(TEXT("out"), OutProfile.Out);
		Root->TryGetStringField(TEXT("prefix"), OutProfile.Prefix);
		Root->TryGetStringField(TEXT("drawerParent"), OutProfile.DrawerParent);
		Root->TryGetStringField(TEXT("nameplateParent"), OutProfile.NameplateParent);
		Root->TryGetStringField(TEXT("pauseParent"), OutProfile.PauseParent);
		Root->TryGetStringField(TEXT("confirmParent"), OutProfile.ConfirmParent);
		Root->TryGetStringField(TEXT("settingsParent"), OutProfile.SettingsParent);
		Root->TryGetStringField(TEXT("keybindRowParent"), OutProfile.KeybindRowParent);

		const TSharedPtr<FJsonObject>* MenusObj = nullptr;
		if (Root->TryGetObjectField(TEXT("menus"), MenusObj) && MenusObj && MenusObj->IsValid())
		{
			(*MenusObj)->TryGetStringField(TEXT("out"), OutProfile.MenusOut);
			(*MenusObj)->TryGetStringField(TEXT("prefix"), OutProfile.MenusPrefix);
		}

		const TSharedPtr<FJsonObject>* PaletteObj = nullptr;
		if (Root->TryGetObjectField(TEXT("palette"), PaletteObj) && PaletteObj && PaletteObj->IsValid())
		{
			auto ApplyColor = [&](const TCHAR* Key, FLinearColor& Field)
			{
				FString Hex;
				if ((*PaletteObj)->TryGetStringField(Key, Hex))
				{
					Field = ParseHexColor(Hex);
				}
			};
			ApplyColor(TEXT("Surface"), OutProfile.Palette.Surface);
			ApplyColor(TEXT("SurfaceLow"), OutProfile.Palette.SurfaceLow);
			ApplyColor(TEXT("SurfaceHigh"), OutProfile.Palette.SurfaceHigh);
			ApplyColor(TEXT("SurfaceHighest"), OutProfile.Palette.SurfaceHighest);
			ApplyColor(TEXT("Primary"), OutProfile.Palette.Primary);
			ApplyColor(TEXT("PrimaryContainer"), OutProfile.Palette.PrimaryContainer);
			ApplyColor(TEXT("Secondary"), OutProfile.Palette.Secondary);
			ApplyColor(TEXT("Tertiary"), OutProfile.Palette.Tertiary);
			ApplyColor(TEXT("Text"), OutProfile.Palette.Text);
			ApplyColor(TEXT("TextSecondary"), OutProfile.Palette.TextSecondary);
			ApplyColor(TEXT("Outline"), OutProfile.Palette.Outline);
			ApplyColor(TEXT("Online"), OutProfile.Palette.Online);
			ApplyColor(TEXT("Idle"), OutProfile.Palette.Idle);
			ApplyColor(TEXT("Error"), OutProfile.Palette.Error);
			ApplyColor(TEXT("HostText"), OutProfile.Palette.HostText);
			ApplyColor(TEXT("Member0"), OutProfile.Palette.Member0);
			ApplyColor(TEXT("Member1"), OutProfile.Palette.Member1);
			ApplyColor(TEXT("Member2"), OutProfile.Palette.Member2);
			ApplyColor(TEXT("Member3"), OutProfile.Palette.Member3);
			ApplyColor(TEXT("Member4"), OutProfile.Palette.Member4);
			ApplyColor(TEXT("Member5"), OutProfile.Palette.Member5);
		}

		const TSharedPtr<FJsonObject>* FontsObj = nullptr;
		if (Root->TryGetObjectField(TEXT("fonts"), FontsObj) && FontsObj && FontsObj->IsValid())
		{
			auto ApplyFont = [&](const TCHAR* Key, FFoundryUIFontRef& Ref)
			{
				const TSharedPtr<FJsonObject>* FontObj = nullptr;
				if ((*FontsObj)->TryGetObjectField(Key, FontObj) && FontObj && FontObj->IsValid())
				{
					(*FontObj)->TryGetStringField(TEXT("asset"), Ref.AssetPath);
					FString Face;
					if ((*FontObj)->TryGetStringField(TEXT("face"), Face))
					{
						Ref.Face = Face;
					}
				}
			};
			ApplyFont(TEXT("regular"), OutProfile.Fonts.Regular);
			ApplyFont(TEXT("bold"), OutProfile.Fonts.Bold);
			ApplyFont(TEXT("heading"), OutProfile.Fonts.Heading);
		}

		const TSharedPtr<FJsonObject>* RadiiObj = nullptr;
		if (Root->TryGetObjectField(TEXT("radii"), RadiiObj) && RadiiObj && RadiiObj->IsValid())
		{
			double V = 0.0;
			if ((*RadiiObj)->TryGetNumberField(TEXT("panel"), V)) { OutProfile.Radii.Panel = (float)V; }
			if ((*RadiiObj)->TryGetNumberField(TEXT("input"), V)) { OutProfile.Radii.Input = (float)V; }
			if ((*RadiiObj)->TryGetNumberField(TEXT("button"), V)) { OutProfile.Radii.Button = (float)V; }
		}

		return true;
	}

	FString FormatLinearColor(const FLinearColor& C)
	{
		return FString::Printf(TEXT("(R=%f,G=%f,B=%f,A=%f)"), C.R, C.G, C.B, C.A);
	}

	/** The [/Script/FoundryFSDKUI.FoundryUISettings] ini block a project pastes into its own
	 *  DefaultGame.ini to point UFoundryUISettings at the just-built skinned copies and palette -
	 *  printed so a profile run never has to be hand-derived into ini form again. */
	FString BuildIniBlock(const FString& SocialPath, const FString& Prefix, const FString& MenusPath, const FString& MenusPrefix, const FFoundryUIPalette& Palette)
	{
		FString S;
		S += TEXT("[/Script/FoundryFSDKUI.FoundryUISettings]") LINE_TERMINATOR;
		S += FString::Printf(TEXT("DefaultSocialDrawer=%s/%sDrawer.%sDrawer_C") LINE_TERMINATOR, *SocialPath, *Prefix, *Prefix);
		S += FString::Printf(TEXT("DefaultFriendRow=%s/%sFriendRow.%sFriendRow_C") LINE_TERMINATOR, *SocialPath, *Prefix, *Prefix);
		S += FString::Printf(TEXT("DefaultInviteRow=%s/%sInviteRow.%sInviteRow_C") LINE_TERMINATOR, *SocialPath, *Prefix, *Prefix);
		S += FString::Printf(TEXT("DefaultFriendMenu=%s/%sFriendMenu.%sFriendMenu_C") LINE_TERMINATOR, *SocialPath, *Prefix, *Prefix);
		S += FString::Printf(TEXT("DefaultPartyNameplate=%s/%sNameplate.%sNameplate_C") LINE_TERMINATOR, *SocialPath, *Prefix, *Prefix);
		S += FString::Printf(TEXT("DefaultPauseMenu=%s/%sPauseMenu.%sPauseMenu_C") LINE_TERMINATOR, *MenusPath, *MenusPrefix, *MenusPrefix);
		S += FString::Printf(TEXT("DefaultConfirmDialog=%s/%sConfirmDialog.%sConfirmDialog_C") LINE_TERMINATOR, *MenusPath, *MenusPrefix, *MenusPrefix);
		S += FString::Printf(TEXT("DefaultSettingsMenu=%s/%sSettingsMenu.%sSettingsMenu_C") LINE_TERMINATOR, *MenusPath, *MenusPrefix, *MenusPrefix);
		S += FString::Printf(TEXT("DefaultKeybindRow=%s/%sKeybindRow.%sKeybindRow_C") LINE_TERMINATOR, *MenusPath, *MenusPrefix, *MenusPrefix);
		S += FString::Printf(TEXT("DefaultTooltip=%s/%sTooltip.%sTooltip_C") LINE_TERMINATOR, *MenusPath, *MenusPrefix, *MenusPrefix);
		S += TEXT("Palette=(");
		S += FString::Printf(TEXT("Surface=%s,"), *FormatLinearColor(Palette.Surface));
		S += FString::Printf(TEXT("SurfaceLow=%s,"), *FormatLinearColor(Palette.SurfaceLow));
		S += FString::Printf(TEXT("SurfaceHigh=%s,"), *FormatLinearColor(Palette.SurfaceHigh));
		S += FString::Printf(TEXT("SurfaceHighest=%s,"), *FormatLinearColor(Palette.SurfaceHighest));
		S += FString::Printf(TEXT("Primary=%s,"), *FormatLinearColor(Palette.Primary));
		S += FString::Printf(TEXT("PrimaryContainer=%s,"), *FormatLinearColor(Palette.PrimaryContainer));
		S += FString::Printf(TEXT("Secondary=%s,"), *FormatLinearColor(Palette.Secondary));
		S += FString::Printf(TEXT("Tertiary=%s,"), *FormatLinearColor(Palette.Tertiary));
		S += FString::Printf(TEXT("Text=%s,"), *FormatLinearColor(Palette.Text));
		S += FString::Printf(TEXT("TextSecondary=%s,"), *FormatLinearColor(Palette.TextSecondary));
		S += FString::Printf(TEXT("Outline=%s,"), *FormatLinearColor(Palette.Outline));
		S += FString::Printf(TEXT("Online=%s,"), *FormatLinearColor(Palette.Online));
		S += FString::Printf(TEXT("Idle=%s,"), *FormatLinearColor(Palette.Idle));
		S += FString::Printf(TEXT("Error=%s,"), *FormatLinearColor(Palette.Error));
		S += FString::Printf(TEXT("HostText=%s,"), *FormatLinearColor(Palette.HostText));
		S += FString::Printf(TEXT("Member0=%s,"), *FormatLinearColor(Palette.Member0));
		S += FString::Printf(TEXT("Member1=%s,"), *FormatLinearColor(Palette.Member1));
		S += FString::Printf(TEXT("Member2=%s,"), *FormatLinearColor(Palette.Member2));
		S += FString::Printf(TEXT("Member3=%s,"), *FormatLinearColor(Palette.Member3));
		S += FString::Printf(TEXT("Member4=%s,"), *FormatLinearColor(Palette.Member4));
		S += FString::Printf(TEXT("Member5=%s"), *FormatLinearColor(Palette.Member5));
		S += TEXT(")") LINE_TERMINATOR;
		return S;
	}

	// -- package / save (mirrors GooEditorAssetUtil's pattern - see GooCrewEditor) ---------------

	UPackage* CreateAssetPackage(const FString& LongPackagePath, const FString& AssetName)
	{
		const FString PackageName = LongPackagePath / AssetName;
		UPackage* Package = ::CreatePackage(*PackageName);
		Package->FullyLoad();
		return Package;
	}

	bool SaveAsset(UObject* Asset)
	{
		if (Asset == nullptr)
		{
			return false;
		}
		UPackage* Package = Asset->GetOutermost();
		Package->MarkPackageDirty();
		FAssetRegistryModule::AssetCreated(Asset);
		const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs Args;
		Args.TopLevelFlags = RF_Public | RF_Standalone;
		Args.SaveFlags = SAVE_NoError;
		const bool bOk = UPackage::SavePackage(Package, Asset, *FileName, Args);
		UE_LOG(LogFoundryUIBuild, Log, TEXT("%s %s"), bOk ? TEXT("saved") : TEXT("FAILED to save"), *Package->GetName());
		return bOk;
	}

	// -- icon import ------------------------------------------------------------------------------

	TMap<FString, UTexture2D*> ImportIcons(const FString& ContentDir, const FString& OutRoot, bool& bOutAnySaveFailed)
	{
		TMap<FString, UTexture2D*> Result;
		const FString SrcDir = ContentDir / TEXT("UI/Icons/src");
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(SrcDir / TEXT("*.png")), true, false);

		if (Files.Num() == 0)
		{
			UE_LOG(LogFoundryUIBuild, Warning, TEXT("no icon PNGs found under %s"), *SrcDir);
			return Result;
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
		const FString DestPath = OutRoot / TEXT("Icons");
		UTextureFactory::SuppressImportOverwriteDialog(true);

		for (const FString& FileName : Files)
		{
			const FString BaseName = FPaths::GetBaseFilename(FileName);
			const FString FullSrc = SrcDir / FileName;

			// Seed the FACTORY with the settings we want, so the FIRST DDC build (the one the
			// import itself kicks off) is already right - see .claude/rules/unreal-plugin-
			// conventions.md "Importing textures and glTF from a commandlet". The PreEditChange/
			// PostEditChange pass below still runs (idempotent re-runs, and it covers the two
			// fields the factory has no equivalent for: NeverStream, Filter).
			UTextureFactory* Factory = NewObject<UTextureFactory>();
			Factory->CompressionSettings = TC_EditorIcon;
			Factory->LODGroup = TEXTUREGROUP_UI;
			Factory->MipGenSettings = TMGS_SimpleAverage;

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = FullSrc;
			Task->DestinationPath = DestPath;
			Task->DestinationName = BaseName;
			Task->bReplaceExisting = true;
			Task->bReplaceExistingSettings = true;
			Task->bAutomated = true;
			Task->bSave = false;
			Task->bAsync = false;
			Task->Factory = Factory;

			TArray<UAssetImportTask*> Tasks;
			Tasks.Add(Task);
			AssetTools.ImportAssetTasks(Tasks);

			UTexture2D* Tex = nullptr;
			for (UObject* Obj : Task->GetObjects())
			{
				Tex = Cast<UTexture2D>(Obj);
				if (Tex != nullptr)
				{
					break;
				}
			}
			if (Tex == nullptr)
			{
				UE_LOG(LogFoundryUIBuild, Error, TEXT("failed to import icon '%s' from %s"), *BaseName, *FullSrc);
				continue;
			}

			// PreEditChange/PostEditChange around the post-import settings change - see
			// .claude/rules/unreal-plugin-conventions.md "Importing textures and glTF from a
			// commandlet": touching CompressionSettings while the DDC build from the import is
			// still in flight can assert. SimpleAverage (not FromTextureGroup, which on the UI
			// group resolves to NoMipmaps) gives these small UI glyphs a real mip chain - without
			// one, every icon was sampled at native res and pixelated when shown smaller/scaled.
			// NeverStream + Trilinear keep them resident and smoothly filtered regardless.
			Tex->PreEditChange(nullptr);
			Tex->CompressionSettings = TC_EditorIcon;
			Tex->LODGroup = TEXTUREGROUP_UI;
			Tex->MipGenSettings = TMGS_SimpleAverage;
			Tex->SRGB = true;
			Tex->NeverStream = true;
			Tex->Filter = TF_Trilinear;
			Tex->PostEditChange();
			if (!SaveAsset(Tex))
			{
				bOutAnySaveFailed = true;
			}

			Result.Add(BaseName, Tex);
			UE_LOG(LogFoundryUIBuild, Log, TEXT("imported icon %s -> %s/%s"), *FileName, *DestPath, *BaseName);
		}
		return Result;
	}

	// -- icon verification (-verifyicons) - proves the mip/filter settings above actually landed,
	// without hand-opening each texture asset in the editor ----------------------------------------

	void VerifyIcons(const TMap<FString, UTexture2D*>& Icons)
	{
		UE_LOG(LogFoundryUIBuild, Log, TEXT("-- icon verification (-verifyicons) --"));
		for (const TPair<FString, UTexture2D*>& Pair : Icons)
		{
			UTexture2D* Tex = Pair.Value;
			if (Tex == nullptr)
			{
				UE_LOG(LogFoundryUIBuild, Error, TEXT("%s: null texture"), *Pair.Key);
				continue;
			}
			// Force the DDC build to finish so NumMips reads the REAL built mip chain, not 0/1
			// from a still-in-flight async build.
			TArray<UTexture*> ToFinish;
			ToFinish.Add(Tex);
			FTextureCompilingManager::Get().FinishCompilation(ToFinish);

			const UEnum* MipGenEnum = StaticEnum<TextureMipGenSettings>();
			const UEnum* GroupEnum = StaticEnum<TextureGroup>();
			const UEnum* FilterEnum = StaticEnum<TextureFilter>();
			UE_LOG(LogFoundryUIBuild, Log,
				TEXT("%s: MipGenSettings=%s LODGroup=%s NumMips=%d NeverStream=%d Filter=%s SRGB=%d"),
				*Pair.Key,
				MipGenEnum ? *MipGenEnum->GetNameStringByValue((int64)Tex->MipGenSettings.GetValue()) : TEXT("?"),
				GroupEnum ? *GroupEnum->GetNameStringByValue((int64)Tex->LODGroup.GetValue()) : TEXT("?"),
				Tex->GetNumMips(),
				Tex->NeverStream ? 1 : 0,
				FilterEnum ? *FilterEnum->GetNameStringByValue((int64)Tex->Filter.GetValue()) : TEXT("?"),
				Tex->SRGB ? 1 : 0);
		}
	}

	// -- generic widget builder ---------------------------------------------------------------------

	UWidget* BuildWidget(UWidgetBlueprint* WBP, const FFoundryWidgetSpec& Spec, const FFoundryUIPalette& Palette,
		const FFoundryUIRadii& Radii, const FFoundryUIFonts& Fonts, const TMap<FString, UTexture2D*>& Icons)
	{
		const FString TypeStr = Spec.Type.ToString();

		auto GetProp = [&Spec](const TCHAR* Key) -> FString
		{
			const FString* Found = Spec.Props.Find(Key);
			return Found ? *Found : FString();
		};
		auto GetPropOr = [&Spec](const TCHAR* Key, const TCHAR* Default) -> FString
		{
			const FString* Found = Spec.Props.Find(Key);
			return Found ? *Found : FString(Default);
		};
		auto GetFloat = [&GetProp](const TCHAR* Key, float Default) -> float
		{
			const FString V = GetProp(Key);
			return V.IsEmpty() ? Default : FCString::Atof(*V);
		};
		auto GetBool = [&GetProp](const TCHAR* Key) -> bool
		{
			return GetProp(Key) == TEXT("1");
		};
		auto GetColor = [&Palette, &GetPropOr](const TCHAR* Key, const TCHAR* Default) -> FLinearColor
		{
			return TokenColor(Palette, GetPropOr(Key, Default));
		};

		UWidget* NewWidget = nullptr;

		if (TypeStr == TEXT("SizeBox"))
		{
			USizeBox* Box = WBP->WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), Spec.Name);
			const FString W = GetProp(TEXT("W"));
			if (!W.IsEmpty()) { Box->SetWidthOverride(FCString::Atof(*W)); }
			const FString H = GetProp(TEXT("H"));
			if (!H.IsEmpty()) { Box->SetHeightOverride(FCString::Atof(*H)); }
			const FString MaxH = GetProp(TEXT("MaxH"));
			if (!MaxH.IsEmpty()) { Box->SetMaxDesiredHeight(FCString::Atof(*MaxH)); }
			NewWidget = Box;
		}
		else if (TypeStr == TEXT("Overlay"))
		{
			NewWidget = WBP->WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), Spec.Name);
		}
		else if (TypeStr == TEXT("VerticalBox"))
		{
			NewWidget = WBP->WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), Spec.Name);
		}
		else if (TypeStr == TEXT("HorizontalBox"))
		{
			NewWidget = WBP->WidgetTree->ConstructWidget<UHorizontalBox>(UHorizontalBox::StaticClass(), Spec.Name);
		}
		else if (TypeStr == TEXT("ScrollBox"))
		{
			UScrollBox* Box = WBP->WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), Spec.Name);
			const float Thickness = GetFloat(TEXT("BarThickness"), 4.0f);
			Box->SetScrollbarThickness(FVector2D(Thickness, Thickness));
			NewWidget = Box;
		}
		else if (TypeStr == TEXT("Spacer"))
		{
			USpacer* Sp = WBP->WidgetTree->ConstructWidget<USpacer>(USpacer::StaticClass(), Spec.Name);
			const float S = GetFloat(TEXT("SpacerSize"), 8.0f);
			Sp->SetSize(FVector2D(S, S));
			NewWidget = Sp;
		}
		else if (TypeStr == TEXT("Border"))
		{
			UBorder* B = WBP->WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), Spec.Name);
			FLinearColor Fill = GetColor(TEXT("Fill"), TEXT("Surface"));
			if (Spec.Props.Contains(TEXT("FillAlpha")))
			{
				// A modal backdrop wants the token's RGB at a dialled-down alpha (e.g. Surface at
				// 0.6), never a second palette entry - the token table has no alpha of its own.
				Fill.A = GetFloat(TEXT("FillAlpha"), Fill.A);
			}
			const bool bHalf = GetBool(TEXT("HalfHeight"));
			// An explicit Radius always wins; absent, the DEFAULT comes from the node's RadiusRole
			// ("Panel" unless tagged "Input") - a project's -profile radii.panel/radii.input dial
			// without touching individual node specs.
			const bool bInputRole = GetProp(TEXT("RadiusRole")) == TEXT("Input");
			const float Radius = GetFloat(TEXT("Radius"), bInputRole ? Radii.Input : Radii.Panel);
			const FLinearColor OutlineC = Spec.Props.Contains(TEXT("OutlineColor"))
				? GetColor(TEXT("OutlineColor"), TEXT("Surface"))
				: FLinearColor(Fill.R, Fill.G, Fill.B, 0.0f);
			const float OutlineW = GetFloat(TEXT("OutlineWidth"), 0.0f);
			B->SetBrush(MakeRoundedBrush(Fill, bHalf, Radius, OutlineC, OutlineW));
			NewWidget = B;
		}
		else if (TypeStr == TEXT("Button"))
		{
			UButton* Btn = WBP->WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Spec.Name);
			const FString NormalTok = GetPropOr(TEXT("Normal"), TEXT("Transparent"));
			const FString HoveredTok = GetPropOr(TEXT("Hovered"), *NormalTok);
			const FString PressedTok = GetPropOr(TEXT("Pressed"), *HoveredTok);
			const FString DisabledTok = GetPropOr(TEXT("Disabled"), *NormalTok);
			const bool bHalf = GetBool(TEXT("HalfHeight"));
			// An explicit Radius always wins (e.g. ManageAccountButton's Fixed 10); absent, the
			// default comes from -profile radii.button.
			const float Radius = GetFloat(TEXT("Radius"), Radii.Button);
			auto BrushFor = [&](const FString& Tok) -> FSlateBrush
			{
				const bool bTransparent = Tok == TEXT("Transparent");
				const FLinearColor C = TokenColor(Palette, Tok);
				const FLinearColor Fill = bTransparent ? FLinearColor(C.R, C.G, C.B, 0.0f) : C;
				return MakeRoundedBrush(Fill, bHalf, Radius, FLinearColor(Fill.R, Fill.G, Fill.B, 0.0f), 0.0f);
			};
			FButtonStyle Style;
			Style.Normal = BrushFor(NormalTok);
			Style.Hovered = BrushFor(HoveredTok);
			Style.Pressed = BrushFor(PressedTok);
			Style.Disabled = BrushFor(DisabledTok);
			Style.NormalPadding = FMargin(0.0f);
			Style.PressedPadding = FMargin(0.0f);
			Btn->SetStyle(Style);
			NewWidget = Btn;
		}
		else if (TypeStr == TEXT("Image"))
		{
			UImage* Img = WBP->WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), Spec.Name);
			const FString IconName = GetProp(TEXT("Icon"));
			if (!IconName.IsEmpty())
			{
				const float W = GetFloat(TEXT("IconW"), 24.0f);
				const float H = GetFloat(TEXT("IconH"), 24.0f);
				UTexture2D* const* Found = Icons.Find(IconName);
				UTexture2D* Tex = Found ? *Found : nullptr;
				if (Tex == nullptr)
				{
					UE_LOG(LogFoundryUIBuild, Error, TEXT("icon '%s' not found (node '%s') - using an empty brush"), *IconName, *Spec.Name.ToString());
				}
				const FLinearColor Tint = Spec.Props.Contains(TEXT("Tint")) ? GetColor(TEXT("Tint"), TEXT("Text")) : FLinearColor::White;
				Img->SetBrush(MakeIconBrush(Tex, W, H, Tint));
			}
			else
			{
				const FLinearColor Fill = GetColor(TEXT("Fill"), TEXT("White"));
				const bool bHalf = GetBool(TEXT("HalfHeight"));
				const float Radius = GetFloat(TEXT("Radius"), 8.0f);
				const FLinearColor OutlineC = Spec.Props.Contains(TEXT("OutlineColor"))
					? GetColor(TEXT("OutlineColor"), TEXT("Surface"))
					: FLinearColor(Fill.R, Fill.G, Fill.B, 0.0f);
				const float OutlineW = GetFloat(TEXT("OutlineWidth"), 0.0f);
				const float W = GetFloat(TEXT("W"), 24.0f);
				const float H = GetFloat(TEXT("H"), 24.0f);
				FSlateBrush Brush = MakeRoundedBrush(Fill, bHalf, Radius, OutlineC, OutlineW);
				Brush.ImageSize = FVector2D(W, H);
				Img->SetBrush(Brush);
			}
			NewWidget = Img;
		}
		else if (TypeStr == TEXT("TextBlock"))
		{
			UTextBlock* Txt = WBP->WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Spec.Name);
			const FString Literal = GetProp(TEXT("Text"));
			if (!Literal.IsEmpty())
			{
				Txt->SetText(FText::FromString(Literal));
			}
			const bool bHeading = GetProp(TEXT("FontRole")) == TEXT("Heading");
			Txt->SetFont(ResolveFont(Fonts, GetFloat(TEXT("FontSize"), 16.0f), GetBool(TEXT("Bold")), bHeading));
			Txt->SetColorAndOpacity(FSlateColor(GetColor(TEXT("Color"), TEXT("Text"))));
			if (GetBool(TEXT("Wrap")))
			{
				Txt->SetAutoWrapText(true);
			}
			NewWidget = Txt;
		}
		else if (TypeStr == TEXT("EditableTextBox"))
		{
			UEditableTextBox* Box = WBP->WidgetTree->ConstructWidget<UEditableTextBox>(UEditableTextBox::StaticClass(), Spec.Name);
			const FString Hint = GetProp(TEXT("Hint"));
			if (!Hint.IsEmpty())
			{
				Box->SetHintText(FText::FromString(Hint));
			}
			const FSlateBrush TransparentBrush = MakeRoundedBrush(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), false, 0.0f, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
			FEditableTextBoxStyle Style;
			Style.BackgroundImageNormal = TransparentBrush;
			Style.BackgroundImageHovered = TransparentBrush;
			Style.BackgroundImageFocused = TransparentBrush;
			Style.BackgroundImageReadOnly = TransparentBrush;
			Style.TextStyle.Font = ResolveFont(Fonts, GetFloat(TEXT("FontSize"), 16.0f), false, false);
			const FLinearColor TextColor = GetColor(TEXT("Color"), TEXT("Text"));
			Style.TextStyle.ColorAndOpacity = FSlateColor(TextColor);
			Style.ForegroundColor = FSlateColor(TextColor);
			Style.Padding = FMargin(0.0f);
			Box->SetWidgetStyle(Style);
			NewWidget = Box;
		}
		else if (TypeStr == TEXT("ComboBoxString"))
		{
			UComboBoxString* Combo = WBP->WidgetTree->ConstructWidget<UComboBoxString>(UComboBoxString::StaticClass(), Spec.Name);
			const float Radius = GetFloat(TEXT("Radius"), Radii.Input);
			// SurfaceHigh, not SurfaceLow - in the Goo palette SurfaceLow and SurfaceHighest are the
			// SAME cream, so a combo filled with the old default vanished into the SurfaceHighest
			// panel it sits in (Justin: "the buttons are not visible"). Still Fill-overridable.
			const FLinearColor Fill = GetColor(TEXT("Fill"), TEXT("SurfaceHigh"));
			const FSlateBrush FillBrush = MakeRoundedBrush(Fill, false, Radius, FLinearColor(Fill.R, Fill.G, Fill.B, 0.0f), 0.0f);
			const FLinearColor TextColorVal = GetColor(TEXT("Color"), TEXT("Text"));
			// Hover = Primary at 0.35 composited over the resting fill - a second brush, never a
			// second flat colour, so the combo still reads as "the same control, lit up" on hover.
			const FLinearColor HoverFill = FoundryUITheme::CompositeOver(Fill, TokenColor(Palette, TEXT("Primary")), 0.35f);
			const FSlateBrush HoverBrush = MakeRoundedBrush(HoverFill, false, Radius, FLinearColor(HoverFill.R, HoverFill.G, HoverFill.B, 0.0f), 0.0f);

			FComboBoxStyle Style = Combo->GetWidgetStyle();
			Style.ComboButtonStyle.ButtonStyle.Normal = FillBrush;
			Style.ComboButtonStyle.ButtonStyle.Hovered = HoverBrush;
			Style.ComboButtonStyle.ButtonStyle.Pressed = HoverBrush;
			Style.ComboButtonStyle.ButtonStyle.Disabled = FillBrush;
			Style.ComboButtonStyle.ButtonStyle.NormalPadding = FMargin(0.0f);
			Style.ComboButtonStyle.ButtonStyle.PressedPadding = FMargin(0.0f);
			// The engine default carries stock texture names ("Old/Menu_Background", "Common/
			// ComboArrow") on these two - rebuild them nameless so no engine art survives into a kit
			// combo (see .claude/rules/umg-ui-conventions.md "stock widget default brushes").
			Style.ComboButtonStyle.MenuBorderBrush = MakeRoundedBrush(GetColor(TEXT("MenuFill"), TEXT("SurfaceHighest")), false, Radii.Panel, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
			{
				UTexture2D* const* ChevronTex = Icons.Find(TEXT("icon_chevron_down"));
				if (ChevronTex == nullptr)
				{
					UE_LOG(LogFoundryUIBuild, Error, TEXT("icon 'icon_chevron_down' not found (node '%s') - combo down-arrow left blank"), *Spec.Name.ToString());
				}
				Style.ComboButtonStyle.DownArrowImage = MakeIconBrush(ChevronTex ? *ChevronTex : nullptr, 16.0f, 16.0f, TextColorVal);
			}
			Style.MenuRowPadding = FMargin(14.0f, 8.0f); // the dropdown rows, not the button (Justin: "the dropdowns need more padding")
			Combo->SetWidgetStyle(Style);

			// ItemStyle's row brushes carry the same stock-texture problem ("Common/Selection",
			// "Common/Selector") - rebuild every one of them (GooLobbyChatPanel::ScrubChannelSelect
			// Brushes touches the identical 11-brush set against a Designer-built combo; here the
			// combo is built fresh in code, so a plain overwrite replaces both the name AND the
			// resource in one step, no stale-name detection needed).
			FTableRowStyle RowStyle = Combo->GetItemStyle();
			RowStyle.TextColor = FSlateColor(TextColorVal);
			FSlateBrush NoBrush;
			NoBrush.DrawAs = ESlateBrushDrawType::NoDrawType;
			const FLinearColor SelectionColor = TokenColor(Palette, TEXT("SurfaceHigh"));
			const FSlateBrush SelectionBrush = MakeRoundedBrush(FLinearColor(SelectionColor.R, SelectionColor.G, SelectionColor.B, 0.5f), false, 8.0f, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
			const FLinearColor SelectorColor = TokenColor(Palette, TEXT("Primary"));
			const FSlateBrush SelectorBrush = MakeRoundedBrush(FLinearColor(SelectorColor.R, SelectorColor.G, SelectorColor.B, 0.5f), false, 8.0f, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
			RowStyle.EvenRowBackgroundBrush = NoBrush;
			RowStyle.EvenRowBackgroundHoveredBrush = SelectionBrush;
			RowStyle.OddRowBackgroundBrush = NoBrush;
			RowStyle.OddRowBackgroundHoveredBrush = SelectionBrush;
			RowStyle.ActiveBrush = SelectionBrush;
			RowStyle.ActiveHoveredBrush = SelectionBrush;
			RowStyle.InactiveBrush = NoBrush;
			RowStyle.InactiveHoveredBrush = SelectionBrush;
			RowStyle.ActiveHighlightedBrush = SelectionBrush;
			RowStyle.InactiveHighlightedBrush = SelectionBrush;
			RowStyle.SelectorFocusedBrush = SelectorBrush;
			Combo->SetItemStyle(RowStyle);

			// Font and ForegroundColor have no public runtime setter (construction-only, same as
			// InitFont) - the engine's own RebuildWidget touches these same fields the same way
			// (ComboBoxString.cpp), so this mirrors stock behavior rather than working around it.
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			Combo->Font = ResolveFont(Fonts, GetFloat(TEXT("FontSize"), 14.0f), false, false);
			Combo->ForegroundColor = FSlateColor(TextColorVal);
			PRAGMA_ENABLE_DEPRECATION_WARNINGS

			// Breathing room for the selected-item text (the combo's own fill has none by default -
			// Justin: "the video settings pills could use a bit of padding for their content").
			Combo->SetContentPadding(FMargin(18.0f, 9.0f));

			NewWidget = Combo;
		}
		else if (TypeStr == TEXT("CheckBox"))
		{
			UCheckBox* Check = WBP->WidgetTree->ConstructWidget<UCheckBox>(UCheckBox::StaticClass(), Spec.Name);
			const float Radius = GetFloat(TEXT("Radius"), 6.0f);
			const float W = GetFloat(TEXT("W"), 22.0f);
			const float H = GetFloat(TEXT("H"), 22.0f);
			auto ColorBrush = [&](const FLinearColor& Color) -> FSlateBrush
			{
				FSlateBrush Brush = MakeRoundedBrush(Color, false, Radius, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
				Brush.ImageSize = FVector2D(W, H);
				return Brush;
			};
			const FLinearColor UncheckedColor = TokenColor(Palette, TEXT("SurfaceHigh"));
			const FLinearColor CheckedColor = TokenColor(Palette, TEXT("Primary"));
			// Hover is a tint of the resting state (the kit's one hover formula), not a repeat of the
			// same flat colour - Justin: "we need the cursor and the hover colors there".
			const FSlateBrush UncheckedBrush = ColorBrush(UncheckedColor);
			const FSlateBrush CheckedBrush = ColorBrush(CheckedColor);
			const FSlateBrush UncheckedHoveredBrush = ColorBrush(FoundryUITheme::CompositeOver(UncheckedColor, TokenColor(Palette, TEXT("Primary")), 0.35f));
			const FSlateBrush CheckedHoveredBrush = ColorBrush(FoundryUITheme::CompositeOver(CheckedColor, TokenColor(Palette, TEXT("Text")), 0.2f));

			FCheckBoxStyle Style = Check->GetWidgetStyle();
			Style.UncheckedImage = UncheckedBrush;
			Style.UncheckedHoveredImage = UncheckedHoveredBrush;
			Style.UncheckedPressedImage = UncheckedHoveredBrush;
			Style.CheckedImage = CheckedBrush;
			Style.CheckedHoveredImage = CheckedHoveredBrush;
			Style.CheckedPressedImage = CheckedHoveredBrush;
			Style.Padding = FMargin(0.0f);
			Check->SetWidgetStyle(Style);
			NewWidget = Check;
		}
		else if (TypeStr == TEXT("Slider"))
		{
			USlider* Sl = WBP->WidgetTree->ConstructWidget<USlider>(USlider::StaticClass(), Spec.Name);
			Sl->SetMinValue(GetFloat(TEXT("MinValue"), 0.0f));
			Sl->SetMaxValue(GetFloat(TEXT("MaxValue"), 1.0f));
			Sl->SetStepSize(GetFloat(TEXT("StepSize"), 0.01f));
			Sl->SetSliderBarColor(GetColor(TEXT("BarColor"), TEXT("SurfaceHigh")));
			Sl->SetSliderHandleColor(GetColor(TEXT("HandleColor"), TEXT("Primary")));

			// The thumb images default to a stock texture ("Common/Button"). SSlider multiplies the
			// brush's OWN tint by SetSliderHandleColor's attribute (SSlider.cpp
			// ThumbImage->GetTint() * SliderHandleColorSlateAttribute), so the brush itself must stay
			// WHITE (shape only, no baked colour) or the two would multiply into a darker result.
			FSliderStyle Style = Sl->GetWidgetStyle();
			FSlateBrush ThumbBrush = MakeRoundedBrush(FLinearColor::White, true, 0.0f, FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), 0.0f);
			ThumbBrush.ImageSize = FVector2D(16.0f, 16.0f);
			Style.NormalThumbImage = ThumbBrush;
			Style.HoveredThumbImage = ThumbBrush;
			Style.DisabledThumbImage = ThumbBrush;
			Sl->SetWidgetStyle(Style);

			NewWidget = Sl;
		}

		if (NewWidget == nullptr)
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("unknown widget spec type '%s' (node '%s')"), *TypeStr, *Spec.Name.ToString());
			return nullptr;
		}

		NewWidget->bIsVariable = Spec.bIsVariable;
		NewWidget->SetVisibility(GetBool(TEXT("Collapsed")) ? ESlateVisibility::Collapsed : ESlateVisibility::Visible);
		if (GetBool(TEXT("Clipping")))
		{
			NewWidget->SetClipping(EWidgetClipping::ClipToBounds);
		}
		if (Spec.Props.Contains(TEXT("Pivot")))
		{
			TArray<FString> PivotParts;
			GetProp(TEXT("Pivot")).ParseIntoArray(PivotParts, TEXT(","), true);
			if (PivotParts.Num() == 2)
			{
				NewWidget->SetRenderTransformPivot(FVector2D(FCString::Atof(*PivotParts[0]), FCString::Atof(*PivotParts[1])));
			}
			else
			{
				UE_LOG(LogFoundryUIBuild, Warning, TEXT("bad Pivot '%s' (node '%s') - expected \"X,Y\""), *GetProp(TEXT("Pivot")), *Spec.Name.ToString());
			}
		}

		if (UPanelWidget* Panel = Cast<UPanelWidget>(NewWidget))
		{
			for (const FFoundryWidgetSpec& ChildSpec : Spec.Children)
			{
				UWidget* ChildWidget = BuildWidget(WBP, ChildSpec, Palette, Radii, Fonts, Icons);
				if (ChildWidget == nullptr)
				{
					continue;
				}
				UPanelSlot* Slot = Panel->AddChild(ChildWidget);
				if (Slot == nullptr)
				{
					continue;
				}

				const FString* ChildHAlignProp = ChildSpec.Props.Find(TEXT("HAlign"));
				const FString* ChildVAlignProp = ChildSpec.Props.Find(TEXT("VAlign"));
				const FString* ParentChildHAlign = Spec.Props.Find(TEXT("ChildHAlign"));
				const FString* ParentChildVAlign = Spec.Props.Find(TEXT("ChildVAlign"));
				const FString HAlignStr = ChildHAlignProp ? *ChildHAlignProp : (ParentChildHAlign ? *ParentChildHAlign : FString(TEXT("Fill")));
				const FString VAlignStr = ChildVAlignProp ? *ChildVAlignProp : (ParentChildVAlign ? *ParentChildVAlign : FString(TEXT("Fill")));
				const EHorizontalAlignment HAlign = ParseHAlign(HAlignStr);
				const EVerticalAlignment VAlign = ParseVAlign(VAlignStr);
				const FString* SlotPadProp = ChildSpec.Props.Find(TEXT("SlotPad"));
				const FMargin SlotPad = SlotPadProp ? ParseMargin(*SlotPadProp) : FMargin(0.0f);
				const FString* SizeProp = ChildSpec.Props.Find(TEXT("Size"));
				const bool bFill = SizeProp && *SizeProp == TEXT("Fill");
				const FString* FillValueProp = ChildSpec.Props.Find(TEXT("FillValue"));
				const float FillValue = FillValueProp ? FCString::Atof(**FillValueProp) : 1.0f;

				if (UVerticalBoxSlot* VSlot = Cast<UVerticalBoxSlot>(Slot))
				{
					VSlot->SetPadding(SlotPad);
					VSlot->SetHorizontalAlignment(HAlign);
					VSlot->SetVerticalAlignment(VAlign);
					FSlateChildSize SizeStruct;
					SizeStruct.SizeRule = bFill ? ESlateSizeRule::Fill : ESlateSizeRule::Automatic;
					SizeStruct.Value = FillValue;
					VSlot->SetSize(SizeStruct);
				}
				else if (UHorizontalBoxSlot* HSlot = Cast<UHorizontalBoxSlot>(Slot))
				{
					HSlot->SetPadding(SlotPad);
					HSlot->SetHorizontalAlignment(HAlign);
					HSlot->SetVerticalAlignment(VAlign);
					if (bFill)
					{
						FSlateChildSize SizeStruct;
						SizeStruct.SizeRule = ESlateSizeRule::Fill;
						SizeStruct.Value = FillValue;
						HSlot->SetSize(SizeStruct);
					}
					else
					{
						HSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
					}
				}
				else if (UOverlaySlot* OSlot = Cast<UOverlaySlot>(Slot))
				{
					OSlot->SetPadding(SlotPad);
					OSlot->SetHorizontalAlignment(HAlign);
					OSlot->SetVerticalAlignment(VAlign);
				}
				else if (UBorderSlot* BdSlot = Cast<UBorderSlot>(Slot))
				{
					BdSlot->SetPadding(SlotPad);
					BdSlot->SetHorizontalAlignment(HAlign);
					BdSlot->SetVerticalAlignment(VAlign);
				}
				else if (UButtonSlot* BtnSlot = Cast<UButtonSlot>(Slot))
				{
					BtnSlot->SetPadding(SlotPad);
					BtnSlot->SetHorizontalAlignment(HAlign);
					BtnSlot->SetVerticalAlignment(VAlign);
				}
				else if (UScrollBoxSlot* SbSlot = Cast<UScrollBoxSlot>(Slot))
				{
					SbSlot->SetPadding(SlotPad);
					SbSlot->SetHorizontalAlignment(HAlign);
					SbSlot->SetVerticalAlignment(VAlign);
				}
				else if (USizeBoxSlot* SzSlot = Cast<USizeBoxSlot>(Slot))
				{
					SzSlot->SetPadding(SlotPad);
					SzSlot->SetHorizontalAlignment(HAlign);
					SzSlot->SetVerticalAlignment(VAlign);
				}
			}
		}

		return NewWidget;
	}

	FString DumpNode(const FFoundryWidgetSpec& Spec, int32 Depth)
	{
		FString Line = FString::ChrN(Depth * 2, TEXT(' '));
		Line += TEXT("- ");
		Line += Spec.Type.ToString();
		if (Spec.bIsVariable)
		{
			Line += FString::Printf(TEXT(" **%s**"), *Spec.Name.ToString());
		}
		TArray<FString> PropStrs;
		for (const TPair<FName, FString>& Pair : Spec.Props)
		{
			PropStrs.Add(FString::Printf(TEXT("%s=%s"), *Pair.Key.ToString(), *Pair.Value));
		}
		if (PropStrs.Num() > 0)
		{
			Line += TEXT("  (") + FString::Join(PropStrs, TEXT(", ")) + TEXT(")");
		}
		Line += LINE_TERMINATOR;
		for (const FFoundryWidgetSpec& Child : Spec.Children)
		{
			Line += DumpNode(Child, Depth + 1);
		}
		return Line;
	}

	bool BuildWBP(const FString& SocialPath, const TCHAR* AssetName, UClass* ParentClass, const FFoundryWidgetSpec& RootSpec,
		const FFoundryUIPalette& Palette, const FFoundryUIRadii& Radii, const FFoundryUIFonts& Fonts,
		const TMap<FString, UTexture2D*>& Icons, FString& OutDump)
	{
		UPackage* Package = CreateAssetPackage(SocialPath, AssetName);

		// Re-runnable: FKismetEditorUtilities::CreateBlueprint asserts no object of this name exists
		// yet, but a previous run's Blueprint is still loaded in this same package (CreatePackage
		// returns the existing in-memory package, then FullyLoad() loads its saved contents back in).
		// Evict it - rename it out of the package into the transient package and mark it garbage -
		// before creating the fresh one, so re-running the commandlet replaces it cleanly.
		if (UObject* Existing = FindObject<UObject>(Package, AssetName))
		{
			Existing->ClearFlags(RF_Standalone | RF_Public);
			Existing->RemoveFromRoot();
			Existing->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional | REN_DoNotDirty);
			Existing->MarkAsGarbage();
		}

		UBlueprint* NewBPBase = FKismetEditorUtilities::CreateBlueprint(ParentClass, Package, FName(AssetName), BPTYPE_Normal,
			UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass());
		UWidgetBlueprint* WBP = CastChecked<UWidgetBlueprint>(NewBPBase);

		UWidget* Root = BuildWidget(WBP, RootSpec, Palette, Radii, Fonts, Icons);
		WBP->WidgetTree->RootWidget = Root;

		FKismetEditorUtilities::CompileBlueprint(WBP);
		const bool bSaved = SaveAsset(WBP);

		OutDump += FString::Printf(TEXT("%s%s"), AssetName, LINE_TERMINATOR);
		OutDump += DumpNode(RootSpec, 0);
		OutDump += LINE_TERMINATOR;

		UE_LOG(LogFoundryUIBuild, Log, TEXT("built %s"), AssetName);
		return bSaved;
	}

	// -- the default social-drawer trees (section 7 of the S1b spec, +PartyNameplateSpec from S4;
	// bound names == the parents' BindWidgetOptional members verbatim) -----------------------------

	FFoundryWidgetSpec FriendRowSpec()
	{
		FFoundryWidgetSpec Root;
		Root.Type = TEXT("SizeBox");
		Root.P(TEXT("H"), 60.0f);
		Root.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
			.P(TEXT("ChildVAlign"), TEXT("Center"))
			.C(FFoundryWidgetSpec{ TEXT("SizeBox") }
				.P(TEXT("W"), 48.0f).P(TEXT("H"), 48.0f)
				.C(FFoundryWidgetSpec{ TEXT("Overlay") }
					.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("Avatar"), true }
						.P(TEXT("Icon"), TEXT("icon_person")).P(TEXT("IconW"), 48.0f).P(TEXT("IconH"), 48.0f))
					.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("PresenceDot"), true }
						.P(TEXT("Fill"), TEXT("White")).P(TEXT("HalfHeight"), TEXT("1"))
						.P(TEXT("W"), 14.0f).P(TEXT("H"), 14.0f)
						.P(TEXT("OutlineColor"), TEXT("Surface")).P(TEXT("OutlineWidth"), 2.0f)
						.P(TEXT("HAlign"), TEXT("Right")).P(TEXT("VAlign"), TEXT("Bottom")))))
			.C(FFoundryWidgetSpec{ TEXT("VerticalBox") }
				.P(TEXT("Size"), TEXT("Fill")).P(TEXT("SlotPad"), TEXT("14,0,0,0"))
				.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
					.P(TEXT("ChildVAlign"), TEXT("Center"))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("NameText"), true }
						.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")))
					// Party rows only - hidden unless the member is joined AND ready
					// (UFoundryFriendRow::Setup).
					.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("ReadyIcon"), true }
						.P(TEXT("Icon"), TEXT("icon_check")).P(TEXT("IconW"), 16.0f).P(TEXT("IconH"), 16.0f).P(TEXT("Tint"), TEXT("Online"))
						.P(TEXT("Collapsed"), TEXT("1")).P(TEXT("SlotPad"), TEXT("6,0,0,0"))))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("StatusText"), true }
					// Resting colour only - UFoundryFriendRow::Setup overrides this per-row from the
					// friend's live presence (or TextSecondary for a blocked row) on every poll.
					.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("TextSecondary")))
				.C(FFoundryWidgetSpec{ TEXT("Border"), TEXT("HostBadge"), true }
					.P(TEXT("Fill"), TEXT("Tertiary")).P(TEXT("HalfHeight"), TEXT("1")).P(TEXT("HAlign"), TEXT("Left"))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
						.P(TEXT("Text"), TEXT("Host")).P(TEXT("FontSize"), 12.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("HostText"))
						.P(TEXT("SlotPad"), TEXT("8,1,8,1")))))
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ActionButton"), true }
				.P(TEXT("Normal"), TEXT("Secondary")).P(TEXT("Hovered"), TEXT("SurfaceHigh")).P(TEXT("HalfHeight"), TEXT("1"))
				.P(TEXT("Collapsed"), TEXT("1"))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ActionText"), true }
					.P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
					.P(TEXT("SlotPad"), TEXT("14,4,14,4")))));
		return Root;
	}

	FFoundryWidgetSpec InviteRowSpec()
	{
		FFoundryWidgetSpec Root;
		Root.Type = TEXT("Border");
		Root.P(TEXT("Fill"), TEXT("SurfaceLow")).P(TEXT("Radius"), 16.0f);
		Root.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
			.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("12,8,12,8"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("LeaderText"), true }
				.P(TEXT("FontSize"), 16.0f).P(TEXT("Color"), TEXT("Text")).P(TEXT("Size"), TEXT("Fill")))
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("AcceptButton"), true }
				.P(TEXT("Normal"), TEXT("Secondary")).P(TEXT("HalfHeight"), TEXT("1"))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
					.P(TEXT("Text"), TEXT("Accept")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
					.P(TEXT("SlotPad"), TEXT("12,4,12,4"))))
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("DeclineButton"), true }
				.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("HalfHeight"), TEXT("1"))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
					.P(TEXT("Text"), TEXT("Decline")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("TextSecondary"))
					.P(TEXT("SlotPad"), TEXT("12,4,12,4")))));
		return Root;
	}

	// One context-menu item row: an icon + a label, inside a flat (alpha-0 normal, hover-highlight)
	// button. Shared shape for SendMessage / Invite / RequestToJoin / RemoveFriend / Block / Unblock.
	FFoundryWidgetSpec MenuItemButton(const TCHAR* Name, const TCHAR* Icon, const TCHAR* Label, const TCHAR* TextColor)
	{
		return FFoundryWidgetSpec{ TEXT("Button"), Name, true }
			.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("Hovered"), TEXT("SurfaceHigh")).P(TEXT("Radius"), 10.0f)
			.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
				.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("12,8,12,8"))
				.C(FFoundryWidgetSpec{ TEXT("Image") }
					.P(TEXT("Icon"), Icon).P(TEXT("IconW"), 20.0f).P(TEXT("IconH"), 20.0f).P(TEXT("Tint"), TextColor)
					.P(TEXT("SlotPad"), TEXT("0,0,10,0")))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
					.P(TEXT("Text"), Label).P(TEXT("FontSize"), 16.0f).P(TEXT("Color"), TextColor)));
	}

	FFoundryWidgetSpec FriendMenuSpec()
	{
		FFoundryWidgetSpec Root;
		Root.Type = TEXT("Border");
		Root.P(TEXT("Fill"), TEXT("SurfaceHighest")).P(TEXT("Radius"), 16.0f);
		Root.C(FFoundryWidgetSpec{ TEXT("VerticalBox") }
			.P(TEXT("SlotPad"), TEXT("8,8,8,8"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("TitleText"), true }
				.P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("TextSecondary"))
				.P(TEXT("SlotPad"), TEXT("12,6,12,6")))
			.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("ItemsPanel"), true }
				.C(MenuItemButton(TEXT("SendMessageButton"), TEXT("icon_send"), TEXT("Send Message"), TEXT("Text")))
				.C(MenuItemButton(TEXT("InviteButton"), TEXT("icon_group_plus"), TEXT("Invite"), TEXT("Text")))
				.C(MenuItemButton(TEXT("RequestJoinButton"), TEXT("icon_group_question"), TEXT("Request to Join"), TEXT("Text")))
				.C(MenuItemButton(TEXT("RemoveFriendButton"), TEXT("icon_person_minus"), TEXT("Remove Friend"), TEXT("Text")))
				.C(MenuItemButton(TEXT("BlockButton"), TEXT("icon_block"), TEXT("Block Player"), TEXT("Error")))
				.C(MenuItemButton(TEXT("UnblockButton"), TEXT("icon_block"), TEXT("Unblock"), TEXT("Text"))))
			.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("ConfirmPanel"), true }
				.P(TEXT("Collapsed"), TEXT("1"))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ConfirmText"), true }
					.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Text")).P(TEXT("Wrap"), TEXT("1"))
					.P(TEXT("SlotPad"), TEXT("12,8,12,8")))
				.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
					.P(TEXT("HAlign"), TEXT("Right"))
					.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("CancelButton"), true }
						.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("HalfHeight"), TEXT("1"))
						.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
							.P(TEXT("Text"), TEXT("Cancel")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("TextSecondary"))
							.P(TEXT("SlotPad"), TEXT("12,4,12,4"))))
					.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ConfirmButton"), true }
						.P(TEXT("Normal"), TEXT("Error")).P(TEXT("HalfHeight"), TEXT("1"))
						.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ConfirmButtonText"), true }
							.P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
							.P(TEXT("SlotPad"), TEXT("14,6,14,6")))))));
		return Root;
	}

	// A floating/anchored party-member name plate: crown + name + ready check in a pill, a HOST
	// badge underneath - crown AND the host pill both mark the party LEADER. Social family (built
	// alongside the drawer/rows, not the menus) - see S4 spec section 3.
	FFoundryWidgetSpec PartyNameplateSpec()
	{
		// An Overlay so the HOST badge OVERLAPS the pill's bottom edge (Justin, 2026-10-12: "the host
		// badge should be overlapping") instead of stacking under it; the pill keeps 15 px of bottom
		// padding so the badge's centre line sits ~6 px under its rim (2026-10-13: "move it down a
		// bit"). Fill at 90 % so the stage reads through the plate.
		FFoundryWidgetSpec Root{ TEXT("Overlay") };
		Root.P(TEXT("HAlign"), TEXT("Center"));

		Root.C(FFoundryWidgetSpec{ TEXT("Border") }
			.P(TEXT("Fill"), TEXT("SurfaceHighest")).P(TEXT("FillAlpha"), 0.9f).P(TEXT("HalfHeight"), TEXT("1"))
			.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Top")).P(TEXT("SlotPad"), TEXT("0,0,0,15"))
			// The badge's top overlaps the pill's bottom ~4 px, which is this padding, never text.
			.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
				.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("10,4,12,6"))
				.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("CrownIcon"), true }
					.P(TEXT("Icon"), TEXT("icon_crown")).P(TEXT("IconW"), 14.0f).P(TEXT("IconH"), 14.0f).P(TEXT("Tint"), TEXT("Idle")))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("NameText"), true }
					.P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")).P(TEXT("SlotPad"), TEXT("6,0,0,0")))
				.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("ReadyIcon"), true }
					.P(TEXT("Icon"), TEXT("icon_check")).P(TEXT("IconW"), 14.0f).P(TEXT("IconH"), 14.0f).P(TEXT("Tint"), TEXT("Online"))
					.P(TEXT("SlotPad"), TEXT("6,0,0,0")))));

		Root.C(FFoundryWidgetSpec{ TEXT("Border"), TEXT("HostBadge"), true }
			.P(TEXT("Fill"), TEXT("Tertiary")).P(TEXT("HalfHeight"), TEXT("1")).P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Bottom"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("HOST")).P(TEXT("FontSize"), 11.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("HostText"))
				.P(TEXT("SlotPad"), TEXT("8,1,8,1"))));

		return Root;
	}

	// One expandable section header: "<label>   N" + a chevron, in a flat (hover-only) button.
	FFoundryWidgetSpec SectionToggle(const TCHAR* ButtonName, const TCHAR* HeaderName, const TCHAR* ChevronName, const TCHAR* SlotPad)
	{
		FFoundryWidgetSpec Btn{ TEXT("Button"), ButtonName, true };
		Btn.P(TEXT("Normal"), TEXT("Transparent"));
		if (SlotPad != nullptr)
		{
			Btn.P(TEXT("SlotPad"), SlotPad);
		}
		return Btn.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
			.P(TEXT("ChildVAlign"), TEXT("Center"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), HeaderName, true }
				.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
				.P(TEXT("FontRole"), TEXT("Heading")).P(TEXT("Size"), TEXT("Fill")))
			// Pivot centre: the runtime (UFoundrySocialDrawer::HandleToggle*) rotates this 0<->180 as
			// the section expands/collapses, via TweenAngle - it must spin in place, not swing an edge.
			.C(FFoundryWidgetSpec{ TEXT("Image"), ChevronName, true }
				.P(TEXT("Icon"), TEXT("icon_chevron_down")).P(TEXT("IconW"), 20.0f).P(TEXT("IconH"), 20.0f).P(TEXT("Tint"), TEXT("Text"))
				.P(TEXT("Pivot"), TEXT("0.5,0.5"))));
	}

	FFoundryWidgetSpec SocialDrawerSpec(const FFoundryUIRadii& Radii)
	{
		FFoundryWidgetSpec Root;
		Root.Type = TEXT("Border");
		Root.P(TEXT("Fill"), TEXT("Surface")).P(TEXT("Radius"), Radii.Panel);

		FFoundryWidgetSpec Column{ TEXT("VerticalBox") };
		Column.P(TEXT("SlotPad"), TEXT("24,24,24,24"));

		// -- title row --------------------------------------------------------------------------
		Column.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
			.P(TEXT("ChildVAlign"), TEXT("Center"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("FRIENDS")).P(TEXT("FontSize"), 27.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
				.P(TEXT("FontRole"), TEXT("Heading")).P(TEXT("Size"), TEXT("Fill")))
			.C(FFoundryWidgetSpec{ TEXT("SizeBox") }
				.P(TEXT("W"), 44.0f).P(TEXT("H"), 44.0f)
				.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("CloseButton"), true }
					.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("Hovered"), TEXT("Transparent")).P(TEXT("HalfHeight"), TEXT("1"))
					.C(FFoundryWidgetSpec{ TEXT("Image") }
						.P(TEXT("Icon"), TEXT("icon_close")).P(TEXT("IconW"), 16.0f).P(TEXT("IconH"), 16.0f).P(TEXT("Tint"), TEXT("Text"))))));

		Column.C(FFoundryWidgetSpec{ TEXT("Spacer") }.P(TEXT("SpacerSize"), 16.0f));

		// -- account block: avatar/name row, then Manage account as its OWN row underneath -------
		Column.C(FFoundryWidgetSpec{ TEXT("Border") }
			.P(TEXT("Fill"), TEXT("SurfaceLow")).P(TEXT("Radius"), 16.0f)
			.C(FFoundryWidgetSpec{ TEXT("VerticalBox") }
				.P(TEXT("SlotPad"), TEXT("14,12,14,12"))
				.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
					.P(TEXT("ChildVAlign"), TEXT("Center"))
					.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("AccountAvatar"), true }
						.P(TEXT("Icon"), TEXT("icon_person")).P(TEXT("IconW"), 48.0f).P(TEXT("IconH"), 48.0f))
					.C(FFoundryWidgetSpec{ TEXT("VerticalBox") }
						.P(TEXT("Size"), TEXT("Fill")).P(TEXT("SlotPad"), TEXT("14,0,0,0")).P(TEXT("Clipping"), TEXT("1"))
						.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("AccountNameText"), true }
							.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")))
						.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("AccountUsernameText"), true }
							.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("TextSecondary")))))
				// A plain rounded rect (never a pill, Radius stays Fixed 10 regardless of the
				// project's button radius) that fills the account block's width like the row above
				// it, with its icon+label group CENTRED inside rather than stretched to match.
				.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ManageAccountButton"), true }
					.P(TEXT("Normal"), TEXT("SurfaceHigh")).P(TEXT("Radius"), 10.0f)
					.P(TEXT("HAlign"), TEXT("Fill")).P(TEXT("SlotPad"), TEXT("0,8,0,0"))
					.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
						.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("12,6,12,6"))
						.C(FFoundryWidgetSpec{ TEXT("Image") }
							.P(TEXT("Icon"), TEXT("icon_external")).P(TEXT("IconW"), 18.0f).P(TEXT("IconH"), 18.0f).P(TEXT("Tint"), TEXT("Secondary"))
							.P(TEXT("SlotPad"), TEXT("0,0,6,0")))
						.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
							.P(TEXT("Text"), TEXT("Manage account")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Secondary")))))));

		Column.C(FFoundryWidgetSpec{ TEXT("Spacer") }.P(TEXT("SpacerSize"), 16.0f));

		// -- the scrolling list -------------------------------------------------------------------
		FFoundryWidgetSpec Scroll{ TEXT("ScrollBox") };
		Scroll.P(TEXT("Size"), TEXT("Fill")).P(TEXT("BarThickness"), 4.0f);

		Scroll.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
			.P(TEXT("ChildVAlign"), TEXT("Center"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("PartyHeaderText"), true }
				.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")).P(TEXT("FontRole"), TEXT("Heading"))
				.P(TEXT("Size"), TEXT("Fill")))
			// Visible only while in a party (UFoundrySocialDrawer::Rebuild); toggles your own ready
			// flag via SetReady.
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ReadyToggleButton"), true }
				.P(TEXT("Normal"), TEXT("Primary")).P(TEXT("Hovered"), TEXT("SurfaceHigh")).P(TEXT("HalfHeight"), TEXT("1"))
				.P(TEXT("Collapsed"), TEXT("1"))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ReadyToggleText"), true }
					.P(TEXT("Text"), TEXT("READY")).P(TEXT("FontSize"), 13.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
					.P(TEXT("SlotPad"), TEXT("14,5,14,5")))));
		Scroll.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("CapConflictText"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Error")).P(TEXT("Wrap"), TEXT("1")));
		Scroll.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("PartyList"), true });
		Scroll.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("InvitesSection"), true }
			.P(TEXT("SlotPad"), TEXT("0,16,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("PARTY INVITES")).P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
				.P(TEXT("FontRole"), TEXT("Heading")))
			.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("InvitesList"), true }));
		Scroll.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("RequestsNote"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("Wrap"), TEXT("1")).P(TEXT("SlotPad"), TEXT("0,12,0,0")));

		// -- search frame (under YOUR PARTY, above ONLINE) ---------------------------------------
		Scroll.C(FFoundryWidgetSpec{ TEXT("Border") }
			.P(TEXT("Fill"), TEXT("SurfaceLow")).P(TEXT("Radius"), Radii.Input).P(TEXT("RadiusRole"), TEXT("Input"))
			.P(TEXT("SlotPad"), TEXT("0,12,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
				.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("14,10,14,10"))
				.C(FFoundryWidgetSpec{ TEXT("Image") }
					.P(TEXT("Icon"), TEXT("icon_search")).P(TEXT("IconW"), 22.0f).P(TEXT("IconH"), 22.0f).P(TEXT("Tint"), TEXT("TextSecondary")))
				.C(FFoundryWidgetSpec{ TEXT("EditableTextBox"), TEXT("SearchBox"), true }
					.P(TEXT("Hint"), TEXT("Search friends")).P(TEXT("Color"), TEXT("Text")).P(TEXT("FontSize"), 16.0f).P(TEXT("Size"), TEXT("Fill")))));

		Scroll.C(SectionToggle(TEXT("OnlineToggleButton"), TEXT("OnlineHeaderText"), TEXT("OnlineChevron"), TEXT("0,16,0,0")));
		Scroll.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("OnlineList"), true });
		Scroll.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("OnlineEmptyText"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("Wrap"), TEXT("1")));

		Scroll.C(SectionToggle(TEXT("OfflineToggleButton"), TEXT("OfflineHeaderText"), TEXT("OfflineChevron"), TEXT("0,16,0,0")));
		Scroll.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("OfflineList"), true }.P(TEXT("Collapsed"), TEXT("1")));

		Scroll.C(SectionToggle(TEXT("BlockedToggleButton"), TEXT("BlockedHeaderText"), TEXT("BlockedChevron"), TEXT("0,16,0,0")));
		Scroll.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("BlockedList"), true }.P(TEXT("Collapsed"), TEXT("1")));

		Column.C(MoveTemp(Scroll));

		// -- add friend --------------------------------------------------------------------------
		Column.C(FFoundryWidgetSpec{ TEXT("HorizontalBox"), TEXT("AddFriendEntry"), true }
			.P(TEXT("Collapsed"), TEXT("1")).P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("0,12,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("EditableTextBox"), TEXT("AddFriendInput"), true }
				.P(TEXT("Hint"), TEXT("Add by username")).P(TEXT("Color"), TEXT("Text")).P(TEXT("FontSize"), 16.0f).P(TEXT("Size"), TEXT("Fill")))
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("AddFriendSendButton"), true }
				.P(TEXT("Normal"), TEXT("Secondary")).P(TEXT("HalfHeight"), TEXT("1"))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
					.P(TEXT("Text"), TEXT("Send")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
					.P(TEXT("SlotPad"), TEXT("14,6,14,6")))));

		Column.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("AddFriendNote"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Error")).P(TEXT("Wrap"), TEXT("1")));

		Column.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ActionNote"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Error")).P(TEXT("Wrap"), TEXT("1")).P(TEXT("Collapsed"), TEXT("1")));

		Column.C(FFoundryWidgetSpec{ TEXT("Spacer") }.P(TEXT("SpacerSize"), 12.0f));

		Column.C(FFoundryWidgetSpec{ TEXT("SizeBox") }
			.P(TEXT("H"), 56.0f)
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("AddFriendButton"), true }
				.P(TEXT("Normal"), TEXT("Primary")).P(TEXT("Pressed"), TEXT("PrimaryContainer")).P(TEXT("Radius"), 16.0f)
				.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
					.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Center"))
					.C(FFoundryWidgetSpec{ TEXT("Image") }
						.P(TEXT("Icon"), TEXT("icon_person_plus")).P(TEXT("IconW"), 24.0f).P(TEXT("IconH"), 24.0f).P(TEXT("Tint"), TEXT("Surface"))
						// Never Fill - the HorizontalBox's taller TextBlock-driven row height would
						// otherwise stretch the (square) icon brush vertically.
						.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("0,0,10,0")))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
						.P(TEXT("Text"), TEXT("ADD FRIEND")).P(TEXT("FontSize"), 16.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))))));

		Column.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("LeaveButton"), true }
			.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("SlotPad"), TEXT("0,8,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("Leave party")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Error"))));

		Root.C(MoveTemp(Column));
		return Root;
	}

	// -- the two default menu trees (S3a): a full-screen Overlay (a semi-transparent Backdrop +
	// a centered Panel) is shared shape; only the Panel's content differs. -----------------------

	/** A full-screen Overlay root, with a dimmed Backdrop filling it and a centered, PanelWidth-wide
	 *  SizeBox ready to hold the caller's Panel content. AddToViewport already stretches the WBP's
	 *  root to the whole screen (the same way it does for every other kit widget) - no extra sizing
	 *  is needed on the Overlay itself. */
	FFoundryWidgetSpec MenuBackdropAndSizeBox(float PanelWidth, FFoundryWidgetSpec& OutSizeBox)
	{
		FFoundryWidgetSpec Root;
		Root.Type = TEXT("Overlay");
		Root.C(FFoundryWidgetSpec{ TEXT("Border"), TEXT("Backdrop"), true }
			.P(TEXT("Fill"), TEXT("Surface")).P(TEXT("FillAlpha"), 0.6f).P(TEXT("Radius"), 0.0f));

		OutSizeBox = FFoundryWidgetSpec{ TEXT("SizeBox") };
		OutSizeBox.P(TEXT("W"), PanelWidth).P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Center"));
		return Root;
	}

	// One full-width pill button for the pause menu: a solid row (never auto-width - VerticalBox's
	// default child HAlign is Fill) with its label centered via the TEXT BLOCK's own HAlign/VAlign
	// (no wrapping HorizontalBox needed for a single label) and vertical SlotPad giving it a real
	// clickable height. Hovered is explicit (never left to default-to-Normal) - every pill on this
	// menu states its own hover colour.
	FFoundryWidgetSpec PauseMenuButton(const TCHAR* ButtonName, const TCHAR* NormalTok, const TCHAR* HoveredTok, const TCHAR* TextName, const TCHAR* TextTok)
	{
		return FFoundryWidgetSpec{ TEXT("Button"), ButtonName, true }
			.P(TEXT("Normal"), NormalTok).P(TEXT("Hovered"), HoveredTok).P(TEXT("HalfHeight"), TEXT("1")).P(TEXT("SlotPad"), TEXT("0,6,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TextName, true }
				.P(TEXT("FontSize"), 15.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TextTok)
				.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("0,14,0,14")));
	}

	FFoundryWidgetSpec PauseMenuSpec(const FFoundryUIRadii& Radii)
	{
		FFoundryWidgetSpec SizeBoxNode;
		FFoundryWidgetSpec Root = MenuBackdropAndSizeBox(440.0f, SizeBoxNode);

		FFoundryWidgetSpec Panel{ TEXT("Border"), TEXT("Panel"), true };
		Panel.P(TEXT("Fill"), TEXT("SurfaceHighest")).P(TEXT("Radius"), Radii.Panel);

		FFoundryWidgetSpec Column{ TEXT("VerticalBox") };
		Column.P(TEXT("SlotPad"), TEXT("24,24,24,24"));

		Column.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("TitleText"), true }
			.P(TEXT("FontSize"), 22.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
			.P(TEXT("FontRole"), TEXT("Heading")).P(TEXT("HAlign"), TEXT("Center")));

		Column.C(PauseMenuButton(TEXT("SettingsButton"), TEXT("SurfaceHigh"), TEXT("SurfaceHigh"), TEXT("SettingsText"), TEXT("Text")));
		Column.C(PauseMenuButton(TEXT("PrimaryButton"), TEXT("Primary"), TEXT("Primary"), TEXT("PrimaryText"), TEXT("Surface")));
		// Was Normal=Transparent/text=TextSecondary - invisible against the SurfaceHighest panel
		// (the same family of bug as the settings-menu combos). A real pill now, like BackButton.
		Column.C(PauseMenuButton(TEXT("ResumeButton"), TEXT("SurfaceHigh"), TEXT("Primary"), TEXT("ResumeText"), TEXT("Text")));

		Panel.C(MoveTemp(Column));
		SizeBoxNode.C(MoveTemp(Panel));
		Root.C(MoveTemp(SizeBoxNode));
		return Root;
	}

	FFoundryWidgetSpec ConfirmDialogSpec(const FFoundryUIRadii& Radii)
	{
		FFoundryWidgetSpec SizeBoxNode;
		FFoundryWidgetSpec Root = MenuBackdropAndSizeBox(480.0f, SizeBoxNode);

		FFoundryWidgetSpec Panel{ TEXT("Border"), TEXT("Panel"), true };
		Panel.P(TEXT("Fill"), TEXT("SurfaceHighest")).P(TEXT("Radius"), Radii.Panel);

		FFoundryWidgetSpec Column{ TEXT("VerticalBox") };
		Column.P(TEXT("SlotPad"), TEXT("24,24,24,24"));

		Column.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("TitleText"), true }
			.P(TEXT("FontSize"), 22.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
			.P(TEXT("FontRole"), TEXT("Heading")).P(TEXT("HAlign"), TEXT("Center")));

		Column.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("BodyText"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("Wrap"), TEXT("1"))
			.P(TEXT("SlotPad"), TEXT("0,10,0,0")));

		FFoundryWidgetSpec ButtonRow{ TEXT("HorizontalBox") };
		ButtonRow.P(TEXT("HAlign"), TEXT("Right")).P(TEXT("SlotPad"), TEXT("0,20,0,0"));
		// Was Normal=Transparent/text=TextSecondary - invisible against the SurfaceHighest panel.
		// A real pill now, like the pause menu's BackButton/ResumeButton.
		ButtonRow.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("CancelButton"), true }
			.P(TEXT("Normal"), TEXT("SurfaceHigh")).P(TEXT("Hovered"), TEXT("Primary")).P(TEXT("HalfHeight"), TEXT("1")).P(TEXT("SlotPad"), TEXT("0,0,8,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("CancelText"), true }
				.P(TEXT("FontSize"), 15.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
				.P(TEXT("SlotPad"), TEXT("16,8,16,8"))));
		ButtonRow.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ConfirmButton"), true }
			.P(TEXT("Normal"), TEXT("Error")).P(TEXT("HalfHeight"), TEXT("1"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ConfirmText"), true }
				.P(TEXT("FontSize"), 15.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
				.P(TEXT("SlotPad"), TEXT("16,8,16,8"))));
		Column.C(MoveTemp(ButtonRow));

		Panel.C(MoveTemp(Column));
		SizeBoxNode.C(MoveTemp(Panel));
		Root.C(MoveTemp(SizeBoxNode));
		return Root;
	}

	// -- the settings menu + its keybind row (S3b, two-pane S3c) --------------------------------

	// One "label | 280-wide control" row, like PauseMenuButton is one full-width row - shared shape
	// for every Video/Audio/Interface row on the settings menu. RowName binds the whole row (not
	// just its control) so the parent's hover/focus-to-describe pass can IsHovered()/
	// HasFocusedDescendants() the ROW - hovering the label, not just the combo/slider itself, still
	// selects the description.
	FFoundryWidgetSpec SettingsRow(const TCHAR* RowName, const TCHAR* Label, FFoundryWidgetSpec Control)
	{
		FFoundryWidgetSpec Row{ TEXT("HorizontalBox"), RowName, true };
		// A bit more air than the dialog/footer rows elsewhere in the kit - Justin: "the pills have
		// no space right now".
		Row.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("0,12,0,0"));
		Row.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
			.P(TEXT("Text"), Label).P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Text")).P(TEXT("Size"), TEXT("Fill")));
		Row.C(FFoundryWidgetSpec{ TEXT("SizeBox") }.P(TEXT("W"), 280.0f).C(MoveTemp(Control)));
		return Row;
	}

	// A slider + its value text (W 56, HAlign Right) sat beside it, filling the row's 280-wide slot.
	FFoundryWidgetSpec SliderControl(const TCHAR* SliderName, const TCHAR* ValueTextName, float MinValue, float MaxValue)
	{
		FFoundryWidgetSpec Box{ TEXT("HorizontalBox") };
		Box.P(TEXT("ChildVAlign"), TEXT("Center"));
		Box.C(FFoundryWidgetSpec{ TEXT("Slider"), SliderName, true }
			.P(TEXT("Size"), TEXT("Fill")).P(TEXT("MinValue"), MinValue).P(TEXT("MaxValue"), MaxValue));
		Box.C(FFoundryWidgetSpec{ TEXT("SizeBox") }.P(TEXT("W"), 56.0f).P(TEXT("SlotPad"), TEXT("8,0,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), ValueTextName, true }
				.P(TEXT("FontSize"), 13.0f).P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("HAlign"), TEXT("Right"))));
		return Box;
	}

	// One section: a "VIDEO"/"AUDIO"/... heading over its rows, named <SectionName> so the parent
	// binds the whole VerticalBox as its VideoSection/AudioSection/... UWidget member. Used for
	// Video/Audio/Interface, whose SECTION HEADER itself is never hover-tested (only their rows
	// are) - Controls builds its own header inline (as ControlsRow) in SettingsMenuSpec below.
	FFoundryWidgetSpec SettingsSection(const TCHAR* SectionName, const TCHAR* HeadingLabel)
	{
		FFoundryWidgetSpec Section{ TEXT("VerticalBox"), SectionName, true };
		Section.P(TEXT("SlotPad"), TEXT("0,16,0,0"));
		Section.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
			.P(TEXT("Text"), HeadingLabel).P(TEXT("FontSize"), 15.0f).P(TEXT("Bold"), TEXT("1"))
			.P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("FontRole"), TEXT("Heading")));
		return Section;
	}

	// A half-width footer pill for the settings menu's Apply/Back row - unlike PauseMenuButton's
	// full-width VerticalBox rows, these sit SIDE BY SIDE in a HorizontalBox, each taking an even
	// share (Size Fill) of the row.
	FFoundryWidgetSpec SettingsFooterButton(const TCHAR* ButtonName, const TCHAR* NormalTok, const TCHAR* HoveredTok,
		const TCHAR* TextName, const TCHAR* Label, const TCHAR* TextTok)
	{
		return FFoundryWidgetSpec{ TEXT("Button"), ButtonName, true }
			.P(TEXT("Normal"), NormalTok).P(TEXT("Hovered"), HoveredTok).P(TEXT("HalfHeight"), TEXT("1")).P(TEXT("Size"), TEXT("Fill"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TextName, true }
				.P(TEXT("Text"), Label).P(TEXT("FontSize"), 15.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TextTok)
				.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("0,14,0,14")));
	}

	// Full-screen two-pane layout: a 92%-alpha backdrop over the whole screen, LEFT column (55%)
	// scrolling Video/Audio/Interface/Controls + the Apply/Back footer, RIGHT column (45%) the
	// description pane that reads whatever the left side is hovering (Fortnite-style).
	FFoundryWidgetSpec SettingsMenuSpec(const FFoundryUIRadii& Radii)
	{
		FFoundryWidgetSpec Root{ TEXT("Overlay") };
		Root.C(FFoundryWidgetSpec{ TEXT("Border"), TEXT("Backdrop"), true }
			.P(TEXT("Fill"), TEXT("Surface")).P(TEXT("FillAlpha"), 0.92f).P(TEXT("Radius"), 0.0f));

		FFoundryWidgetSpec Content{ TEXT("HorizontalBox") };
		Content.P(TEXT("SlotPad"), TEXT("64,48,64,48"));

		// -- LEFT: title, the scrolling sections, the Apply/Back footer --------------------------
		FFoundryWidgetSpec Left{ TEXT("VerticalBox") };
		Left.P(TEXT("Size"), TEXT("Fill")).P(TEXT("FillValue"), 0.55f);

		Left.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("TitleText"), true }
			.P(TEXT("Text"), TEXT("SETTINGS")).P(TEXT("FontSize"), 26.0f).P(TEXT("Bold"), TEXT("1"))
			.P(TEXT("Color"), TEXT("Text")).P(TEXT("FontRole"), TEXT("Heading")));

		// VIDEO.
		FFoundryWidgetSpec VideoSection = SettingsSection(TEXT("VideoSection"), TEXT("VIDEO"));
		VideoSection.C(SettingsRow(TEXT("WindowModeRow"), TEXT("Window mode"), FFoundryWidgetSpec{ TEXT("ComboBoxString"), TEXT("WindowModeCombo"), true }));
		VideoSection.C(SettingsRow(TEXT("ResolutionRow"), TEXT("Resolution"), FFoundryWidgetSpec{ TEXT("ComboBoxString"), TEXT("ResolutionCombo"), true }));
		VideoSection.C(SettingsRow(TEXT("QualityRow"), TEXT("Quality"), FFoundryWidgetSpec{ TEXT("ComboBoxString"), TEXT("QualityCombo"), true }));
		VideoSection.C(SettingsRow(TEXT("FpsCapRow"), TEXT("Frame rate cap"), FFoundryWidgetSpec{ TEXT("ComboBoxString"), TEXT("FpsCapCombo"), true }));
		VideoSection.C(SettingsRow(TEXT("VSyncRow"), TEXT("V-Sync"), FFoundryWidgetSpec{ TEXT("CheckBox"), TEXT("VSyncCheck"), true }));

		// AUDIO.
		FFoundryWidgetSpec AudioSection = SettingsSection(TEXT("AudioSection"), TEXT("AUDIO"));
		AudioSection.C(SettingsRow(TEXT("MasterRow"), TEXT("Master volume"), SliderControl(TEXT("MasterSlider"), TEXT("MasterValueText"), 0.0f, 1.0f)));
		AudioSection.C(SettingsRow(TEXT("MusicRow"), TEXT("Music volume"), SliderControl(TEXT("MusicSlider"), TEXT("MusicValueText"), 0.0f, 1.0f)));
		AudioSection.C(SettingsRow(TEXT("SFXRow"), TEXT("SFX volume"), SliderControl(TEXT("SFXSlider"), TEXT("SFXValueText"), 0.0f, 1.0f)));

		// INTERFACE.
		FFoundryWidgetSpec InterfaceSection = SettingsSection(TEXT("InterfaceSection"), TEXT("INTERFACE"));
		InterfaceSection.C(SettingsRow(TEXT("UIScaleRow"), TEXT("UI scale"), SliderControl(TEXT("UIScaleSlider"), TEXT("UIScaleValueText"), 0.75f, 1.5f)));
		InterfaceSection.C(SettingsRow(TEXT("ReducedMotionRow"), TEXT("Reduced motion"), FFoundryWidgetSpec{ TEXT("CheckBox"), TEXT("ReducedMotionCheck"), true }));

		// CONTROLS - the header is its OWN named row (ControlsRow), built inline rather than via
		// SettingsSection, so the hover-to-describe pass treats it like every other row; the keybind
		// rows underneath are hover-tested separately (RebuildKeybindRows creates them at runtime,
		// never from this spec).
		FFoundryWidgetSpec ControlsSection{ TEXT("VerticalBox"), TEXT("ControlsSection"), true };
		ControlsSection.P(TEXT("SlotPad"), TEXT("0,16,0,0"));
		FFoundryWidgetSpec ControlsHeaderRow{ TEXT("HorizontalBox"), TEXT("ControlsRow"), true };
		ControlsHeaderRow.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
			.P(TEXT("Text"), TEXT("CONTROLS")).P(TEXT("FontSize"), 15.0f).P(TEXT("Bold"), TEXT("1"))
			.P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("FontRole"), TEXT("Heading")));
		ControlsSection.C(MoveTemp(ControlsHeaderRow));
		ControlsSection.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ControlsNoteText"), true }
			.P(TEXT("FontSize"), 13.0f).P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("Wrap"), TEXT("1")));
		ControlsSection.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("KeybindList"), true }.P(TEXT("SlotPad"), TEXT("0,8,0,0")));
		ControlsSection.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ResetAllKeysButton"), true }
			.P(TEXT("Normal"), TEXT("SurfaceHigh")).P(TEXT("Hovered"), TEXT("Primary")).P(TEXT("HalfHeight"), TEXT("1")).P(TEXT("SlotPad"), TEXT("0,10,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("RESET ALL")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
				.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("0,10,0,10"))));

		// The four sections scroll together; Size=Fill so the scroll area grows with the screen
		// (the old centered-SizeBox layout capped it at 620 - the full-screen layout has no reason
		// to cap height, so no SizeBox/MaxH wrap here).
		FFoundryWidgetSpec Scroll{ TEXT("ScrollBox") };
		Scroll.P(TEXT("BarThickness"), 4.0f).P(TEXT("Size"), TEXT("Fill"));
		Scroll.C(MoveTemp(VideoSection));
		Scroll.C(MoveTemp(AudioSection));
		Scroll.C(MoveTemp(InterfaceSection));
		Scroll.C(MoveTemp(ControlsSection));
		Left.C(MoveTemp(Scroll));

		// Bottom: Apply (staged video, disabled while nothing is pending) left of Back, then the
		// fading note line.
		FFoundryWidgetSpec FooterRow{ TEXT("HorizontalBox") };
		FooterRow.P(TEXT("SlotPad"), TEXT("0,16,0,0"));
		FooterRow.C(SettingsFooterButton(TEXT("ApplyButton"), TEXT("Primary"), TEXT("Primary"), TEXT("ApplyText"), TEXT("APPLY"), TEXT("Surface"))
			.P(TEXT("Disabled"), TEXT("SurfaceHigh")).P(TEXT("SlotPad"), TEXT("0,0,8,0")));
		FooterRow.C(SettingsFooterButton(TEXT("BackButton"), TEXT("SurfaceHigh"), TEXT("Primary"), TEXT("BackText"), TEXT("BACK"), TEXT("Text")));
		Left.C(MoveTemp(FooterRow));
		Left.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("NoteText"), true }
			.P(TEXT("FontSize"), 13.0f).P(TEXT("Color"), TEXT("Online")).P(TEXT("HAlign"), TEXT("Center"))
			.P(TEXT("SlotPad"), TEXT("0,8,0,0")));

		Content.C(MoveTemp(Left));

		// -- RIGHT: the description pane ---------------------------------------------------------
		FFoundryWidgetSpec DescriptionPanel{ TEXT("Border"), TEXT("DescriptionPanel"), true };
		DescriptionPanel.P(TEXT("Fill"), TEXT("SurfaceHighest")).P(TEXT("Radius"), Radii.Panel)
			.P(TEXT("Size"), TEXT("Fill")).P(TEXT("FillValue"), 0.45f).P(TEXT("SlotPad"), TEXT("40,56,0,0"));

		FFoundryWidgetSpec DescriptionColumn{ TEXT("VerticalBox") };
		DescriptionColumn.P(TEXT("SlotPad"), TEXT("28,28,28,28"));
		DescriptionColumn.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("DescriptionTitleText"), true }
			.P(TEXT("Text"), TEXT("Settings")).P(TEXT("FontSize"), 20.0f).P(TEXT("Bold"), TEXT("1"))
			.P(TEXT("Color"), TEXT("Text")).P(TEXT("FontRole"), TEXT("Heading")));
		DescriptionColumn.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("DescriptionBodyText"), true }
			.P(TEXT("Text"), TEXT("Hover a setting to read what it does.")).P(TEXT("FontSize"), 15.0f)
			.P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("Wrap"), TEXT("1")).P(TEXT("SlotPad"), TEXT("0,10,0,0")));
		// No Icon - the runtime sets a brush (UFoundrySettingsMenu::Previews) when the hovered row
		// has one; Collapsed until then.
		DescriptionColumn.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("PreviewImage"), true }
			.P(TEXT("Collapsed"), TEXT("1")).P(TEXT("HAlign"), TEXT("Fill")).P(TEXT("H"), 220.0f).P(TEXT("SlotPad"), TEXT("0,16,0,0")));
		DescriptionColumn.C(FFoundryWidgetSpec{ TEXT("Spacer") }.P(TEXT("Size"), TEXT("Fill")));
		DescriptionPanel.C(MoveTemp(DescriptionColumn));

		Content.C(MoveTemp(DescriptionPanel));
		Root.C(MoveTemp(Content));

		// Top-right X - same behavior as BackButton (UFoundrySettingsMenu wires both to
		// HandleBackClicked). A third Overlay child, over everything else.
		FFoundryWidgetSpec CloseBox{ TEXT("SizeBox") };
		// Sits on the title row (top 40 ~ the SETTINGS baseline) flush with the description panel's right
		// edge (64 = the content margin); the panel itself starts BELOW it (its slot pads the top by 56).
		CloseBox.P(TEXT("HAlign"), TEXT("Right")).P(TEXT("VAlign"), TEXT("Top")).P(TEXT("SlotPad"), TEXT("0,40,64,0"));
		CloseBox.P(TEXT("W"), 44.0f).P(TEXT("H"), 44.0f);
		CloseBox.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("CloseButton"), true }
			.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("Hovered"), TEXT("Transparent")).P(TEXT("HalfHeight"), TEXT("1"))
			.C(FFoundryWidgetSpec{ TEXT("Image") }
				.P(TEXT("Icon"), TEXT("icon_close")).P(TEXT("IconW"), 16.0f).P(TEXT("IconH"), 16.0f).P(TEXT("Tint"), TEXT("Text"))));
		Root.C(MoveTemp(CloseBox));

		return Root;
	}

	FFoundryWidgetSpec KeybindRowSpec()
	{
		FFoundryWidgetSpec Root{ TEXT("HorizontalBox") };
		// A bit more air between rows - the KeyButton pills touched each other today.
		Root.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("0,8,0,0"));
		Root.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("ActionText"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Text")).P(TEXT("Size"), TEXT("Fill")));
		Root.C(FFoundryWidgetSpec{ TEXT("SizeBox") }.P(TEXT("W"), 180.0f)
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("KeyButton"), true }
				.P(TEXT("Normal"), TEXT("SurfaceHigh")).P(TEXT("HalfHeight"), TEXT("1"))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("KeyText"), true }
					.P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
					.P(TEXT("HAlign"), TEXT("Center")).P(TEXT("VAlign"), TEXT("Center"))
					.P(TEXT("SlotPad"), TEXT("16,8,16,8")))));
		Root.C(FFoundryWidgetSpec{ TEXT("SizeBox") }.P(TEXT("W"), 32.0f).P(TEXT("SlotPad"), TEXT("8,0,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ResetButton"), true }
				.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("Radius"), 8.0f)
				.C(FFoundryWidgetSpec{ TEXT("Image") }
					.P(TEXT("Icon"), TEXT("icon_close")).P(TEXT("IconW"), 16.0f).P(TEXT("IconH"), 16.0f).P(TEXT("Tint"), TEXT("TextSecondary")))));
		return Root;
	}

	// -- the kit tooltip pill (S3c) - a small pill UFoundryUIWidgetBase::ApplyKitTooltips/SetKitToolTip
	// wrap plain tooltip text in. Never reparentable (like FriendRow/InviteRow/FriendMenu) - always
	// UFoundryTooltip itself.
	FFoundryWidgetSpec TooltipSpec()
	{
		FFoundryWidgetSpec Root{ TEXT("Border") };
		Root.P(TEXT("Fill"), TEXT("SurfaceHighest")).P(TEXT("HalfHeight"), TEXT("1"));
		Root.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("LabelText"), true }
			.P(TEXT("FontSize"), 13.0f).P(TEXT("Color"), TEXT("Text")).P(TEXT("SlotPad"), TEXT("12,6,12,6")));
		return Root;
	}
}

UFoundryUIBuildCommandlet::UFoundryUIBuildCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UFoundryUIBuildCommandlet::Main(const FString& Params)
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamsMap;
	ParseCommandLine(*Params, Tokens, Switches, ParamsMap);

	const bool bDump = Switches.Contains(TEXT("dump"));
	const bool bVerifyIcons = Switches.Contains(TEXT("verifyicons"));
	FString OutRoot = TEXT("/FoundryFSDK/UI");
	if (const FString* OutParam = ParamsMap.Find(TEXT("out")))
	{
		OutRoot = *OutParam;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("FoundryFSDK"));
	if (!Plugin.IsValid())
	{
		UE_LOG(LogFoundryUIBuild, Error, TEXT("FoundryFSDK plugin not found - cannot resolve its Content directory."));
		return 1;
	}
	const FString ContentDir = Plugin->GetContentDir();

	// -profile: a game's own skinned copies, generated into its own project. Loaded BEFORE the
	// icon import so a bad/missing profile file aborts the whole run early (icons/WBPs always
	// build together).
	FFoundryUIProfile Profile;
	bool bHasProfile = false;
	if (const FString* ProfileParam = ParamsMap.Find(TEXT("profile")))
	{
		if (!LoadProfile(*ProfileParam, UFoundryUISettings::Get()->Palette, Profile))
		{
			return 1;
		}
		bHasProfile = true;
		UE_LOG(LogFoundryUIBuild, Log, TEXT("loaded profile '%s'"), **ProfileParam);
	}

	bool bAnySaveFailed = false;
	const TMap<FString, UTexture2D*> Icons = ImportIcons(ContentDir, OutRoot, bAnySaveFailed);
	UE_LOG(LogFoundryUIBuild, Log, TEXT("imported %d icon(s)"), Icons.Num());
	if (bVerifyIcons)
	{
		VerifyIcons(Icons);
	}

	const FFoundryUIPalette Palette = bHasProfile ? Profile.Palette : UFoundryUISettings::Get()->Palette;
	const FFoundryUIRadii Radii = bHasProfile ? Profile.Radii : FFoundryUIRadii();
	const FFoundryUIFonts Fonts = bHasProfile ? Profile.Fonts : FFoundryUIFonts();

	// Without a profile the social path/prefix/parent are byte-identical to before -profile
	// existed. WITH one, "out" is the Social WBPs' destination DIRECTLY (not OutRoot/Social - a
	// profile names exactly where it wants WBP_LobbyDrawer etc. to land), and "prefix" replaces
	// the WBP_Foundry* names with <prefix>Drawer/<prefix>FriendRow/<prefix>InviteRow/<prefix>FriendMenu.
	const FString SocialPath = (bHasProfile && !Profile.Out.IsEmpty()) ? Profile.Out : (OutRoot / TEXT("Social"));
	const FString Prefix = (bHasProfile && !Profile.Prefix.IsEmpty()) ? Profile.Prefix : TEXT("WBP_Foundry");
	const FString DrawerAssetName = bHasProfile ? (Prefix + TEXT("Drawer")) : TEXT("WBP_FoundrySocialDrawer");
	const FString FriendRowAssetName = bHasProfile ? (Prefix + TEXT("FriendRow")) : TEXT("WBP_FoundryFriendRow");
	const FString InviteRowAssetName = bHasProfile ? (Prefix + TEXT("InviteRow")) : TEXT("WBP_FoundryInviteRow");
	const FString FriendMenuAssetName = bHasProfile ? (Prefix + TEXT("FriendMenu")) : TEXT("WBP_FoundryFriendMenu");
	const FString PartyNameplateAssetName = bHasProfile ? (Prefix + TEXT("Nameplate")) : TEXT("WBP_FoundryPartyNameplate");

	// Friend row / invite row / friend menu always parent to the plugin's OWN classes - only the
	// drawer and the party nameplate are reparentable, and only when a profile names a class that
	// actually resolves.
	UClass* DrawerParentClass = UFoundrySocialDrawer::StaticClass();
	if (bHasProfile && !Profile.DrawerParent.IsEmpty())
	{
		UClass* Loaded = StaticLoadClass(UFoundrySocialDrawer::StaticClass(), nullptr, *Profile.DrawerParent);
		if (Loaded != nullptr)
		{
			DrawerParentClass = Loaded;
		}
		else
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile drawerParent '%s' failed to resolve - using UFoundrySocialDrawer"), *Profile.DrawerParent);
		}
	}
	UClass* NameplateParentClass = UFoundryPartyNameplate::StaticClass();
	if (bHasProfile && !Profile.NameplateParent.IsEmpty())
	{
		UClass* Loaded = StaticLoadClass(UFoundryPartyNameplate::StaticClass(), nullptr, *Profile.NameplateParent);
		if (Loaded != nullptr)
		{
			NameplateParentClass = Loaded;
		}
		else
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile nameplateParent '%s' failed to resolve - using UFoundryPartyNameplate"), *Profile.NameplateParent);
		}
	}

	// Menus (S3a): a profile's "menus": {out, prefix} is optional and falls back to the SOCIAL
	// out/prefix (already resolved above, defaults and all) when absent - so a profile with no
	// "menus" object at all gets its pause menu / confirm dialog built right alongside the drawer.
	// Without a profile, the plugin's own default lands under OutRoot/Menus (distinct from
	// OutRoot/Social), named like every other plugin-default asset.
	const FString MenusPath = bHasProfile ? (!Profile.MenusOut.IsEmpty() ? Profile.MenusOut : SocialPath) : (OutRoot / TEXT("Menus"));
	const FString MenusPrefix = bHasProfile ? (!Profile.MenusPrefix.IsEmpty() ? Profile.MenusPrefix : Prefix) : TEXT("WBP_Foundry");
	const FString PauseMenuAssetName = bHasProfile ? (MenusPrefix + TEXT("PauseMenu")) : TEXT("WBP_FoundryPauseMenu");
	const FString ConfirmDialogAssetName = bHasProfile ? (MenusPrefix + TEXT("ConfirmDialog")) : TEXT("WBP_FoundryConfirmDialog");
	const FString SettingsMenuAssetName = bHasProfile ? (MenusPrefix + TEXT("SettingsMenu")) : TEXT("WBP_FoundrySettingsMenu");
	const FString KeybindRowAssetName = bHasProfile ? (MenusPrefix + TEXT("KeybindRow")) : TEXT("WBP_FoundryKeybindRow");
	// Never reparentable (like FriendRow/InviteRow/FriendMenu) - always UFoundryTooltip itself.
	const FString TooltipAssetName = bHasProfile ? (MenusPrefix + TEXT("Tooltip")) : TEXT("WBP_FoundryTooltip");

	UClass* PauseParentClass = UFoundryPauseMenu::StaticClass();
	if (bHasProfile && !Profile.PauseParent.IsEmpty())
	{
		UClass* Loaded = StaticLoadClass(UFoundryPauseMenu::StaticClass(), nullptr, *Profile.PauseParent);
		if (Loaded != nullptr)
		{
			PauseParentClass = Loaded;
		}
		else
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile pauseParent '%s' failed to resolve - using UFoundryPauseMenu"), *Profile.PauseParent);
		}
	}
	UClass* ConfirmParentClass = UFoundryConfirmDialog::StaticClass();
	if (bHasProfile && !Profile.ConfirmParent.IsEmpty())
	{
		UClass* Loaded = StaticLoadClass(UFoundryConfirmDialog::StaticClass(), nullptr, *Profile.ConfirmParent);
		if (Loaded != nullptr)
		{
			ConfirmParentClass = Loaded;
		}
		else
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile confirmParent '%s' failed to resolve - using UFoundryConfirmDialog"), *Profile.ConfirmParent);
		}
	}
	UClass* SettingsParentClass = UFoundrySettingsMenu::StaticClass();
	if (bHasProfile && !Profile.SettingsParent.IsEmpty())
	{
		UClass* Loaded = StaticLoadClass(UFoundrySettingsMenu::StaticClass(), nullptr, *Profile.SettingsParent);
		if (Loaded != nullptr)
		{
			SettingsParentClass = Loaded;
		}
		else
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile settingsParent '%s' failed to resolve - using UFoundrySettingsMenu"), *Profile.SettingsParent);
		}
	}
	UClass* KeybindRowParentClass = UFoundryKeybindRow::StaticClass();
	if (bHasProfile && !Profile.KeybindRowParent.IsEmpty())
	{
		UClass* Loaded = StaticLoadClass(UFoundryKeybindRow::StaticClass(), nullptr, *Profile.KeybindRowParent);
		if (Loaded != nullptr)
		{
			KeybindRowParentClass = Loaded;
		}
		else
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("profile keybindRowParent '%s' failed to resolve - using UFoundryKeybindRow"), *Profile.KeybindRowParent);
		}
	}

	// Two separate buffers - so a -dump that lands the menus WBPs in their own folder gets a
	// TREES.md listing ONLY the menus widgets there, and the social folder's TREES.md lists ONLY
	// the social ones. When the two paths are the SAME folder, both buffers are written into one
	// file (below), so that case still reads as a single complete dump.
	FString SocialDump;
	FString MenusDump;

	bAnySaveFailed |= !BuildWBP(SocialPath, *FriendRowAssetName, UFoundryFriendRow::StaticClass(), FriendRowSpec(), Palette, Radii, Fonts, Icons, SocialDump);
	bAnySaveFailed |= !BuildWBP(SocialPath, *InviteRowAssetName, UFoundryInviteRow::StaticClass(), InviteRowSpec(), Palette, Radii, Fonts, Icons, SocialDump);
	bAnySaveFailed |= !BuildWBP(SocialPath, *FriendMenuAssetName, UFoundryFriendMenu::StaticClass(), FriendMenuSpec(), Palette, Radii, Fonts, Icons, SocialDump);
	bAnySaveFailed |= !BuildWBP(SocialPath, *PartyNameplateAssetName, NameplateParentClass, PartyNameplateSpec(), Palette, Radii, Fonts, Icons, SocialDump);
	bAnySaveFailed |= !BuildWBP(SocialPath, *DrawerAssetName, DrawerParentClass, SocialDrawerSpec(Radii), Palette, Radii, Fonts, Icons, SocialDump);
	bAnySaveFailed |= !BuildWBP(MenusPath, *PauseMenuAssetName, PauseParentClass, PauseMenuSpec(Radii), Palette, Radii, Fonts, Icons, MenusDump);
	bAnySaveFailed |= !BuildWBP(MenusPath, *ConfirmDialogAssetName, ConfirmParentClass, ConfirmDialogSpec(Radii), Palette, Radii, Fonts, Icons, MenusDump);
	bAnySaveFailed |= !BuildWBP(MenusPath, *KeybindRowAssetName, KeybindRowParentClass, KeybindRowSpec(), Palette, Radii, Fonts, Icons, MenusDump);
	bAnySaveFailed |= !BuildWBP(MenusPath, *SettingsMenuAssetName, SettingsParentClass, SettingsMenuSpec(Radii), Palette, Radii, Fonts, Icons, MenusDump);
	bAnySaveFailed |= !BuildWBP(MenusPath, *TooltipAssetName, UFoundryTooltip::StaticClass(), TooltipSpec(), Palette, Radii, Fonts, Icons, MenusDump);

	if (bHasProfile)
	{
		// Printed unconditionally (not gated on -dump) - this is the thing a profile run exists to
		// save someone from hand-deriving: paste it into the project's DefaultGame.ini.
		const FString Ini = BuildIniBlock(SocialPath, Prefix, MenusPath, MenusPrefix, Palette);
		UE_LOG(LogFoundryUIBuild, Log, TEXT("-- paste into DefaultGame.ini --\n%s"), *Ini);
	}

	if (bDump)
	{
		UE_LOG(LogFoundryUIBuild, Log, TEXT("%s%s"), *SocialDump, *MenusDump);

		// A profile's "out" is a project content path (e.g. "/Game/UI/Lobby"), not the plugin's own
		// Content dir - resolve it to its mounted disk directory; without a profile, TREES.md still
		// lands in the plugin's own Content/UI/Social (or .../Menus), exactly as before -profile
		// existed.
		auto ResolveTreesDir = [&](const FString& PackagePath, const TCHAR* PluginSubdir) -> FString
		{
			if (!bHasProfile)
			{
				return ContentDir / PluginSubdir;
			}
			FString Resolved;
			if (FPackageName::TryConvertLongPackageNameToFilename(PackagePath + TEXT("/"), Resolved))
			{
				return Resolved;
			}
			UE_LOG(LogFoundryUIBuild, Error, TEXT("could not resolve '%s' to a disk path - TREES.md not written"), *PackagePath);
			return FString();
		};
		auto WriteTreesFile = [&](const FString& Dir, const FString& Content) -> bool
		{
			const FString TreesPath = Dir / TEXT("TREES.md");
			if (FFileHelper::SaveStringToFile(Content, *TreesPath))
			{
				UE_LOG(LogFoundryUIBuild, Log, TEXT("wrote %s"), *TreesPath);
				return true;
			}
			UE_LOG(LogFoundryUIBuild, Error, TEXT("FAILED to write %s"), *TreesPath);
			return false;
		};

		const FString TreesDir = ResolveTreesDir(SocialPath, TEXT("UI/Social"));
		const FString MenusTreesDir = ResolveTreesDir(MenusPath, TEXT("UI/Menus"));

		if (MenusTreesDir == TreesDir)
		{
			// The menus WBPs share the social folder - one file, both sections (still the complete
			// dump for that folder).
			bAnySaveFailed |= TreesDir.IsEmpty() || !WriteTreesFile(TreesDir, SocialDump + MenusDump);
		}
		else
		{
			bAnySaveFailed |= TreesDir.IsEmpty() || !WriteTreesFile(TreesDir, SocialDump);
			bAnySaveFailed |= MenusTreesDir.IsEmpty() || !WriteTreesFile(MenusTreesDir, MenusDump);
		}
	}

	UE_LOG(LogFoundryUIBuild, Log, TEXT("FoundryUIBuild complete."));
	return bAnySaveFailed ? 1 : 0;
}
