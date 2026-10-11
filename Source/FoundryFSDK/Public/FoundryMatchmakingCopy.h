// Copyright Foundry Media. Player-facing matchmaking copy (S5 "matchmaking wait").

#pragma once

#include "CoreMinimal.h"
#include "FoundryFSDKSubsystem.h" // EFoundryFsdkResult / EFoundryWaitReason

/**
 * The ONE table every matchmaking failure/wait sentence comes from - no numeric result or wait-
 * reason code ever reaches player-facing text (the code goes to the log line only; see
 * UFMMSSubsystem::HandlePoll). FoundryFSDK stays independent of the FoundryFSDKUI module (UI
 * modules depend on the core, never the other way - see FoundryFSDKUI.Build.cs), so a game's
 * override lives on UFoundryUISettings (FoundryFSDKUI) and is pushed in here once at that module's
 * startup (FFoundryFSDKUIModule::StartupModule -> UFoundryUISettings::ApplyMatchmakingCopyOverrides)
 * - FoundryMatchmakingCopy itself never references UFoundryUISettings.
 *
 * A game that never links FoundryFSDKUI (and so never installs an override) still gets sensible
 * default English copy from every function here.
 */
class FOUNDRYFSDK_API FoundryMatchmakingCopy
{
public:
	/** Player-facing sentence for a TERMINAL fsdk result (a Fail()). Never includes the numeric code
	 *  - log (int32)Result alongside this at the call site for diagnostics. */
	static FText ForResult(EFoundryFsdkResult Result);

	/**
	 * Player-facing sentence for a server-told wait reason while a ticket is QUEUED (see
	 * FFoundryTicketWait). For CapacityStarting with a positive EstimateSeconds, appends
	 * "(~N min left)" to the base sentence; EstimateSeconds is ignored for every other reason.
	 */
	static FText ForWait(EFoundryWaitReason Reason, int32 EstimateSeconds);

	/** "No match found in time" - used for BOTH the server-reported EFoundryMatchStatus::Expired
	 *  status and UFMMSSubsystem's own client-side wait ceiling (the server kept answering QUEUED
	 *  past the ticket's expiresAt, or past 25 minutes when that was never known). */
	static FText Expired();

	/**
	 * Override hooks - installed once by a UI-layer module (see the class doc above). An empty
	 * FText clears that entry's override, falling back to the default. Not thread-safe by design:
	 * call these only from a module's StartupModule (or equivalent one-time init), before any
	 * matchmaking flow runs - exactly like fsdk_set_http_transport et al. ("set once at startup").
	 */
	static void SetResultOverride(EFoundryFsdkResult Result, const FText& Text);
	static void SetWaitOverride(EFoundryWaitReason Reason, const FText& Text);

private:
	static TMap<EFoundryFsdkResult, FText> ResultOverrides;
	static TMap<EFoundryWaitReason, FText> WaitOverrides;
};
