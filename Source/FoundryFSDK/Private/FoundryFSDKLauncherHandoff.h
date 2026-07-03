// Copyright Foundry Media. Launcher -> game session handoff (the DEFAULT auth path).
#pragma once

#include "CoreMinimal.h"

/**
 * Read a short-lived matchmaking token from the Foundry launcher's session daemon
 * via the `FOUNDRY_IPC` handoff the launcher sets (`<pipe>;<nonce>`) when it spawns
 * the game. Returns true + OutToken on success. Windows-only; false elsewhere or
 * when there is no daemon handoff (the caller then FAILS FAST - the default posture:
 * no in-game login unless the FID-embedded build flag is set).
 *
 * The token is the player's own scoped 15-min `aud=fsdk-fmms` token, minted by the
 * daemon; it never persists - pass it straight to fsdk_set_player_token. See
 * .claude/plans/vivid-hatching-wave.md and .claude/rules/fsdk-security.md.
 */
bool FoundryFSDKReadLauncherToken(FString& OutToken);
