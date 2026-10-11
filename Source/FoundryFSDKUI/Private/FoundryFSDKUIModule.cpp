// Copyright Foundry Media. FoundryFSDKUI module entry point.

#include "Modules/ModuleManager.h"

#include "FoundryFSDKUILog.h"
#include "FoundryUISettings.h"

DEFINE_LOG_CATEGORY(LogFoundryFSDKUI);

/**
 * FoundryFSDKUI runtime module: the default UMG parent classes (UFoundrySocialDrawer and friends)
 * and UFoundryUISettings. Each parent resolves its default widget classes lazily
 * (UFoundryUISettings::LoadWidgetClass) the first time a host constructs one. The one thing this
 * module installs eagerly: UFoundryUISettings' matchmaking copy overrides (S5), pushed into
 * FoundryMatchmakingCopy (core FoundryFSDK module) once, before any matchmaking flow can run.
 */
class FFoundryFSDKUIModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		UFoundryUISettings::ApplyMatchmakingCopyOverrides();
	}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FFoundryFSDKUIModule, FoundryFSDKUI)
