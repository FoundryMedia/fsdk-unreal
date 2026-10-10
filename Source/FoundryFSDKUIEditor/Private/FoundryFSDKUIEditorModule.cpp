// Copyright Foundry Media. FoundryFSDKUIEditor module entry point.

#include "Modules/ModuleManager.h"

/**
 * FoundryFSDKUIEditor: editor-only home for the FoundryUIBuild commandlet (a later lane in this
 * same program - see .claude/plans/melodic-sparking-rain.md section B "Generator"). Nothing to
 * install at startup yet.
 */
class FFoundryFSDKUIEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override {}
	virtual void ShutdownModule() override {}
};

IMPLEMENT_MODULE(FFoundryFSDKUIEditorModule, FoundryFSDKUIEditor)
