#include "FoundryMatchResults.h"

#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Engine/World.h"
#include "FoundryFSDKUILog.h"
#include "FoundryMenuHost.h"
#include "GameFramework/GameStateBase.h"

void UFoundryMatchResults::SetResult(const FText& Title, const FText& Detail)
{
	SetText(TitleText, Title.ToString());
	SetText(DetailText, Detail.ToString());
	Show(DetailText, !Detail.IsEmpty());
}

void UFoundryMatchResults::SetReturnDeadline(double ServerTimeSeconds)
{
	ReturnDeadline = ServerTimeSeconds;
	LastShownSeconds = MIN_int32; // force the next RefreshCountdown to apply
	Show(CountdownText, ReturnDeadline > 0.0);
	RefreshCountdown();
}

void UFoundryMatchResults::RefreshCountdown()
{
	if (ReturnDeadline <= 0.0 || CountdownText == nullptr)
	{
		return;
	}
	const UWorld* World = GetWorld();
	const AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
	if (GameState == nullptr)
	{
		return; // no replicated clock yet: keep whatever is showing, the next tick retries
	}

	const double Remaining = ReturnDeadline - GameState->GetServerWorldTimeSeconds();
	const int32 Seconds = Remaining > 0.0 ? FMath::CeilToInt32(Remaining) : 0;
	if (Seconds == LastShownSeconds)
	{
		return;
	}
	LastShownSeconds = Seconds;
	SetText(CountdownText, Seconds > 0
		? FString::Printf(TEXT("Returning to lobby in %d s"), Seconds)
		: FString(TEXT("Returning to lobby...")));
}

void UFoundryMatchResults::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	if (ReturnButton) { ReturnButton->OnClicked.AddDynamic(this, &UFoundryMatchResults::HandleReturnButtonClicked); }
	// Nothing to count down to until the game calls SetReturnDeadline.
	Show(CountdownText, false);
	// Focusable root: the host's PushMenu focuses the root widget, and NativeOnFocusReceived below
	// forwards that to the button (without this the focus falls through to the viewport).
	SetIsFocusable(true);
}

FReply UFoundryMatchResults::NativeOnFocusReceived(const FGeometry& InGeometry, const FFocusEvent& InFocusEvent)
{
	if (ReturnButton && ReturnButton->GetIsEnabled())
	{
		ReturnButton->SetKeyboardFocus();
		return FReply::Handled();
	}
	return Super::NativeOnFocusReceived(InGeometry, InFocusEvent);
}

void UFoundryMatchResults::NativeConstruct()
{
	Super::NativeConstruct();
	if (ReturnButton)
	{
		ReturnButton->SetKeyboardFocus(); // gamepad: A presses RETURN TO LOBBY
	}
	PlayOrTween(FadeAnim, true, 0.25f, this);
	RefreshCountdown();
}

void UFoundryMatchResults::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);
	RefreshCountdown();
}

void UFoundryMatchResults::HandleReturnButtonClicked()
{
	if (bReturnRequested)
	{
		return; // the leave is already under way; a second click must not run it twice
	}
	bReturnRequested = true;
	if (ReturnButton)
	{
		ReturnButton->SetIsEnabled(false);
	}

	// The results screen is the post-horn leave: a party leader's RETURN takes the party (the component
	// checks leader + match-over itself). Notified before either branch so a game that binds its own
	// OnReturnRequested gets the same behavior as the host path.
	if (OnReturnRequested.IsBound())
	{
		NotifyPartyOfLeave();
		OnReturnRequested.Broadcast();
	}
	else if (LeaveMatchViaHost())
	{
		// left through the host
	}
	else
	{
		UE_LOG(LogFoundryFSDKUI, Warning, TEXT("[UI] %s: RETURN TO LOBBY pressed with nothing bound to OnReturnRequested and no IFoundryMenuHost - nothing to do"), *GetClass()->GetName());
	}
}
