// Copyright Foundry Media. FoundryFSDK Unreal module entry point.

#include "Modules/ModuleManager.h"

/**
 * FoundryFSDK runtime module. The plugin's functionality is exposed through
 * UFoundryFSDKSubsystem; the module itself just registers with UE. Using the
 * default module implementation keeps this minimal - no startup/shutdown work
 * is needed beyond what the subsystem lifecycle handles.
 */
IMPLEMENT_MODULE(FDefaultModuleImpl, FoundryFSDK)
