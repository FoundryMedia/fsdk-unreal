// Copyright Foundry Media. Curated developer console + diagnostics surface.

#include "FoundryConsoleSubsystem.h"

#include "FoundryFSDKSubsystem.h"

#include "Containers/Queue.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "Framework/Application/IInputProcessor.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/OutputDevice.h"
#include "Misc/ScopeLock.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h" // GConfig-backed persisted command history
#include "Misc/Parse.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Text/STextBlock.h"

// ── UE_LOG capture (mirrors selected categories into the scrollback) ────────────

/** Registered with GLog; UE_LOG fires from any thread, so matches are queued and
 *  drained onto the scrollback by a game-thread ticker in the subsystem. */
class FFoundryConsoleLogCapture final : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		if (Verbosity > ELogVerbosity::Log)
		{
			return; // no Verbose/VeryVerbose spam
		}
		{
			FScopeLock Lock(&CategoriesCS);
			if (!Categories.Contains(Category))
			{
				return;
			}
		}
		const TCHAR* Tag =
			(Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Fatal) ? TEXT(" error")
			: Verbosity == ELogVerbosity::Warning ? TEXT(" warning") : TEXT("");
		Pending.Enqueue(FString::Printf(TEXT("[%s%s] %s"), *Category.ToString(), Tag, V));
	}

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	void Add(FName Category) { FScopeLock Lock(&CategoriesCS); Categories.Add(Category); }
	void Remove(FName Category) { FScopeLock Lock(&CategoriesCS); Categories.Remove(Category); }
	bool Contains(FName Category) const { FScopeLock Lock(&CategoriesCS); return Categories.Contains(Category); }
	TArray<FName> GetCategories() const { FScopeLock Lock(&CategoriesCS); return Categories.Array(); }

	TQueue<FString, EQueueMode::Mpsc> Pending;

private:
	mutable FCriticalSection CategoriesCS;
	TSet<FName> Categories;
};

// ── Shared status text (the `status` command + the stats overlay) ───────────────

namespace
{
	FString BuildFoundryStatusText(UGameInstance* GI)
	{
		if (GI == nullptr)
		{
			return TEXT("no game instance");
		}
		TStringBuilder<512> S;
		if (UWorld* World = GI->GetWorld())
		{
			const TCHAR* Mode =
				World->GetNetMode() == NM_Client ? TEXT("client")
				: World->GetNetMode() == NM_ListenServer ? TEXT("listen")
				: World->GetNetMode() == NM_Standalone ? TEXT("standalone") : TEXT("server");
			S.Appendf(TEXT("map %s   net %s\n"), *World->GetMapName(), Mode);
			if (APlayerController* PC = GI->GetFirstLocalPlayerController(World))
			{
				if (PC->PlayerState != nullptr)
				{
					// Compressed ping is ms/4 (UE 5.7 dropped GetPingInMs).
					S.Appendf(TEXT("ping %d ms\n"), int32(PC->PlayerState->GetCompressedPing()) * 4);
				}
			}
		}
		if (UFoundryFSDKSubsystem* Fsdk = GI->GetSubsystem<UFoundryFSDKSubsystem>())
		{
			if (Fsdk->IsLoggedIn())
			{
				const FString Name = Fsdk->GetDisplayName();
				S.Appendf(TEXT("foundry %s\n"), Name.IsEmpty() ? TEXT("(player)") : *Name);
			}
			else
			{
				S.Append(TEXT("foundry signed out\n"));
			}
		}
		if (UFMMSSubsystem* Fmms = GI->GetSubsystem<UFMMSSubsystem>())
		{
			S.Appendf(TEXT("fmms %s"), *UEnum::GetDisplayValueAsText(Fmms->GetPhase()).ToString());
		}
		return FString(S.ToString());
	}
}

// ── The stats overlay Slate widget ──────────────────────────────────────────────

/** Read-only diagnostics panel, refreshed on a short cadence. Shipping-safe: no
 *  console, no STATS system — everything here is plain engine state. */
class SFoundryStatsOverlay : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFoundryStatsOverlay) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UGameInstance>, GameInstance)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		GameInstance = InArgs._GameInstance;
		SetVisibility(EVisibility::HitTestInvisible); // never eats game/UI input

		ChildSlot
		[
			SNew(SBox)
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Top)
			.Padding(FMargin(12.f, 12.f))
			[
				SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FLinearColor(0.f, 0.f, 0.f, 0.55f))
				.Padding(FMargin(10.f, 8.f))
				[
					SNew(STextBlock)
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
					.ColorAndOpacity(FLinearColor(0.75f, 0.95f, 1.f))
					.Text_Lambda([this]() { return CachedText; })
				]
			]
		];
	}

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		SmoothedDelta = (SmoothedDelta <= 0.f) ? InDeltaTime : FMath::Lerp(SmoothedDelta, InDeltaTime, 0.08f);
		if (InCurrentTime >= NextRefresh)
		{
			NextRefresh = InCurrentTime + 0.25;
			CachedText = FText::FromString(BuildText());
		}
	}

private:
	FString BuildText() const
	{
		TStringBuilder<1024> S;
		S.Appendf(TEXT("%s %s (%s)\n"), FApp::GetProjectName(),
			*FString(FApp::GetBuildVersion()), LexToString(FApp::GetBuildConfiguration()));

		const float Fps = (SmoothedDelta > 0.f) ? (1.f / SmoothedDelta) : 0.f;
		S.Appendf(TEXT("FPS %5.1f   frame %5.2f ms\n"), Fps, SmoothedDelta * 1000.f);

		UGameInstance* GI = GameInstance.Get();
		UWorld* World = GI ? GI->GetWorld() : nullptr;
		if (World != nullptr && GI != nullptr)
		{
			if (APlayerController* PC = GI->GetFirstLocalPlayerController(World))
			{
				if (UNetConnection* Conn = PC->GetNetConnection())
				{
					const float InLoss = Conn->InPackets > 0
						? 100.f * float(Conn->InPacketsLost) / float(Conn->InPackets + Conn->InPacketsLost) : 0.f;
					const float OutLoss = Conn->OutPackets > 0
						? 100.f * float(Conn->OutPacketsLost) / float(Conn->OutPackets + Conn->OutPacketsLost) : 0.f;
					S.Appendf(TEXT("pkts in %d (loss %.1f%%)  out %d (loss %.1f%%)\n"),
					          Conn->InPackets, InLoss, Conn->OutPackets, OutLoss);
				}
			}
		}
		S.Append(BuildFoundryStatusText(GI));
		return FString(S.ToString());
	}

	TWeakObjectPtr<UGameInstance> GameInstance;
	FText CachedText;
	double NextRefresh = 0.0;
	float SmoothedDelta = 0.f;
};

// ── The developer console Slate widget (full-screen; view/input only) ───────────

/** Pure view over the subsystem's scrollback + an input line. All echo, masking,
 *  and dispatch live in UFoundryConsoleSubsystem::SubmitLine. */
class SFoundryConsole : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFoundryConsole) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UFoundryConsoleSubsystem>, Owner)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Owner = InArgs._Owner;

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
			.BorderBackgroundColor(FLinearColor(0.01f, 0.02f, 0.04f, 0.92f))
			.Padding(FMargin(16.f, 12.f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot()
				.FillHeight(1.f)
				[
					SAssignNew(ScrollBox, SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(LogText, STextBlock)
						.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
						.ColorAndOpacity(FLinearColor(0.75f, 0.95f, 1.f))
					]
				]
				+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(0.f, 8.f, 0.f, 0.f)
				[
					SAssignNew(InputBox, SEditableTextBox)
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 10))
					.HintText(FText::FromString(TEXT("Type 'help' for commands")))
					.OnTextCommitted(this, &SFoundryConsole::HandleCommitted)
					.OnKeyDownHandler(this, &SFoundryConsole::HandleInputKeyDown)
				]
			]
		];

		if (UFoundryConsoleSubsystem* Sub = Owner.Get())
		{
			LastSerial = Sub->GetScrollbackSerial();
			LogText->SetText(Sub->GetScrollbackText());
		}
	}

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		UFoundryConsoleSubsystem* Sub = Owner.Get();
		if (Sub != nullptr && Sub->GetScrollbackSerial() != LastSerial)
		{
			LastSerial = Sub->GetScrollbackSerial();
			LogText->SetText(Sub->GetScrollbackText());
			ScrollBox->ScrollToEnd();
		}
	}

	void FocusInput()
	{
		// Defer one tick: the toggle key's WM_CHAR arrives AFTER the (swallowed) keydown,
		// and IInputProcessor has no char hook — an immediate focus would type '`' into
		// the box. Unfocused this frame, the stray char routes harmlessly elsewhere.
		RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateSP(this, &SFoundryConsole::FocusInputDeferred));
	}

	/** Masked password capture (`foundry login`): dots in the box, no history/tab. */
	void SetPasswordMode(bool bOn)
	{
		if (InputBox.IsValid())
		{
			InputBox->SetIsPassword(bOn);
			InputBox->SetHintText(FText::FromString(bOn
				? TEXT("Password (input hidden) - Enter to submit, Esc to cancel")
				: TEXT("Type 'help' for commands")));
		}
	}

private:
	EActiveTimerReturnType FocusInputDeferred(double, float)
	{
		if (InputBox.IsValid() && FSlateApplication::IsInitialized())
		{
			FSlateApplication::Get().SetKeyboardFocus(InputBox, EFocusCause::SetDirectly);
		}
		return EActiveTimerReturnType::Stop;
	}

	void HandleCommitted(const FText& Text, ETextCommit::Type CommitType)
	{
		if (CommitType != ETextCommit::OnEnter)
		{
			return;
		}
		const FString Line = Text.ToString();
		InputBox->SetText(FText::GetEmpty());
		HistoryIndex = INDEX_NONE;
		PendingInput.Reset();
		FocusInput(); // keep typing without re-clicking
		if (UFoundryConsoleSubsystem* Sub = Owner.Get())
		{
			Sub->SubmitLine(Line);
		}
	}

	FReply HandleInputKeyDown(const FGeometry& Geometry, const FKeyEvent& KeyEvent)
	{
		const FKey Key = KeyEvent.GetKey();
		if (UFoundryConsoleSubsystem* Sub = Owner.Get(); Sub != nullptr && Sub->IsAwaitingPassword())
		{
			if (Key == EKeys::Escape)
			{
				InputBox->SetText(FText::GetEmpty());
				Sub->CancelPendingLogin();
				return FReply::Handled();
			}
			if (Key == EKeys::Up || Key == EKeys::Down || Key == EKeys::Tab)
			{
				return FReply::Handled(); // no history recall / completion over a password
			}
			return FReply::Unhandled();
		}
		if (Key == EKeys::Up)
		{
			NavigateHistory(-1);
			return FReply::Handled();
		}
		if (Key == EKeys::Down)
		{
			NavigateHistory(+1);
			return FReply::Handled();
		}
		if (Key == EKeys::Tab)
		{
			TabComplete(); // also keeps Tab from walking focus out of the box
			return FReply::Handled();
		}
		return FReply::Unhandled();
	}

	void NavigateHistory(int32 Direction)
	{
		UFoundryConsoleSubsystem* Sub = Owner.Get();
		if (Sub == nullptr)
		{
			return;
		}
		const TArray<FString>& Hist = Sub->GetCommandHistory();
		if (Hist.Num() == 0)
		{
			return;
		}
		if (HistoryIndex == INDEX_NONE)
		{
			if (Direction > 0)
			{
				return; // nothing newer than the live input
			}
			PendingInput = InputBox->GetText().ToString();
			HistoryIndex = Hist.Num() - 1;
		}
		else
		{
			HistoryIndex += Direction;
		}
		if (HistoryIndex >= Hist.Num())
		{
			// walked past the newest entry - restore what was being typed
			HistoryIndex = INDEX_NONE;
			InputBox->SetText(FText::FromString(PendingInput));
			return;
		}
		HistoryIndex = FMath::Max(HistoryIndex, 0);
		InputBox->SetText(FText::FromString(Hist[HistoryIndex]));
	}

	void TabComplete()
	{
		UFoundryConsoleSubsystem* Sub = Owner.Get();
		if (Sub == nullptr)
		{
			return;
		}
		TArray<FString> Candidates;
		const FString Current = InputBox->GetText().ToString();
		const FString NewLine = Sub->CompleteLine(Current, Candidates);
		if (Candidates.Num() > 1)
		{
			Sub->Print(FString::Join(Candidates, TEXT("   ")));
		}
		if (NewLine != Current)
		{
			InputBox->SetText(FText::FromString(NewLine));
		}
	}

	TWeakObjectPtr<UFoundryConsoleSubsystem> Owner;
	TSharedPtr<SScrollBox> ScrollBox;
	TSharedPtr<STextBlock> LogText;
	TSharedPtr<SEditableTextBox> InputBox;
	FString PendingInput;
	int32 HistoryIndex = INDEX_NONE;
	uint32 LastSerial = 0;
};

// ── The net graph (rolling ping + per-interval packet loss) ─────────────────────

/** Draws the plot + samples the connection. Loss is per-SAMPLE (delta of the
 *  connection's running totals), not cumulative-since-connect, so spikes show. */
class SFoundryNetGraphPlot : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SFoundryNetGraphPlot) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UGameInstance>, GameInstance)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		GameInstance = InArgs._GameInstance;
		SetVisibility(EVisibility::HitTestInvisible);
		SetCanTick(true);
	}

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(340.0, 150.0); }

	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override
	{
		SLeafWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
		if (InCurrentTime < NextSample)
		{
			return;
		}
		NextSample = InCurrentTime + SampleInterval;

		float Ping = 0.f, LossIn = 0.f, LossOut = 0.f;
		bHasConnection = false;
		if (UGameInstance* GI = GameInstance.Get())
		{
			if (UWorld* World = GI->GetWorld())
			{
				if (APlayerController* PC = GI->GetFirstLocalPlayerController(World))
				{
					if (PC->PlayerState != nullptr)
					{
						Ping = float(int32(PC->PlayerState->GetCompressedPing()) * 4);
					}
					if (UNetConnection* Conn = PC->GetNetConnection())
					{
						bHasConnection = true;
						// Per-interval deltas of the running totals; a reconnect resets them.
						if (Conn->InPackets < PrevIn || Conn->OutPackets < PrevOut)
						{
							PrevIn = PrevInLost = PrevOut = PrevOutLost = 0;
						}
						const int32 DIn = Conn->InPackets - PrevIn;
						const int32 DInLost = Conn->InPacketsLost - PrevInLost;
						const int32 DOut = Conn->OutPackets - PrevOut;
						const int32 DOutLost = Conn->OutPacketsLost - PrevOutLost;
						PrevIn = Conn->InPackets;   PrevInLost = Conn->InPacketsLost;
						PrevOut = Conn->OutPackets; PrevOutLost = Conn->OutPacketsLost;
						LossIn = (DIn + DInLost) > 0 ? 100.f * float(DInLost) / float(DIn + DInLost) : 0.f;
						LossOut = (DOut + DOutLost) > 0 ? 100.f * float(DOutLost) / float(DOut + DOutLost) : 0.f;
					}
				}
			}
		}
		Push(PingMs, Ping);
		Push(InLossPct, LossIn);
		Push(OutLossPct, LossOut);
	}

	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle,
		bool bParentEnabled) const override
	{
		const FVector2f Size = FVector2f(AllottedGeometry.GetLocalSize());

		FSlateDrawElement::MakeBox(OutDrawElements, LayerId++, AllottedGeometry.ToPaintGeometry(),
			FCoreStyle::Get().GetBrush("WhiteBrush"), ESlateDrawEffect::None, FLinearColor(0.f, 0.f, 0.f, 0.55f));

		float MaxPing = 100.f;
		for (const float P : PingMs)
		{
			MaxPing = FMath::Max(MaxPing, P);
		}
		MaxPing = FMath::CeilToFloat(MaxPing / 50.f) * 50.f;

		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Mono", 9);
		const FString Header = bHasConnection
			? FString::Printf(TEXT("net  ping %3.0f ms   loss in %.1f%%  out %.1f%%"),
				PingMs.Num() ? PingMs.Last() : 0.f,
				InLossPct.Num() ? InLossPct.Last() : 0.f,
				OutLossPct.Num() ? OutLossPct.Last() : 0.f)
			: FString(TEXT("net  (no connection)"));
		FSlateDrawElement::MakeText(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(FVector2f(Size.X - 16.f, 14.f), FSlateLayoutTransform(FVector2f(8.f, 6.f))),
			Header, Font, ESlateDrawEffect::None, FLinearColor(0.75f, 0.95f, 1.f));
		const FString Scale = FString::Printf(TEXT("ping 0-%.0f ms   loss 0-%.0f%%   %.0f s window"),
			MaxPing, LossScaleMaxPct, float(MaxSamples) * SampleInterval);
		FSlateDrawElement::MakeText(OutDrawElements, LayerId,
			AllottedGeometry.ToPaintGeometry(FVector2f(Size.X - 16.f, 12.f), FSlateLayoutTransform(FVector2f(8.f, 21.f))),
			Scale, Font, ESlateDrawEffect::None, FLinearColor(0.45f, 0.55f, 0.65f));

		const float Left = 8.f, Right = Size.X - 8.f, Top = 40.f, Bottom = Size.Y - 8.f;

		const TArray<FVector2f> TopLine = { FVector2f(Left, Top), FVector2f(Right, Top) };
		const TArray<FVector2f> BottomLine = { FVector2f(Left, Bottom), FVector2f(Right, Bottom) };
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			TopLine, ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, 0.12f), false, 1.f);
		FSlateDrawElement::MakeLines(OutDrawElements, LayerId, AllottedGeometry.ToPaintGeometry(),
			BottomLine, ESlateDrawEffect::None, FLinearColor(1.f, 1.f, 1.f, 0.12f), false, 1.f);
		++LayerId;

		DrawSeries(OutDrawElements, AllottedGeometry, LayerId, InLossPct, LossScaleMaxPct,
			Left, Right, Top, Bottom, FLinearColor(1.f, 0.35f, 0.3f), 1.f);
		DrawSeries(OutDrawElements, AllottedGeometry, LayerId, OutLossPct, LossScaleMaxPct,
			Left, Right, Top, Bottom, FLinearColor(1.f, 0.7f, 0.25f), 1.f);
		DrawSeries(OutDrawElements, AllottedGeometry, LayerId, PingMs, MaxPing,
			Left, Right, Top, Bottom, FLinearColor(0.3f, 0.9f, 1.f), 1.5f);

		return LayerId;
	}

private:
	static void Push(TArray<float>& Arr, float Value)
	{
		Arr.Add(Value);
		if (Arr.Num() > MaxSamples)
		{
			Arr.RemoveAt(0);
		}
	}

	static void DrawSeries(FSlateWindowElementList& Out, const FGeometry& Geo, int32& LayerId,
		const TArray<float>& Data, float ScaleMax, float Left, float Right, float Top, float Bottom,
		const FLinearColor& Color, float Thickness)
	{
		if (Data.Num() < 2 || ScaleMax <= 0.f)
		{
			return;
		}
		const float Step = (Right - Left) / float(MaxSamples - 1);
		TArray<FVector2f> Points;
		Points.Reserve(Data.Num());
		for (int32 i = 0; i < Data.Num(); ++i)
		{
			const float X = Right - Step * float(Data.Num() - 1 - i); // newest sample at the right edge
			const float Y = Bottom - FMath::Clamp(Data[i] / ScaleMax, 0.f, 1.f) * (Bottom - Top);
			Points.Add(FVector2f(X, Y));
		}
		FSlateDrawElement::MakeLines(Out, LayerId++, Geo.ToPaintGeometry(),
			Points, ESlateDrawEffect::None, Color, true, Thickness);
	}

	TWeakObjectPtr<UGameInstance> GameInstance;
	TArray<float> PingMs;
	TArray<float> InLossPct;
	TArray<float> OutLossPct;
	int32 PrevIn = 0, PrevInLost = 0, PrevOut = 0, PrevOutLost = 0;
	double NextSample = 0.0;
	bool bHasConnection = false;
	static constexpr int32 MaxSamples = 120;
	static constexpr float SampleInterval = 0.25f;
	static constexpr float LossScaleMaxPct = 20.f;
};

/** Anchors the plot bottom-right of the viewport. */
class SFoundryNetGraph : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SFoundryNetGraph) {}
		SLATE_ARGUMENT(TWeakObjectPtr<UGameInstance>, GameInstance)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		SetVisibility(EVisibility::HitTestInvisible);
		ChildSlot
		[
			SNew(SBox)
			.HAlign(HAlign_Right)
			.VAlign(VAlign_Bottom)
			.Padding(FMargin(12.f))
			[
				SNew(SFoundryNetGraphPlot)
				.GameInstance(InArgs._GameInstance)
			]
		];
	}
};

// ── The key preprocessor (sees input in EVERY input mode, incl. UI-only menus) ──

class FFoundryConsoleKeyProcessor : public IInputProcessor
{
public:
	explicit FFoundryConsoleKeyProcessor(UFoundryConsoleSubsystem* InOwner) : Owner(InOwner) {}

	virtual void Tick(const float DeltaTime, FSlateApplication& SlateApp, TSharedRef<ICursor> Cursor) override {}

	virtual bool HandleKeyDownEvent(FSlateApplication& SlateApp, const FKeyEvent& InKeyEvent) override
	{
		UFoundryConsoleSubsystem* Sub = Owner.Get();
		if (Sub == nullptr || !Sub->IsEnabled() || InKeyEvent.IsRepeat())
		{
			return false;
		}
		const FKey Key = InKeyEvent.GetKey();
		if (Sub->GetConsoleKey().IsValid() && Key == Sub->GetConsoleKey())
		{
			Sub->ToggleConsole();
			return true;
		}
		if (Key == EKeys::Escape && Sub->IsConsoleOpen())
		{
			Sub->ToggleConsole();
			return true;
		}
		if (Sub->GetOverlayKey().IsValid() && Key == Sub->GetOverlayKey())
		{
			Sub->ToggleStatsOverlay();
			return true;
		}
		return false;
	}

private:
	TWeakObjectPtr<UFoundryConsoleSubsystem> Owner;
};

// ── The subsystem ───────────────────────────────────────────────────────────────

bool UFoundryConsoleSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Client-side surface only — no dedicated servers, no commandlets/cooks.
	return Super::ShouldCreateSubsystem(Outer) && !IsRunningDedicatedServer() && !IsRunningCommandlet();
}

void UFoundryConsoleSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency(UFoundryFSDKSubsystem::StaticClass());
	Collection.InitializeDependency(UFMMSSubsystem::StaticClass());

	// The host game pushes its persisted enable flag + keys after startup; the
	// command line can force dev mode on for any build.
	bEnabled = FParse::Param(FCommandLine::Get(), TEXT("DevMode"));
	ConsoleKey = EKeys::Tilde;
	OverlayKey = EKeys::F9;

	RegisterBuiltins();
	LoadPersistedHistory();

	// Mirror the SDK's own log categories into the scrollback; games add theirs
	// via CaptureLogCategory / the `log` command.
	LogCapture = MakeShared<FFoundryConsoleLogCapture>();
	LogCapture->Add(TEXT("LogFoundryFSDK"));
	LogCapture->Add(TEXT("LogFoundryFSDKCore"));
	LogCapture->Add(TEXT("LogFMMS"));
	if (GLog != nullptr)
	{
		GLog->AddOutputDevice(LogCapture.Get());
	}
	LogDrainTicker = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UFoundryConsoleSubsystem::DrainCapturedLogs));

	if (UGameInstance* GI = GetGameInstance())
	{
		if (UFoundryFSDKSubsystem* Fsdk = GI->GetSubsystem<UFoundryFSDKSubsystem>())
		{
			Fsdk->OnLoginComplete.AddDynamic(this, &UFoundryConsoleSubsystem::HandleLoginEvent);
			Fsdk->OnLoggedOut.AddDynamic(this, &UFoundryConsoleSubsystem::HandleLoggedOutEvent);
		}
		if (UFMMSSubsystem* Fmms = GI->GetSubsystem<UFMMSSubsystem>())
		{
			Fmms->OnFMMSStatus.AddDynamic(this, &UFoundryConsoleSubsystem::HandleFmmsStatus);
		}
	}

	if (FSlateApplication::IsInitialized())
	{
		KeyProcessor = MakeShared<FFoundryConsoleKeyProcessor>(this);
		FSlateApplication::Get().RegisterInputPreProcessor(KeyProcessor);
	}
}

void UFoundryConsoleSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(LogDrainTicker);
	if (LogCapture.IsValid() && GLog != nullptr)
	{
		GLog->RemoveOutputDevice(LogCapture.Get());
	}
	LogCapture.Reset();
	if (KeyProcessor.IsValid() && FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().UnregisterInputPreProcessor(KeyProcessor);
	}
	KeyProcessor.Reset();
	CloseConsole();
	RemoveStatsOverlay();
	RemoveNetGraph();
	ConsoleWidget.Reset();
	NetGraphWidget.Reset();
	Super::Deinitialize();
}

void UFoundryConsoleSubsystem::SetEnabled(bool bInEnabled)
{
	bEnabled = bInEnabled;
	if (!bEnabled)
	{
		CloseConsole();
		RemoveStatsOverlay();
		RemoveNetGraph();
	}
}

void UFoundryConsoleSubsystem::ToggleConsole()
{
	if (bConsoleOpen)
	{
		CloseConsole();
		return;
	}
	UGameInstance* GI = GetGameInstance();
	if (GI == nullptr || GI->GetGameViewportClient() == nullptr)
	{
		return;
	}
	if (!ConsoleWidget.IsValid()) // kept alive across open/close so scrollback view state survives
	{
		ConsoleWidget = SNew(SFoundryConsole).Owner(this);
	}
	GI->GetGameViewportClient()->AddViewportWidgetContent(ConsoleWidget.ToSharedRef(), /*ZOrder=*/10002);
	bConsoleOpen = true;
	ConsoleWidget->FocusInput();
}

void UFoundryConsoleSubsystem::CloseConsole()
{
	if (!bConsoleOpen)
	{
		return;
	}
	bConsoleOpen = false;
	CancelPendingLogin(); // an interrupted masked prompt never survives a close
	if (ConsoleWidget.IsValid())
	{
		UGameInstance* GI = GetGameInstance();
		if (GI != nullptr && GI->GetGameViewportClient() != nullptr)
		{
			GI->GetGameViewportClient()->RemoveViewportWidgetContent(ConsoleWidget.ToSharedRef());
		}
	}
}

void UFoundryConsoleSubsystem::CancelPendingLogin()
{
	if (!PendingLoginEmail.IsEmpty())
	{
		PendingLoginEmail.Reset();
		SetWidgetPasswordMode(false);
		Print(TEXT("login cancelled."));
	}
}

void UFoundryConsoleSubsystem::SetWidgetPasswordMode(bool bOn)
{
	if (ConsoleWidget.IsValid())
	{
		ConsoleWidget->SetPasswordMode(bOn);
	}
}

// ── History persistence (shell-style recall across runs) ────────────────────────

void UFoundryConsoleSubsystem::LoadPersistedHistory()
{
	if (GConfig == nullptr)
	{
		return;
	}
	TArray<FString> Persisted;
	GConfig->GetArray(TEXT("FoundryConsole"), TEXT("History"), Persisted, GGameUserSettingsIni);
	if (Persisted.Num() > MaxPersistedHistoryLines)
	{
		Persisted.RemoveAt(0, Persisted.Num() - MaxPersistedHistoryLines);
	}
	History = MoveTemp(Persisted);
}

void UFoundryConsoleSubsystem::SavePersistedHistory() const
{
	if (GConfig == nullptr)
	{
		return;
	}
	TArray<FString> Tail = History;
	if (Tail.Num() > MaxPersistedHistoryLines)
	{
		Tail.RemoveAt(0, Tail.Num() - MaxPersistedHistoryLines);
	}
	GConfig->SetArray(TEXT("FoundryConsole"), TEXT("History"), Tail, GGameUserSettingsIni);
	GConfig->Flush(false, GGameUserSettingsIni);
}

void UFoundryConsoleSubsystem::SetStatsOverlayVisible(bool bVisible)
{
	if (bVisible == IsStatsOverlayVisible())
	{
		return;
	}
	if (!bVisible)
	{
		RemoveStatsOverlay();
		return;
	}
	UGameInstance* GI = GetGameInstance();
	if (GI == nullptr || GI->GetGameViewportClient() == nullptr)
	{
		return;
	}
	StatsOverlayWidget = SNew(SFoundryStatsOverlay).GameInstance(GI);
	GI->GetGameViewportClient()->AddViewportWidgetContent(StatsOverlayWidget.ToSharedRef(), /*ZOrder=*/10000);
}

void UFoundryConsoleSubsystem::RemoveStatsOverlay()
{
	if (StatsOverlayWidget.IsValid())
	{
		UGameInstance* GI = GetGameInstance();
		if (GI != nullptr && GI->GetGameViewportClient() != nullptr)
		{
			GI->GetGameViewportClient()->RemoveViewportWidgetContent(StatsOverlayWidget.ToSharedRef());
		}
		StatsOverlayWidget.Reset();
	}
}

void UFoundryConsoleSubsystem::SetNetGraphVisible(bool bVisible)
{
	if (bVisible == bNetGraphOpen)
	{
		return;
	}
	if (!bVisible)
	{
		RemoveNetGraph();
		return;
	}
	UGameInstance* GI = GetGameInstance();
	if (GI == nullptr || GI->GetGameViewportClient() == nullptr)
	{
		return;
	}
	if (!NetGraphWidget.IsValid()) // kept alive across toggles so the sample window survives
	{
		NetGraphWidget = SNew(SFoundryNetGraph).GameInstance(GI);
	}
	GI->GetGameViewportClient()->AddViewportWidgetContent(NetGraphWidget.ToSharedRef(), /*ZOrder=*/10001);
	bNetGraphOpen = true;
}

void UFoundryConsoleSubsystem::RemoveNetGraph()
{
	if (!bNetGraphOpen)
	{
		return;
	}
	bNetGraphOpen = false;
	if (NetGraphWidget.IsValid())
	{
		UGameInstance* GI = GetGameInstance();
		if (GI != nullptr && GI->GetGameViewportClient() != nullptr)
		{
			GI->GetGameViewportClient()->RemoveViewportWidgetContent(NetGraphWidget.ToSharedRef());
		}
	}
}

void UFoundryConsoleSubsystem::SetConsoleKey(FKey Key)
{
	if (Key.IsValid())
	{
		ConsoleKey = Key;
	}
}

void UFoundryConsoleSubsystem::SetOverlayKey(FKey Key)
{
	if (Key.IsValid())
	{
		OverlayKey = Key;
	}
}

// ── Scrollback ──────────────────────────────────────────────────────────────────

void UFoundryConsoleSubsystem::Print(const FString& Line)
{
	AppendScrollback(Line);
}

void UFoundryConsoleSubsystem::AppendScrollback(const FString& Entry)
{
	TArray<FString> NewLines;
	Entry.ParseIntoArrayLines(NewLines, /*bCullEmpty=*/false);
	Scrollback.Append(NewLines);
	if (Scrollback.Num() > MaxScrollbackLines)
	{
		Scrollback.RemoveAt(0, Scrollback.Num() - MaxScrollbackLines);
	}
	++ScrollbackSerial;
}

void UFoundryConsoleSubsystem::ClearScrollback()
{
	Scrollback.Reset();
	++ScrollbackSerial;
}

FText UFoundryConsoleSubsystem::GetScrollbackText() const
{
	return FText::FromString(FString::Join(Scrollback, TEXT("\n")));
}

void UFoundryConsoleSubsystem::SubmitLine(const FString& RawLine)
{
	if (!PendingLoginEmail.IsEmpty())
	{
		// Masked password line: dispatched straight to login — never echoed, never
		// in history/scrollback, never held as state. Untrimmed (passwords may
		// legitimately carry spaces); an all-whitespace line cancels.
		const FString Email = PendingLoginEmail;
		PendingLoginEmail.Reset();
		SetWidgetPasswordMode(false);
		if (RawLine.TrimStartAndEnd().IsEmpty())
		{
			Print(TEXT("login cancelled."));
			return;
		}
		UGameInstance* GI = GetGameInstance();
		UFoundryFSDKSubsystem* Fsdk = GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
		if (Fsdk == nullptr)
		{
			Print(TEXT("FSDK unavailable."));
			return;
		}
		Print(FString::Printf(TEXT("signing in as %s..."), *Email));
		Fsdk->Login(Email, RawLine, /*bRememberMe=*/true);
		return;
	}
	const FString Line = RawLine.TrimStartAndEnd();
	if (Line.IsEmpty())
	{
		return;
	}
	const FString Echo = SanitizeForEcho(Line);
	AppendScrollback(TEXT("> ") + Echo);
	// History stores the SANITIZED line — a login password is never recallable
	// via arrow keys (deliberate trade: the masked line won't re-run).
	if (History.Num() == 0 || History.Last() != Echo)
	{
		History.Add(Echo);
		if (History.Num() > MaxHistoryLines)
		{
			History.RemoveAt(0);
		}
		SavePersistedHistory();
	}
	const FString Response = Execute(Line);
	if (!Response.IsEmpty())
	{
		AppendScrollback(Response);
	}
}

bool UFoundryConsoleSubsystem::DrainCapturedLogs(float)
{
	if (LogCapture.IsValid())
	{
		FString Line;
		while (LogCapture->Pending.Dequeue(Line))
		{
			AppendScrollback(Line);
		}
	}
	return true; // keep ticking
}

void UFoundryConsoleSubsystem::CaptureLogCategory(FName Category)
{
	if (LogCapture.IsValid() && !Category.IsNone())
	{
		LogCapture->Add(Category);
	}
}

void UFoundryConsoleSubsystem::StopCapturingLogCategory(FName Category)
{
	if (LogCapture.IsValid())
	{
		LogCapture->Remove(Category);
	}
}

TArray<FName> UFoundryConsoleSubsystem::GetCapturedLogCategories() const
{
	return LogCapture.IsValid() ? LogCapture->GetCategories() : TArray<FName>();
}

FString UFoundryConsoleSubsystem::SanitizeForEcho(const FString& Line) const
{
	// Never echo credentials into the scrollback: `foundry login <email> <password>`.
	TArray<FString> Tokens;
	Line.ParseIntoArrayWS(Tokens);
	if (Tokens.Num() > 3
		&& Tokens[0].Equals(TEXT("foundry"), ESearchCase::IgnoreCase)
		&& Tokens[1].Equals(TEXT("login"), ESearchCase::IgnoreCase))
	{
		return FString::Printf(TEXT("foundry login %s ****"), *Tokens[2]);
	}
	return Line;
}

// ── Command registry ────────────────────────────────────────────────────────────

void UFoundryConsoleSubsystem::RegisterCommand(const FString& Name, const FString& Help,
	EFoundryConsoleAccess Access, FFoundryConsoleHandler Handler)
{
	FCommandEntry Entry;
	Entry.Help = Help;
	Entry.Access = Access;
	Entry.Handler = MoveTemp(Handler);
	Commands.Add(Name.ToLower(), MoveTemp(Entry));
}

void UFoundryConsoleSubsystem::RegisterConsoleCommand(const FString& Name, const FString& Help,
	EFoundryConsoleAccess Access, FFoundryConsoleHandlerBP Handler)
{
	RegisterCommand(Name, Help, Access, FFoundryConsoleHandler::CreateLambda(
		[Handler](const TArray<FString>& Args)
		{
			return Handler.IsBound() ? Handler.Execute(Args) : FString();
		}));
}

void UFoundryConsoleSubsystem::UnregisterConsoleCommand(const FString& Name)
{
	Commands.Remove(Name.ToLower());
}

bool UFoundryConsoleSubsystem::IsSignedIn() const
{
	UGameInstance* GI = GetGameInstance();
	UFoundryFSDKSubsystem* Fsdk = GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
	return Fsdk != nullptr && Fsdk->IsLoggedIn();
}

bool UFoundryConsoleSubsystem::IsCommandAvailable(const FCommandEntry& Entry) const
{
	switch (Entry.Access)
	{
	case EFoundryConsoleAccess::SignedIn:  return IsSignedIn();
	case EFoundryConsoleAccess::SignedOut: return !IsSignedIn();
	default:                               return true;
	}
}

bool UFoundryConsoleSubsystem::ParseOnOff(const TArray<FString>& Args, TOptional<bool>& OutValue)
{
	OutValue.Reset();
	if (Args.Num() == 0)
	{
		return true; // no arg = toggle
	}
	const FString A = Args[0].ToLower();
	if (A == TEXT("1") || A == TEXT("on") || A == TEXT("true"))
	{
		OutValue = true;
		return true;
	}
	if (A == TEXT("0") || A == TEXT("off") || A == TEXT("false"))
	{
		OutValue = false;
		return true;
	}
	return false;
}

FString UFoundryConsoleSubsystem::Execute(const FString& Line)
{
	TArray<FString> Tokens;
	Line.ParseIntoArrayWS(Tokens);
	if (Tokens.Num() == 0)
	{
		return FString();
	}

	if (Tokens[0].Equals(TEXT("foundry"), ESearchCase::IgnoreCase))
	{
		if (Tokens.Num() < 2)
		{
			return TEXT("usage: foundry <command> - type 'help' for the list");
		}
		TArray<FString> Args(Tokens.GetData() + 2, Tokens.Num() - 2);
		return Dispatch(FoundryCommands, Tokens[1].ToLower(), Args, TEXT("foundry "));
	}

	TArray<FString> Args(Tokens.GetData() + 1, Tokens.Num() - 1);
	return Dispatch(Commands, Tokens[0].ToLower(), Args, TEXT(""));
}

FString UFoundryConsoleSubsystem::Dispatch(const TMap<FString, FCommandEntry>& Registry, const FString& Name,
	const TArray<FString>& Args, const TCHAR* GroupPrefix)
{
	const FCommandEntry* Entry = Registry.Find(Name);
	if (Entry == nullptr)
	{
		return FString::Printf(TEXT("Unknown command '%s%s'. Type 'help'."), GroupPrefix, *Name);
	}
	if (!IsCommandAvailable(*Entry))
	{
		return Entry->Access == EFoundryConsoleAccess::SignedIn
			? TEXT("requires a signed-in session.")
			: TEXT("only available while signed out.");
	}
	return Entry->Handler.IsBound() ? Entry->Handler.Execute(Args) : FString();
}

namespace
{
	/** Longest case-insensitive common prefix; candidates are canonical lower-case. */
	FString FoundryConsoleCommonPrefix(const TArray<FString>& Strings)
	{
		if (Strings.Num() == 0)
		{
			return FString();
		}
		FString Prefix = Strings[0];
		for (int32 i = 1; i < Strings.Num(); ++i)
		{
			const FString& S = Strings[i];
			int32 j = 0;
			while (j < Prefix.Len() && j < S.Len() && FChar::ToLower(Prefix[j]) == FChar::ToLower(S[j]))
			{
				++j;
			}
			Prefix.LeftInline(j);
		}
		return Prefix;
	}
}

FString UFoundryConsoleSubsystem::CompleteLine(const FString& Current, TArray<FString>& OutCandidates) const
{
	OutCandidates.Reset();
	const FString Trimmed = Current.TrimStartAndEnd();
	const bool bTrailingSpace = Current.TrimStart().EndsWith(TEXT(" "));
	TArray<FString> Tokens;
	Trimmed.ParseIntoArrayWS(Tokens);

	// Completing the FIRST token: bare commands + the `foundry` group keyword.
	if (Tokens.Num() == 0 || (Tokens.Num() == 1 && !bTrailingSpace))
	{
		const FString Prefix = Tokens.Num() ? Tokens[0].ToLower() : FString();
		for (const TPair<FString, FCommandEntry>& Pair : Commands)
		{
			if (IsCommandAvailable(Pair.Value) && Pair.Key.StartsWith(Prefix))
			{
				OutCandidates.Add(Pair.Key);
			}
		}
		if (FString(TEXT("foundry")).StartsWith(Prefix))
		{
			OutCandidates.Add(TEXT("foundry"));
		}
		OutCandidates.Sort();
		if (OutCandidates.Num() == 0)
		{
			return Current;
		}
		if (OutCandidates.Num() == 1)
		{
			const FString Line = OutCandidates[0] + TEXT(" ");
			OutCandidates.Reset(); // unambiguous - nothing to list
			return Line;
		}
		return FoundryConsoleCommonPrefix(OutCandidates);
	}

	// Completing a `foundry` SUBcommand.
	if (Tokens[0].Equals(TEXT("foundry"), ESearchCase::IgnoreCase)
		&& (Tokens.Num() == 1 || (Tokens.Num() == 2 && !bTrailingSpace)))
	{
		const FString Prefix = Tokens.Num() > 1 ? Tokens[1].ToLower() : FString();
		for (const TPair<FString, FCommandEntry>& Pair : FoundryCommands)
		{
			if (IsCommandAvailable(Pair.Value) && Pair.Key.StartsWith(Prefix))
			{
				OutCandidates.Add(Pair.Key);
			}
		}
		OutCandidates.Sort();
		if (OutCandidates.Num() == 0)
		{
			return Current;
		}
		if (OutCandidates.Num() == 1)
		{
			const FString Line = TEXT("foundry ") + OutCandidates[0] + TEXT(" ");
			OutCandidates.Reset();
			return Line;
		}
		return TEXT("foundry ") + FoundryConsoleCommonPrefix(OutCandidates);
	}

	return Current; // command arguments are freeform
}

FString UFoundryConsoleSubsystem::BuildHelpText() const
{
	TArray<FString> Lines;
	TArray<FString> Keys;

	Commands.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const FCommandEntry& Entry = Commands[Key];
		if (IsCommandAvailable(Entry))
		{
			Lines.Add(FString::Printf(TEXT("%-24s %s"), *Key, *Entry.Help));
		}
	}

	Keys.Reset();
	FoundryCommands.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		const FCommandEntry& Entry = FoundryCommands[Key];
		if (IsCommandAvailable(Entry))
		{
			Lines.Add(FString::Printf(TEXT("%-24s %s"), *(TEXT("foundry ") + Key), *Entry.Help));
		}
	}

	return FString::Join(Lines, TEXT("\n"));
}

void UFoundryConsoleSubsystem::RegisterBuiltins()
{
	RegisterCommand(TEXT("help"), TEXT("list available commands"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>&)
		{
			return BuildHelpText();
		}));

	RegisterCommand(TEXT("clear"), TEXT("clear the console"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>&)
		{
			ClearScrollback(); // also removes the echoed 'clear' line
			return FString();
		}));

	RegisterCommand(TEXT("status"), TEXT("one-shot session/network status"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>&)
		{
			return BuildFoundryStatusText(GetGameInstance());
		}));

	RegisterCommand(TEXT("netgraph"), TEXT("[on|off|1|0] toggle the network graph"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>& Args)
		{
			TOptional<bool> Want;
			if (!ParseOnOff(Args, Want))
			{
				return FString(TEXT("usage: netgraph [on|off|1|0]"));
			}
			SetNetGraphVisible(Want.IsSet() ? *Want : !bNetGraphOpen);
			return FString(bNetGraphOpen ? TEXT("netgraph: on") : TEXT("netgraph: off"));
		}));

	RegisterCommand(TEXT("stats"), TEXT("[on|off|1|0] toggle the stats overlay"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>& Args)
		{
			TOptional<bool> Want;
			if (!ParseOnOff(Args, Want))
			{
				return FString(TEXT("usage: stats [on|off|1|0]"));
			}
			SetStatsOverlayVisible(Want.IsSet() ? *Want : !IsStatsOverlayVisible());
			return FString(IsStatsOverlayVisible() ? TEXT("stats overlay: on") : TEXT("stats overlay: off"));
		}));

	RegisterCommand(TEXT("log"), TEXT("[Category] [on|off] mirror a UE_LOG category into the console"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>& Args) -> FString
		{
			if (Args.Num() == 0)
			{
				TArray<FName> Cats = GetCapturedLogCategories();
				if (Cats.Num() == 0)
				{
					return TEXT("capturing: (none)");
				}
				Cats.Sort(FNameLexicalLess());
				TArray<FString> Names;
				for (const FName& C : Cats)
				{
					Names.Add(C.ToString());
				}
				return TEXT("capturing: ") + FString::Join(Names, TEXT(", "));
			}
			const FName Category(*Args[0]);
			TOptional<bool> Want;
			const TArray<FString> OnOff(Args.GetData() + 1, Args.Num() - 1);
			if (!ParseOnOff(OnOff, Want))
			{
				return FString(TEXT("usage: log [Category] [on|off]"));
			}
			const bool bCurrent = LogCapture.IsValid() && LogCapture->Contains(Category);
			const bool bNew = Want.IsSet() ? *Want : !bCurrent;
			if (bNew)
			{
				CaptureLogCategory(Category);
			}
			else
			{
				StopCapturingLogCategory(Category);
			}
			return FString::Printf(TEXT("log %s: %s"), *Category.ToString(), bNew ? TEXT("on") : TEXT("off"));
		}));

	// ── The `foundry` group: the FGS surface (a mini foundry CLI in-engine) ─────

	FoundryCommands.Add(TEXT("whoami"), { TEXT("show the signed-in Foundry identity"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>&) -> FString
		{
			UGameInstance* GI = GetGameInstance();
			UFoundryFSDKSubsystem* Fsdk = GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
			if (Fsdk == nullptr || !Fsdk->IsLoggedIn())
			{
				return TEXT("signed out.");
			}
			const FString Name = Fsdk->GetDisplayName();
			const FString Id = Fsdk->GetFoundryId();
			return FString::Printf(TEXT("signed in as %s%s"),
				Name.IsEmpty() ? TEXT("(player)") : *Name,
				Id.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Id));
		}) });

	FoundryCommands.Add(TEXT("login"), { TEXT("<email> sign in (password prompted, input hidden)"), EFoundryConsoleAccess::SignedOut,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>& Args) -> FString
		{
#if !FOUNDRY_FSDK_FID_AUTH
			(void)Args;
			return TEXT("in-game login is not compiled into this build - sign in via the Foundry launcher.");
#else
			if (Args.Num() > 1)
			{
				// An inline password was VISIBLE while typed - refuse it outright.
				return TEXT("password must not be typed inline - use: foundry login <email> (a hidden prompt follows)");
			}
			if (Args.Num() != 1 || Args[0].IsEmpty())
			{
				return TEXT("usage: foundry login <email>   (password prompted, input hidden)");
			}
			UGameInstance* GI = GetGameInstance();
			UFoundryFSDKSubsystem* Fsdk = GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
			if (Fsdk == nullptr)
			{
				return TEXT("FSDK unavailable.");
			}
			PendingLoginEmail = Args[0];
			SetWidgetPasswordMode(true);
			return FString::Printf(TEXT("password for %s: (input hidden - Enter to submit, Esc to cancel)"), *Args[0]);
#endif
		}) });

	FoundryCommands.Add(TEXT("resume"), { TEXT("resume the saved session (no password)"), EFoundryConsoleAccess::SignedOut,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>&) -> FString
		{
			UGameInstance* GI = GetGameInstance();
			UFoundryFSDKSubsystem* Fsdk = GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
			if (Fsdk == nullptr)
			{
				return TEXT("FSDK unavailable.");
			}
			Fsdk->TryResumeSession();
			return TEXT("resuming saved session...");
		}) });

	FoundryCommands.Add(TEXT("logout"), { TEXT("sign out + clear the saved session"), EFoundryConsoleAccess::SignedIn,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>&) -> FString
		{
			UGameInstance* GI = GetGameInstance();
			UFoundryFSDKSubsystem* Fsdk = GI ? GI->GetSubsystem<UFoundryFSDKSubsystem>() : nullptr;
			if (Fsdk == nullptr)
			{
				return TEXT("FSDK unavailable.");
			}
			Fsdk->Logout();
			return TEXT("logging out...");
		}) });

	FoundryCommands.Add(TEXT("findmatch"), { TEXT("<queue> queue for a match"), EFoundryConsoleAccess::SignedIn,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>& Args) -> FString
		{
			if (Args.Num() < 1)
			{
				return TEXT("usage: foundry findmatch <queue>   (e.g. conquest/solo)");
			}
			UGameInstance* GI = GetGameInstance();
			UFMMSSubsystem* Fmms = GI ? GI->GetSubsystem<UFMMSSubsystem>() : nullptr;
			if (Fmms == nullptr)
			{
				return TEXT("FMMS unavailable.");
			}
			Fmms->FindMatchAuthenticated(Args[0], TEXT("{}"));
			return FString::Printf(TEXT("finding match in '%s'..."), *Args[0]);
		}) });

	FoundryCommands.Add(TEXT("cancel"), { TEXT("cancel matchmaking"), EFoundryConsoleAccess::Any,
		FFoundryConsoleHandler::CreateLambda([this](const TArray<FString>&) -> FString
		{
			UGameInstance* GI = GetGameInstance();
			UFMMSSubsystem* Fmms = GI ? GI->GetSubsystem<UFMMSSubsystem>() : nullptr;
			if (Fmms == nullptr)
			{
				return TEXT("FMMS unavailable.");
			}
			Fmms->Cancel();
			return TEXT("matchmaking cancelled.");
		}) });
}

// ── Session events -> scrollback (the lines that used to be screen messages) ────

void UFoundryConsoleSubsystem::HandleLoginEvent(EFoundryFsdkResult Result, const FString& DisplayName)
{
	if (Result == EFoundryFsdkResult::Ok)
	{
		Print(FString::Printf(TEXT("signed in as %s."),
			DisplayName.IsEmpty() ? TEXT("(player)") : *DisplayName));
	}
	else if (Result == EFoundryFsdkResult::NotImplemented)
	{
		Print(TEXT("sign-in failed: in-game login is not compiled into this build - sign in via the Foundry launcher."));
	}
	else
	{
		Print(FString::Printf(TEXT("sign-in failed: %s"), *UEnum::GetValueAsString(Result)));
	}
}

void UFoundryConsoleSubsystem::HandleLoggedOutEvent()
{
	Print(TEXT("logged out."));
}

void UFoundryConsoleSubsystem::HandleFmmsStatus(EFMMSPhase Phase, const FString& Message)
{
	Print(FString::Printf(TEXT("[fmms] %s"), *Message));
}
