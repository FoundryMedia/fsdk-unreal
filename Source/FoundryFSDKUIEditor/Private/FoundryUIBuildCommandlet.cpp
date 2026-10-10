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
//   Container props (read for EACH child that doesn't give its own HAlign/VAlign):
//     ChildHAlign, ChildVAlign
//   Content props (per node Type):
//     SizeBox:          W, H (float overrides; axis omitted = no override)
//     Border:           Fill (palette token), Radius (float) or HalfHeight="1", OutlineColor,
//                        OutlineWidth (default: OutlineColor=Fill at alpha 0, OutlineWidth=0)
//     Button:            Normal, Hovered, Pressed, Disabled (palette token or "Transparent";
//                        unspecified state defaults to the previous one, so the style is still
//                        explicitly assigned on all four fields), Radius/HalfHeight as Border
//     Image (icon):      Icon (basename under Content/UI/Icons/src, no extension), IconW, IconH
//                        (default 24 each), Tint (default: no override, stays white - the runtime
//                        tints it, e.g. Avatar)
//     Image (shape):      Fill, Radius/HalfHeight, OutlineColor, OutlineWidth, W, H - same as
//                        Border's brush, used when no Icon is given (e.g. PresenceDot)
//     TextBlock:         Text (literal; empty for a field the runtime fills), FontSize, Bold="1",
//                        Color, Wrap="1"
//     EditableTextBox:   Hint, FontSize, Color (backgrounds are always fully transparent)
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
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/Texture2D.h"
#include "Factories/TextureFactory.h"
#include "FoundryFriendMenu.h"
#include "FoundryFriendRow.h"
#include "FoundryInviteRow.h"
#include "FoundrySocialDrawer.h"
#include "FoundryUISettings.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
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
		UE_LOG(LogFoundryUIBuild, Warning, TEXT("unknown palette token '%s' - defaulting to Text"), *Token);
		return P.Text;
	}

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

			UAssetImportTask* Task = NewObject<UAssetImportTask>();
			Task->Filename = FullSrc;
			Task->DestinationPath = DestPath;
			Task->DestinationName = BaseName;
			Task->bReplaceExisting = true;
			Task->bReplaceExistingSettings = true;
			Task->bAutomated = true;
			Task->bSave = false;
			Task->bAsync = false;
			Task->Factory = NewObject<UTextureFactory>();

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
			// still in flight can assert.
			Tex->PreEditChange(nullptr);
			Tex->CompressionSettings = TC_EditorIcon;
			Tex->LODGroup = TEXTUREGROUP_UI;
			Tex->MipGenSettings = TMGS_FromTextureGroup;
			Tex->SRGB = true;
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

	// -- generic widget builder ---------------------------------------------------------------------

	UWidget* BuildWidget(UWidgetBlueprint* WBP, const FFoundryWidgetSpec& Spec, const FFoundryUIPalette& Palette, const TMap<FString, UTexture2D*>& Icons)
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
			const FLinearColor Fill = GetColor(TEXT("Fill"), TEXT("Surface"));
			const bool bHalf = GetBool(TEXT("HalfHeight"));
			const float Radius = GetFloat(TEXT("Radius"), 16.0f);
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
			const float Radius = GetFloat(TEXT("Radius"), 16.0f);
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
			Txt->SetFont(RobotoFont(GetFloat(TEXT("FontSize"), 16.0f), GetBool(TEXT("Bold"))));
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
			Style.TextStyle.Font = RobotoFont(GetFloat(TEXT("FontSize"), 16.0f), false);
			const FLinearColor TextColor = GetColor(TEXT("Color"), TEXT("Text"));
			Style.TextStyle.ColorAndOpacity = FSlateColor(TextColor);
			Style.ForegroundColor = FSlateColor(TextColor);
			Style.Padding = FMargin(0.0f);
			Box->SetWidgetStyle(Style);
			NewWidget = Box;
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

		if (UPanelWidget* Panel = Cast<UPanelWidget>(NewWidget))
		{
			for (const FFoundryWidgetSpec& ChildSpec : Spec.Children)
			{
				UWidget* ChildWidget = BuildWidget(WBP, ChildSpec, Palette, Icons);
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
		const FFoundryUIPalette& Palette, const TMap<FString, UTexture2D*>& Icons, FString& OutDump)
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

		UWidget* Root = BuildWidget(WBP, RootSpec, Palette, Icons);
		WBP->WidgetTree->RootWidget = Root;

		FKismetEditorUtilities::CompileBlueprint(WBP);
		const bool bSaved = SaveAsset(WBP);

		OutDump += FString::Printf(TEXT("%s%s"), AssetName, LINE_TERMINATOR);
		OutDump += DumpNode(RootSpec, 0);
		OutDump += LINE_TERMINATOR;

		UE_LOG(LogFoundryUIBuild, Log, TEXT("built %s"), AssetName);
		return bSaved;
	}

	// -- the four default social-drawer trees (section 7 of the S1b spec; bound names == the
	// parents' BindWidgetOptional members verbatim) -----------------------------------------------

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
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("NameText"), true }
					.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")))
				.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("StatusText"), true }
					.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Online")))
				.C(FFoundryWidgetSpec{ TEXT("Border"), TEXT("HostBadge"), true }
					.P(TEXT("Fill"), TEXT("Tertiary")).P(TEXT("HalfHeight"), TEXT("1")).P(TEXT("HAlign"), TEXT("Left"))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
						.P(TEXT("Text"), TEXT("Host")).P(TEXT("FontSize"), 12.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
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
				.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")).P(TEXT("Size"), TEXT("Fill")))
			.C(FFoundryWidgetSpec{ TEXT("Image"), ChevronName, true }
				.P(TEXT("Icon"), TEXT("icon_chevron_down")).P(TEXT("IconW"), 20.0f).P(TEXT("IconH"), 20.0f).P(TEXT("Tint"), TEXT("Text"))));
	}

	FFoundryWidgetSpec SocialDrawerSpec()
	{
		FFoundryWidgetSpec Root;
		Root.Type = TEXT("Border");
		Root.P(TEXT("Fill"), TEXT("Surface")).P(TEXT("Radius"), 24.0f);

		FFoundryWidgetSpec Column{ TEXT("VerticalBox") };
		Column.P(TEXT("SlotPad"), TEXT("24,24,24,24"));

		// -- title row --------------------------------------------------------------------------
		Column.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
			.P(TEXT("ChildVAlign"), TEXT("Center"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("FRIENDS")).P(TEXT("FontSize"), 27.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text"))
				.P(TEXT("Size"), TEXT("Fill")))
			.C(FFoundryWidgetSpec{ TEXT("SizeBox") }
				.P(TEXT("W"), 44.0f).P(TEXT("H"), 44.0f)
				.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("CloseButton"), true }
					.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("Hovered"), TEXT("SurfaceHigh")).P(TEXT("HalfHeight"), TEXT("1"))
					.C(FFoundryWidgetSpec{ TEXT("Image") }
						.P(TEXT("Icon"), TEXT("icon_close")).P(TEXT("IconW"), 22.0f).P(TEXT("IconH"), 22.0f).P(TEXT("Tint"), TEXT("Text"))))));

		Column.C(FFoundryWidgetSpec{ TEXT("Spacer") }.P(TEXT("SpacerSize"), 16.0f));

		// -- account block ------------------------------------------------------------------------
		Column.C(FFoundryWidgetSpec{ TEXT("Border") }
			.P(TEXT("Fill"), TEXT("SurfaceLow")).P(TEXT("Radius"), 16.0f)
			.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
				.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("14,12,14,12"))
				.C(FFoundryWidgetSpec{ TEXT("Image"), TEXT("AccountAvatar"), true }
					.P(TEXT("Icon"), TEXT("icon_person")).P(TEXT("IconW"), 48.0f).P(TEXT("IconH"), 48.0f))
				.C(FFoundryWidgetSpec{ TEXT("VerticalBox") }
					.P(TEXT("Size"), TEXT("Fill")).P(TEXT("SlotPad"), TEXT("14,0,0,0")).P(TEXT("Clipping"), TEXT("1"))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("AccountNameText"), true }
						.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("AccountUsernameText"), true }
						.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("TextSecondary"))))
				.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("ManageAccountButton"), true }
					.P(TEXT("Normal"), TEXT("SurfaceHigh")).P(TEXT("HalfHeight"), TEXT("1"))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
						.P(TEXT("Text"), TEXT("Manage account")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Secondary"))
						.P(TEXT("SlotPad"), TEXT("12,6,12,6"))))));

		Column.C(FFoundryWidgetSpec{ TEXT("Spacer") }.P(TEXT("SpacerSize"), 16.0f));

		// -- search frame -----------------------------------------------------------------------
		Column.C(FFoundryWidgetSpec{ TEXT("Border") }
			.P(TEXT("Fill"), TEXT("SurfaceLow")).P(TEXT("Radius"), 16.0f)
			.C(FFoundryWidgetSpec{ TEXT("HorizontalBox") }
				.P(TEXT("ChildVAlign"), TEXT("Center")).P(TEXT("SlotPad"), TEXT("14,10,14,10"))
				.C(FFoundryWidgetSpec{ TEXT("Image") }
					.P(TEXT("Icon"), TEXT("icon_search")).P(TEXT("IconW"), 22.0f).P(TEXT("IconH"), 22.0f).P(TEXT("Tint"), TEXT("TextSecondary")))
				.C(FFoundryWidgetSpec{ TEXT("EditableTextBox"), TEXT("SearchBox"), true }
					.P(TEXT("Hint"), TEXT("Find a friend")).P(TEXT("Color"), TEXT("Text")).P(TEXT("FontSize"), 16.0f).P(TEXT("Size"), TEXT("Fill")))));

		Column.C(FFoundryWidgetSpec{ TEXT("Spacer") }.P(TEXT("SpacerSize"), 16.0f));

		// -- the scrolling list -------------------------------------------------------------------
		FFoundryWidgetSpec Scroll{ TEXT("ScrollBox") };
		Scroll.P(TEXT("Size"), TEXT("Fill")).P(TEXT("BarThickness"), 4.0f);

		Scroll.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("PartyHeaderText"), true }
			.P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")));
		Scroll.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("CapConflictText"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("Error")).P(TEXT("Wrap"), TEXT("1")));
		Scroll.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("PartyList"), true });
		Scroll.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("InvitesSection"), true }
			.P(TEXT("SlotPad"), TEXT("0,16,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("PARTY INVITES")).P(TEXT("FontSize"), 18.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Text")))
			.C(FFoundryWidgetSpec{ TEXT("VerticalBox"), TEXT("InvitesList"), true }));
		Scroll.C(FFoundryWidgetSpec{ TEXT("TextBlock"), TEXT("RequestsNote"), true }
			.P(TEXT("FontSize"), 14.0f).P(TEXT("Color"), TEXT("TextSecondary")).P(TEXT("Wrap"), TEXT("1")).P(TEXT("SlotPad"), TEXT("0,12,0,0")));

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
						.P(TEXT("Icon"), TEXT("icon_person_plus")).P(TEXT("IconW"), 24.0f).P(TEXT("IconH"), 24.0f).P(TEXT("Tint"), TEXT("Surface")))
					.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
						.P(TEXT("Text"), TEXT("ADD FRIEND")).P(TEXT("FontSize"), 16.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Surface"))
						.P(TEXT("SlotPad"), TEXT("10,0,0,0"))))));

		Column.C(FFoundryWidgetSpec{ TEXT("Button"), TEXT("LeaveButton"), true }
			.P(TEXT("Normal"), TEXT("Transparent")).P(TEXT("SlotPad"), TEXT("0,8,0,0"))
			.C(FFoundryWidgetSpec{ TEXT("TextBlock") }
				.P(TEXT("Text"), TEXT("Leave party")).P(TEXT("FontSize"), 14.0f).P(TEXT("Bold"), TEXT("1")).P(TEXT("Color"), TEXT("Error"))));

		Root.C(MoveTemp(Column));
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

	bool bAnySaveFailed = false;
	const TMap<FString, UTexture2D*> Icons = ImportIcons(ContentDir, OutRoot, bAnySaveFailed);
	UE_LOG(LogFoundryUIBuild, Log, TEXT("imported %d icon(s)"), Icons.Num());

	const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
	const FString SocialPath = OutRoot / TEXT("Social");
	FString Dump;

	bAnySaveFailed |= !BuildWBP(SocialPath, TEXT("WBP_FoundryFriendRow"), UFoundryFriendRow::StaticClass(), FriendRowSpec(), Palette, Icons, Dump);
	bAnySaveFailed |= !BuildWBP(SocialPath, TEXT("WBP_FoundryInviteRow"), UFoundryInviteRow::StaticClass(), InviteRowSpec(), Palette, Icons, Dump);
	bAnySaveFailed |= !BuildWBP(SocialPath, TEXT("WBP_FoundryFriendMenu"), UFoundryFriendMenu::StaticClass(), FriendMenuSpec(), Palette, Icons, Dump);
	bAnySaveFailed |= !BuildWBP(SocialPath, TEXT("WBP_FoundrySocialDrawer"), UFoundrySocialDrawer::StaticClass(), SocialDrawerSpec(), Palette, Icons, Dump);

	if (bDump)
	{
		UE_LOG(LogFoundryUIBuild, Log, TEXT("%s"), *Dump);
		const FString TreesPath = ContentDir / TEXT("UI/Social/TREES.md");
		if (FFileHelper::SaveStringToFile(Dump, *TreesPath))
		{
			UE_LOG(LogFoundryUIBuild, Log, TEXT("wrote %s"), *TreesPath);
		}
		else
		{
			UE_LOG(LogFoundryUIBuild, Error, TEXT("FAILED to write %s"), *TreesPath);
			bAnySaveFailed = true;
		}
	}

	UE_LOG(LogFoundryUIBuild, Log, TEXT("FoundryUIBuild complete."));
	return bAnySaveFailed ? 1 : 0;
}
