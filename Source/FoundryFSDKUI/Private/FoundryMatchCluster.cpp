#include "FoundryMatchCluster.h"

#include "Animation/WidgetAnimation.h"
#include "Components/Button.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "FoundryFSDKUILog.h"
#include "FoundryPartyRules.h"
#include "FoundrySocialSubsystem.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr float ModeMenuSeconds = 0.15f;

	/**
	 * The Find Match gate for the party the local player is in, measured against the SELECTED queue's game:
	 * ready is per game, and the queue key's prefix is the slug fid checks every member's ready flag against.
	 * Ungated (solo, no party, no queue yet) answers a default FFindMatchGate.
	 */
	FoundryPartyRules::FFindMatchGate GateFor(const UFoundrySocialSubsystem* S, const FString& QueueKey)
	{
		FoundryPartyRules::FFindMatchGateInput In;
		if (S && S->HasParty() && !QueueKey.IsEmpty())
		{
			const FString Slug = FoundryPartyRules::GameSlugOfQueue(QueueKey);
			In.Joined = S->JoinedCount();
			In.bLocalLeader = S->IsLocalLeader();
			In.bAllReady = S->AllReadyIn(Slug);
			In.UnreadyNames = S->UnreadyNames(Slug);
			In.LeaderName = S->LeaderDisplayName();
		}
		return FoundryPartyRules::PlanFindMatch(In);
	}

	/** A joined member of a party of 2+ who does not lead it: its search is the leader's, followed. */
	bool IsFollower(const UFoundrySocialSubsystem* S)
	{
		return S && S->HasParty() && S->JoinedCount() >= 2 && !S->IsLocalLeader();
	}

	/**
	 * The ticket attributes the cluster submits: the mode's own JSON object (empty -> {}) with the
	 * party id merged in as "partyId" when the player is in a party. A mode JSON that does not parse
	 * as an object is replaced by a fresh one when there is a party id to carry (the party matters
	 * more than a malformed config), and passed through untouched otherwise.
	 */
	FString MergePartyId(const FString& ModeJson, const FString& PartyId)
	{
		if (PartyId.IsEmpty())
		{
			return ModeJson.IsEmpty() ? FString(TEXT("{}")) : ModeJson;
		}

		TSharedPtr<FJsonObject> Object;
		if (!ModeJson.IsEmpty())
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ModeJson);
			if (!FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
			{
				UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] A matchmaking mode's AttributesJson is not a JSON object - submitting only the party id."));
				Object.Reset();
			}
		}
		if (!Object.IsValid())
		{
			Object = MakeShared<FJsonObject>();
		}
		Object->SetStringField(TEXT("partyId"), PartyId);

		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		return Out;
	}
}

UFMMSSubsystem* UFoundryMatchCluster::Fmms() const
{
	UFMMSSubsystem* Cached = FmmsWeak.Get();
	if (Cached)
	{
		return Cached;
	}
	const UGameInstance* GI = GetGameInstance();
	UFMMSSubsystem* Found = GI ? GI->GetSubsystem<UFMMSSubsystem>() : nullptr;
	FmmsWeak = Found;
	return Found;
}

UFoundrySocialSubsystem* UFoundryMatchCluster::Social() const
{
	UFoundrySocialSubsystem* Cached = SocialWeak.Get();
	if (Cached)
	{
		return Cached;
	}
	const UGameInstance* GI = GetGameInstance();
	UFoundrySocialSubsystem* Found = GI ? GI->GetSubsystem<UFoundrySocialSubsystem>() : nullptr;
	SocialWeak = Found;
	return Found;
}

TArray<FFoundryMatchmakingMode> UFoundryMatchCluster::GetModes_Implementation() const
{
	return UFoundryUISettings::Get()->MatchmakingModes;
}

void UFoundryMatchCluster::NativeOnInitialized()
{
	Super::NativeOnInitialized(); // hand cursor + kit tooltips
	if (FindMatchButton)
	{
		FindMatchButton->OnClicked.AddDynamic(this, &UFoundryMatchCluster::HandleFindMatch);
	}
	if (CancelButton)
	{
		CancelButton->OnClicked.AddDynamic(this, &UFoundryMatchCluster::HandleCancel);
	}
	if (LeaveMatchButton)
	{
		LeaveMatchButton->OnClicked.AddDynamic(this, &UFoundryMatchCluster::HandleLeaveMatch);
	}
	if (ModeButton)
	{
		ModeButton->OnClicked.AddDynamic(this, &UFoundryMatchCluster::HandleModeButton);
	}
	// Pivot once, in place - the code spin and the chevron tween then turn about their own centres.
	if (Spinner) { Spinner->SetRenderTransformPivot(FVector2D(0.5f, 0.5f)); }
	if (ModeChevron) { ModeChevron->SetRenderTransformPivot(FVector2D(0.5f, 0.5f)); }
	Show(ModeMenu, false);
	Show(CancelButton, false);
}

void UFoundryMatchCluster::NativeConstruct()
{
	if (StatusText && !bStatusColorCaptured)
	{
		StatusNormalColor = StatusText->GetColorAndOpacity(); // the designer's colour
		bStatusColorCaptured = true;
		bDesignerWrapsStatus = StatusText->GetWrapTextAt() > 0.0f || StatusText->GetAutoWrapText();
	}
	Super::NativeConstruct();

	if (UFMMSSubsystem* F = Fmms())
	{
		F->OnMatchmakingStateChanged.AddUObject(this, &UFoundryMatchCluster::HandleFmmsState);
	}
	if (UFoundrySocialSubsystem* S = Social())
	{
		S->RegisterConsumer(); // the party count reads the roster - keep the poller alive for it
		// The ready flag and the Find Match gate are per game. The settings push covers GameSlug / the
		// settings' modes; a game that builds its modes in GetModes has no settings modes, so derive from those.
		UFoundryUISettings::PushGameSlug(S);
		if (UFoundryUISettings::Get()->GameSlug.IsEmpty())
		{
			for (const FFoundryMatchmakingMode& Mode : GetModes())
			{
				if (!Mode.QueueName.IsEmpty())
				{
					S->SetDefaultGameSlugFromQueue(Mode.QueueName);
					break;
				}
			}
		}
		S->OnChanged.AddUObject(this, &UFoundryMatchCluster::Refresh);
		S->OnPartyChanged.AddUObject(this, &UFoundryMatchCluster::Refresh);
	}
	Refresh();
}

void UFoundryMatchCluster::NativeDestruct()
{
	if (UFMMSSubsystem* F = Fmms())
	{
		F->OnMatchmakingStateChanged.RemoveAll(this);
	}
	if (UFoundrySocialSubsystem* S = Social())
	{
		S->OnChanged.RemoveAll(this);
		S->OnPartyChanged.RemoveAll(this);
		S->UnregisterConsumer();
	}
	Super::NativeDestruct();
}

void UFoundryMatchCluster::HandleFmmsState(EFoundryMatchmakingState State, const FString& Message)
{
	Refresh();
}

void UFoundryMatchCluster::ResolveSelectedMode()
{
	if (Modes.IsValidIndex(SelectedModeIndex))
	{
		return;
	}
	SelectedModeIndex = Modes.Num() > 0 ? 0 : INDEX_NONE;
	for (int32 i = 0; i < Modes.Num(); ++i)
	{
		if (!Modes[i].QueueName.IsEmpty())
		{
			SelectedModeIndex = i;
			break;
		}
	}
}

void UFoundryMatchCluster::SyncSelectedQueueKey()
{
	UFoundrySocialSubsystem* S = Social();
	if (!S)
	{
		return;
	}
	// The drawer's invite gate and header measure the party against THIS mode's cap in game (D4a), so
	// the subsystem has to know which queue is picked. Only on a change: SetSelectedQueueKey may notify.
	const FString Key = Modes.IsValidIndex(SelectedModeIndex) ? Modes[SelectedModeIndex].QueueName : FString();
	if (S->SelectedQueueKey() != Key)
	{
		S->SetSelectedQueueKey(Key);
	}
}

void UFoundryMatchCluster::Refresh()
{
	using EState = EFoundryMatchmakingState;
	UFMMSSubsystem* F = Fmms();
	const UFoundrySocialSubsystem* S = Social();

	const EState State = F ? F->GetMatchmakingState() : EState::Idle;
	const FString Message = F ? F->GetMatchmakingMessage() : FString();
	const bool bSigned = S && S->IsSignedIn();
	const bool bFixture = F && F->IsMatchmakingFixtureActive();
	const bool bCanCancel = F && F->CanCancel();

	Modes = GetModes();
	ResolveSelectedMode();
	SyncSelectedQueueKey();

	// The selected mode's party cap (-1 until the platform answered) vs the party - a party bigger than
	// the mode holds can neither queue for it nor be seated by it (fid answers 409 on submit).
	const FString SelectedQueue = Modes.IsValidIndex(SelectedModeIndex) ? Modes[SelectedModeIndex].QueueName : FString();
	const int32 SelectedModeCap = (S && !SelectedQueue.IsEmpty()) ? S->PartyCapForQueue(SelectedQueue) : -1;
	const int32 PartyJoined = S ? FMath::Max(1, S->JoinedCount()) : 1;
	const bool bPartyTooBig = FoundryPartyRules::IsTooBig(PartyJoined, SelectedModeCap);
	const FoundryPartyRules::FFindMatchGate Gate = GateFor(S, SelectedQueue);
	// A member following its leader's search reads the subsystem's own sentence ("Searching with Ada").
	const FString FollowLead = (IsFollower(S) && !Message.IsEmpty()) ? Message : FString();

	// -- mode pill
	if (Modes.IsValidIndex(SelectedModeIndex))
	{
		SetText(ModeText, Modes[SelectedModeIndex].DisplayName.ToString().ToUpper()); // no tooltip on the pill: the mode row carries the detail
	}
	const bool bCanChooseMode = State == EState::Idle || State == EState::Failed;
	if (ModeButton)
	{
		ModeButton->SetIsEnabled(bCanChooseMode); // the mode is fixed once a search is running
	}
	if (!bCanChooseMode)
	{
		CloseModeMenu();
	}
	if (bModeMenuOpen)
	{
		RebuildModeRows();
	}

	// -- button + status
	FString Label = TEXT("FIND MATCH");
	FString Status;
	// A dev fixture keeps the button's full look (the subsystem refuses the request itself).
	const bool bCanFind = F && F->CanRequest() && Modes.IsValidIndex(SelectedModeIndex) && !Modes[SelectedModeIndex].QueueName.IsEmpty();
	bool bPrimaryEnabled = bCanFind || bFixture;
	bool bShowSpinner = false;
	bool bShowLeave = false;
	switch (State)
	{
	case EState::Idle:
		if (!bSigned)
		{
			Status = TEXT("Sign in through the Foundry launcher to play.");
		}
		else if (bFixture)
		{
			Status = TEXT("Dev fixture - Find Match disabled.");
		}
		else if (!Message.IsEmpty())
		{
			Status = Message;
		}
		break;
	// While a search can still be cancelled the big button IS the cancel: CANCEL SEARCH, clickable
	// (with a CancelButton bound it is that button; otherwise FindMatchButton relabels and the WBP
	// swaps it to the danger style on OnMatchmakingStateChanged). Once the subsystem can no longer
	// cancel (the seat is being taken) it reads as a state again.
	case EState::Pending:
		Status = TEXT("Requesting a match"); bShowSpinner = true;
		bPrimaryEnabled = bCanCancel;
		Label = bPrimaryEnabled ? TEXT("CANCEL SEARCH") : TEXT("REQUESTING...");
		break;
	case EState::Searching:
		Status = FollowLead.IsEmpty() ? FString(TEXT("Searching for")) : FollowLead; bShowSpinner = true;
		bPrimaryEnabled = bCanCancel;
		Label = bPrimaryEnabled ? TEXT("CANCEL SEARCH") : TEXT("SEARCHING...");
		break;
	case EState::WaitingForCapacity:
		// The server-composed sentence (incl. any "~N min left") IS the status line - never
		// overwritten by the local elapsed clock (NativeTick only re-renders it for
		// Pending/Searching, deliberately leaving this state's text alone).
		Status = !Message.IsEmpty() ? Message : FString(TEXT("Waiting for a server..."));
		bShowSpinner = true;
		bPrimaryEnabled = bCanCancel;
		Label = bPrimaryEnabled ? TEXT("CANCEL SEARCH") : TEXT("WAITING...");
		break;
	case EState::Found:
		Status = TEXT("Connecting to the server"); bShowSpinner = true;
		bPrimaryEnabled = bCanCancel;
		Label = bPrimaryEnabled ? TEXT("CANCEL SEARCH") : TEXT("MATCH FOUND");
		break;
	case EState::Traveling:
		Label = TEXT("JOINING..."); Status = TEXT("Joining the server"); bShowSpinner = true; bPrimaryEnabled = false;
		break;
	case EState::CancelPending:
		Label = TEXT("CANCELLING..."); Status = TEXT("Cancelling the search"); bShowSpinner = true; bPrimaryEnabled = false;
		break;
	case EState::Failed:
		Status = TEXT("Matchmaking failed: ") + (!Message.IsEmpty() ? Message : FString(TEXT("try again.")));
		break;
	case EState::Reconnectable:
		Label = TEXT("RECONNECT"); Status = TEXT("You are seated in a live match."); bPrimaryEnabled = F && !bFixture; bShowLeave = true;
		break;
	}

	// Find Match is refused, with the reason on the status line, while the party does not fit the mode
	// (Idle / Failed are the only states a new search can start from; the sentence replaces a stale failure).
	const bool bTooBigBlocksFind = bPartyTooBig && bSigned && (State == EState::Idle || State == EState::Failed);
	if (bTooBigBlocksFind)
	{
		Status = FoundryPartyRules::TooBigText(PartyJoined, SelectedModeCap);
		bPrimaryEnabled = false;
	}

	// Ready belongs to the game: in a party of 2+ only the LEADER starts a search, and only once every joined
	// member is ready in this game (the party-cap sentence above still comes first). Start states only - a
	// running search keeps its CANCEL SEARCH whoever started it.
	const bool bStartState = State == EState::Idle || State == EState::Failed;
	bool bHideFind = false;
	bool bGateStatusShown = false;
	const bool bLiveIdleNotice = State == EState::Idle && bSigned && !bFixture && !Message.IsEmpty();
	bIdleNoticeShown = bLiveIdleNotice;
	if (Gate.bGated && bSigned && bStartState)
	{
		if (!Gate.bShowButton)
		{
			bHideFind = true;
			bPrimaryEnabled = false;
		}
		else
		{
			bPrimaryEnabled = bPrimaryEnabled && Gate.bEnabled;
			if (!Gate.Label.IsEmpty())
			{
				Label = Gate.Label;
			}
		}
		// A live Idle notice ("Your party's search was cancelled.", "Match over") is news the player has not read
		// yet and it carries its own TTL - it keeps the line until it expires, then the waiting sentence takes over.
		if (!bTooBigBlocksFind && !bLiveIdleNotice && !Gate.Status.IsEmpty())
		{
			Status = Gate.Status;
			bGateStatusShown = true;
		}
	}

	// The label is CANCEL SEARCH exactly when the primary button is the cancel.
	const bool bCancelLabel = Label == TEXT("CANCEL SEARCH");
	const bool bSplitCancel = CancelButton != nullptr && bCancelLabel;
	// A one-button WBP (no CancelButton) restyles its primary from this flip - the label and the look
	// change on the same refresh, whatever state produced the label.
	const bool bPrimaryCancelNow = bCancelLabel && !bSplitCancel;
	if (bPrimaryCancelNow != bPrimaryIsCancel)
	{
		bPrimaryIsCancel = bPrimaryCancelNow;
		OnPrimaryCancelChanged(bPrimaryIsCancel);
	}
	if (CancelButton)
	{
		SetText(CancelText, TEXT("CANCEL SEARCH"));
		Show(CancelButton, bSplitCancel, true);
	}

	SetText(FindMatchText, Label);
	if (FindMatchButton)
	{
		const bool bBusy = !bPrimaryEnabled && State != EState::Idle && State != EState::Failed;
		FindMatchButton->SetIsEnabled(bPrimaryEnabled || bBusy);
		// Hidden, not Collapsed, for a waiting member: the cluster keeps its height, so the party's state
		// changing never shifts the layout.
		FindMatchButton->SetVisibility(bSplitCancel ? ESlateVisibility::Collapsed
			: bHideFind ? ESlateVisibility::Hidden
			: bBusy ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Visible);
	}
	SetText(StatusText, Status.IsEmpty() ? FString(TEXT(" ")) : Status); // a blank line keeps the row's height
	if (StatusText && bStatusColorCaptured)
	{
		// "Waiting for X to ready up" is not an error, even where it replaced a stale failure sentence.
		const bool bErrorStatus = (State == EState::Failed && !bGateStatusShown) || bTooBigBlocksFind;
		StatusText->SetColorAndOpacity(bErrorStatus ? FSlateColor(UFoundryUISettings::Get()->Palette.Error) : StatusNormalColor);
	}
	RefreshSpinner(bShowSpinner);
	Show(LeaveMatchButton, bShowLeave, true);
	if (StatusRow)
	{
		// Hidden, not Collapsed: the row keeps its height, so a status line appearing never shifts
		// the cluster.
		const bool bAny = !Status.IsEmpty() || bShowLeave;
		StatusRow->SetVisibility(bAny ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Hidden);
	}

	// -- "Party N / cap": solo reads as a party of one
	SetText(PartyText, S && bSigned ? FString::Printf(TEXT("Party %d / %d"), PartyJoined, FoundryPartyRules::CapFor(*S)) : FString());

	LastElapsedShown = -2;
	if (!bHaveLastState || State != LastState)
	{
		bHaveLastState = true;
		LastState = State;
		NativeOnMatchmakingStateChanged(State);
		OnMatchmakingStateChanged(State);
	}
}

void UFoundryMatchCluster::RefreshSpinner(bool bShow)
{
	bSpinnerBusy = bShow;
	Show(Spinner, bShow);
	if (SpinAnim)
	{
		if (bShow)
		{
			if (!IsAnimationPlaying(SpinAnim))
			{
				PlayAnimation(SpinAnim, 0.0f, 0 /*loop forever*/);
			}
		}
		else
		{
			StopAnimation(SpinAnim);
		}
	}
}

void UFoundryMatchCluster::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (bSpinnerBusy && !SpinAnim && bCodeSpinner && Spinner)
	{
		SpinAngle = FMath::Fmod(SpinAngle + 360.0f * InDeltaTime, 360.0f);
		Spinner->SetRenderTransformAngle(SpinAngle);
	}

	// A long status line ("we're starting a server for you ... (~8 min left)") must never run out
	// from under the button into whatever sits beside the cluster (the lobby chat panel): wrap it at
	// the FindMatchButton's rendered width unless the Designer wraps it already. The button is the one
	// element whose width is a design constant, and its geometry exists only once laid out - hence here.
	if (StatusText && FindMatchButton && !bDesignerWrapsStatus)
	{
		const float ButtonWidth = FindMatchButton->GetCachedGeometry().GetLocalSize().X;
		if (ButtonWidth > 0.0f && !FMath::IsNearlyEqual(ButtonWidth, StatusWrapWidth))
		{
			StatusWrapWidth = ButtonWidth;
			StatusText->SetWrapTextAt(ButtonWidth);
		}
	}

	const UFMMSSubsystem* F = Fmms();
	// A TTL'd Idle notice leaves no state change behind when it expires: without this the waiting sentence it
	// was holding back would not appear until the next party poll.
	if (bIdleNoticeShown && F && F->GetMatchmakingMessage().IsEmpty())
	{
		Refresh();
	}
	if (!F || !StatusText)
	{
		return;
	}
	const EFoundryMatchmakingState State = F->GetMatchmakingState();
	const double Elapsed = F->GetElapsedSearchSeconds();
	if ((State == EFoundryMatchmakingState::Searching || State == EFoundryMatchmakingState::Pending) && Elapsed >= 0.0)
	{
		const int32 Seconds = FMath::FloorToInt(Elapsed);
		if (Seconds != LastElapsedShown)
		{
			LastElapsedShown = Seconds;
			FString Lead = State == EFoundryMatchmakingState::Searching ? FString(TEXT("Searching for")) : FString(TEXT("Requesting"));
			if (State == EFoundryMatchmakingState::Searching && IsFollower(Social()) && !F->GetMatchmakingMessage().IsEmpty())
			{
				Lead = F->GetMatchmakingMessage(); // "Searching with Ada" - same sentence Refresh renders
			}
			SetText(StatusText, FString::Printf(TEXT("%s %d:%02d"), *Lead, Seconds / 60, Seconds % 60));
		}
	}
}

void UFoundryMatchCluster::RebuildModeRows()
{
	if (!ModeList)
	{
		return;
	}
	if (!ModeRowClass)
	{
		ModeRowClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultModeRow, TEXT("DefaultModeRow")).Get();
	}
	if (!ModeRowClass)
	{
		return;
	}
	ModeList->ClearChildren();
	for (int32 i = 0; i < Modes.Num(); ++i)
	{
		const FFoundryMatchmakingMode& Mode = Modes[i];
		UFoundryModeRow* Row = CreateWidget<UFoundryModeRow>(this, ModeRowClass);
		if (!Row)
		{
			continue;
		}
		ModeList->AddChild(Row);
		Row->Setup(i, Mode.DisplayName, Mode.Detail, i == SelectedModeIndex, !Mode.QueueName.IsEmpty());
		// "Which modes fit the current party": a mode that holds fewer players than the party shows why and
		// cannot be picked.
		if (const UFoundrySocialSubsystem* S = Social())
		{
			Row->SetPartyGate(FMath::Max(1, S->JoinedCount()), Mode.QueueName.IsEmpty() ? -1 : S->PartyCapForQueue(Mode.QueueName));
		}
		Row->OnPicked.AddWeakLambda(this, [this](int32 Index)
		{
			// Close first: SelectMode refreshes, and a refresh with the menu still open would rebuild
			// (destroy) the very row whose click is running this lambda.
			SetModeMenuOpen(false);
			SelectMode(Index);
			Refresh();
		});
	}
}

void UFoundryMatchCluster::SelectMode(int32 Index)
{
	const UFMMSSubsystem* F = Fmms();
	const EFoundryMatchmakingState State = F ? F->GetMatchmakingState() : EFoundryMatchmakingState::Idle;
	if (State != EFoundryMatchmakingState::Idle && State != EFoundryMatchmakingState::Failed)
	{
		return; // the mode is fixed once a search is running
	}
	Modes = GetModes();
	if (!Modes.IsValidIndex(Index) || Modes[Index].QueueName.IsEmpty() || Index == SelectedModeIndex)
	{
		return;
	}
	SelectedModeIndex = Index;
	SyncSelectedQueueKey();
	Refresh();
}

void UFoundryMatchCluster::SetModeMenuOpen(bool bOpen)
{
	if (bOpen == bModeMenuOpen)
	{
		return;
	}
	bModeMenuOpen = bOpen;
	// ModeMenuAnim (the WBP's fade-in + chevron turn, any length) runs forward on open and backward on
	// close, a toggle mid-flight just reverses it (PlayOrTween); with none bound the code fades the
	// menu and turns the chevron instead. The hide waits for the reverse to finish
	// (NativeOnTransitionFinished) - no timing to hand-match.
	if (bOpen)
	{
		RebuildModeRows();
		Show(ModeMenu, true, true); // visible BEFORE the transition: the fade starts from 0
		PlayOrTween(ModeMenuAnim, true, ModeMenuSeconds, ModeMenu);
		if (!ModeMenuAnim && ModeChevron)
		{
			TweenAngle(ModeChevron, 0.0f, 180.0f, ModeMenuSeconds);
		}
		OnModeMenuChanged(true);
		return;
	}
	OnModeMenuChanged(false);
	if (ModeMenu)
	{
		ModeMenu->SetVisibility(ESlateVisibility::HitTestInvisible); // on screen for the close transition, not clickable
	}
	PlayOrTween(ModeMenuAnim, false, ModeMenuSeconds, ModeMenu);
	if (!ModeMenuAnim && ModeChevron)
	{
		TweenAngle(ModeChevron, 180.0f, 0.0f, ModeMenuSeconds);
	}
	if (ModeButton)
	{
		ModeButton->SetKeyboardFocus();
	}
}

void UFoundryMatchCluster::NativeOnTransitionFinished(bool bForward)
{
	Super::NativeOnTransitionFinished(bForward);
	if (!bForward && !bModeMenuOpen)
	{
		Show(ModeMenu, false, true); // the reverse finished: now it can leave the layout
	}
}

bool UFoundryMatchCluster::CloseModeMenu()
{
	if (!bModeMenuOpen)
	{
		return false;
	}
	SetModeMenuOpen(false);
	return true;
}

void UFoundryMatchCluster::HandleModeButton()
{
	SetModeMenuOpen(!bModeMenuOpen);
}

void UFoundryMatchCluster::HandleFindMatch()
{
	CloseModeMenu();
	UFMMSSubsystem* F = Fmms();
	if (!F)
	{
		return;
	}
	switch (F->GetMatchmakingState())
	{
	case EFoundryMatchmakingState::Reconnectable:
		F->RequestReconnect();
		break;
	case EFoundryMatchmakingState::Pending:
	case EFoundryMatchmakingState::Searching:
	case EFoundryMatchmakingState::WaitingForCapacity:
	case EFoundryMatchmakingState::Found:
		F->RequestCancel(); // the button reads CANCEL SEARCH in these states (no-op once it cannot cancel)
		break;
	default:
		{
			Modes = GetModes();
			ResolveSelectedMode();
			if (!Modes.IsValidIndex(SelectedModeIndex) || Modes[SelectedModeIndex].QueueName.IsEmpty())
			{
				break;
			}
			const UFoundrySocialSubsystem* S = Social();
			// Refused here too (the button is disabled, but a key binding / Blueprint can still call this):
			// the same sentence the status line shows.
			if (S && FoundryPartyRules::IsTooBig(FMath::Max(1, S->JoinedCount()), S->PartyCapForQueue(Modes[SelectedModeIndex].QueueName)))
			{
				Refresh();
				break;
			}
			// Ready belongs to the game: a waiting member never starts a search, and the leader waits for everyone
			// (the subsystem and fid refuse too - this keeps a key binding from even asking).
			if (S && S->IsSignedIn())
			{
				const FoundryPartyRules::FFindMatchGate Gate = GateFor(S, Modes[SelectedModeIndex].QueueName);
				if (Gate.bGated && (!Gate.bShowButton || !Gate.bEnabled))
				{
					Refresh();
					break;
				}
			}
			const FString PartyId = S ? S->Party().PartyId : FString();
			// Values, not references: RequestFindMatch broadcasts Pending before it consumes its arguments and
			// this widget's HandleFmmsState -> Refresh() reassigns Modes on that broadcast (the 0.1.1 crash).
			const FString QueueName = Modes[SelectedModeIndex].QueueName;
			const FString Attributes = MergePartyId(Modes[SelectedModeIndex].AttributesJson, PartyId);
			F->RequestFindMatch(QueueName, Attributes); // duplicate-safe: the subsystem refuses a second request
		}
		break;
	}
}

void UFoundryMatchCluster::HandleCancel()
{
	if (UFMMSSubsystem* F = Fmms())
	{
		F->RequestCancel();
	}
}

void UFoundryMatchCluster::HandleLeaveMatch()
{
	// Reconnectable only: decline the live seat (the old routing to RequestCancel was a no-op there).
	if (UFMMSSubsystem* F = Fmms())
	{
		F->DeclineReconnectSeat();
	}
}
