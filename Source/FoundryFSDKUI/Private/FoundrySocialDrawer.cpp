#include "FoundrySocialDrawer.h"

#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/Image.h"
#include "Components/PanelWidget.h"
#include "Components/TextBlock.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/IMenu.h"
#include "Framework/Application/SlateApplication.h"
#include "FoundryInviteRow.h"
#include "FoundrySocialSubsystem.h"
#include "FoundryUISettings.h"
#include "FoundryUITheme.h"
#include "HAL/PlatformProcess.h"
#include "TimerManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogFoundryFSDKUI, Log, All);

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
	Show(AddFriendEntry, false);
	Show(AddFriendNote, false);
	Show(OfflineList, false);
	Show(BlockedList, false);
}

void UFoundrySocialDrawer::NativeConstruct()
{
	Super::NativeConstruct();
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->RegisterConsumer();
		Sdk->OnChanged.AddUObject(this, &UFoundrySocialDrawer::MarkDirty);
		Sdk->OnActionFailed.AddUObject(this, &UFoundrySocialDrawer::HandleActionFailed);
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
		Sdk->UnregisterConsumer();
	}
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ActionNoteTimerHandle);
	}
	CloseFriendMenu();
	Super::NativeDestruct();
}

void UFoundrySocialDrawer::SetOpen(bool bOpen)
{
	if (UFoundrySocialSubsystem* Sdk = Subsystem())
	{
		Sdk->SetPanelOpen(bOpen);
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

void UFoundrySocialDrawer::HandleActionFailed(const FString& Action, const FString& Message)
{
	if (Action == TEXT("friend.request"))
	{
		SetText(AddFriendNote, Message);
		Show(AddFriendNote, true);
		return;
	}

	SetText(ActionNote, Message);
	Show(ActionNote, true);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(ActionNoteTimerHandle, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			Show(ActionNote, false);
		}), 6.0f, false);
	}
	OnActionMessage.Broadcast(Message);
}

void UFoundrySocialDrawer::Rebuild()
{
	UFoundrySocialSubsystem* Sdk = Subsystem();
	const bool bSignedIn = Sdk && Sdk->IsSignedIn();
	const bool bFixture = Sdk && Sdk->IsFixtureActive();
	const int32 Cap = Sdk ? Sdk->PartyCapacity() : UFoundrySocialSubsystem::DefaultPartyCapacity;
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

	// -- party header + cap conflict note
	SetText(PartyHeaderText, FString::Printf(TEXT("YOUR PARTY   %d / %d"), Joined, Cap));
	FString Conflict;
	if (Sdk && bInParty && Sdk->Party().MaxSize > 0 && Sdk->Party().MaxSize != UFoundrySocialSubsystem::DefaultPartyCapacity)
	{
		Conflict = FString::Printf(TEXT("This party is capped at %d (not %d)."), Sdk->Party().MaxSize, UFoundrySocialSubsystem::DefaultPartyCapacity);
	}
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

	// -- party rows (every member - joined AND pending/invited; local + pending rows open no menu)
	if (PartyList)
	{
		PartyList->ClearChildren();
		if (Sdk)
		{
			if (bInParty)
			{
				const FFoundryParty& Party = Sdk->Party();
				for (int32 i = 0; i < Party.Members.Num(); ++i)
				{
					const FFoundryPartyMember& M = Party.Members[i];
					const bool bJoinedMember = M.State.IsEmpty() || M.State.Equals(TEXT("JOINED"), ESearchCase::IgnoreCase);
					const bool bLocal = !Sdk->LocalFoundryId().IsEmpty() && M.FoundryId == Sdk->LocalFoundryId();
					const bool bHost = !Party.LeaderFoundryId.IsEmpty() && M.FoundryId == Party.LeaderFoundryId;

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

					if (UFoundryFriendRow* Row = AddFriendRow(PartyList))
					{
						const FString Status = bJoinedMember ? FString() : TEXT("Invited - not joined");
						Row->Setup(F, EFoundryFriendRowKind::Party, bHost, bLocal, bJoinedMember,
							GetMemberTint(F.FoundryId, i, bLocal), Status);
						// The local player's own row and a pending-invited row never open a menu.
						if (!bLocal && bJoinedMember)
						{
							Row->OnContextMenuRequested.AddWeakLambda(this, [this](UFoundryFriendRow* R, const FVector2D& Pos)
							{
								OpenFriendMenu(R, Pos);
							});
						}
					}
				}
			}
			else if (UFoundryFriendRow* Row = AddFriendRow(PartyList))
			{
				FFoundryFriend Me;
				Me.FoundryId = Sdk->LocalFoundryId();
				Me.DisplayName = bSignedIn ? Sdk->LocalDisplayName() : TEXT("Not signed in");
				Me.Username = Sdk->LocalUsername();
				Me.Presence = bSignedIn ? TEXT("online") : TEXT("offline");
				Row->Setup(Me, EFoundryFriendRowKind::Party, true, true, true, GetMemberTint(Me.FoundryId, 0, true), FString());
			}
		}
	}

	// -- incoming party invites
	const int32 NumInvites = Sdk ? Sdk->Invites().Num() : 0;
	Show(InvitesSection, NumInvites > 0);
	if (InvitesList)
	{
		InvitesList->ClearChildren();
		if (Sdk && InviteRowClass)
		{
			for (const FFoundryParty& Inv : Sdk->Invites())
			{
				UFoundryInviteRow* Row = CreateWidget<UFoundryInviteRow>(this, InviteRowClass);
				InvitesList->AddChild(Row);
				Row->Setup(Inv.LeaderDisplayName, Inv.PartyId);
				Row->OnAccept.AddWeakLambda(this, [this](const FString& Id) { if (UFoundrySocialSubsystem* S = Subsystem()) { S->AcceptInvite(Id); } });
				Row->OnDecline.AddWeakLambda(this, [this](const FString& Id) { if (UFoundrySocialSubsystem* S = Subsystem()) { S->DeclineInvite(Id); } });
			}
		}
	}

	const int32 PendingRequests = Sdk ? Sdk->PendingRequests() : 0;
	SetText(RequestsNote, PendingRequests > 0 ? FString::Printf(TEXT("%d friend request%s waiting - accept them in the Foundry launcher."), PendingRequests, PendingRequests == 1 ? TEXT("") : TEXT("s")) : FString());
	Show(RequestsNote, PendingRequests > 0);

	// -- online / offline friends (search filters both; blocked is never filtered)
	int32 OnlineCount = 0;
	int32 OfflineCount = 0;
	int32 OnlineListed = 0;
	int32 OnlineTintIndex = 0;
	int32 OfflineTintIndex = 0;
	if (OnlineList) { OnlineList->ClearChildren(); }
	if (OfflineList) { OfflineList->ClearChildren(); }
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
					if (UFoundryFriendRow* Row = AddFriendRow(OnlineList))
					{
						const FString Status = F.PresenceGame.IsEmpty() ? FString() : FString::Printf(TEXT("Playing %s"), *F.PresenceGame);
						Row->Setup(F, EFoundryFriendRowKind::Online, false, false, true, GetMemberTint(F.FoundryId, OnlineTintIndex++, false), Status);
						Row->OnContextMenuRequested.AddWeakLambda(this, [this](UFoundryFriendRow* R, const FVector2D& Pos) { OpenFriendMenu(R, Pos); });
						++OnlineListed;
					}
				}
			}
			else
			{
				++OfflineCount;
				if (bOfflineExpanded && bMatchesFilter)
				{
					if (UFoundryFriendRow* Row = AddFriendRow(OfflineList))
					{
						Row->Setup(F, EFoundryFriendRowKind::Offline, false, false, true, GetMemberTint(F.FoundryId, OfflineTintIndex++, false), FString());
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

	// -- blocked (never filtered)
	const int32 BlockedCount = Sdk ? Sdk->Blocked().Num() : 0;
	SetText(BlockedHeaderText, FString::Printf(TEXT("BLOCKED   %d"), BlockedCount));
	if (BlockedList)
	{
		BlockedList->ClearChildren();
		if (Sdk && bBlockedExpanded)
		{
			for (const FFoundryFriend& F : Sdk->Blocked())
			{
				if (UFoundryFriendRow* Row = AddFriendRow(BlockedList))
				{
					Row->Setup(F, EFoundryFriendRowKind::Blocked, false, false, true, GetMemberTint(F.FoundryId, 0, false), FString());
					Row->OnContextMenuRequested.AddWeakLambda(this, [this](UFoundryFriendRow* R, const FVector2D& Pos) { OpenFriendMenu(R, Pos); });
				}
			}
		}
	}
	Show(BlockedList, bBlockedExpanded);

	Show(LeaveButton, bInParty && !bFixture, true);
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
	OnOnlineExpandedChanged(bOnlineExpanded);
	MarkDirty();
}

void UFoundrySocialDrawer::HandleToggleOffline()
{
	bOfflineExpanded = !bOfflineExpanded;
	OnOfflineExpandedChanged(bOfflineExpanded);
	MarkDirty();
}

void UFoundrySocialDrawer::HandleToggleBlocked()
{
	bBlockedExpanded = !bBlockedExpanded;
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
	const bool bSignedIn = Sdk && Sdk->IsSignedIn();
	const bool bFull = Sdk && Sdk->JoinedCount() >= Sdk->PartyCapacity();
	const bool bLocalLeader = !Sdk || !Sdk->HasParty() || Sdk->IsLocalLeader();
	bool bAlreadyInParty = false;
	if (Sdk && Sdk->HasParty())
	{
		for (const FFoundryPartyMember& M : Sdk->Party().Members)
		{
			if (M.FoundryId == Row->Friend.FoundryId)
			{
				bAlreadyInParty = true;
				break;
			}
		}
	}
	const bool bCanInvite = bSignedIn && !bFull && bLocalLeader && !bAlreadyInParty;
	const bool bCanRequestJoin = bSignedIn && Sdk && !Sdk->HasParty();

	ActiveMenu = CreateWidget<UFoundryFriendMenu>(this, FriendMenuClass);
	ActiveMenu->Setup(Row->Kind, Row->Friend.DisplayName, bCanInvite, bCanRequestJoin);
	ActiveMenu->OnItem.AddUObject(this, &UFoundrySocialDrawer::HandleMenuItem);
	ActiveMenu->OnConfirm.AddUObject(this, &UFoundrySocialDrawer::HandleMenuConfirm);
	ActiveMenu->OnCancel.AddUObject(this, &UFoundrySocialDrawer::HandleMenuCancel);
	PendingActionFoundryId = Row->Friend.FoundryId;
	PendingActionUsername = Row->Friend.Username.IsEmpty() ? Row->Friend.DisplayName : Row->Friend.Username;

	TSharedRef<SWidget> MenuContent = ActiveMenu->TakeWidget();
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
