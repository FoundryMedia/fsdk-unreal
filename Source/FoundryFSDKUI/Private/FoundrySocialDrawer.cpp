#include "FoundrySocialDrawer.h"
#include "SFoundryPopupHost.h"

#include "Animation/WidgetAnimation.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/IMenu.h"
#include "Framework/Application/SlateApplication.h"
#include "FoundryConfirmDialog.h"
#include "FoundryFSDKUILog.h"
#include "FoundryInviteRow.h"
#include "FoundryPartyRules.h"
#include "FoundryToastSubsystem.h"
#include "FoundrySocialSubsystem.h"
#include "FoundryUISettings.h"
#include "FoundryUITheme.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"

namespace
{
	// The drawer's default slide distance (slate units): the Lobby's own canvas slot for the
	// placed-by-default drawer is 416 wide + a 32px inset from the edge it's anchored to (see
	// GooLobbyWidget.cpp) - 448 slides it fully clear of that edge.
	constexpr float DefaultDrawerTweenOffset = 448.0f;
	constexpr float DefaultDrawerTweenSeconds = 0.22f;
	constexpr float ChevronTweenSeconds = 0.15f;

	// The subsystem names a social action either as an FString or an FName; the drawer's handlers
	// take a string. Overloads (not LexToString) so both delegate shapes bind through a generic lambda.
	FString ActionKey(const FString& Action) { return Action; }
	FString ActionKey(const FName& Action) { return Action.ToString(); }

	/** The local member's ready flag FOR THIS GAME. IsSelfReady() is ready in ANY game: a member ready in
	 *  another title would read READY-already and the toggle would un-ready them instead of readying here. */
	bool IsSelfReadyHere(const UFoundrySocialSubsystem& Sdk)
	{
		const FString Slug = Sdk.GameSlug();
		const FString Me = Sdk.LocalFoundryId();
		if (Slug.IsEmpty() || Me.IsEmpty())
		{
			return false;
		}
		for (const FFoundryPartyMember& M : Sdk.Party().Members)
		{
			if (M.FoundryId == Me)
			{
				return M.bReady && M.ReadyGameSlug.Equals(Slug, ESearchCase::IgnoreCase);
			}
		}
		return false;
	}

	bool IsJoinedState(const FString& State)
	{
		return State.IsEmpty() || State.Equals(TEXT("JOINED"), ESearchCase::IgnoreCase);
	}

	/**
	 * Reconciles RowMap/List down to exactly WantedKeys, in that order, REUSING an existing row for a
	 * key that's still wanted instead of destroying and recreating it (the drawer rebuilds on every
	 * poll landing - recreating every row each time flashed/lost state and was needlessly expensive).
	 * Keys no longer wanted are removed from the panel (and the map); keys not yet present are created
	 * via CreateRow and appended; every wanted row is then ShiftChild'd into its final index only if
	 * it isn't already there. The caller still calls Setup() on each row afterwards (this only handles
	 * identity/order, never content) - RowMap.Find(Key) after this call is always the right widget.
	 */
	template <typename TRow>
	void SyncRowList(TMap<FString, TObjectPtr<TRow>>& RowMap, UPanelWidget* List,
		const TArray<FString>& WantedKeys, TFunctionRef<TRow*()> CreateRow)
	{
		if (!List)
		{
			RowMap.Reset();
			return;
		}
		for (auto It = RowMap.CreateIterator(); It; ++It)
		{
			if (!WantedKeys.Contains(It.Key()))
			{
				if (TRow* Row = It.Value())
				{
					List->RemoveChild(Row);
				}
				It.RemoveCurrent();
			}
		}
		for (const FString& Key : WantedKeys)
		{
			if (!RowMap.Contains(Key))
			{
				if (TRow* Row = CreateRow())
				{
					RowMap.Add(Key, Row);
				}
			}
		}
		for (int32 i = 0; i < WantedKeys.Num(); ++i)
		{
			if (TObjectPtr<TRow>* Found = RowMap.Find(WantedKeys[i]))
			{
				if (TRow* Row = *Found)
				{
					if (List->GetChildIndex(Row) != i)
					{
						List->ShiftChild(i, Row);
					}
				}
			}
		}
	}
}

UFoundrySocialDrawer::UFoundrySocialDrawer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	TweenOffset = DefaultDrawerTweenOffset;
}

UFoundrySocialSubsystem* UFoundrySocialDrawer::Subsystem()
{
	if (UFoundrySocialSubsystem* Cached = SubsystemWeak.Get())
	{
		return Cached;
	}
	UGameInstance* GI = GetGameInstance();
	UFoundrySocialSubsystem* Found = GI ? GI->GetSubsystem<UFoundrySocialSubsystem>() : nullptr;
	SubsystemWeak = Found;
	return Found;
}

void UFoundrySocialDrawer::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// Class Defaults left null -> resolve from the project-wide settings (LoadSynchronous).
	if (!FriendRowClass)
	{
		FriendRowClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultFriendRow, TEXT("DefaultFriendRow")).Get();
	}
	if (!InviteRowClass)
	{
		InviteRowClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultInviteRow, TEXT("DefaultInviteRow")).Get();
	}
	if (!FriendMenuClass)
	{
		FriendMenuClass = UFoundryUISettings::LoadWidgetClass(UFoundryUISettings::Get()->DefaultFriendMenu, TEXT("DefaultFriendMenu")).Get();
	}

	Show(ActionNote, false);
	if (CloseButton) { CloseButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleClose); }
	if (ManageAccountButton) { ManageAccountButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleManageAccount); }
	if (SearchBox) { SearchBox->OnTextChanged.AddDynamic(this, &UFoundrySocialDrawer::HandleSearchChanged); }
	if (OnlineToggleButton) { OnlineToggleButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleToggleOnline); }
	if (OfflineToggleButton) { OfflineToggleButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleToggleOffline); }
	if (BlockedToggleButton) { BlockedToggleButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleToggleBlocked); }
	if (AddFriendButton) { AddFriendButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleAddFriendButton); }
	if (AddFriendSendButton) { AddFriendSendButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleAddFriendSend); }
	if (AddFriendInput) { AddFriendInput->OnTextCommitted.AddDynamic(this, &UFoundrySocialDrawer::HandleAddFriendCommitted); }
	if (LeaveButton) { LeaveButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleLeaveParty); }
	if (ReadyToggleButton) { ReadyToggleButton->OnClicked.AddDynamic(this, &UFoundrySocialDrawer::HandleReadyToggle); }
	// Pivot once, in place - TweenAngle then spins each chevron about its own centre instead of an
	// edge. bOnlineExpanded starts true (see below), so OnlineChevron starts pointing UP (180).
	if (OnlineChevron) { OnlineChevron->SetRenderTransformPivot(FVector2D(0.5f, 0.5f)); OnlineChevron->SetRenderTransformAngle(180.0f); }
	if (OfflineChevron) { OfflineChevron->SetRenderTransformPivot(FVector2D(0.5f, 0.5f)); }
	if (BlockedChevron) { BlockedChevron->SetRenderTransformPivot(FVector2D(0.5f, 0.5f)); }
	Show(AddFriendEntry, false);
	Show(AddFriendNote, false);
	Show(OfflineList, false);
	Show(BlockedList, false);

	// A freshly created drawer starts at its HIDDEN resting pose (opacity 0, translated off by
	// TweenOffset) instead of the WBP's default (opacity 1, translation 0) - so the very first
	// SetOpen(true) never shows a frame of the "open" pose before the tween's first tick corrects
	// it (TweenProgress's own default, 0, already means "hidden"; this just makes the RENDER state
	// agree with it from the start). A host that shows the drawer open by default should call
	// SetOpen(true, false) itself to snap it forward.
	SetRenderOpacity(0.0f);
	{
		FWidgetTransform T = GetRenderTransform();
		T.Translation.X = TweenOffset;
		SetRenderTransform(T);
	}
}

void UFoundrySocialDrawer::NativeConstruct()
{
	Super::NativeConstruct();
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		// SetReady sends this slug; the subsystem cannot be reached at module startup, so the first kit widget
		// to construct pushes it (the match cluster does the same - both are cheap setters).
		UFoundryUISettings::PushGameSlug(Sdk);
		Sdk->RegisterConsumer();
		Sdk->OnChanged.AddUObject(this, &UFoundrySocialDrawer::HandleSocialChanged);
		Sdk->OnActionFailed.AddWeakLambda(this, [this](const auto& Action, const FString& Message)
		{
			HandleActionFailed(ActionKey(Action), Message);
		});
		Sdk->OnActionSucceeded.AddDynamic(this, &UFoundrySocialDrawer::HandleActionSucceeded);
		NoteInviteClocks(); // rows already in the cache when the drawer is built
	}
	MarkDirty();
}

void UFoundrySocialDrawer::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	if (bDirty && GetVisibility() != ESlateVisibility::Collapsed)
	{
		bDirty = false;
		Rebuild();
	}
}

void UFoundrySocialDrawer::NativeDestruct()
{
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->OnChanged.RemoveAll(this);
		Sdk->OnActionFailed.RemoveAll(this);
		Sdk->OnActionSucceeded.RemoveDynamic(this, &UFoundrySocialDrawer::HandleActionSucceeded);
		Sdk->UnregisterConsumer();
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionNoteTimerHandle);
	}
	CloseFriendMenu();
	Super::NativeDestruct();
}

void UFoundrySocialDrawer::SetOpen(bool bOpen, bool bAnimate)
{
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->SetPanelOpen(bOpen);
	}

	// Opening must be visible BEFORE the tween/animation starts (it animates a visible widget's
	// opacity/translation, not a collapsed one's) - and must already SIT at the hidden pose the
	// instant it becomes visible, or the first paint shows a frame of the resting state (opacity 1,
	// translation 0) before the tween's first tick corrects it. NativeOnInitialized leaves a fresh
	// drawer hidden too, but snap again here regardless of how this drawer got into this state (a
	// host that snapped it open with bAnimate=false earlier, then closes and reopens animated).
	if (bOpen)
	{
		if (bAnimate)
		{
			SetRenderOpacity(0.0f);
			FWidgetTransform T = GetRenderTransform();
			T.Translation.X = TweenOffset;
			SetRenderTransform(T);
		}
		SetVisibility(ESlateVisibility::SelfHitTestInvisible);

		// The first visible frame shows real rows, not an empty list that pops in a tick later -
		// NativeTick only rebuilds a dirty drawer that is already Visible, which would otherwise be
		// this same tick at the earliest and the frame after at the latest.
		if (bDirty)
		{
			bDirty = false;
			Rebuild();
		}
	}

	if (bAnimate)
	{
		PlayOrTween(DrawerAnim, bOpen, DefaultDrawerTweenSeconds, this);
		return; // NativeOnTransitionFinished(false) collapses on the close side, once it lands
	}

	// No motion wanted (a host playing its OWN animation on this drawer, or reduced motion with no
	// host animation) - snap straight to the end state; no tween, no DrawerAnim playback.
	if (DrawerAnim)
	{
		StopAnimation(DrawerAnim);
	}
	SetRenderOpacity(bOpen ? 1.0f : 0.0f);
	FWidgetTransform T = GetRenderTransform();
	T.Translation.X = bOpen ? 0.0f : TweenOffset;
	SetRenderTransform(T);
	if (!bOpen)
	{
		SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UFoundrySocialDrawer::NativeOnTransitionFinished(bool bForward)
{
	if (!bForward)
	{
		SetVisibility(ESlateVisibility::Collapsed);
	}
}

void UFoundrySocialDrawer::FocusFirstControl()
{
	if (SearchBox)
	{
		SearchBox->SetKeyboardFocus();
	}
}

bool UFoundrySocialDrawer::OpenFirstRowMenuForTesting()
{
	if (!OnlineList)
	{
		return false;
	}
	for (int32 i = 0; i < OnlineList->GetChildrenCount(); ++i)
	{
		if (UFoundryFriendRow* Row = Cast<UFoundryFriendRow>(OnlineList->GetChildAt(i)))
		{
			const FGeometry& Geo = Row->GetCachedGeometry();
			const FVector2D ScreenCentre = Geo.GetAbsolutePositionAtCoordinates(FVector2D(0.5f, 0.5f));
			OpenFriendMenu(Row, ScreenCentre);
			return true;
		}
	}
	return false;
}

FLinearColor UFoundrySocialDrawer::GetMemberTint_Implementation(const FString& /*FoundryId*/, int32 RosterIndex, bool bLocal) const
{
	return FoundryUITheme::MemberTint(RosterIndex, bLocal);
}

UFoundryFriendRow* UFoundrySocialDrawer::AddFriendRow(UPanelWidget* List)
{
	if (!List || !FriendRowClass)
	{
		if (List && !bWarnedNoRowClass)
		{
			bWarnedNoRowClass = true;
			UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[SocialDrawer] %s has no FriendRowClass (Class Defaults, or set UFoundryUISettings::DefaultFriendRow) - the party/online/offline/blocked lists stay empty."), *GetClass()->GetName());
		}
		return nullptr;
	}
	UFoundryFriendRow* Row = CreateWidget<UFoundryFriendRow>(this, FriendRowClass);
	List->AddChild(Row);
	return Row;
}

void UFoundrySocialDrawer::MarkDirty()
{
	bDirty = true;
}

void UFoundrySocialDrawer::HandleSocialChanged()
{
	// Stamp at RECEIPT, not at rebuild: a closed drawer rebuilds only when it next opens.
	NoteInviteClocks();
	MarkDirty();
}

void UFoundrySocialDrawer::NoteInviteClocks()
{
	const UFoundrySocialSubsystem* Sdk = Subsystem();
	const double Now = FPlatformTime::Seconds();
	TSet<FString> Live;

	auto Note = [this, &Live, Now](const FString& Key, const FDateTime& ExpiresAt, const FDateTime& InvitedAt)
	{
		Live.Add(Key);
		const FInviteAnchor* Existing = InviteAnchors.Find(Key);
		if (Existing && Existing->ExpiresAt == ExpiresAt)
		{
			return; // the same invite still: keep the anchor from when it was first received
		}
		FInviteAnchor Anchor;
		Anchor.ExpiresAt = ExpiresAt;
		Anchor.Clock = FoundryPartyRules::FInviteClock::From(ExpiresAt, InvitedAt, Now);
		InviteAnchors.Add(Key, Anchor);
	};

	if (Sdk)
	{
		for (const FFoundryPartyMember& M : Sdk->Party().Members)
		{
			if (!IsJoinedState(M.State))
			{
				Note(TEXT("m:") + M.FoundryId, M.ExpiresAt, M.InvitedAt);
			}
		}
		for (const FFoundryParty& Inv : Sdk->Invites())
		{
			// A received invite row carries THIS invite's clock on the party itself (the core copies the
			// caller's own member row up); the member row is the fallback when the core did not know the
			// session's foundry id yet.
			FDateTime Invited = Inv.InvitedAt;
			FDateTime Expires = Inv.InviteExpiresAt;
			if (Expires.GetTicks() == 0)
			{
				for (const FFoundryPartyMember& M : Inv.Members)
				{
					if (!IsJoinedState(M.State) && !Sdk->LocalFoundryId().IsEmpty() && M.FoundryId == Sdk->LocalFoundryId())
					{
						Invited = M.InvitedAt;
						Expires = M.ExpiresAt;
						break;
					}
				}
			}
			Note(TEXT("i:") + Inv.PartyId, Expires, Invited);
		}
	}

	for (auto It = InviteAnchors.CreateIterator(); It; ++It)
	{
		if (!Live.Contains(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
}

FoundryPartyRules::FInviteClock UFoundrySocialDrawer::ClockFor(const FString& Key) const
{
	const FInviteAnchor* Found = InviteAnchors.Find(Key);
	return Found ? Found->Clock : FoundryPartyRules::FInviteClock();
}

void UFoundrySocialDrawer::HandleActionFailed(const FString& Action, const FString& Message)
{
	if (Action == TEXT("friend.request"))
	{
		SetText(AddFriendNote, Message);
		Show(AddFriendNote, true);
		return;
	}

	ShowActionNote(Message, true, 6.0f);
	OnActionMessage.Broadcast(Message);
}

void UFoundrySocialDrawer::HandleActionSucceeded(FName Action, const FString& Message)
{
	// The platform's own sentence ("Asked Sam to join") on the same line failures use, for 4 s. Not
	// mirrored to OnActionMessage: that delegate is documented as the FAILURE feed.
	if (!Message.IsEmpty())
	{
		ShowActionNote(Message, false, 4.0f);
	}
}

void UFoundrySocialDrawer::ShowActionNote(const FString& Message, bool bError, float Seconds)
{
	if (bError)
	{
		// Errors are a toast (they must be seen even with the drawer half off-screen); successes stay on the line.
		Show(ActionNote, false);
		UFoundryToastSubsystem::ShowToast(this, FText::FromString(Message), EFoundryToastKind::Error);
		return;
	}
	SetText(ActionNote, Message);
	if (ActionNote)
	{
		const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
		ActionNote->SetColorAndOpacity(FSlateColor(bError ? Palette.Error : Palette.Online));
	}
	Show(ActionNote, true);
	if (UWorld* World = GetWorld())
	{
		// SetTimer on the same handle replaces a pending hide, so a success right after a failure
		// gets its own full 4 s rather than the failure's remainder.
		World->GetTimerManager().SetTimer(ActionNoteTimerHandle, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			Show(ActionNote, false);
		}), Seconds, false);
	}
}

void UFoundrySocialDrawer::HandleAcceptInvite(const FString& PartyId)
{
	UFoundrySocialSubsystem* Sdk = Subsystem();
	if (!Sdk)
	{
		return;
	}
	// D3: fid's accept auto-leaves the caller's current party; a LEADER's leave transfers leadership to
	// the longest-standing member and the party survives (the leader's own pending invites end). Only a
	// lone leader's party ends with them - nobody to lose, so that case accepts without a prompt.
	const int32 PartyMembers = Sdk->JoinedCount();
	const bool bLeavesAParty = Sdk->HasParty() && Sdk->IsLocalLeader() && PartyMembers > 1;
	if (!bLeavesAParty)
	{
		Sdk->AcceptInvite(PartyId);
		return;
	}
	if (ActiveConfirm)
	{
		return; // one confirm at a time
	}
	UFoundryConfirmDialog* Dialog = OpenConfirmDialog(
		FText::FromString(TEXT("Accept this invite?")),
		FText::FromString(FString::Printf(
			TEXT("You'll leave your current party (%d members): leadership passes to the next member and your pending invites end."),
			PartyMembers)),
		FText::FromString(TEXT("Accept")), FText::FromString(TEXT("Cancel")), 0.0f);
	if (!Dialog)
	{
		// OpenConfirmDialog already warned (no dialog class). Refusing beats leaving a party unasked.
		ShowActionNote(TEXT("Could not ask for confirmation - leave your party first, then accept."), true, 6.0f);
		return;
	}
	ActiveConfirm = Dialog;
	PendingAcceptPartyId = PartyId;
	Dialog->OnConfirmed.AddDynamic(this, &UFoundrySocialDrawer::HandleAcceptConfirmed);
	Dialog->OnCancelled.AddDynamic(this, &UFoundrySocialDrawer::HandleAcceptCancelled);
}

void UFoundrySocialDrawer::HandleAcceptConfirmed()
{
	const FString PartyId = PendingAcceptPartyId;
	PendingAcceptPartyId.Reset();
	ActiveConfirm = nullptr;
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->AcceptInvite(PartyId);
	}
}

void UFoundrySocialDrawer::HandleAcceptCancelled()
{
	PendingAcceptPartyId.Reset();
	ActiveConfirm = nullptr;
}

void UFoundrySocialDrawer::AskKickMember(const FString& FoundryId, const FString& DisplayName)
{
	if (FoundryId.IsEmpty() || ActiveKickConfirm)
	{
		return; // one confirm at a time
	}
	UFoundryConfirmDialog* Dialog = OpenConfirmDialog(
		FText::FromString(FString::Printf(TEXT("Remove %s from the party?"), *DisplayName)),
		FText::FromString(TEXT("They can be invited again.")),
		FText::FromString(TEXT("Remove")), FText::FromString(TEXT("Cancel")), 0.0f);
	if (!Dialog)
	{
		// OpenConfirmDialog already warned (no dialog class). Refusing beats removing someone unasked.
		ShowActionNote(TEXT("Could not ask for confirmation, so nobody was removed."), true, 6.0f);
		return;
	}
	ActiveKickConfirm = Dialog;
	PendingKickFoundryId = FoundryId;
	Dialog->OnConfirmed.AddDynamic(this, &UFoundrySocialDrawer::HandleKickConfirmed);
	Dialog->OnCancelled.AddDynamic(this, &UFoundrySocialDrawer::HandleKickCancelled);
}

void UFoundrySocialDrawer::HandleKickConfirmed()
{
	const FString FoundryId = PendingKickFoundryId;
	PendingKickFoundryId.Reset();
	ActiveKickConfirm = nullptr;
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->Kick(FoundryId);
	}
}

void UFoundrySocialDrawer::HandleKickCancelled()
{
	PendingKickFoundryId.Reset();
	ActiveKickConfirm = nullptr;
}

void UFoundrySocialDrawer::Rebuild()
{
	UFoundrySocialSubsystem* Sdk = Subsystem();
	const bool bSignedIn = Sdk && Sdk->IsSignedIn();
	const bool bFixture = Sdk && Sdk->IsFixtureActive();
	// In game the cap is the SELECTED mode's party cap (the queue decides the size - D4a), else the
	// platform cap; one number for the header, the invite gate and the match cluster.
	const int32 Cap = Sdk ? FoundryPartyRules::CapFor(*Sdk) : UFoundrySocialSubsystem::DefaultPartyCapacity;
	const int32 Joined = Sdk ? Sdk->JoinedCount() : 0;
	const bool bInParty = Sdk && Sdk->HasParty();

	// -- account block
	const FFoundryUIPalette& Palette = UFoundryUISettings::Get()->Palette;
	if (AccountAvatar)
	{
		AccountAvatar->SetColorAndOpacity(bSignedIn ? GetMemberTint(Sdk->LocalFoundryId(), 0, true) : Palette.TextSecondary);
	}
	SetText(AccountNameText, bSignedIn ? Sdk->LocalDisplayName() : TEXT("Not signed in"));
	const FString Username = bSignedIn ? Sdk->LocalUsername() : FString();
	SetText(AccountUsernameText, Username.IsEmpty() ? FString() : TEXT("@") + Username);
	Show(AccountUsernameText, !Username.IsEmpty());

	// -- party header + over-size note. A party that grew past its cap is never kicked down (fid keeps
	// everyone); it just cannot grow, and cannot queue for a mode that holds fewer.
	const bool bTooBig = FoundryPartyRules::IsTooBig(Joined, Cap);
	SetText(PartyHeaderText, TEXT("YOUR PARTY   ") + FoundryPartyRules::HeaderText(Joined, Cap));
	if (PartyHeaderText)
	{
		if (!bPartyHeaderColorCaptured)
		{
			PartyHeaderNormalColor = PartyHeaderText->GetColorAndOpacity();
			bPartyHeaderColorCaptured = true;
		}
		PartyHeaderText->SetColorAndOpacity(bTooBig ? FSlateColor(Palette.Error) : PartyHeaderNormalColor);
	}
	const FString Conflict = bTooBig
		? FString::Printf(TEXT("Your party is bigger than this mode holds (%d > %d). Pick another mode, or remove members."), Joined, Cap)
		: FString();
	SetText(CapConflictText, Conflict);
	Show(CapConflictText, !Conflict.IsEmpty());

	// -- why adding a friend is blocked (empty = it is not)
	AddFriendBlockedReason.Reset();
	if (!bSignedIn)
	{
		AddFriendBlockedReason = TEXT("Sign in through the Foundry launcher to add friends.");
	}
	else if (bFixture)
	{
		AddFriendBlockedReason = TEXT("Dev fixture: actions disabled.");
	}
	ApplyAddFriendEntry();

	// -- party rows (every member - joined AND pending/invited; local + pending rows open no menu).
	// Rows are REUSED (PartyRowsByFoundryId), never destroyed and recreated, so a party read that
	// lands a moment apart from the friends-presence read it's joined against can't flash a half-
	// updated row in between (the two-states-per-poll flicker).
	if (PartyList)
	{
		TArray<FString> WantedPartyKeys;
		int32 RawJoinedCount = 0;
		if (Sdk && bInParty)
		{
			for (const FFoundryPartyMember& M : Sdk->Party().Members)
			{
				WantedPartyKeys.Add(M.FoundryId);
				if (IsJoinedState(M.State))
				{
					++RawJoinedCount;
				}
			}
		}
		else if (Sdk)
		{
			// Not in a party: a single "you" row - key on the local foundry id, or a fixed sentinel
			// when signed out (so the dev-fixture / signed-out state still gets a stable key).
			WantedPartyKeys.Add(Sdk->LocalFoundryId().IsEmpty() ? TEXT("__me__") : Sdk->LocalFoundryId());
		}
		SyncRowList<UFoundryFriendRow>(PartyRowsByFoundryId, PartyList, WantedPartyKeys,
			[this]() { return AddFriendRow(PartyList); });

		if (Sdk && bInParty)
		{
			const FFoundryParty& Party = Sdk->Party();
			for (int32 i = 0; i < Party.Members.Num(); ++i)
			{
				const FFoundryPartyMember& M = Party.Members[i];
				const bool bLocal = !Sdk->LocalFoundryId().IsEmpty() && M.FoundryId == Sdk->LocalFoundryId();
				// The local player is always joined from THEIR OWN perspective - never render your
				// own row as "Invited - not joined" off a momentarily-stale/transitional party read.
				const bool bJoinedMember = bLocal || IsJoinedState(M.State);
				bool bHost = !Party.LeaderFoundryId.IsEmpty() && M.FoundryId == Party.LeaderFoundryId;
				if (Party.LeaderFoundryId.IsEmpty() && bLocal && RawJoinedCount == 1)
				{
					// Leader not reported yet (transitional) and you're the party's one truly-joined
					// member - you're obviously the host.
					bHost = true;
				}

				FFoundryFriend F;
				F.FoundryId = M.FoundryId;
				F.DisplayName = M.DisplayName.IsEmpty() ? M.Username : M.DisplayName;
				F.Username = M.Username;
				// The party read carries no presence; the friends list does - join them by id.
				for (const FFoundryFriend& Fr : Sdk->Friends())
				{
					if (Fr.FoundryId == M.FoundryId)
					{
						F.Presence = Fr.Presence;
						F.PresenceGame = Fr.PresenceGame;
						break;
					}
				}
				if (bLocal)
				{
					F.Presence = TEXT("online");
				}

				if (TObjectPtr<UFoundryFriendRow>* Found = PartyRowsByFoundryId.Find(M.FoundryId))
				{
					if (UFoundryFriendRow* Row = *Found)
					{
						// A member in a match reads "In match" (overrides the presence label, which says only
						// "Playing <game>" for the lobby too); an invitee is still "Invited - not joined". A member
						// in the lobby reads "Ready in <game>" / "Not ready": ready lives in the GAME now, so the
						// row names the game the flag was set in.
						const FString Status = !bJoinedMember ? FString(TEXT("Invited - not joined"))
							: (M.bInMatch ? FString(TEXT("In match")) : FoundryPartyRules::ReadyStatusText(M.bReady, M.ReadyGameSlug));
						Row->Setup(F, EFoundryFriendRowKind::Party, bHost, bLocal, bJoinedMember,
							GetMemberTint(F.FoundryId, i, bLocal), Status, M.bReady);
						// A pending row shows how long the invite still lives ("Invited - expires in 3 min").
						if (!bJoinedMember)
						{
							Row->SetInviteExpiry(ClockFor(TEXT("m:") + M.FoundryId));
						}
						// The local player's own row never opens a menu; a pending-invited row opens one only for
						// the leader (Re-invite / Cancel invite - the plan's "no way to re-invite from the game").
						// Cleared first since this row may be REUSED from a prior poll where the condition was
						// different (e.g. an invited member just joined).
						Row->OnContextMenuRequested.RemoveAll(this);
						if (!bLocal && (bJoinedMember || Sdk->IsLocalLeader()))
						{
							Row->OnContextMenuRequested.AddWeakLambda(this, [this](UFoundryFriendRow* R, const FVector2D& Pos)
							{
								OpenFriendMenu(R, Pos);
							});
						}
					}
				}
			}
		}
		else if (Sdk)
		{
			const FString MeKey = Sdk->LocalFoundryId().IsEmpty() ? TEXT("__me__") : Sdk->LocalFoundryId();
			if (TObjectPtr<UFoundryFriendRow>* Found = PartyRowsByFoundryId.Find(MeKey))
			{
				if (UFoundryFriendRow* Row = *Found)
				{
					FFoundryFriend Me;
					Me.FoundryId = Sdk->LocalFoundryId();
					Me.DisplayName = bSignedIn ? Sdk->LocalDisplayName() : TEXT("Not signed in");
					Me.Username = Sdk->LocalUsername();
					Me.Presence = bSignedIn ? TEXT("online") : TEXT("offline");
					Row->Setup(Me, EFoundryFriendRowKind::Party, true, true, true, GetMemberTint(Me.FoundryId, 0, true), FString());
					Row->OnContextMenuRequested.RemoveAll(this); // solo row never opens a menu
				}
			}
		}
	}

	// -- incoming party invites (reused by party id, same reuse pattern as friend rows)
	const int32 NumInvites = Sdk ? Sdk->Invites().Num() : 0;
	Show(InvitesSection, NumInvites > 0);
	if (InvitesList)
	{
		TArray<FString> WantedInviteKeys;
		if (Sdk && InviteRowClass)
		{
			for (const FFoundryParty& Inv : Sdk->Invites())
			{
				WantedInviteKeys.Add(Inv.PartyId);
			}
		}
		SyncRowList<UFoundryInviteRow>(InviteRowsByPartyId, InvitesList, WantedInviteKeys,
			[this]() { return InviteRowClass ? CreateWidget<UFoundryInviteRow>(this, InviteRowClass) : nullptr; });

		if (Sdk)
		{
			for (const FFoundryParty& Inv : Sdk->Invites())
			{
				if (TObjectPtr<UFoundryInviteRow>* Found = InviteRowsByPartyId.Find(Inv.PartyId))
				{
					if (UFoundryInviteRow* Row = *Found)
					{
						// "N / cap" is the INVITING party's own size against its own platform cap; the countdown is
						// the local player's own member row in it (fid expires each invitee separately), anchored
						// by NoteInviteClocks when the row was received.
						int32 InvJoined = 0;
						for (const FFoundryPartyMember& M : Inv.Members)
						{
							if (IsJoinedState(M.State))
							{
								++InvJoined;
							}
						}
						const int32 InvCap = FoundryPartyRules::EffectiveCap(-1, Inv.PlatformMaxSize,
							Inv.MaxSize > 0 ? Inv.MaxSize : Sdk->PartyCapacity());
						Row->Setup(Inv.LeaderDisplayName, Inv.PartyId, InvJoined, InvCap, ClockFor(TEXT("i:") + Inv.PartyId));
						Row->OnAccept.RemoveAll(this);
						Row->OnDecline.RemoveAll(this);
						Row->OnAccept.AddWeakLambda(this, [this](const FString& Id) { HandleAcceptInvite(Id); });
						Row->OnDecline.AddWeakLambda(this, [this](const FString& Id) { if (UFoundrySocialSubsystem* S = Subsystem()) { S->DeclineInvite(Id); } });
					}
				}
			}
		}
	}

	// -- the leader's join requests: players asking to join THIS party. Visible only while the local
	// player leads and the list is non-empty (the subsystem fills it only for a leader anyway). The rows
	// are invite rows in join-request mode, so the look and the Accept / Decline wiring are the kit's own.
	const bool bShowRequests = Sdk && Sdk->IsLocalLeader() && Sdk->JoinRequests().Num() > 0;
	Show(JoinRequestsSection, bShowRequests);
	if (JoinRequestsList)
	{
		TArray<FString> WantedRequestKeys;
		if (bShowRequests && InviteRowClass)
		{
			for (const FFoundryJoinRequest& Req : Sdk->JoinRequests())
			{
				WantedRequestKeys.Add(Req.FoundryId);
			}
		}
		SyncRowList<UFoundryInviteRow>(JoinRequestRowsByFoundryId, JoinRequestsList, WantedRequestKeys,
			[this]() { return InviteRowClass ? CreateWidget<UFoundryInviteRow>(this, InviteRowClass) : nullptr; });

		if (bShowRequests)
		{
			for (const FFoundryJoinRequest& Req : Sdk->JoinRequests())
			{
				if (TObjectPtr<UFoundryInviteRow>* Found = JoinRequestRowsByFoundryId.Find(Req.FoundryId))
				{
					if (UFoundryInviteRow* Row = *Found)
					{
						Row->SetupJoinRequest(Req.FoundryId, Req.DisplayName.IsEmpty() ? Req.Username : Req.DisplayName);
						Row->OnAccept.RemoveAll(this);
						Row->OnDecline.RemoveAll(this);
						Row->OnAccept.AddWeakLambda(this, [this](const FString& Id) { if (UFoundrySocialSubsystem* S = Subsystem()) { S->AcceptJoinRequest(Id); } });
						Row->OnDecline.AddWeakLambda(this, [this](const FString& Id) { if (UFoundrySocialSubsystem* S = Subsystem()) { S->DeclineJoinRequest(Id); } });
					}
				}
			}
		}
	}

	const int32 PendingRequests = Sdk ? Sdk->PendingRequests() : 0;
	SetText(RequestsNote, PendingRequests > 0 ? FString::Printf(TEXT("%d friend request%s waiting - accept them in the Foundry launcher."), PendingRequests, PendingRequests == 1 ? TEXT("") : TEXT("s")) : FString());
	Show(RequestsNote, PendingRequests > 0);

	// -- online / offline friends (search filters both; blocked is never filtered) - reused by id
	int32 OnlineCount = 0;
	int32 OfflineCount = 0;
	int32 OnlineListed = 0;
	TArray<FString> WantedOnlineKeys;
	TArray<FString> WantedOfflineKeys;
	if (Sdk)
	{
		for (const FFoundryFriend& F : Sdk->Friends())
		{
			const bool bMatchesFilter = Filter.IsEmpty() || F.DisplayName.Contains(Filter) || F.Username.Contains(Filter);
			const bool bOnline = !(F.Presence == TEXT("offline") || F.Presence.IsEmpty());
			if (bOnline)
			{
				++OnlineCount;
				if (bOnlineExpanded && bMatchesFilter)
				{
					WantedOnlineKeys.Add(F.FoundryId);
				}
			}
			else
			{
				++OfflineCount;
				if (bOfflineExpanded && bMatchesFilter)
				{
					WantedOfflineKeys.Add(F.FoundryId);
				}
			}
		}
	}
	SyncRowList<UFoundryFriendRow>(OnlineRowsByFoundryId, OnlineList, WantedOnlineKeys,
		[this]() { return AddFriendRow(OnlineList); });
	SyncRowList<UFoundryFriendRow>(OfflineRowsByFoundryId, OfflineList, WantedOfflineKeys,
		[this]() { return AddFriendRow(OfflineList); });
	if (Sdk)
	{
		int32 OnlineTintIndex = 0;
		int32 OfflineTintIndex = 0;
		for (const FFoundryFriend& F : Sdk->Friends())
		{
			const bool bMatchesFilter = Filter.IsEmpty() || F.DisplayName.Contains(Filter) || F.Username.Contains(Filter);
			const bool bOnline = !(F.Presence == TEXT("offline") || F.Presence.IsEmpty());
			if (bOnline)
			{
				if (bOnlineExpanded && bMatchesFilter)
				{
					if (TObjectPtr<UFoundryFriendRow>* Found = OnlineRowsByFoundryId.Find(F.FoundryId))
					{
						if (UFoundryFriendRow* Row = *Found)
						{
							// "In a party 2 / 6" when the friend is in one (the partyId itself is never on the wire).
							FString Status = FoundryPartyRules::FriendPartyLine(F.PresenceGame, F.bInParty, F.PartySize, F.PartyMaxSize);
							if (Status.IsEmpty() && !F.PresenceGame.IsEmpty())
							{
								Status = FString::Printf(TEXT("Playing %s"), *F.PresenceGame);
							}
							Row->Setup(F, EFoundryFriendRowKind::Online, false, false, true, GetMemberTint(F.FoundryId, OnlineTintIndex++, false), Status);
							Row->OnContextMenuRequested.RemoveAll(this);
							Row->OnContextMenuRequested.AddWeakLambda(this, [this](UFoundryFriendRow* R, const FVector2D& Pos) { OpenFriendMenu(R, Pos); });
							++OnlineListed;
						}
					}
				}
			}
			else if (bOfflineExpanded && bMatchesFilter)
			{
				if (TObjectPtr<UFoundryFriendRow>* Found = OfflineRowsByFoundryId.Find(F.FoundryId))
				{
					if (UFoundryFriendRow* Row = *Found)
					{
						Row->Setup(F, EFoundryFriendRowKind::Offline, false, false, true, GetMemberTint(F.FoundryId, OfflineTintIndex++, false), FString());
						Row->OnContextMenuRequested.RemoveAll(this);
						Row->OnContextMenuRequested.AddWeakLambda(this, [this](UFoundryFriendRow* R, const FVector2D& Pos) { OpenFriendMenu(R, Pos); });
					}
				}
			}
		}
	}
	SetText(OnlineHeaderText, FString::Printf(TEXT("ONLINE   %d"), OnlineCount));
	Show(OnlineList, bOnlineExpanded);
	SetText(OnlineEmptyText, bSignedIn ? TEXT("No friends online right now.") : TEXT("Sign in to see your friends."));
	Show(OnlineEmptyText, bOnlineExpanded && OnlineListed == 0);
	SetText(OfflineHeaderText, FString::Printf(TEXT("OFFLINE   %d"), OfflineCount));
	Show(OfflineList, bOfflineExpanded);

	// -- blocked (never filtered) - reused by id
	const int32 BlockedCount = Sdk ? Sdk->Blocked().Num() : 0;
	SetText(BlockedHeaderText, FString::Printf(TEXT("BLOCKED   %d"), BlockedCount));
	TArray<FString> WantedBlockedKeys;
	if (Sdk && bBlockedExpanded)
	{
		for (const FFoundryFriend& F : Sdk->Blocked())
		{
			WantedBlockedKeys.Add(F.FoundryId);
		}
	}
	SyncRowList<UFoundryFriendRow>(BlockedRowsByFoundryId, BlockedList, WantedBlockedKeys,
		[this]() { return AddFriendRow(BlockedList); });
	if (Sdk && bBlockedExpanded)
	{
		for (const FFoundryFriend& F : Sdk->Blocked())
		{
			if (TObjectPtr<UFoundryFriendRow>* Found = BlockedRowsByFoundryId.Find(F.FoundryId))
			{
				if (UFoundryFriendRow* Row = *Found)
				{
					Row->Setup(F, EFoundryFriendRowKind::Blocked, false, false, true, GetMemberTint(F.FoundryId, 0, false), FString());
					Row->OnContextMenuRequested.RemoveAll(this);
					Row->OnContextMenuRequested.AddWeakLambda(this, [this](UFoundryFriendRow* R, const FVector2D& Pos) { OpenFriendMenu(R, Pos); });
				}
			}
		}
	}
	Show(BlockedList, bBlockedExpanded);

	Show(LeaveButton, bInParty && !bFixture, true);

	// -- ready toggle (visible only while in a party; mirrors your own ready flag)
	const bool bSelfReady = Sdk && !bFixture && IsSelfReadyHere(*Sdk);
	SetText(ReadyToggleText, bSelfReady ? TEXT("UNREADY") : TEXT("READY"));
	Show(ReadyToggleButton, bInParty && !bFixture, true);
}

void UFoundrySocialDrawer::ApplyAddFriendEntry()
{
	const bool bOpen = bAddFriendEntryOpen && AddFriendBlockedReason.IsEmpty();
	Show(AddFriendEntry, bOpen, true);
	SetText(AddFriendNote, AddFriendBlockedReason);
	Show(AddFriendNote, !AddFriendBlockedReason.IsEmpty() && bAddFriendNoteRequested);
	OnAddFriendEntryChanged(bOpen);
}

void UFoundrySocialDrawer::HandleClose()
{
	OnCloseRequested.Broadcast();
}

void UFoundrySocialDrawer::HandleManageAccount()
{
	FPlatformProcess::LaunchURL(*UFoundryUISettings::Get()->AccountUrl, nullptr, nullptr);
}

void UFoundrySocialDrawer::HandleSearchChanged(const FText& Text)
{
	Filter = Text.ToString().TrimStartAndEnd();
	MarkDirty();
}

void UFoundrySocialDrawer::HandleToggleOnline()
{
	bOnlineExpanded = !bOnlineExpanded;
	if (OnlineChevron)
	{
		TweenAngle(OnlineChevron, bOnlineExpanded ? 0.0f : 180.0f, bOnlineExpanded ? 180.0f : 0.0f, ChevronTweenSeconds);
	}
	OnOnlineExpandedChanged(bOnlineExpanded);
	MarkDirty();
}

void UFoundrySocialDrawer::HandleToggleOffline()
{
	bOfflineExpanded = !bOfflineExpanded;
	if (OfflineChevron)
	{
		TweenAngle(OfflineChevron, bOfflineExpanded ? 0.0f : 180.0f, bOfflineExpanded ? 180.0f : 0.0f, ChevronTweenSeconds);
	}
	OnOfflineExpandedChanged(bOfflineExpanded);
	MarkDirty();
}

void UFoundrySocialDrawer::HandleToggleBlocked()
{
	bBlockedExpanded = !bBlockedExpanded;
	if (BlockedChevron)
	{
		TweenAngle(BlockedChevron, bBlockedExpanded ? 0.0f : 180.0f, bBlockedExpanded ? 180.0f : 0.0f, ChevronTweenSeconds);
	}
	OnBlockedExpandedChanged(bBlockedExpanded);
	MarkDirty();
}

void UFoundrySocialDrawer::HandleAddFriendButton()
{
	if (!AddFriendBlockedReason.IsEmpty())
	{
		bAddFriendNoteRequested = true; // say WHY instead of doing nothing
		bAddFriendEntryOpen = false;
		ApplyAddFriendEntry();
		return;
	}
	bAddFriendNoteRequested = false;
	bAddFriendEntryOpen = !bAddFriendEntryOpen;
	ApplyAddFriendEntry();
	if (bAddFriendEntryOpen && AddFriendInput)
	{
		AddFriendInput->SetKeyboardFocus();
	}
}

void UFoundrySocialDrawer::HandleAddFriendSend()
{
	UFoundrySocialSubsystem* Sdk = Subsystem();
	if (!Sdk || !AddFriendInput)
	{
		return;
	}
	const FString Username = AddFriendInput->GetText().ToString().TrimStartAndEnd();
	if (!Username.IsEmpty())
	{
		Sdk->SendFriendRequest(Username);
		AddFriendInput->SetText(FText::GetEmpty());
	}
}

void UFoundrySocialDrawer::HandleAddFriendCommitted(const FText& Text, ETextCommit::Type Commit)
{
	if (Commit == ETextCommit::OnEnter)
	{
		HandleAddFriendSend();
	}
	else if (Commit == ETextCommit::OnCleared)
	{
		// Escape in the field closes the entry first (the topmost transient thing), not the drawer.
		bAddFriendEntryOpen = false;
		ApplyAddFriendEntry();
		if (AddFriendButton)
		{
			AddFriendButton->SetKeyboardFocus();
		}
	}
}

void UFoundrySocialDrawer::HandleLeaveParty()
{
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->LeaveParty();
	}
}

void UFoundrySocialDrawer::HandleReadyToggle()
{
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->SetReady(!IsSelfReadyHere(*Sdk)); // ready elsewhere still READIES here; only ready-here un-readies
	}
}

// -- context menu --------------------------------------------------------------------------------------

void UFoundrySocialDrawer::OpenFriendMenu(UFoundryFriendRow* Row, const FVector2D& ScreenPosition)
{
	if (!Row || !FriendMenuClass)
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[SocialDrawer] OpenFriendMenu skipped: row %s, FriendMenuClass %s"),
			Row ? TEXT("ok") : TEXT("null"), FriendMenuClass ? TEXT("ok") : TEXT("null (Class Defaults / UFoundryUISettings::DefaultFriendMenu)"));
		return;
	}
	CloseFriendMenu(); // only one menu open at a time

	UFoundrySocialSubsystem* Sdk = Subsystem();

	// Everything the menu's entries depend on, gathered once; the decision itself (which entries, which
	// enabled, why not) is FoundryPartyRules::PlanFriendMenu so the launcher-parity rules live in one
	// tested place instead of here.
	FoundryPartyRules::FFriendMenuContext Ctx;
	Ctx.Kind = Row->Kind;
	Ctx.bPendingMember = Row->Kind == EFoundryFriendRowKind::Party && !Row->bIsJoined;
	Ctx.bSignedIn = Sdk && Sdk->IsSignedIn();
	Ctx.bHasParty = Sdk && Sdk->HasParty();
	Ctx.bLocalLeader = !Sdk || !Sdk->HasParty() || Sdk->IsLocalLeader();
	Ctx.bTargetInviteExpired = Row->IsInviteExpired();
	Ctx.bFriendPartyJoinable = Row->Friend.bPartyJoinable;
	Ctx.PartyCount = Sdk ? Sdk->JoinedCount() + Sdk->LiveInvitedCount() : 0;
	Ctx.Cap = Sdk ? FoundryPartyRules::CapFor(*Sdk) : 0;
	if (Sdk && Sdk->HasParty())
	{
		for (const FFoundryPartyMember& M : Sdk->Party().Members)
		{
			// JOINED members only: a friend who merely holds a live invite is not "already in your party" -
			// Invite stays enabled for them and a repeat invite is fid's call (it resets the invite's clock
			// and re-notifies; Justin, 2026-10-17: "we just don't want to disable the invite button").
			if (M.FoundryId == Row->Friend.FoundryId && IsJoinedState(M.State))
			{
				Ctx.bTargetInLocalParty = true;
				break;
			}
		}
	}
	const FoundryPartyRules::FFriendMenuPlan Plan = FoundryPartyRules::PlanFriendMenu(Ctx);
	if (!Plan.AnyShown())
	{
		return; // a row the local player has nothing to do with (e.g. a pending invitee on a non-leader's roster)
	}

	ActiveMenu = CreateWidget<UFoundryFriendMenu>(this, FriendMenuClass);
	ActiveMenu->Setup(Row->Kind, Row->Friend.DisplayName, Plan);
	ActiveMenu->OnItem.AddUObject(this, &UFoundrySocialDrawer::HandleMenuItem);
	ActiveMenu->OnConfirm.AddUObject(this, &UFoundrySocialDrawer::HandleMenuConfirm);
	ActiveMenu->OnCancel.AddUObject(this, &UFoundrySocialDrawer::HandleMenuCancel);
	PendingActionFoundryId = Row->Friend.FoundryId;
	PendingActionUsername = Row->Friend.Username.IsEmpty() ? Row->Friend.DisplayName : Row->Friend.Username;

	// SFoundryPopupHost: kit tooltips drawn in the game window (never an OS tooltip window) and the
	// project's hardware cursor re-applied over the menu - see the class comment.
	TSharedRef<SWidget> MenuContent = SNew(SFoundryPopupHost)
		.ViewportClient(GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
		[
			ActiveMenu->TakeWidget()
		];
	FWidgetPath OwnerPath;
	const bool bPath = FSlateApplication::Get().GeneratePathToWidgetUnchecked(TakeWidget(), OwnerPath);
	// An empty OwnerPath is acceptable - PushMenu falls back to resolving the parent window from
	// TakeWidget() itself when no path is given.
	TSharedPtr<IMenu> Menu = FSlateApplication::Get().PushMenu(
		TakeWidget(), OwnerPath, MenuContent, ScreenPosition,
		FPopupTransitionEffect(FPopupTransitionEffect::ContextMenu),
		/*bFocusImmediately*/ true);
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[SocialDrawer] friend menu for %s at (%.0f, %.0f): path %s, menu %s"),
		*Row->Friend.DisplayName, ScreenPosition.X, ScreenPosition.Y, bPath ? TEXT("ok") : TEXT("EMPTY"), Menu.IsValid() ? TEXT("pushed") : TEXT("REFUSED"));
	if (const TSharedPtr<SWidget> Focused = FSlateApplication::Get().GetUserFocusedWidget(0))
	{
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[SocialDrawer] after push, focused widget: %s"), *Focused->GetTypeAsString());
	}
	else
	{
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[SocialDrawer] after push, focused widget: NONE"));
	}
	if (Menu.IsValid())
	{
		ActiveMenuHandle = Menu;
		Menu->GetOnMenuDismissed().AddUObject(this, &UFoundrySocialDrawer::HandleMenuDismissed);
		// The engine draws its dark Menu.Background box under every pushed menu - black corners
		// around the rounded menu panel until it is hidden (see HideEngineMenuChrome).
		const int32 Hidden = HideEngineMenuChrome(Menu);
		UE_LOG(LogFoundryFSDKUI, Log, TEXT("[SocialDrawer] engine menu chrome hidden: %d image(s)"), Hidden);
	}
}

void UFoundrySocialDrawer::CloseFriendMenu()
{
	if (TSharedPtr<IMenu> Menu = ActiveMenuHandle.Pin())
	{
		Menu->Dismiss(); // HandleMenuDismissed (bound to GetOnMenuDismissed) clears ActiveMenu/ActiveMenuHandle
	}
}

void UFoundrySocialDrawer::HandleMenuDismissed(TSharedRef<IMenu> /*DismissedMenu*/)
{
	UE_LOG(LogFoundryFSDKUI, Log, TEXT("[SocialDrawer] friend menu dismissed"));
	ActiveMenu = nullptr;
	ActiveMenuHandle.Reset();
}

void UFoundrySocialDrawer::HandleMenuItem(EFoundryFriendMenuItem Item)
{
	if (!ActiveMenu)
	{
		return;
	}
	UFoundrySocialSubsystem* Sdk = Subsystem();
	const FString TargetId = PendingActionFoundryId;
	const FString TargetDisplayName = ActiveMenu->TargetName;

	switch (Item)
	{
	case EFoundryFriendMenuItem::SendMessage:
		OnWhisperRequested.Broadcast(TargetId, TargetDisplayName);
		CloseFriendMenu();
		break;
	case EFoundryFriendMenuItem::Invite:
		if (Sdk)
		{
			Sdk->InviteByUsername(PendingActionUsername);
		}
		CloseFriendMenu();
		break;
	case EFoundryFriendMenuItem::RequestToJoin:
		if (Sdk)
		{
			Sdk->RequestToJoin(TargetId);
		}
		CloseFriendMenu();
		break;
	case EFoundryFriendMenuItem::ReInvite:
		// Resets the invite's 15-minute clock and re-notifies (fid: a repeat invite bumps expires_at).
		if (Sdk)
		{
			Sdk->ReInvite(TargetId);
		}
		CloseFriendMenu();
		break;
	case EFoundryFriendMenuItem::CancelInvite:
		if (Sdk)
		{
			Sdk->CancelInvite(TargetId);
		}
		CloseFriendMenu();
		break;
	case EFoundryFriendMenuItem::RemoveFromParty:
		// TargetId / TargetDisplayName were copied above: closing the menu clears ActiveMenu.
		CloseFriendMenu();
		AskKickMember(TargetId, TargetDisplayName);
		break;
	case EFoundryFriendMenuItem::RemoveFriend:
		PendingAction = Item;
		ActiveMenu->ShowConfirm(FString::Printf(TEXT("Remove %s?\nThey won't be notified. You can add each other again any time."), *TargetDisplayName), TEXT("Remove"));
		break;
	case EFoundryFriendMenuItem::Block:
		PendingAction = Item;
		ActiveMenu->ShowConfirm(FString::Printf(TEXT("Block %s?\nBlocks %s: removes them from your friends and stops requests/messages."), *TargetDisplayName, *TargetDisplayName), TEXT("Block"));
		break;
	case EFoundryFriendMenuItem::Unblock:
		if (Sdk)
		{
			Sdk->UnblockPlayer(TargetId);
		}
		CloseFriendMenu();
		break;
	}
}

void UFoundrySocialDrawer::HandleMenuConfirm()
{
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		if (PendingAction == EFoundryFriendMenuItem::RemoveFriend)
		{
			Sdk->RemoveFriend(PendingActionFoundryId);
		}
		else if (PendingAction == EFoundryFriendMenuItem::Block)
		{
			Sdk->BlockPlayer(PendingActionFoundryId);
		}
	}
	CloseFriendMenu();
}

void UFoundrySocialDrawer::HandleMenuCancel()
{
	// Back to the items (never a dismiss - the menu stays open so another action can be picked).
	if (ActiveMenu)
	{
		ActiveMenu->ShowItems();
	}
}
