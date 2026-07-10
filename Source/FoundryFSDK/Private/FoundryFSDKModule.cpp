// Copyright Foundry Media. FoundryFSDK Unreal module entry point.

#include "Modules/ModuleManager.h"
#include "FoundryFSDKTransport.h"
#include "FoundryFSDKKeyring.h"

/**
 * FoundryFSDK runtime module. fsdk-core (vendored in Private/FsdkCore) bakes in no
 * network stack, log sink, or secret store, so the module installs all host bridges
 * at startup - the engine-HTTP-backed transport, a UE_LOG sink, and the OS-keyring
 * secret store (refresh-token persistence) - before any client is created. The
 * player-facing API itself lives in UFoundryFSDKSubsystem.
 */
class FFoundryFSDKModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		FoundryFSDKInstallLogSink();
		FoundryFSDKInstallHttpTransport();
		FoundryFSDKInstallWsTransport(); // FRC chat frames (fsdk_set_ws_transport)
		FoundryFSDKInstallSecretStore();
	}

	virtual void ShutdownModule() override
	{
		FoundryFSDKShutdownBridges();
		FoundryFSDKShutdownSecretStore();
	}
};

IMPLEMENT_MODULE(FFoundryFSDKModule, FoundryFSDK)
