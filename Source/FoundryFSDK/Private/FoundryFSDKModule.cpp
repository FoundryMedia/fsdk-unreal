// Copyright Foundry Media. FoundryFSDK Unreal module entry point.

#include "Modules/ModuleManager.h"
#include "FoundryFSDKTransport.h"

/**
 * FoundryFSDK runtime module. fsdk-core (vendored in Private/FsdkCore) bakes in no
 * network stack or log sink, so the module installs both host bridges at startup -
 * the engine-HTTP-backed transport and a UE_LOG sink - before any client is
 * created. The player-facing API itself lives in UFoundryFSDKSubsystem.
 */
class FFoundryFSDKModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FoundryFSDKInstallLogSink();
		FoundryFSDKInstallHttpTransport();
	}

	virtual void ShutdownModule() override
	{
		FoundryFSDKShutdownBridges();
	}
};

IMPLEMENT_MODULE(FFoundryFSDKModule, FoundryFSDK)
