// Copyright Foundry Media. FoundryFSDK default UI kit design tokens code needs.

#pragma once

#include "CoreMinimal.h"

/**
 * Presence colour/label and the party-member tint ramp, both read from
 * UFoundryUISettings::Palette - a project-wide colour change (Project Settings -> Game ->
 * Foundry UI) reaches the parents without a rebuild.
 */
namespace FoundryUITheme
{
	/** The dot colour for a presence string (online / idle / away / dnd / offline / "" (unknown,
	 *  treated as online)). */
	FOUNDRYFSDKUI_API FLinearColor PresenceColor(const FString& Presence);

	/** "Online", "Idle", ... for a presence string ("In party" when unknown). */
	FOUNDRYFSDKUI_API FString PresenceLabel(const FString& Presence);

	/** Tint for a roster/member index: Palette.Primary for the local player, else one of the six
	 *  Palette.Member0..5 tokens by index (wraps). UFoundrySocialDrawer::GetMemberTint calls this as
	 *  its native default; a game overrides the BlueprintNativeEvent with its own cast colours. */
	FOUNDRYFSDKUI_API FLinearColor MemberTint(int32 RosterIndex, bool bLocal);
}
