// Copyright Foundry Media. Curated developer console + diagnostics surface.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "InputCoreTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "FMMSSubsystem.h" // EFMMSPhase (also pulls EFoundryFsdkResult)
#include "FoundryConsoleSubsystem.generated.h"

class FFoundryConsoleKeyProcessor;
class FFoundryConsoleLogCapture;
class SFoundryConsole;
class SFoundryStatsOverlay;
class SFoundryNetGraph;

/** Who may run (and see, in `help`) a console command. Evaluated live against the
 *  FSDK session, so the available surface changes with auth state. */
UENUM(BlueprintType)
enum class EFoundryConsoleAccess : uint8
{
	Any       UMETA(DisplayName = "Any"),
	SignedIn  UMETA(DisplayName = "Signed In"),
	SignedOut UMETA(DisplayName = "Signed Out")
};

/** Native command handler: args (without the command token) -> response text. */
DECLARE_DELEGATE_RetVal_OneParam(FString, FFoundryConsoleHandler, const TArray<FString>&);

/** Blueprint command handler: args -> response text. */
DECLARE_DYNAMIC_DELEGATE_RetVal_OneParam(FString, FFoundryConsoleHandlerBP, const TArray<FString>&, Args);

/**
 * The Foundry developer console — the Shipping-safe replacement for the engine
 * console, shipped with the SDK so every Foundry game gets it. It dispatches a
 * WHITELISTED command registry only; it never routes to engine Exec, so a game
 * that compiles the engine console out of Shipping keeps its lockdown.
 *
 * Built-ins: `help`, `clear`, `status`, `netgraph [on|off|1|0]`, `stats [...]`.
 * The `foundry` group is the FGS surface (a mini foundry CLI in-engine):
 * `foundry login/logout/resume/whoami/findmatch/cancel`. Login machinery only
 * exists in FOUNDRY_FSDK_FID_AUTH=1 builds — in default launcher builds the
 * command surfaces the stub's NotImplemented result. Host games register their
 * own commands via RegisterCommand (C++) / RegisterConsoleCommand (Blueprint),
 * each gated by an EFoundryConsoleAccess level.
 *
 * Session events (login, logout, matchmaking phases) are printed into the
 * scrollback automatically; games pipe their own lifecycle lines via Print().
 *
 * The subsystem does NOT persist anything — the host game owns settings
 * (enable flag + keys) and pushes them in at startup (see Conquest's
 * UConquestDebugOverlaySubsystem for the reference glue).
 */
UCLASS()
class FOUNDRYFSDK_API UFoundryConsoleSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ── Master switch (gates both keybinds; -DevMode forces it on) ──────────────

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void SetEnabled(bool bInEnabled);

	UFUNCTION(BlueprintPure, Category = "Foundry|Console")
	bool IsEnabled() const { return bEnabled; }

	// ── Console ─────────────────────────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void ToggleConsole();

	UFUNCTION(BlueprintPure, Category = "Foundry|Console")
	bool IsConsoleOpen() const { return bConsoleOpen; }

	// ── Stats overlay + net graph ───────────────────────────────────────────────

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void SetStatsOverlayVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void ToggleStatsOverlay() { SetStatsOverlayVisible(!IsStatsOverlayVisible()); }

	UFUNCTION(BlueprintPure, Category = "Foundry|Console")
	bool IsStatsOverlayVisible() const { return StatsOverlayWidget.IsValid(); }

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void SetNetGraphVisible(bool bVisible);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void ToggleNetGraph() { SetNetGraphVisible(!bNetGraphOpen); }

	UFUNCTION(BlueprintPure, Category = "Foundry|Console")
	bool IsNetGraphVisible() const { return bNetGraphOpen; }

	// ── Keys (host game persists; we just hold the live values) ────────────────

	UFUNCTION(BlueprintPure, Category = "Foundry|Console")
	FKey GetConsoleKey() const { return ConsoleKey; }

	UFUNCTION(BlueprintPure, Category = "Foundry|Console")
	FKey GetOverlayKey() const { return OverlayKey; }

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void SetConsoleKey(FKey Key);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void SetOverlayKey(FKey Key);

	// ── Scrollback ──────────────────────────────────────────────────────────────

	/** Append a line (multi-line ok) to the console scrollback — visible next open.
	 *  Games pipe lifecycle logs ("match started", ...) here. Never pass secrets. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void Print(const FString& Line);

	// ── UE_LOG capture (the `log` command manages this at runtime too) ──────────

	/** Mirror a UE_LOG category (Warning/Error/Display/Log verbosity) into the
	 *  scrollback. The SDK's own categories are captured by default; games add
	 *  theirs here (e.g. their gamemode-lifecycle category). */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void CaptureLogCategory(FName Category);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void StopCapturingLogCategory(FName Category);

	UFUNCTION(BlueprintPure, Category = "Foundry|Console")
	TArray<FName> GetCapturedLogCategories() const;

	// ── Command registry ────────────────────────────────────────────────────────

	/** Register/replace a bare command (C++). Name is a single lower-case token. */
	void RegisterCommand(const FString& Name, const FString& Help, EFoundryConsoleAccess Access,
	                     FFoundryConsoleHandler Handler);

	/** Register/replace a bare command from Blueprint. */
	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void RegisterConsoleCommand(const FString& Name, const FString& Help, EFoundryConsoleAccess Access,
	                            FFoundryConsoleHandlerBP Handler);

	UFUNCTION(BlueprintCallable, Category = "Foundry|Console")
	void UnregisterConsoleCommand(const FString& Name);

	// ── Console-widget seam (not for game code) ─────────────────────────────────

	/** Echo (credential-masked) + dispatch one input line. The widget calls this. */
	void SubmitLine(const FString& RawLine);

	/** Monotonic change counter + cached text for the widget's cheap-diff render. */
	uint32 GetScrollbackSerial() const { return ScrollbackSerial; }
	FText GetScrollbackText() const;

	/** Submitted lines, oldest first (login lines stored credential-masked). */
	const TArray<FString>& GetCommandHistory() const { return History; }

	/** Tab completion: returns the (possibly extended) line; fills the candidate
	 *  names when the completion is ambiguous. Access-filtered like `help`. */
	FString CompleteLine(const FString& Current, TArray<FString>& OutCandidates) const;

	/** True while `foundry login <email>` is awaiting the masked password line. */
	bool IsAwaitingPassword() const { return !PendingLoginEmail.IsEmpty(); }

	/** Abort a pending masked-password capture (widget Esc / console close). */
	void CancelPendingLogin();

private:
	struct FCommandEntry
	{
		FString Help;
		EFoundryConsoleAccess Access = EFoundryConsoleAccess::Any;
		FFoundryConsoleHandler Handler;
	};

	void RegisterBuiltins();
	FString Execute(const FString& Line);
	FString Dispatch(const TMap<FString, FCommandEntry>& Registry, const FString& Name,
	                 const TArray<FString>& Args, const TCHAR* GroupPrefix);
	FString BuildHelpText() const;
	FString SanitizeForEcho(const FString& Line) const;
	bool IsSignedIn() const;
	bool IsCommandAvailable(const FCommandEntry& Entry) const;

	void AppendScrollback(const FString& Entry);
	void ClearScrollback();

	void CloseConsole();
	void RemoveNetGraph();
	void RemoveStatsOverlay();
	void SetWidgetPasswordMode(bool bOn);
	void LoadPersistedHistory();
	void SavePersistedHistory() const;

	bool DrainCapturedLogs(float DeltaTime);

	UFUNCTION() void HandleLoginEvent(EFoundryFsdkResult Result, const FString& DisplayName);
	UFUNCTION() void HandleLoggedOutEvent();
	UFUNCTION() void HandleFmmsStatus(EFMMSPhase Phase, const FString& Message);

	static bool ParseOnOff(const TArray<FString>& Args, TOptional<bool>& OutValue);

	TMap<FString, FCommandEntry> Commands;        // bare commands (built-ins + game-registered)
	TMap<FString, FCommandEntry> FoundryCommands; // the `foundry <sub>` FGS group

	TSharedPtr<FFoundryConsoleKeyProcessor> KeyProcessor;
	TSharedPtr<SFoundryConsole> ConsoleWidget;       // kept alive across open/close
	TSharedPtr<SFoundryStatsOverlay> StatsOverlayWidget;
	TSharedPtr<SFoundryNetGraph> NetGraphWidget;     // kept alive so the sample window survives

	TArray<FString> Scrollback;
	TArray<FString> History;
	uint32 ScrollbackSerial = 0;

	/** Non-empty = the next submitted line is that account's password (masked input,
	 *  never echoed, never stored). The password itself is never held as state. */
	FString PendingLoginEmail;

	TSharedPtr<FFoundryConsoleLogCapture> LogCapture; // shared: deleter is type-erased (fwd-decl safe)
	FTSTicker::FDelegateHandle LogDrainTicker;

	FKey ConsoleKey;
	FKey OverlayKey;
	bool bEnabled = false;
	bool bConsoleOpen = false;
	bool bNetGraphOpen = false;

	static constexpr int32 MaxScrollbackLines = 400;
	static constexpr int32 MaxHistoryLines = 64;
	/** Shell-style recall across RUNS: the newest N history lines persist to the
	 *  game-user-settings ini (lines are sanitized at submit — no credentials). */
	static constexpr int32 MaxPersistedHistoryLines = 25;
};
