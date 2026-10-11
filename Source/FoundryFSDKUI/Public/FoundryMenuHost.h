// Copyright Foundry Media. FoundryFSDK default UI kit: the game's menu-layer contract.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "FoundryMenuHost.generated.h"

class UUserWidget;

/** Which pause-menu wording/behavior applies: Lobby (QUIT TO DESKTOP / CANCEL) or Match (LEAVE
 *  MATCH / RESUME). */
UENUM(BlueprintType)
enum class EFoundryPauseContext : uint8
{
	Lobby,
	Match
};

// No CannotImplementInterfaceInBlueprint meta here (deliberately - that flag is PRESENCE-only,
// Class->HasMetaData(...), so writing it as "=false" would still set it and BLOCK Blueprint
// implementation; the engine default with no such tag already lets a BlueprintType interface be
// implemented in both C++ and Blueprint, which is what a Blueprint-only game's PC needs).
UINTERFACE(BlueprintType)
class FOUNDRYFSDKUI_API UFoundryMenuHost : public UInterface
{
	GENERATED_BODY()
};

/**
 * The game's menu-layer owner (its player controller, normally). The kit's pause menu and confirm
 * dialog never add or remove themselves from the viewport; they ask the host
 * (UFoundryUIWidgetBase::FindMenuHost resolves it from the owning player controller). Names carry
 * the Request prefix so a game whose PC already has PushMenu / LeaveMatch (Goo Crew) can implement
 * this without a signature clash. BlueprintNativeEvent so a Blueprint-only game implements it in a
 * BP player controller, with no C++ subclass required.
 */
class FOUNDRYFSDKUI_API IFoundryMenuHost
{
	GENERATED_BODY()

public:
	/** Add Menu as the new top layer (viewport + input mode + keyboard focus are the host's job). */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI|Menu")
	void RequestPushMenu(UUserWidget* Menu);

	/** Remove the current top layer. */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI|Menu")
	void RequestPopMenu();

	/** Open the host's own settings menu (over whatever is already open). */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI|Menu")
	void RequestOpenSettings();

	/** Drop the current match/session and return to the front end. */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI|Menu")
	void RequestLeaveMatch();

	/** Which pause-menu wording/behavior the host wants right now. */
	UFUNCTION(BlueprintNativeEvent, Category = "Foundry UI|Menu")
	EFoundryPauseContext GetPauseContext() const;
};
