// Copyright Foundry Media. Automation test for the lobby-facing matchmaking state machine in UFMMSSubsystem.
//
// Pins the FMMS-phase -> EFoundryMatchmakingState mapping, the search clock, the cancel round trip and the
// dev fixture - the rules a screenshot cannot prove. The subsystem is built on a bare transient
// GameInstance (never Init()ed), so Fsdk() is null and the TTL timers never fire: nothing here touches the network.
// Run: UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests FoundryFSDK.FMMS" -unattended -nopause -NullRHI

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "FMMSSubsystem.h"
#include "FoundryQueueKey.h"
#include "FoundrySocialSubsystem.h"
#include "Misc/AutomationTest.h"
#include "TimerManager.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSMatchmakingStateTest, "FoundryFSDK.FMMS.MatchmakingStateMachine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSMatchmakingStateTest::RunTest(const FString& Parameters)
{
	// UGameInstanceSubsystem is Within=GameInstance: an outer-less NewObject ensures, and the ensure fails the test.
	// A bare transient GameInstance is never Init()ed: its subsystem collection stays empty (Fsdk() null) and
	// its timer manager exists but never ticks (TTLs never fire). Nothing here touches the network.
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);

	// No SDK, not signed in: nothing can be requested, and a refused request changes nothing.
	TestFalse(TEXT("cannot request without the SDK"), Fmms->CanRequest());
	TestFalse(TEXT("request refused"), Fmms->RequestFindMatch(TEXT("goo-crew/spread"), TEXT("{}")));
	TestEqual(TEXT("request refused stays Idle"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Idle));
	TestTrue(TEXT("no clock while idle"), Fmms->GetElapsedSearchSeconds() < 0.0f);

	// The FMMS phases map one to one.
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Requesting, TEXT("Requesting"));
	TestEqual(TEXT("Requesting -> Pending"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Pending));
	TestTrue(TEXT("clock started"), Fmms->GetElapsedSearchSeconds() >= 0.0f);
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Searching, TEXT("Searching"));
	TestEqual(TEXT("Searching"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Searching));
	TestTrue(TEXT("can cancel while searching"), Fmms->CanCancel());
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::WaitingForCapacity, TEXT("Starting a server"));
	TestEqual(TEXT("WaitingForCapacity"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::WaitingForCapacity));
	TestTrue(TEXT("a capacity wait is still a live search"), Fmms->CanCancel());
	TestEqual(TEXT("server sentence kept verbatim"), Fmms->GetMatchmakingMessage(), FString(TEXT("Starting a server")));
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Connecting, TEXT("Found"));
	TestEqual(TEXT("Connecting -> Found"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Found));
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("Joining"));
	TestEqual(TEXT("Traveling"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Traveling));
	TestFalse(TEXT("cannot cancel while traveling"), Fmms->CanCancel());

	// Failure resets the clock and keeps its message; Idle clears the message.
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Failed, TEXT("Queue not found"));
	TestEqual(TEXT("Failed"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Failed));
	TestTrue(TEXT("clock stopped on failure"), Fmms->GetElapsedSearchSeconds() < 0.0f);
	TestEqual(TEXT("failure message kept"), Fmms->GetMatchmakingMessage(), FString(TEXT("Queue not found")));
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Idle, FString());
	TestEqual(TEXT("Idle"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Idle));

	// Reconnectable is its own state and the clock does not run.
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Reconnectable, TEXT("live seat"));
	TestEqual(TEXT("Reconnectable"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Reconnectable));
	TestTrue(TEXT("no clock while reconnectable"), Fmms->GetElapsedSearchSeconds() < 0.0f);
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Idle, FString());

	// Cancel round trip. Cancel() flips the FMMS phase to Idle synchronously, so CancelPending is visible
	// only to listeners: record the broadcasts and assert the sequence, then the resolved end state.
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Searching, TEXT("Searching"));
	TArray<EFoundryMatchmakingState> Seen;
	const FDelegateHandle Handle = Fmms->OnMatchmakingStateChanged.AddLambda(
		[&Seen](EFoundryMatchmakingState State, const FString&) { Seen.Add(State); });
	Fmms->RequestCancel();
	Fmms->OnMatchmakingStateChanged.Remove(Handle);
	TestEqual(TEXT("cancel broadcast CancelPending then Idle"), Seen.Num(), 2);
	if (Seen.Num() == 2)
	{
		TestEqual(TEXT("first CancelPending"), static_cast<int32>(Seen[0]), static_cast<int32>(EFoundryMatchmakingState::CancelPending));
		TestEqual(TEXT("then Idle"), static_cast<int32>(Seen[1]), static_cast<int32>(EFoundryMatchmakingState::Idle));
	}
	TestEqual(TEXT("cancel resolves to Idle"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Idle));
	TestEqual(TEXT("cancel message"), Fmms->GetMatchmakingMessage(), FString(TEXT("Search cancelled.")));
	TestTrue(TEXT("clock stopped after cancel"), Fmms->GetElapsedSearchSeconds() < 0.0f);
	// Cancel with nothing live is a no-op.
	Fmms->RequestCancel();
	TestEqual(TEXT("idle cancel stays Idle"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Idle));

	// Dev fixture: replaces the displayed state, ignores real phase changes, refuses requests.
	TestFalse(TEXT("no fixture yet"), Fmms->IsMatchmakingFixtureActive());
	const bool bFixture = Fmms->SetMatchmakingFixture(EFoundryMatchmakingState::Searching, TEXT("x"), 42.0f);
#if !UE_BUILD_SHIPPING
	TestTrue(TEXT("fixture accepted"), bFixture);
	TestTrue(TEXT("fixture active"), Fmms->IsMatchmakingFixtureActive());
	TestEqual(TEXT("fixture state"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Searching));
	TestEqual(TEXT("fixture message"), Fmms->GetMatchmakingMessage(), FString(TEXT("x")));
	TestTrue(TEXT("fixture clock starts in the past"), Fmms->GetElapsedSearchSeconds() >= 41.0f);
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Failed, TEXT("real failure"));
	TestEqual(TEXT("real phase ignored under a fixture"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Searching));
	TestFalse(TEXT("no request under a fixture"), Fmms->CanRequest());
	Fmms->RequestCancel();
	TestEqual(TEXT("no cancel under a fixture"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Searching));
	Fmms->ClearMatchmakingFixture();
	TestFalse(TEXT("fixture cleared"), Fmms->IsMatchmakingFixtureActive());
	TestEqual(TEXT("state re-derived from the real phase"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Idle));
	TestTrue(TEXT("clock stopped after clearing"), Fmms->GetElapsedSearchSeconds() < 0.0f);
#else
	TestFalse(TEXT("fixture refused in Shipping"), bFixture);
#endif
	return true;
}


// A listener that, on the Pending broadcast, replaces the array the caller's queue reference points into -
// the shape of UFoundryMatchCluster::HandleFmmsState -> Refresh() -> Modes = GetModes(). Before the copy-first
// fix RequestFindMatch then read freed memory (client 0.1.1 crash, 2026-10-15).
namespace
{
	struct FFMMSReallocatingListener
	{
		TArray<FString> Holder;
		void OnState(EFoundryMatchmakingState State, const FString& /*Message*/)
		{
			if (State == EFoundryMatchmakingState::Pending)
			{
				TArray<FString> Fresh;
				for (int32 i = 0; i < 64; ++i)
				{
					Fresh.Add(FString::Printf(TEXT("filler-%d-xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"), i));
				}
				Holder = MoveTemp(Fresh); // the old buffer - and the caller's reference into it - is gone
			}
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSFindMatchArgumentsSurviveBroadcastTest, "FoundryFSDK.FMMS.FindMatchArgumentsSurviveBroadcast",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FFMMSFindMatchArgumentsSurviveBroadcastTest::RunTest(const FString& Parameters)
{
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
	FFMMSReallocatingListener Listener;
	Listener.Holder.Add(TEXT("goo-crew/spread"));
	Listener.Holder.Add(TEXT("{\"partyId\":\"p-1\"}"));
	Fmms->OnMatchmakingStateChanged.AddRaw(&Listener, &FFMMSReallocatingListener::OnState);

	// With no FSDK subsystem the request fails inside FindMatchAuthenticated - AFTER the Pending broadcast
	// has run the listener and AFTER both parameters were read. Reaching the assertions is the test; the
	// subsystem must have consumed COPIES, not the references into Listener.Holder.
	const bool bAccepted = Fmms->RequestFindMatch(Listener.Holder[0], Listener.Holder[1]);
	TestFalse(TEXT("no FSDK: the request is refused after the broadcast"), bAccepted);
	TestTrue(TEXT("state is Failed or Idle, never stuck on Pending"),
		Fmms->GetMatchmakingState() != EFoundryMatchmakingState::Pending);
	Fmms->OnMatchmakingStateChanged.RemoveAll(&Listener);
	return true;
}


// -- match end: Cancel from Traveling, NotifyMatchEnded, and the dead-seat belt (goo-match-end-loop) ---------------
// Fsdk() is null on a bare transient GameInstance, so the server-side CancelMatch / AbandonMatch calls are NOT
// observable here; these tests pin the state transitions and the local handle bookkeeping (EndedTicketId, the
// seat memory) around them. The live proof is the lobby after a real match: no Reconnect offered.
namespace
{
	FFoundrySessionSeat MakeLiveSeat(const TCHAR* Ticket, const TCHAR* Match)
	{
		FFoundrySessionSeat Seat;
		Seat.bActive = true;
		Seat.TicketId = Ticket;
		Seat.MatchId = Match;
		return Seat;
	}

	int32 StateOf(const UFMMSSubsystem* Fmms)
	{
		return static_cast<int32>(Fmms->GetMatchmakingState());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSCancelFromTravelingTest, "FoundryFSDK.FMMS.CancelFromTravelingResetsToIdle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSCancelFromTravelingTest::RunTest(const FString& Parameters)
{
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);

	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("Joining"));
	TestEqual(TEXT("precondition: Traveling"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Traveling));
	Fmms->Cancel();
	TestEqual(TEXT("Cancel from Traveling resets to Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
	TestEqual(TEXT("cancel message"), Fmms->GetMatchmakingMessage(), FString(TEXT("Cancelled.")));
	TestEqual(TEXT("FMMS phase Idle"), static_cast<int32>(Fmms->GetPhase()), static_cast<int32>(EFMMSPhase::Idle));
	TestTrue(TEXT("no clock after cancel"), Fmms->GetElapsedSearchSeconds() < 0.0f);
	TestTrue(TEXT("no ticket id was ever learned, so none is remembered"), Fmms->GetEndedTicketId().IsEmpty());

	// Cancel from Reconnectable declines the live seat and remembers it as released.
	UFMMSSubsystem* Fmms2 = NewObject<UFMMSSubsystem>(GI);
	Fmms2->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
	TestEqual(TEXT("live seat offered"), StateOf(Fmms2), static_cast<int32>(EFoundryMatchmakingState::Reconnectable));
	Fmms2->Cancel();
	TestEqual(TEXT("decline resets to Idle"), StateOf(Fmms2), static_cast<int32>(EFoundryMatchmakingState::Idle));
	TestFalse(TEXT("no active match after decline"), Fmms2->HasActiveMatch());
	TestEqual(TEXT("the declined ticket is remembered"), Fmms2->GetEndedTicketId(), FString(TEXT("t-1")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSNotifyMatchEndedTest, "FoundryFSDK.FMMS.NotifyMatchEnded",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSNotifyMatchEndedTest::RunTest(const FString& Parameters)
{
	// From Found (Connecting) and from Traveling: Idle + "Match over", exactly ONE state broadcast.
	const EFMMSPhase StartPhases[] = { EFMMSPhase::Connecting, EFMMSPhase::Traveling };
	for (const EFMMSPhase Start : StartPhases)
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(Start, TEXT("in match"));
		TestTrue(TEXT("precondition: not Idle"), Fmms->GetMatchmakingState() != EFoundryMatchmakingState::Idle);
		TestFalse(TEXT("precondition: the match is not over yet"), Fmms->WasMatchEnded());

		TArray<EFoundryMatchmakingState> Seen;
		const FDelegateHandle Handle = Fmms->OnMatchmakingStateChanged.AddLambda(
			[&Seen](EFoundryMatchmakingState State, const FString&) { Seen.Add(State); });
		Fmms->NotifyMatchEnded();
		TestTrue(TEXT("WasMatchEnded answers true after the horn (the party-follow gate)"), Fmms->WasMatchEnded());
		TestEqual(TEXT("exactly one state broadcast"), Seen.Num(), 1);
		if (Seen.Num() == 1)
		{
			TestEqual(TEXT("the broadcast is Idle"), static_cast<int32>(Seen[0]), static_cast<int32>(EFoundryMatchmakingState::Idle));
		}
		TestEqual(TEXT("Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestEqual(TEXT("message"), Fmms->GetMatchmakingMessage(), FString(TEXT("Match over")));
		TestTrue(TEXT("Find Match is not blocked"), Fmms->GetMatchmakingState() == EFoundryMatchmakingState::Idle);
		TestEqual(TEXT("FMMS phase Idle"), static_cast<int32>(Fmms->GetPhase()), static_cast<int32>(EFMMSPhase::Idle));

		// Idempotent: the second call changes nothing and broadcasts nothing.
		Seen.Reset();
		Fmms->NotifyMatchEnded();
		TestEqual(TEXT("second call broadcasts nothing"), Seen.Num(), 0);
		TestEqual(TEXT("second call keeps Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestEqual(TEXT("second call keeps the message"), Fmms->GetMatchmakingMessage(), FString(TEXT("Match over")));
		Fmms->OnMatchmakingStateChanged.Remove(Handle);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSEndedSeatNeverReconnectableTest, "FoundryFSDK.FMMS.EndedSeatNeverReconnectable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSEndedSeatNeverReconnectableTest::RunTest(const FString& Parameters)
{
	// 1. Ticket id never learned: the match ended, my-session (reaper lag / failed cancel) still reports a seat.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("in match"));
		Fmms->NotifyMatchEnded();
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
		TestTrue(TEXT("unknown-id belt: not Reconnectable"), Fmms->GetMatchmakingState() != EFoundryMatchmakingState::Reconnectable);
		TestEqual(TEXT("unknown-id belt: stays Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestFalse(TEXT("unknown-id belt: no active match"), Fmms->HasActiveMatch());
		TestEqual(TEXT("the declined ticket is now remembered as ended"), Fmms->GetEndedTicketId(), FString(TEXT("t-1")));
		TestTrue(TEXT("the ended match id is not exposed"), Fmms->GetCurrentMatchId().IsEmpty());
	}

	// 2. Ticket id known (my-session answered during the match): only THAT seat is declined.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
		TestEqual(TEXT("precondition: a live seat is offered before the match ends"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Reconnectable));
		Fmms->NotifyMatchEnded();
		TestEqual(TEXT("ended ticket remembered"), Fmms->GetEndedTicketId(), FString(TEXT("t-1")));
		TestEqual(TEXT("Idle after the match end"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));

		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
		TestEqual(TEXT("the ended seat is declined, state stays Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestFalse(TEXT("the ended seat is never an active match"), Fmms->HasActiveMatch());

		// A DIFFERENT live ticket is a genuinely new seat (a crash mid-way through a later match): still offered.
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-2"), TEXT("m-2")));
		TestEqual(TEXT("a different ticket is a real seat"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Reconnectable));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSCancelThenRacingSeatTest, "FoundryFSDK.FMMS.CancelFromTravelingThenSeatDeclined",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSCancelThenRacingSeatTest::RunTest(const FString& Parameters)
{
	// A deliberate LEAVE (Cancel from Traveling) releases the seat; a my-session read that races the async cancel
	// (the lobby's CheckActiveSession at BeginPlay) must be declined, never offered as Reconnect.
	// 1. Ticket id never learned.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("in match"));
		Fmms->Cancel();
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
		TestEqual(TEXT("unknown id: seat declined, stays Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestFalse(TEXT("unknown id: no active match"), Fmms->HasActiveMatch());
		TestEqual(TEXT("the declined ticket is remembered"), Fmms->GetEndedTicketId(), FString(TEXT("t-1")));
	}
	// 2. Ticket id known: only that seat is declined; a different ticket is a real new seat.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1"))); // learned during the match
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("in match"));
		Fmms->Cancel();
		TestEqual(TEXT("released ticket remembered"), Fmms->GetEndedTicketId(), FString(TEXT("t-1")));
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
		TestEqual(TEXT("racing report for that ticket: declined, stays Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestFalse(TEXT("racing report: no active match"), Fmms->HasActiveMatch());
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-2"), TEXT("m-2")));
		TestEqual(TEXT("a different ticket is a real seat"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Reconnectable));
	}
	// 3. A search-time cancel (no seat) arms nothing: a live seat is offered as before.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Searching, TEXT("Searching"));
		Fmms->Cancel();
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-9"), TEXT("m-9")));
		TestEqual(TEXT("cancel while searching releases no seat"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Reconnectable));
	}
	// 4. NotifyMatchEnded after a seat-releasing Cancel still runs (only a repeat NotifyMatchEnded is a no-op).
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("in match"));
		Fmms->Cancel();
		Fmms->NotifyMatchEnded();
		TestEqual(TEXT("Match over message"), Fmms->GetMatchmakingMessage(), FString(TEXT("Match over")));
		TestEqual(TEXT("the known ended ticket survives"), Fmms->GetEndedTicketId(), FString(TEXT("t-1")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSRefusedJoinReleasesSeatTest, "FoundryFSDK.FMMS.RefusedJoinReleasesSeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSRefusedJoinReleasesSeatTest::RunTest(const FString& Parameters)
{
	// 1. A refused join (PreLogin "The match is over.") while Traveling: Idle with the server's sentence, seat
	//    released, and the lobby's CheckActiveSession read of that seat is declined, never Reconnectable.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1"))); // id learned
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("Joining"));
		Fmms->HandleNetworkFailure(nullptr, nullptr, ENetworkFailure::PendingConnectionFailure, TEXT("The match is over."));
		TestEqual(TEXT("refused join: Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestEqual(TEXT("refused join: the server's sentence"), Fmms->GetMatchmakingMessage(), FString(TEXT("The match is over.")));
		TestEqual(TEXT("refused join: ticket remembered"), Fmms->GetEndedTicketId(), FString(TEXT("t-1")));
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-1"), TEXT("m-1")));
		TestEqual(TEXT("the refused seat is declined, not offered"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestFalse(TEXT("no Reconnect to a refusing seat"), Fmms->HasActiveMatch());
	}
	// 2. A travel failure with an unknown ticket id: any live seat is declined; an empty reason gets a generic sentence.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Connecting, TEXT("Found"));
		Fmms->HandleTravelFailure(nullptr, ETravelFailure::TravelFailure, FString());
		TestEqual(TEXT("travel failure: Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestFalse(TEXT("travel failure: a sentence, never blank"), Fmms->GetMatchmakingMessage().IsEmpty());
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-7"), TEXT("m-7")));
		TestEqual(TEXT("travel failure: seat declined"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
	}
	// 3. An UNPLANNED disconnect (ConnectionLost) keeps Reconnect: nothing is released.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("in match"));
		Fmms->HandleNetworkFailure(nullptr, nullptr, ENetworkFailure::ConnectionLost, TEXT("lost"));
		TestEqual(TEXT("connection lost: untouched"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Traveling));
		Fmms->ApplyMySession(EFoundryFsdkResult::Ok, MakeLiveSeat(TEXT("t-3"), TEXT("m-3")));
		TestEqual(TEXT("connection lost: the live seat is still offered"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Reconnectable));
	}
	// 4. No seat held: a failure is not ours and changes nothing.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Searching, TEXT("Searching"));
		Fmms->HandleTravelFailure(nullptr, ETravelFailure::TravelFailure, TEXT("x"));
		TestEqual(TEXT("no seat: failure ignored"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Searching));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSCancelKeepsMatchOverMessageTest, "FoundryFSDK.FMMS.CancelAfterMatchEndedKeepsMessage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSCancelKeepsMatchOverMessageTest::RunTest(const FString& Parameters)
{
	// RETURN TO LOBBY after the match ended runs Cancel() with the phase already Idle: the lobby keeps "Match over".
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
	Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("in match"));
	Fmms->NotifyMatchEnded();
	TArray<EFoundryMatchmakingState> Seen;
	const FDelegateHandle Handle = Fmms->OnMatchmakingStateChanged.AddLambda(
		[&Seen](EFoundryMatchmakingState State, const FString&) { Seen.Add(State); });
	Fmms->Cancel();
	Fmms->OnMatchmakingStateChanged.Remove(Handle);
	TestEqual(TEXT("message kept"), Fmms->GetMatchmakingMessage(), FString(TEXT("Match over")));
	TestEqual(TEXT("still Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
	TestEqual(TEXT("no state churn"), Seen.Num(), 0);
	return true;
}

// -- lobby notices are transient: "Match over" and a refused-join sentence must not sit under Find Match ----------
// (Justin, 2026-10-16: "there is text under the find match button saying 'The match is over' that shouldnt be
// there".) The bare GameInstance never ticks its timer manager by itself, so the test drives it by hand - TWICE:
// a timer set outside a tick is PENDING and only activated at the end of the next Tick (TimerManager.cpp,
// InternalSetTimer: !HasBeenTickedThisFrame() -> PendingTimerSet), and Tick refuses a second call in the same
// frame, so the test ticks once, bumps GFrameCounter, and ticks again to fire it (first run: 1 tick, 0 fires).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSLobbyNoticesExpireTest, "FoundryFSDK.FMMS.LobbyNoticesExpire",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSLobbyNoticesExpireTest::RunTest(const FString& Parameters)
{
	// 1. NotifyMatchEnded's "Match over" clears itself and tells listeners.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("in match"));
		Fmms->NotifyMatchEnded();
		TestEqual(TEXT("match over: shown first"), Fmms->GetMatchmakingMessage(), FString(TEXT("Match over")));

		TArray<FString> Messages;
		const FDelegateHandle Handle = Fmms->OnMatchmakingStateChanged.AddLambda(
			[&Messages](EFoundryMatchmakingState, const FString& Message) { Messages.Add(Message); });
		GI->GetTimerManager().Tick(0.0f);  // activates the PENDING timer (set outside a tick)
		++GFrameCounter;                    // Tick() refuses a second call in the same frame
		GI->GetTimerManager().Tick(60.0f); // fires it
		Fmms->OnMatchmakingStateChanged.Remove(Handle);
		TestTrue(TEXT("match over: the message expired"), Fmms->GetMatchmakingMessage().IsEmpty());
		TestEqual(TEXT("match over: expiry stays Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
		TestEqual(TEXT("match over: expiry is announced once"), Messages.Num(), 1);
	}
	// 2. A refused join's sentence clears itself too.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Traveling, TEXT("Joining"));
		Fmms->HandleNetworkFailure(nullptr, nullptr, ENetworkFailure::PendingConnectionFailure, TEXT("The match is over."));
		TestEqual(TEXT("refused join: shown first"), Fmms->GetMatchmakingMessage(), FString(TEXT("The match is over.")));
		GI->GetTimerManager().Tick(0.0f);  // activates the PENDING timer (set outside a tick)
		++GFrameCounter;                    // Tick() refuses a second call in the same frame
		GI->GetTimerManager().Tick(60.0f); // fires it
		TestTrue(TEXT("refused join: the sentence expired"), Fmms->GetMatchmakingMessage().IsEmpty());
		TestEqual(TEXT("refused join: expiry stays Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
	}
	// 3. A plain Idle message ("Ready.") is NOT made transient by the same path.
	{
		UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
		UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
		Fmms->ApplyMatchmakingPhase(EFMMSPhase::Idle, TEXT("Ready."));
		GI->GetTimerManager().Tick(0.0f);  // activates the PENDING timer (set outside a tick)
		++GFrameCounter;                    // Tick() refuses a second call in the same frame
		GI->GetTimerManager().Tick(60.0f); // fires it
		TestEqual(TEXT("plain Idle message has no TTL"), Fmms->GetMatchmakingMessage(), FString(TEXT("Ready.")));
	}
	return true;
}

// -- party search (Round 2): the leader's Find Match is gated on ready-in-this-game; a member follows ----------
// "find match needs to be contingent on all players ready. that ensures that all players are actually playing the
// same game." A bare GameInstance has no social subsystem, so the test hands the FMMS one through the seam and
// simulates the party with the social fixture (its LocalFoundryId stands in for the signed-in id).
namespace
{
	void AddJoined(FFoundrySocialFixture& Fixture, const TCHAR* Id, const TCHAR* Name, const TCHAR* ReadySlug)
	{
		FFoundryPartyMember M;
		M.FoundryId = Id;
		M.DisplayName = Name;
		M.State = TEXT("JOINED");
		M.bReady = ReadySlug[0] != TEXT('\0');
		M.ReadyGameSlug = ReadySlug;
		Fixture.Party.Members.Add(M);
	}

	FFoundrySocialFixture PartyOfThree(const TCHAR* LocalId, const TCHAR* AdaSlug, const TCHAR* BoSlug)
	{
		FFoundrySocialFixture Fixture;
		Fixture.bActive = true;
		Fixture.bHasParty = true;
		Fixture.LocalFoundryId = LocalId;
		Fixture.Party.PartyId = TEXT("p-1");
		Fixture.Party.LeaderFoundryId = TEXT("mo-id");
		Fixture.Party.LeaderDisplayName = TEXT("Mo");
		Fixture.Party.MaxSize = 4;
		AddJoined(Fixture, TEXT("mo-id"), TEXT("Mo"), TEXT("goo-crew"));
		AddJoined(Fixture, TEXT("ada-id"), TEXT("Ada"), AdaSlug);
		AddJoined(Fixture, TEXT("bo-id"), TEXT("Bo"), BoSlug);
		return Fixture;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSPartyGateTest, "FoundryFSDK.FMMS.PartyGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSPartyGateTest::RunTest(const FString& Parameters)
{
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
	UFoundrySocialSubsystem* Social = NewObject<UFoundrySocialSubsystem>(GI);
	Fmms->SetSocialForTesting(Social);

	TestEqual(TEXT("game slug of a queue key"), FoundryQueueKey::GameSlug(TEXT("goo-crew/solo")), FString(TEXT("goo-crew")));
	TestEqual(TEXT("a bare id names no game"), FoundryQueueKey::GameSlug(TEXT("3f2a9c")), FString());
	TestEqual(TEXT("an FRN key names the game of its queue tail"),
		FoundryQueueKey::GameSlug(TEXT("frn:fmms:482910573312:queue/goo-crew/solo")), FString(TEXT("goo-crew")));
	TestEqual(TEXT("an FRN that is not a queue names no game"), FoundryQueueKey::GameSlug(TEXT("frn:fgs:482910573312:game/goo-crew")), FString());
	TestEqual(TEXT("whitespace is trimmed"), FoundryQueueKey::GameSlug(TEXT("  goo-crew/solo ")), FString(TEXT("goo-crew")));

	// 1. The LEADER (Mo) with Ada not ready: refused with fid's sentence, before any flow starts.
	Social->SetFixture(PartyOfThree(TEXT("mo-id"), TEXT(""), TEXT("goo-crew")));
	const FString Expected = TEXT("Not everyone is ready in goo-crew: Ada (2 of 3).");
	TestEqual(TEXT("leader with an unready member is refused"), Fmms->PartySearchRefusal(TEXT("goo-crew/solo")), Expected);
	TestTrue(TEXT("RefuseSearchForParty refuses"), Fmms->RefuseSearchForParty(TEXT("goo-crew/solo")));
	TestEqual(TEXT("the refusal is a Failed state"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Failed));
	TestEqual(TEXT("with the sentence as its message"), Fmms->GetMatchmakingMessage(), Expected);
	TestEqual(TEXT("no flow started: the FMMS phase is untouched"), static_cast<int32>(Fmms->GetPhase()), static_cast<int32>(EFMMSPhase::Idle));
	TestTrue(TEXT("no search clock"), Fmms->GetElapsedSearchSeconds() < 0.0f);

	// The notice is transient (8 s): after it the UI is Idle and a new request is accepted by CanRequest's states.
	GI->GetTimerManager().Tick(0.0f);
	++GFrameCounter;
	GI->GetTimerManager().Tick(60.0f);
	TestEqual(TEXT("the refusal clears itself to Idle"), static_cast<int32>(Fmms->GetMatchmakingState()), static_cast<int32>(EFoundryMatchmakingState::Idle));

	// 2. Two members, but ready in ANOTHER game is not ready here: names both.
	Social->SetFixture(PartyOfThree(TEXT("mo-id"), TEXT("conquest"), TEXT("")));
	TestEqual(TEXT("ready in a different game does not count"), Fmms->PartySearchRefusal(TEXT("goo-crew/solo")),
		FString(TEXT("Not everyone is ready in goo-crew: Ada, Bo (1 of 3).")));

	// 3. Everyone ready in the queue's game: the leader may search; a key with no slash defers to fid.
	Social->SetFixture(PartyOfThree(TEXT("mo-id"), TEXT("goo-crew"), TEXT("goo-crew")));
	TestEqual(TEXT("all ready in the game: allowed"), Fmms->PartySearchRefusal(TEXT("goo-crew/solo")), FString());
	Social->SetFixture(PartyOfThree(TEXT("mo-id"), TEXT(""), TEXT("")));
	TestEqual(TEXT("no slash in the key: the client cannot name the game, fid decides"), Fmms->PartySearchRefusal(TEXT("3f2a9c")), FString());
	TestFalse(TEXT("so nothing is refused locally"), Fmms->RefuseSearchForParty(TEXT("3f2a9c")));

	// 4. A joined NON-leader never submits for the party, ready or not.
	Social->SetFixture(PartyOfThree(TEXT("ada-id"), TEXT("goo-crew"), TEXT("goo-crew")));
	TestEqual(TEXT("a member is refused"), Fmms->PartySearchRefusal(TEXT("goo-crew/solo")), FString(TEXT("Only the party leader can start a search.")));

	// 4b. An FRN queue key gates like the plain one (its game is read from the queue/<game>/<mode> tail).
	Social->SetFixture(PartyOfThree(TEXT("mo-id"), TEXT(""), TEXT("goo-crew")));
	TestEqual(TEXT("an FRN key gates on its game"), Fmms->PartySearchRefusal(TEXT("frn:fmms:482910573312:queue/goo-crew/solo")),
		FString(TEXT("Not everyone is ready in goo-crew: Ada (2 of 3).")));

	// 5. Solo: no party, and a party of one, are today's single-ticket path.
	FFoundrySocialFixture None;
	None.bActive = true;
	Social->SetFixture(None);
	TestEqual(TEXT("no party: nothing to gate"), Fmms->PartySearchRefusal(TEXT("goo-crew/solo")), FString());
	FFoundrySocialFixture Alone = PartyOfThree(TEXT("mo-id"), TEXT(""), TEXT(""));
	Alone.Party.Members.SetNum(1);
	Alone.Party.Members[0].ReadyGameSlug.Reset();
	Alone.Party.Members[0].bReady = false;
	Social->SetFixture(Alone);
	TestEqual(TEXT("a party of one needs nobody"), Fmms->PartySearchRefusal(TEXT("goo-crew/solo")), FString());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFMMSPartyFollowTest, "FoundryFSDK.FMMS.PartyFollow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFMMSPartyFollowTest::RunTest(const FString& Parameters)
{
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFMMSSubsystem* Fmms = NewObject<UFMMSSubsystem>(GI);
	UFoundrySocialSubsystem* Social = NewObject<UFoundrySocialSubsystem>(GI);
	Fmms->SetSocialForTesting(Social);

	FFoundryMyTicket Ticket;
	Ticket.TicketId = TEXT("t-1");
	Ticket.QueueKey = TEXT("goo-crew/solo");
	Ticket.LeaderFoundryId = TEXT("mo-id");

	// Ada is a joined non-leader in Mo's party of three.
	Social->SetFixture(PartyOfThree(TEXT("ada-id"), TEXT("goo-crew"), TEXT("goo-crew")));

	// Things that are not a party search to follow change nothing.
	Fmms->ApplyMyTicket(EFoundryFsdkResult::NoMatch, FFoundryMyTicket());
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, FFoundryMyTicket());
	FFoundryMyTicket Solo = Ticket;
	Solo.LeaderFoundryId.Reset();
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Solo);
	TestFalse(TEXT("no ticket / a solo ticket is not followed"), Fmms->IsFollowingParty());
	TestEqual(TEXT("still Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));

	// The leader's own ticket is the leader's submit, never followed.
	Social->SetFixture(PartyOfThree(TEXT("mo-id"), TEXT("goo-crew"), TEXT("goo-crew")));
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Ticket);
	TestFalse(TEXT("a leader does not follow its own ticket"), Fmms->IsFollowingParty());
	Social->SetFixture(PartyOfThree(TEXT("ada-id"), TEXT("goo-crew"), TEXT("goo-crew")));

	// The leader's ticket appears: the member enters Searching, naming the leader.
	TArray<EFoundryMatchmakingState> Seen;
	const FDelegateHandle Handle = Fmms->OnMatchmakingStateChanged.AddLambda(
		[&Seen](EFoundryMatchmakingState State, const FString&) { Seen.Add(State); });
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Ticket);
	Fmms->OnMatchmakingStateChanged.Remove(Handle);
	TestTrue(TEXT("following"), Fmms->IsFollowingParty());
	TestEqual(TEXT("state is Searching"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Searching));
	TestEqual(TEXT("message names the leader"), Fmms->GetMatchmakingMessage(), FString(TEXT("Searching with Mo")));
	TestEqual(TEXT("the queue is the leader's"), Fmms->GetQueue(), FString(TEXT("goo-crew/solo")));
	TestTrue(TEXT("the search clock runs"), Fmms->GetElapsedSearchSeconds() >= 0.0f);
	TestTrue(TEXT("Cancel is offered"), Fmms->CanCancel());
	TestTrue(TEXT("Searching was announced"), Seen.Contains(EFoundryMatchmakingState::Searching));

	// A second read of the same ticket while already searching changes nothing.
	Seen.Reset();
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Ticket);
	TestEqual(TEXT("an already-following member is not re-adopted"), Seen.Num(), 0);

	// Cancel from a member returns to Idle "Search cancelled." and that ticket is never followed again - fid's
	// cancel may not have landed when the next 2 s read asks.
	Fmms->RequestCancel();
	TestFalse(TEXT("follow mode ended with the cancel"), Fmms->IsFollowingParty());
	TestEqual(TEXT("cancel resolves to Idle"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));
	TestEqual(TEXT("cancel message"), Fmms->GetMatchmakingMessage(), FString(TEXT("Search cancelled.")));
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Ticket);
	TestFalse(TEXT("the cancelled ticket is not re-followed while the server still lists it"), Fmms->IsFollowingParty());
	TestEqual(TEXT("still Idle after the lagging read"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));

	// A genuinely new search by the leader is a new ticket: followed.
	FFoundryMyTicket Next = Ticket;
	Next.TicketId = TEXT("t-2");
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Next);
	TestTrue(TEXT("a new ticket is followed"), Fmms->IsFollowingParty());
	TestEqual(TEXT("and reads Searching"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Searching));

	// The match ends: Idle "Match over", and the ended ticket is not resumed by the next read either.
	Fmms->NotifyMatchEnded();
	TestFalse(TEXT("match over ends follow mode"), Fmms->IsFollowingParty());
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Next);
	TestEqual(TEXT("the ended ticket is not followed"), StateOf(Fmms), static_cast<int32>(EFoundryMatchmakingState::Idle));

	// A ticket of ANOTHER game is never adopted (a member now in a different kit game must not travel to the
	// wrong binary): only when both sides name a game, and only a mismatch is refused.
	{
		UFMMSSubsystem* Other = NewObject<UFMMSSubsystem>(GI);
		Other->SetSocialForTesting(Social);
		FFoundrySocialFixture Fx = PartyOfThree(TEXT("ada-id"), TEXT("goo-crew"), TEXT("goo-crew"));
		Social->SetFixture(Fx);
		Social->SetGameSlug(TEXT("conquest"));
		FFoundryMyTicket GooTicket = Ticket;
		GooTicket.TicketId = TEXT("t-other");
		Other->ApplyMyTicket(EFoundryFsdkResult::Ok, GooTicket);
		TestFalse(TEXT("a goo-crew ticket is ignored by a member whose game is conquest"), Other->IsFollowingParty());
		Social->SetGameSlug(TEXT("goo-crew"));
		Other->ApplyMyTicket(EFoundryFsdkResult::Ok, GooTicket);
		TestTrue(TEXT("the same ticket is followed once the member's game matches"), Other->IsFollowingParty());
		FFoundryMyTicket BareTicket = Ticket;
		BareTicket.TicketId = TEXT("t-bare");
		BareTicket.QueueKey = TEXT("3f2a9c");
		UFMMSSubsystem* Bare = NewObject<UFMMSSubsystem>(GI);
		Bare->SetSocialForTesting(Social);
		Social->SetGameSlug(TEXT("conquest"));
		Bare->ApplyMyTicket(EFoundryFsdkResult::Ok, BareTicket);
		TestTrue(TEXT("a bare-id queue key names no game, so it defers to fid and is followed"), Bare->IsFollowingParty());
		Social->SetGameSlug(FString());
	}

	// A Failed phase must not strand a member outside the party's search: a failed search leaves Phase on Failed
	// and nothing returns it to Idle, so the leader's NEXT ticket has to be adopted from Failed too.
	{
		Social->SetFixture(PartyOfThree(TEXT("ada-id"), TEXT("goo-crew"), TEXT("goo-crew")));
		UFMMSSubsystem* Failed = NewObject<UFMMSSubsystem>(GI);
		Failed->SetSocialForTesting(Social);
		Failed->FindMatchAuthenticated(FString(), FString()); // no queue -> Fail("No queue specified.")
		TestEqual(TEXT("precondition: the phase is Failed"), static_cast<int32>(Failed->GetPhase()), static_cast<int32>(EFMMSPhase::Failed));
		TestEqual(TEXT("precondition: the state is Failed"), StateOf(Failed), static_cast<int32>(EFoundryMatchmakingState::Failed));
		FFoundryMyTicket After = Ticket;
		After.TicketId = TEXT("t-after-fail");
		Failed->ApplyMyTicket(EFoundryFsdkResult::Ok, After);
		TestTrue(TEXT("a member in Failed follows the leader's next search"), Failed->IsFollowingParty());
		TestEqual(TEXT("and reads Searching"), StateOf(Failed), static_cast<int32>(EFoundryMatchmakingState::Searching));
	}

	// Not in a party of 2+ any more: nothing to follow.
	FFoundrySocialFixture None;
	None.bActive = true;
	Social->SetFixture(None);
	FFoundryMyTicket Later = Ticket;
	Later.TicketId = TEXT("t-3");
	Fmms->ApplyMyTicket(EFoundryFsdkResult::Ok, Later);
	TestFalse(TEXT("no party, no follow"), Fmms->IsFollowingParty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
