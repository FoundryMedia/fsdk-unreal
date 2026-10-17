// Copyright Foundry Media. Automation test for the party-data half of the social kit: the fsdk-core ->
// struct conversions (ISO-8601 invite clocks, the friend's party block, the platform cap) and the pure
// reads on UFoundrySocialSubsystem (LiveInvitedCount, PartyCapForQueue, SelectedQueueKey, fixture refusal).
//
// The subsystem is built on a bare transient GameInstance (never Init()ed), so Fsdk() is null and every
// platform call is a no-op: nothing here touches the network.
// Run: UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests FoundryFSDK.Social" -unattended -nopause -NullRHI

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "FoundrySocialConvert.h"
#include "FoundrySocialSubsystem.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoundrySocialPartyConvertTest, "FoundryFSDK.Social.PartyConvert",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFoundrySocialPartyConvertTest::RunTest(const FString& Parameters)
{
	// ISO-8601 -> FDateTime. Zero ticks is the "absent" sentinel for an empty or unparseable stamp.
	const FDateTime Expected(2026, 10, 16, 22, 15, 0);
	TestTrue(TEXT("UTC stamp parses"), FoundrySocialConvert::ParseIso8601Utc("2026-10-16T22:15:00Z") == Expected);
	TestTrue(TEXT("fractional seconds parse to the same second"),
		FoundrySocialConvert::ParseIso8601Utc("2026-10-16T22:15:00.250Z").GetTicks() / ETimespan::TicksPerSecond
		== Expected.GetTicks() / ETimespan::TicksPerSecond);
	TestEqual(TEXT("empty stamp is absent"), FoundrySocialConvert::ParseIso8601Utc("").GetTicks(), static_cast<int64>(0));
	TestEqual(TEXT("null stamp is absent"), FoundrySocialConvert::ParseIso8601Utc(nullptr).GetTicks(), static_cast<int64>(0));
	TestEqual(TEXT("garbage is absent"), FoundrySocialConvert::ParseIso8601Utc("not a time").GetTicks(), static_cast<int64>(0));

	// An INVITED member row carries its invite clock into the struct; a JOINED row has none.
	fsdk_party_member Invited;
	FMemory::Memzero(Invited);
	FCStringAnsi::Strncpy(Invited.foundry_id, "bbbb-2", sizeof(Invited.foundry_id));
	FCStringAnsi::Strncpy(Invited.state, "INVITED", sizeof(Invited.state));
	FCStringAnsi::Strncpy(Invited.invited_at, "2026-10-16T22:00:00Z", sizeof(Invited.invited_at));
	FCStringAnsi::Strncpy(Invited.expires_at, "2026-10-16T22:15:00Z", sizeof(Invited.expires_at));
	const FFoundryPartyMember InvitedMember = FoundrySocialConvert::Member(Invited);
	TestEqual(TEXT("member state"), InvitedMember.State, FString(TEXT("INVITED")));
	TestTrue(TEXT("member invited-at"), InvitedMember.InvitedAt == FDateTime(2026, 10, 16, 22, 0, 0));
	TestTrue(TEXT("member expires-at"), InvitedMember.ExpiresAt == Expected);

	fsdk_party_member Joined;
	FMemory::Memzero(Joined);
	FCStringAnsi::Strncpy(Joined.state, "JOINED", sizeof(Joined.state));
	const FFoundryPartyMember JoinedMember = FoundrySocialConvert::Member(Joined);
	TestEqual(TEXT("joined member has no invite clock"),
		JoinedMember.InvitedAt.GetTicks() + JoinedMember.ExpiresAt.GetTicks(), static_cast<int64>(0));

	// The friend's party block (no party id) and the party header's platform cap + received-invite clock.
	fsdk_friend Friend;
	FMemory::Memzero(Friend);
	Friend.in_party = 1;
	Friend.party_size = 2;
	Friend.party_max_size = 6;
	Friend.party_joinable = 1;
	const FFoundryFriend F = FoundrySocialConvert::Friend(Friend);
	TestTrue(TEXT("friend party fields"), F.bInParty && F.PartySize == 2 && F.PartyMaxSize == 6 && F.bPartyJoinable);
	fsdk_friend NoParty;
	FMemory::Memzero(NoParty);
	TestFalse(TEXT("friend with no party block is not in a party"), FoundrySocialConvert::Friend(NoParty).bInParty);

	fsdk_party Header;
	FMemory::Memzero(Header);
	FCStringAnsi::Strncpy(Header.id, "p-9", sizeof(Header.id));
	Header.max_size = 4;
	Header.platform_max_size = 6;
	FCStringAnsi::Strncpy(Header.my_expires_at, "2026-10-16T22:15:00Z", sizeof(Header.my_expires_at));
	FFoundryParty P;
	FoundrySocialConvert::PartyHeader(Header, P);
	TestTrue(TEXT("party caps"), P.MaxSize == 4 && P.PlatformMaxSize == 6);
	TestTrue(TEXT("received invite clock"), P.InviteExpiresAt == Expected);
	TestEqual(TEXT("no invited-at when the row carried none"), P.InvitedAt.GetTicks(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoundrySocialPartyReadsTest, "FoundryFSDK.Social.PartyReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFoundrySocialPartyReadsTest::RunTest(const FString& Parameters)
{
	// Same isolation as the FMMS test: an outer-less NewObject ensures (Within=GameInstance), and a bare,
	// never-Init()ed GameInstance leaves Fsdk() null so nothing reaches the platform.
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFoundrySocialSubsystem* Social = NewObject<UFoundrySocialSubsystem>(GI);

	// PartyCapForQueue: unknown is -1 (and an empty key never asks), however often it is read.
	TestEqual(TEXT("unknown cap reads -1"), Social->PartyCapForQueue(TEXT("goo-crew/ooze")), -1);
	TestEqual(TEXT("still -1 on a second read (the kick is throttled, never a different answer)"),
		Social->PartyCapForQueue(TEXT("goo-crew/ooze")), -1);
	TestEqual(TEXT("empty key reads -1"), Social->PartyCapForQueue(FString()), -1);

	// SelectedQueueKey round-trips, trims, and a repeat is a no-op.
	int32 Changes = 0;
	Social->OnChanged.AddLambda([&Changes]() { ++Changes; });
	TestEqual(TEXT("no mode selected at first"), Social->SelectedQueueKey(), FString());
	Social->SetSelectedQueueKey(TEXT("  goo-crew/ooze "));
	TestEqual(TEXT("selected key stored trimmed"), Social->SelectedQueueKey(), FString(TEXT("goo-crew/ooze")));
	const int32 AfterFirst = Changes;
	Social->SetSelectedQueueKey(TEXT("goo-crew/ooze"));
	TestEqual(TEXT("re-selecting the same mode does not broadcast"), Changes, AfterFirst);
	TestTrue(TEXT("changing the mode broadcast OnChanged"), AfterFirst >= 1);

	// A simulated party: the local leader, a joined friend, and three INVITED rows - alive, expired, and
	// one from an older fid with no clock (never expires).
	const FDateTime Now = FDateTime::UtcNow();
	FFoundrySocialFixture Fixture;
	Fixture.bActive = true;
	Fixture.bHasParty = true;
	Fixture.Party.PartyId = TEXT("p-1");
	Fixture.Party.LeaderFoundryId = TEXT("me");
	Fixture.Party.MaxSize = 6;
	Fixture.Party.PlatformMaxSize = 6;
	auto AddMember = [&Fixture](const TCHAR* Id, const TCHAR* State, FDateTime Expires)
	{
		FFoundryPartyMember M;
		M.FoundryId = Id;
		M.State = State;
		M.ExpiresAt = Expires;
		Fixture.Party.Members.Add(M);
	};
	AddMember(TEXT("me"), TEXT("JOINED"), FDateTime(0));
	AddMember(TEXT("a"), TEXT("JOINED"), FDateTime(0));
	AddMember(TEXT("live"), TEXT("INVITED"), Now + FTimespan::FromMinutes(10));
	AddMember(TEXT("dead"), TEXT("INVITED"), Now - FTimespan::FromMinutes(1));
	AddMember(TEXT("legacy"), TEXT("INVITED"), FDateTime(0));
	Fixture.PartyCaps.Add(TEXT("goo-crew/ooze"), 4);
	Fixture.JoinRequests.Add(FFoundryJoinRequest());
	Social->SetFixture(Fixture);

	TestEqual(TEXT("joined count ignores every INVITED row"), Social->JoinedCount(), 2);
	TestEqual(TEXT("live invites: the future one and the clockless one, not the expired one"),
		Social->LiveInvitedCount(), 2);
	TestEqual(TEXT("fixture cap table answers"), Social->PartyCapForQueue(TEXT("goo-crew/ooze")), 4);
	TestEqual(TEXT("fixture cap table: an unlisted key is unknown"), Social->PartyCapForQueue(TEXT("other/mode")), -1);
	TestEqual(TEXT("fixture join requests are read"), Social->JoinRequests().Num(), 1);

	// A fixture refuses every action with a sentence instead of silently doing nothing.
	TArray<FString> Refused;
	Social->OnActionFailed.AddLambda([&Refused](const FString& Action, const FString& /*Message*/) { Refused.Add(Action); });
	Social->Kick(TEXT("a"));
	Social->CancelInvite(TEXT("live"));
	Social->ReInvite(TEXT("dead"));
	Social->AcceptJoinRequest(TEXT("x"));
	Social->DeclineJoinRequest(TEXT("x"));
	Social->RequestToJoin(TEXT("x"));
	Social->LeaveParty();
	TestEqual(TEXT("each fixture action broadcast a failure"), Refused.Num(), 7);
	TestTrue(TEXT("kick named"), Refused.Contains(TEXT("party.kick")));
	TestTrue(TEXT("cancel named"), Refused.Contains(TEXT("party.cancel_invite")));
	TestTrue(TEXT("re-invite named"), Refused.Contains(TEXT("party.invite")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
