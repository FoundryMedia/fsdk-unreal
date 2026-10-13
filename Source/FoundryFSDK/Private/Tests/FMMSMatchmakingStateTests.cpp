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
#include "Misc/AutomationTest.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
