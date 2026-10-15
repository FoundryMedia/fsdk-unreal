// Copyright Foundry Media. Automation test for UFoundryBootSplashSubsystem's once-per-GameInstance flag.
//
// Only what runs without a world: the subsystem is built on a bare transient GameInstance (never
// Init()ed), so there is no player controller, no viewport and no timer manager ticking. A refused
// TryPlay (no player controller) must leave the "played" flag unset - the caller then continues to
// its menu and a later attempt is still allowed.
// Run: UnrealEditor-Cmd.exe <project> -ExecCmds="Automation RunTests FoundryFSDKUI.BootSplash" -unattended -nopause -NullRHI

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/GameInstance.h"
#include "FoundryBootSplashSubsystem.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFoundryBootSplashSubsystemTest, "FoundryFSDKUI.BootSplash.SubsystemFlag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)
bool FFoundryBootSplashSubsystemTest::RunTest(const FString& Parameters)
{
	// UGameInstanceSubsystem is Within=GameInstance: an outer-less NewObject ensures, and the ensure fails the test.
	// A bare transient GameInstance is never Init()ed, so nothing here touches a world or the network.
	UGameInstance* GI = NewObject<UGameInstance>(GetTransientPackage());
	UFoundryBootSplashSubsystem* Splash = NewObject<UFoundryBootSplashSubsystem>(GI);

	TestFalse(TEXT("not played initially"), Splash->HasPlayed());
	TestFalse(TEXT("not active initially"), Splash->IsActive());

	// No player controller: nothing to show on, so TryPlay refuses and the flag stays unset.
	TestFalse(TEXT("TryPlay without a player controller returns false"), Splash->TryPlay(nullptr, FSimpleDelegate()));
	TestFalse(TEXT("a refused TryPlay does not mark the splash played"), Splash->HasPlayed());
	TestFalse(TEXT("still not active"), Splash->IsActive());

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
