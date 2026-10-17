// Copyright Foundry Media. Automation tests for FoundryPartyRules: the pure party helpers the social
// drawer, rows, friend menu and match cluster share (invite expiry text, the cap gate, which entries a
// row's context menu offers). No world, no subsystem, no widgets - plain inputs and outputs.
// Run: UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests FoundryFSDKUI.PartyRules" -unattended -nopause -NullRHI

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "FoundryPartyRules.h"
#include "Misc/AutomationTest.h"

using namespace FoundryPartyRules;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoundryPartyRulesExpiryTest, "FoundryFSDKUI.PartyRules.ExpiryText",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFoundryPartyRulesExpiryTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("3 minutes left"), ExpiryText(180.0), FString(TEXT("expires in 3 min")));
	TestEqual(TEXT("150 s rounds UP to 3 min"), ExpiryText(150.0), FString(TEXT("expires in 3 min")));
	TestEqual(TEXT("61 s is 2 min"), ExpiryText(61.0), FString(TEXT("expires in 2 min")));
	TestEqual(TEXT("exactly 60 s is 1 min"), ExpiryText(60.0), FString(TEXT("expires in 1 min")));
	TestEqual(TEXT("under a minute speaks in seconds"), ExpiryText(45.0), FString(TEXT("expires in 45 s")));
	TestEqual(TEXT("a live invite never reads 0 s"), ExpiryText(0.2), FString(TEXT("expires in 1 s")));
	TestEqual(TEXT("zero left is expired"), ExpiryText(0.0), FString(TEXT("Expired")));
	TestEqual(TEXT("an unknown expiry says nothing"), ExpiryText(-1.0), FString());

	TestFalse(TEXT("live invite is not expired"), IsExpired(60.0));
	TestTrue(TEXT("zero left is expired"), IsExpired(0.0));
	TestFalse(TEXT("an unknown expiry never expires (older fid)"), IsExpired(-1.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoundryPartyRulesInviteClockTest, "FoundryFSDKUI.PartyRules.InviteClock",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFoundryPartyRulesInviteClockTest::RunTest(const FString& Parameters)
{
	// The lifetime is fid's own ExpiresAt - InvitedAt; only a MONOTONIC elapsed time comes from the client.
	const FDateTime Invited(2026, 10, 16, 12, 0, 0);
	const FInviteClock Clock = FInviteClock::From(Invited + FTimespan::FromMinutes(15), Invited, 100.0);
	TestTrue(TEXT("both timestamps present: set"), Clock.IsSet());
	TestEqual(TEXT("just received: the full 15 min"), Clock.RemainingNow(100.0), 900.0);
	TestEqual(TEXT("60 s after receipt"), Clock.RemainingNow(160.0), 840.0);
	TestEqual(TEXT("60 s after receipt reads 14 min"), ExpiryText(Clock.RemainingNow(160.0)), FString(TEXT("expires in 14 min")));
	TestEqual(TEXT("floors at 0 once the lifetime has elapsed"), Clock.RemainingNow(2000.0), 0.0);
	TestTrue(TEXT("elapsed lifetime reads expired"), IsExpired(Clock.RemainingNow(2000.0)));

	// Clock-skew immunity: timestamps a decade off the real date (or a client clock far ahead) change nothing,
	// because nothing compares them with the local wall clock.
	const FDateTime FarInvited(2036, 1, 1, 0, 0, 0);
	const FInviteClock Skewed = FInviteClock::From(FarInvited + FTimespan::FromMinutes(15), FarInvited, 100.0);
	TestEqual(TEXT("skewed server date: still 15 min at receipt"), Skewed.RemainingNow(100.0), 900.0);
	TestFalse(TEXT("skewed server date: not expired at receipt"), IsExpired(Skewed.RemainingNow(100.0)));

	// A re-invite bumps ExpiresAt: a fresh clock from the new pair starts the full lifetime again.
	const FInviteClock Bumped = FInviteClock::From(Invited + FTimespan::FromMinutes(30), Invited + FTimespan::FromMinutes(15), 500.0);
	TestEqual(TEXT("re-invite: full lifetime again"), Bumped.RemainingNow(500.0), 900.0);

	// Missing timestamps (older fid / JOINED member): no expiry known, never expired.
	TestFalse(TEXT("no ExpiresAt: unset"), FInviteClock::From(FDateTime(), Invited, 0.0).IsSet());
	TestFalse(TEXT("no InvitedAt: unset"), FInviteClock::From(Invited, FDateTime(), 0.0).IsSet());
	TestEqual(TEXT("unset reads -1"), FInviteClock().RemainingNow(50.0), -1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoundryPartyRulesCapTest, "FoundryFSDKUI.PartyRules.CapGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFoundryPartyRulesCapTest::RunTest(const FString& Parameters)
{
	// Which cap applies: the queue's when known, else the platform's, else the fallback.
	TestEqual(TEXT("queue cap wins"), EffectiveCap(4, 6, 8), 4);
	TestEqual(TEXT("unknown queue cap (-1) falls to the platform cap"), EffectiveCap(-1, 6, 8), 6);
	TestEqual(TEXT("not in a party (platform 0) falls to the fallback"), EffectiveCap(-1, 0, 8), 8);

	// Too big / full.
	TestTrue(TEXT("8 in a cap of 4 is too big"), IsTooBig(8, 4));
	TestFalse(TEXT("4 in a cap of 4 fits"), IsTooBig(4, 4));
	TestFalse(TEXT("an unknown cap is never too big"), IsTooBig(8, -1));
	TestTrue(TEXT("3 joined + 1 live invite fills a cap of 4"), IsFull(3, 1, 4));
	TestFalse(TEXT("2 joined + 1 live invite leaves room in 4"), IsFull(2, 1, 4));
	TestFalse(TEXT("an unknown cap is never full"), IsFull(4, 0, 0));

	// The sentences.
	TestEqual(TEXT("header"), HeaderText(3, 4), FString(TEXT("3 / 4")));
	TestEqual(TEXT("header over size"), HeaderText(8, 4), FString(TEXT("8 / 4 - too big for this mode")));
	TestEqual(TEXT("full reason"), FullReason(4, 4), FString(TEXT("Party full (4 / 4)")));
	TestEqual(TEXT("too big"), TooBigText(8, 4), FString(TEXT("Party too big (8 > 4)")));
	TestEqual(TEXT("friend in a party"), FriendPartyLine(FString(), true, 2, 6), FString(TEXT("In a party 2 / 6")));
	TestEqual(TEXT("friend in a party and in a game"), FriendPartyLine(TEXT("Conquest"), true, 2, 6), FString(TEXT("Playing Conquest - party 2 / 6")));
	TestEqual(TEXT("friend in no party"), FriendPartyLine(TEXT("Conquest"), false, 0, 0), FString());
	return true;
}

namespace
{
	FFriendMenuContext LeaderContext(EFoundryFriendRowKind Kind)
	{
		FFriendMenuContext C;
		C.Kind = Kind;
		C.bSignedIn = true;
		C.bHasParty = true;
		C.bLocalLeader = true;
		C.PartyCount = 2;
		C.Cap = 4;
		return C;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoundryPartyRulesFriendMenuTest, "FoundryFSDKUI.PartyRules.FriendMenuPlan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFoundryPartyRulesFriendMenuTest::RunTest(const FString& Parameters)
{
	// -- an online friend, signed in, a leader with room
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Online);
		C.bFriendPartyJoinable = true;
		const FFriendMenuPlan P = PlanFriendMenu(C);
		TestTrue(TEXT("online: Invite shown + enabled"), P.Invite.bShown && P.Invite.bEnabled);
		TestTrue(TEXT("online + joinable: Ask to join shown + enabled"), P.AskToJoin.bShown && P.AskToJoin.bEnabled);
		TestTrue(TEXT("online: whisper, remove, block shown"), P.SendMessage.bShown && P.RemoveFriend.bShown && P.Block.bShown);
		TestFalse(TEXT("online: no party-roster entries"), P.ReInvite.bShown || P.CancelInvite.bShown || P.Kick.bShown || P.Unblock.bShown);
	}
	// -- Ask to join exists only for a joinable party
	{
		const FFriendMenuPlan P = PlanFriendMenu(LeaderContext(EFoundryFriendRowKind::Online));
		TestFalse(TEXT("not joinable: Ask to join is not offered"), P.AskToJoin.bShown);
	}
	// -- already in THEIR party: Ask to join is disabled with a reason
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Online);
		C.bFriendPartyJoinable = true;
		C.bTargetInLocalParty = true;
		const FFriendMenuPlan P = PlanFriendMenu(C);
		TestTrue(TEXT("in their party: shown"), P.AskToJoin.bShown);
		TestFalse(TEXT("in their party: disabled"), P.AskToJoin.bEnabled);
		TestEqual(TEXT("in their party: reason"), P.AskToJoin.Reason, FString(TEXT("You're already in their party.")));
		TestFalse(TEXT("already a member: Invite disabled"), P.Invite.bEnabled);
		TestEqual(TEXT("already a member: Invite reason"), P.Invite.Reason, FString(TEXT("They're already in your party.")));
	}
	// -- a full party disables Invite with the count in the reason (joined + live invites)
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Online);
		C.PartyCount = 4;
		C.Cap = 4;
		const FFriendMenuPlan P = PlanFriendMenu(C);
		TestFalse(TEXT("full: Invite disabled"), P.Invite.bEnabled);
		TestEqual(TEXT("full: reason"), P.Invite.Reason, FString(TEXT("Party full (4 / 4)")));
	}
	// -- only the host invites
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Online);
		C.bLocalLeader = false;
		const FFriendMenuPlan P = PlanFriendMenu(C);
		TestFalse(TEXT("non-leader: Invite disabled"), P.Invite.bEnabled);
		TestEqual(TEXT("non-leader: reason"), P.Invite.Reason, FString(TEXT("Only the host can invite.")));
	}
	// -- an offline friend
	{
		const FFriendMenuPlan P = PlanFriendMenu(LeaderContext(EFoundryFriendRowKind::Offline));
		TestFalse(TEXT("offline: Invite disabled"), P.Invite.bEnabled);
		TestEqual(TEXT("offline: reason"), P.Invite.Reason, FString(TEXT("They're offline.")));
	}
	// -- signed out
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Online);
		C.bSignedIn = false;
		C.bFriendPartyJoinable = true;
		const FFriendMenuPlan P = PlanFriendMenu(C);
		TestFalse(TEXT("signed out: Invite disabled"), P.Invite.bEnabled);
		TestFalse(TEXT("signed out: Ask to join disabled"), P.AskToJoin.bEnabled);
		TestEqual(TEXT("signed out: Ask to join reason"), P.AskToJoin.Reason, FString(TEXT("Sign in to ask to join a party.")));
	}
	// -- a pending (invited) member: the leader gets Re-invite + Cancel invite, nothing else
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Party);
		C.bPendingMember = true;
		const FFriendMenuPlan P = PlanFriendMenu(C);
		TestTrue(TEXT("pending, leader: Re-invite enabled"), P.ReInvite.bShown && P.ReInvite.bEnabled);
		TestTrue(TEXT("pending, leader: Cancel invite enabled"), P.CancelInvite.bShown && P.CancelInvite.bEnabled);
		TestFalse(TEXT("pending, leader: no whisper / kick / invite"), P.SendMessage.bShown || P.Kick.bShown || P.Invite.bShown);

		C.bLocalLeader = false;
		TestFalse(TEXT("pending, non-leader: no menu at all"), PlanFriendMenu(C).AnyShown());
	}
	// -- re-invite on a full party: a LIVE invite is already counted, an EXPIRED one is not
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Party);
		C.bPendingMember = true;
		C.PartyCount = 4;
		C.Cap = 4;
		TestTrue(TEXT("full + live invite: Re-invite still enabled"), PlanFriendMenu(C).ReInvite.bEnabled);
		C.bTargetInviteExpired = true;
		const FFriendMenuPlan P = PlanFriendMenu(C);
		TestFalse(TEXT("full + expired invite: Re-invite disabled"), P.ReInvite.bEnabled);
		TestEqual(TEXT("full + expired invite: reason"), P.ReInvite.Reason, FString(TEXT("Party full (4 / 4)")));
		TestTrue(TEXT("full + expired invite: Cancel invite still there"), P.CancelInvite.bEnabled);
	}
	// -- a joined member: whisper for everyone, Remove from party for the leader only
	{
		FFriendMenuContext C = LeaderContext(EFoundryFriendRowKind::Party);
		const FFriendMenuPlan Leader = PlanFriendMenu(C);
		TestTrue(TEXT("joined, leader: whisper + Remove from party"), Leader.SendMessage.bShown && Leader.Kick.bShown && Leader.Kick.bEnabled);
		TestFalse(TEXT("joined, leader: no invite entries"), Leader.ReInvite.bShown || Leader.CancelInvite.bShown);

		C.bLocalLeader = false;
		const FFriendMenuPlan Member = PlanFriendMenu(C);
		TestTrue(TEXT("joined, non-leader: whisper"), Member.SendMessage.bShown);
		TestFalse(TEXT("joined, non-leader: never Remove from party"), Member.Kick.bShown);
	}
	// -- a blocked row
	{
		const FFriendMenuPlan P = PlanFriendMenu(LeaderContext(EFoundryFriendRowKind::Blocked));
		TestTrue(TEXT("blocked: Unblock"), P.Unblock.bShown && P.Unblock.bEnabled);
		TestFalse(TEXT("blocked: nothing else"), P.SendMessage.bShown || P.Invite.bShown || P.Block.bShown || P.RemoveFriend.bShown);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
