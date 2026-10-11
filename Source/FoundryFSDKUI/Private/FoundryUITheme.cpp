#include "FoundryUITheme.h"

#include "FoundryUISettings.h"

namespace FoundryUITheme
{
	FLinearColor PresenceColor(const FString& Presence)
	{
		const FFoundryUIPalette& P = UFoundryUISettings::Get()->Palette;
		if (Presence == TEXT("online") || Presence.IsEmpty())
		{
			return P.Online;
		}
		if (Presence == TEXT("idle") || Presence == TEXT("away"))
		{
			return P.Idle;
		}
		if (Presence == TEXT("dnd"))
		{
			return P.Error;
		}
		return P.TextSecondary;
	}

	FString PresenceLabel(const FString& Presence)
	{
		if (Presence == TEXT("online")) { return TEXT("Online"); }
		if (Presence == TEXT("idle")) { return TEXT("Idle"); }
		if (Presence == TEXT("away")) { return TEXT("Away"); }
		if (Presence == TEXT("dnd")) { return TEXT("Do not disturb"); }
		if (Presence == TEXT("offline")) { return TEXT("Offline"); }
		return TEXT("In party");
	}

	FLinearColor MemberTint(int32 RosterIndex, bool bLocal)
	{
		const FFoundryUIPalette& P = UFoundryUISettings::Get()->Palette;
		if (bLocal)
		{
			return P.Primary;
		}
		const FLinearColor Cast[] = { P.Member0, P.Member1, P.Member2, P.Member3, P.Member4, P.Member5 };
		return Cast[FMath::Abs(RosterIndex) % UE_ARRAY_COUNT(Cast)];
	}

	FLinearColor CompositeOver(const FLinearColor& Base, const FLinearColor& Overlay, float OverlayAlpha)
	{
		return FLinearColor(
			FMath::Lerp(Base.R, Overlay.R, OverlayAlpha),
			FMath::Lerp(Base.G, Overlay.G, OverlayAlpha),
			FMath::Lerp(Base.B, Overlay.B, OverlayAlpha),
			Base.A);
	}
}
