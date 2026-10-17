// Copyright Foundry Media. FoundryFSDKUI module entry point.

#include "Modules/ModuleManager.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FoundryFSDKUILog.h"
#include "FoundrySocialSubsystem.h"
#include "FoundryUISettings.h"
#include "UObject/WeakObjectPtrTemplates.h"

DEFINE_LOG_CATEGORY(LogFoundryFSDKUI);

/**
 * FoundryFSDKUI runtime module: the default UMG parent classes (UFoundrySocialDrawer and friends)
 * and UFoundryUISettings. Each parent resolves its default widget classes lazily
 * (UFoundryUISettings::LoadWidgetClass) the first time a host constructs one. The one thing this
 * module installs eagerly: UFoundryUISettings' matchmaking copy overrides (S5), pushed into
 * FoundryMatchmakingCopy (core FoundryFSDK module) once, before any matchmaking flow can run, and the
 * party-follow default (bPartyFollowsLeaderOnLeave + the leave executor) into UFoundryPartyFollowComponent.
 */
class FFoundryFSDKUIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UFoundryUISettings::ApplyMatchmakingCopyOverrides();
		UFoundryUISettings::ApplyPartyDefaults();
		// The game slug goes into UFoundrySocialSubsystem, a GameInstance subsystem that does not exist yet
		// at module startup. A game that readies up from its OWN UI never constructs a kit widget, so waiting
		// for one would leave Foundry never told which game it is: hook the first world of each game instance
		// instead (the kit widgets still push it too, as a fallback).
		WorldInitHandle = FWorldDelegates::OnPostWorldInitialization.AddRaw(this, &FFoundryFSDKUIModule::HandlePostWorldInit);
	}
	virtual void ShutdownModule() override
	{
		FWorldDelegates::OnPostWorldInitialization.Remove(WorldInitHandle);
		WorldInitHandle.Reset();
	}

private:
	void HandlePostWorldInit(UWorld* World, const UWorld::InitializationValues /*IVS*/)
	{
		if (World == nullptr || !World->IsGameWorld())
		{
			return;
		}
		UGameInstance* GameInstance = World->GetGameInstance();
		if (GameInstance == nullptr || GameInstance == LastPushedGameInstance.Get())
		{
			return; // once per game instance: level travel creates a new world, not a new subsystem
		}
		if (UFoundrySocialSubsystem* Social = GameInstance->GetSubsystem<UFoundrySocialSubsystem>())
		{
			UFoundryUISettings::PushGameSlug(Social);
			LastPushedGameInstance = GameInstance;
		}
	}

	FDelegateHandle WorldInitHandle;
	TWeakObjectPtr<UGameInstance> LastPushedGameInstance;
};

IMPLEMENT_MODULE(FFoundryFSDKUIModule, FoundryFSDKUI)
