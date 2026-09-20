# Foundry FSDK — Unreal Engine

The **Unreal Engine** plugin for the **Foundry SDK (FSDK)** — the engine-side client for
[Foundry Gaming Services](https://foundryplatform.app). It gives a game:

- **Sign-in** from the Foundry launcher (no credentials in the game, ever)
- **Matchmaking** (FMMS) by queue name, with region cycling, reconnect and parties
- **A dedicated-server admission gate**: the server verifies each joining player's
  platform-signed match token, and drives its own lifecycle (ready / health / drain / idle exit)
- **Chat and social** (global / party / match / team rooms, friends, whispers)
- **A Shipping-safe developer console** (`~`) with the `foundry` command group

Everything is exposed to **C++ and Blueprints** through `UGameInstanceSubsystem`s. The
security-critical logic lives once in the shared `fsdk-core` C ABI, vendored into this module
and compiled from source by the engine toolchain — no CMake, no prebuilt library, no external
dependency beyond the engine's own HTTP/WebSockets modules.

> **Status: beta.** The API surface is still moving. Install a release zip and pin its
> version rather than tracking `main`.

This README is the setup path a stranger follows without us. The reference integration is
**Conquest** (Foundry's first game); every snippet below is lifted from it.

---

## 1. What you need — and why

| Requirement | Why |
|---|---|
| **Unreal Engine 5.7, built from source** | Foundry hosts your game as a **dedicated server**. A dedicated-server target (`TargetType.Server`) can only be built against engine source — the Epic Games Launcher engine does not ship it. 5.7 from source is also the only configuration we verify. |
| **A C++ project** | A Server target is a `Source/<Game>Server.Target.cs` file. A project with a `Source/` folder is a C++ project. This is also a code plugin: it compiles inside your project. You can still write all of your gameplay in Blueprints — the plugin's whole client surface is Blueprint-callable — but the project itself is C++. A Blueprint-only project is not supported. |
| **Windows host** | The verified build host. The Linux server is cross-compiled from Windows. |
| **The Linux cross-toolchain** | The dedicated server runs on Linux. Epic ships a prebuilt clang toolchain for this; install once (below). |
| **Docker Desktop** | `foundry package --server` builds the server container image locally. |
| **A [Foundry](https://foundryplatform.app) account + the `foundry` CLI** | The console registers your game and queues; the CLI packages, pushes and publishes. Install the CLI from [github.com/FoundryMedia/foundry/releases](https://github.com/FoundryMedia/foundry/releases). |

### 1.1 Build UE 5.7 from source (once)

1. Link your GitHub account to your Epic account
   ([unrealengine.com/ue-on-github](https://www.unrealengine.com/ue-on-github)) and accept the
   invitation to the `EpicGames` organization.
2. Clone the `5.7` branch of
   [github.com/EpicGames/UnrealEngine](https://github.com/EpicGames/UnrealEngine) somewhere
   short (e.g. `D:\UnrealEngine`). Avoid spaces in the path if you can — everything works with
   them, but every quoting bug you will ever hit starts there.
3. Run `Setup.bat`, then `GenerateProjectFiles.bat`.
4. Open `UE5.sln` in Visual Studio 2022, set **Development Editor / Win64**, build the **UE5**
   target. First build is 1–3 hours.

Epic's full guide: [Building Unreal Engine from Source](https://dev.epicgames.com/documentation/en-us/unreal-engine/building-unreal-engine-from-source).

### 1.2 Install the Linux cross-toolchain (once)

The engine tells you which version it wants:

```
<UnrealEngine>/Engine/Config/Linux/Linux_SDK.json   ->   "MainVersion": "v26_clang-20.1.8-rockylinux8"
```

Download Epic's prebuilt installer for that exact version and run it (silent install is `/S`):

```
https://cdn.unrealengine.com/CrossToolchain_Linux/v26_clang-20.1.8-rockylinux8.exe
```

It installs to `C:\UnrealToolchains\<version>\` and sets the machine environment variable
`LINUX_MULTIARCH_ROOT`. **Open a new shell afterwards** — a shell started before the install
does not see the variable, and the build silently falls back to "no Linux toolchain".

Verify:

```
<UnrealEngine>\Engine\Build\BatchFiles\RunUAT.bat Turnkey -command=VerifySdk -platform=Linux
```

Building the toolchain from source (`Engine/Build/BatchFiles/Linux/Toolchain/RunMe.bat`) also
works but takes ~1 hour and ~30 GB. Use the prebuilt.

---

## 2. Install the plugin

1. Download `FoundryFSDK-v<version>.zip` from the
   [latest release](https://github.com/FoundryMedia/fsdk-unreal/releases/latest).
2. Unzip it into your project's `Plugins/` folder so this file exists:

   ```
   <YourProject>/Plugins/FoundryFSDK/FoundryFSDK.uplugin
   ```

   The zip already carries the top-level `FoundryFSDK/` folder; do not nest it twice.
3. Enable it in `<YourProject>.uproject` (a project plugin is enabled by default, but list it —
   it makes the dependency explicit for anyone who clones your repo):

   ```json
   "Plugins": [
     { "Name": "FoundryFSDK", "Enabled": true }
   ]
   ```
4. Add the module to your game module's `Source/<Game>/<Game>.Build.cs`:

   ```cs
   PrivateDependencyModuleNames.AddRange(new string[] { "FoundryFSDK" });
   ```
5. Right-click `<YourProject>.uproject`:
   - **Switch Unreal Engine version…** → pick your source build (first time only; this writes
     the engine GUID into `EngineAssociation`).
   - **Generate Visual Studio project files**.
6. Open the `.sln`, set **Development Editor / Win64**, build `<Game>Editor`. The plugin's C
   core and the UE bridge compile as part of your project (about a minute).

**Check:** the editor opens; **Edit → Plugins → Foundry** shows *Foundry FSDK* enabled; in a C++
file `#include "FoundryFSDKSubsystem.h"` resolves.

### 2.1 Starting from nothing: a C++ project with a Server target

If you do not have a project yet:

1. Launch the editor from your source build
   (`<UnrealEngine>\Engine\Binaries\Win64\UnrealEditor.exe`), **New Project → Games → Blank**,
   project type **C++**, create it. You get `Source/<Game>/`, `<Game>.Target.cs` and
   `<Game>Editor.Target.cs`.
2. Add the dedicated-server target next to them — `Source/<Game>Server.Target.cs`:

   ```cs
   using UnrealBuildTool;
   using System.Collections.Generic;

   public class MyGameServerTarget : TargetRules
   {
       public MyGameServerTarget(TargetInfo Target) : base(Target)
       {
           Type = TargetType.Server;
           DefaultBuildSettings = BuildSettingsVersion.V6;
           IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
           ExtraModuleNames.Add("MyGame");
       }
   }
   ```

   (Conquest: `Source/ConquestServer.Target.cs`.) Regenerate project files; `MyGameServer` now
   appears as a build target.
3. Install the plugin as in section 2.

Everything below uses `MyGame` as the project name — substitute yours.

---

## 3. Quickstart — C++

Three pieces: the client signs in and finds a match, the server admits the player, and one
line that keeps client and server able to talk to each other at all.

### 3.1 Sign in — `AutoLoginFromLauncher()`

Players launch your game from the **Foundry launcher**. The launcher spawns the game with an
environment variable `FOUNDRY_IPC` naming a local named pipe plus a one-shot nonce; the plugin
connects to that pipe and receives a short-lived, matchmaking-scoped player token. **Your game
never sees a password, never stores a session, and holds no secret** — a decompiled client can
at most act as that player's matchmaking identity for 15 minutes.

`AutoLoginFromLauncher()` does the whole thing. It is asynchronous and reports through
`OnLoginComplete(Result, DisplayName)`:

- `Ok` — signed in; `DisplayName` is the player's platform display name (put it on screen).
- `NotAuthenticated` — no launcher session (the game was not started from the launcher, or the
  launcher is signed out). Tell the player to sign in through the Foundry launcher.

```cpp
// MyMenuPlayerController.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "FoundryFSDKSubsystem.h" // EFoundryFsdkResult
#include "FMMSSubsystem.h"        // EFMMSPhase
#include "MyMenuPlayerController.generated.h"

UCLASS()
class MYGAME_API AMyMenuPlayerController : public APlayerController
{
    GENERATED_BODY()

public:
    /** Wire this to your Find Match button (also callable from Blueprint). */
    UFUNCTION(BlueprintCallable, Category = "Foundry")
    void FindMatch();

protected:
    virtual void BeginPlay() override;

private:
    UFUNCTION()
    void HandleLoginComplete(EFoundryFsdkResult Result, const FString& DisplayName);

    UFUNCTION()
    void HandleFMMSStatus(EFMMSPhase Phase, const FString& Message);
};
```

```cpp
// MyMenuPlayerController.cpp
#include "MyMenuPlayerController.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"

void AMyMenuPlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (!IsLocalController())
    {
        return;
    }

    UGameInstance* GI = GetGameInstance();
    UFoundryFSDKSubsystem* Fsdk = GI->GetSubsystem<UFoundryFSDKSubsystem>();
    UFMMSSubsystem* Fmms = GI->GetSubsystem<UFMMSSubsystem>();

    Fsdk->OnLoginComplete.AddDynamic(this, &AMyMenuPlayerController::HandleLoginComplete);
    Fmms->OnFMMSStatus.AddDynamic(this, &AMyMenuPlayerController::HandleFMMSStatus);

    // The default sign-in: the launcher handoff (FOUNDRY_IPC). No credentials in the game.
    Fsdk->AutoLoginFromLauncher();
}

void AMyMenuPlayerController::HandleLoginComplete(EFoundryFsdkResult Result, const FString& DisplayName)
{
    if (Result == EFoundryFsdkResult::Ok)
    {
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Green,
            FString::Printf(TEXT("Signed in as %s"), *DisplayName));

        // Reconnect-aware menu entry: if this player is already seated in a live match
        // (a crash, a kick), the FMMS phase becomes Reconnectable and Reconnect() rejoins it.
        GetGameInstance()->GetSubsystem<UFMMSSubsystem>()->CheckActiveSession();
    }
    else
    {
        // NotAuthenticated = no launcher session. Never show a login form here.
        GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Red,
            TEXT("Sign in through the Foundry launcher, then relaunch."));
    }
}

void AMyMenuPlayerController::FindMatch()
{
    // The queue NAME you created in the console: "<game-slug>/<mode>".
    GetGameInstance()->GetSubsystem<UFMMSSubsystem>()->FindMatchAuthenticated(TEXT("goo-crew/spread"), TEXT("{}"));
}

void AMyMenuPlayerController::HandleFMMSStatus(EFMMSPhase Phase, const FString& Message)
{
    // Searching / Match found - joining server... / Failed: <why>. Put it on your menu.
    GEngine->AddOnScreenDebugMessage(-1, 5.0f, FColor::Cyan, Message);
}
```

Conquest's version: `Source/Conquest/Core/Player/MainMenuPlayerController.cpp`
(`BeginPlay` → `AutoLoginFromLauncher`, `HandleLoginComplete`, `Cmd_FindMatch`).

**In the editor there is no launcher, so `AutoLoginFromLauncher()` reports
`NotAuthenticated`.** That is correct behavior, not a bug. For the day-to-day dev loop:

- Run with `-DevMode` (editor command line, or *Additional Launch Parameters* for a Standalone
  Game), press **`~`** for the Foundry console, type `foundry login <email>`; the password is
  prompted masked. The same `OnLoginComplete` fires, so your menu code is exercised unchanged.
  `foundry whoami`, `foundry findmatch <queue>`, `foundry cancel` are there too.
- This credential path exists **only in non-Shipping builds** (`FOUNDRY_FSDK_FID_AUTH`, set by
  `FoundryFSDK.Build.cs`). A Shipping client carries no login code at all — only the launcher
  handoff. That is the point.

The launcher handoff itself is exercised the first time your build is installed and launched
from the launcher (a test-build install; see section 6).

### 3.2 Find a match — `UFMMSSubsystem`

`FindMatchAuthenticated(Queue, AttributesJson)` runs the whole player loop on the session from
3.1: submit a ticket, poll, resolve the connection, and `ClientTravel` to
`<ip>:<port>?token=<matchToken>`. You never touch the token or the endpoint. Phases arrive on
`OnFMMSStatus`: `Authenticating → Requesting → Searching → Connecting → Traveling`, or `Failed`
with a message.

- `Queue` is the queue **name** from the console: `<game-slug>/<mode>` (Conquest's is
  `conquest/classic`). The full FRN also works.
- `AttributesJson` is an opaque JSON object; `{}` is fine. Put `"partyId"` there to keep a party
  together, `"region"` to pin a region. By default the SDK measures latency to every region and
  the matchmaker places in the lowest-latency region with capacity.
- `Cancel()` cancels a search. `CheckActiveSession()` + `Reconnect()` handle "I was in a match
  and got disconnected". `HasActiveMatch()` drives a Reconnect button.
- Rapid re-clicks are throttled inside the subsystem; you do not need a debounce.

The lower-level calls (`InitializeClient` / `Authenticate` / `RequestMatch` / `PollMatch` /
`GetConnection` on `UFoundryFSDKSubsystem`) are still public for games that want to own the
loop; `UFMMSSubsystem` is the recommended surface.

### 3.3 Admit the player — the server half

On the dedicated server the same plugin exposes `FFoundryFSDKServer` — a plain C++ class,
**compiled only for `TargetType.Server`** (`#if UE_SERVER`), so none of it, and none of
OpenSSL, is in the client binary. It does two jobs: verify each joining player's match token
against the platform's public keys (JWKS), and drive the server's lifecycle with the
orchestrator (Ready, Health, drain, Shutdown).

Hook it into your `GameMode`:

```cpp
// MyGameMode.h
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "FoundryFSDKServer.h" // declares FFoundryFSDKServer under UE_SERVER; empty otherwise
#include "MyGameMode.generated.h"

UCLASS()
class MYGAME_API AMyGameMode : public AGameModeBase
{
    GENERATED_BODY()

protected:
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** The admission gate: a joining player must carry a valid platform-signed match token. */
    virtual void PreLogin(const FString& Options, const FString& Address,
                          const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;

#if UE_SERVER
private:
    void TickFoundryHealth();

    TUniquePtr<FFoundryFSDKServer> FoundryServer;
    FTimerHandle FoundryHealthTimer;
    bool bFoundryDraining = false;
#endif
};
```

```cpp
// MyGameMode.cpp
#include "MyGameMode.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"

void AMyGameMode::BeginPlay()
{
    Super::BeginPlay();

#if UE_SERVER
    // Where the match tokens' public keys live (auth-efga JWKS) and where the local
    // lifecycle sidecar listens. Both are handed to the container by the platform;
    // the defaults are right for Foundry hosting.
    FString AuthBase = TEXT("https://auth.foundryplatform.app");
    FParse::Value(FCommandLine::Get(), TEXT("FoundryAuthBase="), AuthBase);
    FString SidecarAddr; // empty = http://127.0.0.1:9358
    FParse::Value(FCommandLine::Get(), TEXT("FoundryAgonesAddr="), SidecarAddr);

    FoundryServer = FFoundryFSDKServer::Create(SidecarAddr, AuthBase);
    if (!FoundryServer.IsValid())
    {
        UE_LOG(LogTemp, Error, TEXT("[Foundry] server SDK init FAILED - every join will be rejected"));
        return;
    }

    // Latch which match this process was allocated for, then tell the orchestrator we are Ready.
    FoundryServer->GetBindingAndReady();

    // Health ping + occupancy report every 5 s.
    GetWorld()->GetTimerManager().SetTimer(FoundryHealthTimer, this,
        &AMyGameMode::TickFoundryHealth, 5.0f, true);
#endif
}

#if UE_SERVER
void AMyGameMode::TickFoundryHealth()
{
    if (!FoundryServer.IsValid())
    {
        return;
    }

    // Reporting the player count is what arms the SDK's idle-empty auto-drain: an allocated
    // server that sits empty for 5 minutes winds itself down. Zero extra code.
    FoundryServer->Health(GetNumPlayers());

    // The platform (or the idle policy) asked us to wind down: stop admitting, let the match end.
    if (!bFoundryDraining && FoundryServer->IsDrainRequested())
    {
        bFoundryDraining = true;
        UE_LOG(LogTemp, Warning, TEXT("[Foundry] winding down - no new joins; exiting when empty"));
    }

    // Winding down AND empty: the host owns process exit. EndPlay calls Shutdown() on the way out.
    if (FoundryServer->ShouldExit())
    {
        FPlatformMisc::RequestExit(false);
    }
}
#endif

void AMyGameMode::PreLogin(const FString& Options, const FString& Address,
                           const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
    Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
    if (!ErrorMessage.IsEmpty())
    {
        return; // the engine already refused (server full, etc.)
    }

#if UE_SERVER
    if (bFoundryDraining)
    {
        ErrorMessage = TEXT("Server is draining");
        return;
    }

    // The client traveled to "<ip>:<port>?token=<matchToken>" (UFMMSSubsystem did that).
    const FString Token = UGameplayStatics::ParseOption(Options, TEXT("token"));
    FString FoundryId, MatchId;
    if (Token.IsEmpty() || !FoundryServer.IsValid()
        || !FoundryServer->ValidatePlayer(Token, FoundryId, MatchId))
    {
        ErrorMessage = TEXT("Invalid match token"); // fail closed: no token, no entry
        return;
    }
    UE_LOG(LogTemp, Log, TEXT("[Foundry] admitted %s (match %s)"), *FoundryId, *MatchId);
#endif
}

void AMyGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
#if UE_SERVER
    if (FoundryServer.IsValid())
    {
        FoundryServer->Shutdown(); // the orchestrator reclaims the process
        FoundryServer.Reset();
    }
#endif
    Super::EndPlay(EndPlayReason);
}
```

Conquest's version: `Source/Conquest/Core/Game/ConquestGameMode.cpp` (`InitFoundryServer`,
`TickFoundryHealth`, `PreLogin`, `EndPlay`).

`ValidatePlayer` also has overloads that return the token's **signed** `display_name` and
**team** claims. Use them in `InitNewPlayer` to name the player and seat them on the team the
matchmaker chose — a hand-built connect URL can spoof `?Name=`, it cannot forge a platform
signature:

```cpp
// inside AMyGameMode::InitNewPlayer, after Super::InitNewPlayer(...)
#if UE_SERVER
    const FString Token = UGameplayStatics::ParseOption(Options, TEXT("token"));
    FString FoundryId, MatchId, DisplayName;
    int32 Team = -1; // -1 = the token carries no team
    if (FoundryServer.IsValid()
        && FoundryServer->ValidatePlayer(Token, FoundryId, MatchId, DisplayName, Team))
    {
        if (!DisplayName.IsEmpty())
        {
            ChangeName(NewPlayerController, DisplayName, false);
        }
        // Team >= 0: the matchmaker's seat for this player. Store it on your PlayerState.
    }
#endif
```

(Conquest: `ConquestGameMode::InitNewPlayer`.)

**Nothing in your server knows or cares whether it runs on Foundry's own boxes or on AWS
GameLift.** The lifecycle calls go to a local sidecar on `127.0.0.1:9358`; on GameLift the
platform injects a shim into your container image that speaks the same four routes inward and
the GameLift Server SDK outward. Your image needs no Agones or GameLift code.

### 3.4 Do this on day one: pin the network protocol version

Unreal hashes your project's `ProjectVersion` into the network handshake. `foundry package`
stamps `ProjectVersion` per release, so a client at 0.13.0 and a server at 0.6.0 refuse each
other with `CloseReason=Upgrade` and the player silently bounces back to the menu. Conquest hit
this. Override the hash with a protocol constant **you** control, in your primary game module:

```cpp
// Source/MyGame/MyGame.cpp
#include "MyGame.h"
#include "Misc/NetworkVersion.h"
#include "Modules/ModuleManager.h"

// Bump ONLY on a breaking replication/protocol change (replicated fields added/removed/retyped,
// RPC signature changes). Clients and servers with different numbers cannot join each other.
static constexpr uint32 MyGameNetProtocolVersion = 1;

class FMyGameModule : public FDefaultGameModuleImpl
{
public:
    virtual void StartupModule() override
    {
        FDefaultGameModuleImpl::StartupModule();
        FNetworkVersion::GetLocalNetworkVersionOverride.BindLambda(
            []() -> uint32 { return MyGameNetProtocolVersion; });
    }
};

IMPLEMENT_PRIMARY_GAME_MODULE(FMyGameModule, MyGame, "MyGame");
```

This replaces the `IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ...)` line the
project wizard generated. (Conquest: `Source/Conquest/Conquest.cpp`.)

---

## 4. Quickstart — Blueprint

The client flow is the same three nodes, no C++ beyond the project itself. In your menu
widget or player controller graph:

1. **Get Game Instance → Get Subsystem (Foundry FSDK)** and **Get Subsystem (FMMS)**.
2. On the Foundry FSDK subsystem, **Bind Event to On Login Complete** → a custom event with
   `Result` (`EFoundryFsdkResult`) and `DisplayName`. Branch on `Result == OK`: show
   `DisplayName`, enable your Find Match button; else show "Sign in through the Foundry
   launcher".
3. Call **Auto Login From Launcher** (category *Foundry | Auth*).
4. On the FMMS subsystem, **Bind Event to On FMMS Status** → a custom event with `Phase`
   (`EFMMSPhase`) and `Message`. Put `Message` on screen; `Phase == Failed` is red.
5. Find Match button → **Find Match Authenticated** (category *Foundry | FMMS*) with
   `Queue = "goo-crew/spread"`, `Attributes Json = "{}"`. Cancel button → **Cancel**.
6. Optional: after sign-in call **Check Active Session**; if **Has Active Match** is true, show
   a Reconnect button that calls **Reconnect**.

Every node lives under the **Foundry |** categories (*FSDK*, *Auth*, *FMMS*, *Chat*,
*Social*, *Console*). Conquest's real menu (`Content/UI/Menus/WBP_MainMenu`) is built exactly
this way on top of the same subsystems.

**The server half stays C++.** `FFoundryFSDKServer` is a plain class (not a `UObject`), gated to
the Server target — section 3.3 is the whole of it, about sixty lines in your `GameMode`. A
Blueprint-only project cannot build a Server target anyway (section 1).

---

## 5. Chat, social, console — the rest of the surface

All on `UFoundryFSDKSubsystem`, all Blueprint-callable, all riding the session from
`AutoLoginFromLauncher`:

| Category | Calls |
|---|---|
| **Foundry \| Chat** | `JoinGlobalChat(GameSlug)`, `JoinPartyChat`, `JoinMatchChat(MatchId)`, `JoinTeamChat(MatchId)`, `SendChat`, `SendChatToChannel`, `LeaveChat`; events `OnChatMessage`, `OnChatStateChanged`. One socket, one subscription per channel. |
| **Foundry \| Social** | friends (`RefreshFriends`, `SendFriendRequest(Username)`, accept/remove/block), friend codes, parties (`CreateParty`, `InviteToParty`, accept/decline/leave, `RefreshPartyInvites`), whispers. A party's id is the `partyId` attribute you pass to `FindMatchAuthenticated`. |
| **Foundry \| FMMS** | `GetCurrentMatchId()` after travel — the key for match/team chat. |
| **Foundry \| Console** | `UFoundryConsoleSubsystem`: `SetEnabled`, `SetConsoleKey` (default `~`), `SetOverlayKey` (default F9), `RegisterConsoleCommand`. The console dispatches a whitelist only — it never reaches the engine's `Exec`, so a Shipping build stays locked down. `-DevMode` on the command line forces it on. |

Nothing here talks to an admin or operator route. The client SDK holds no secrets and is
assumed fully reverse-engineered; every permission is enforced by the platform.

---

## 6. Ship it: package, push, host, publish

The `foundry` CLI drives the pipeline from a `.foundry/config.yml` in your project root.
Sign in once with `foundry login`, register the game once in the console (or
`foundry games create --name "Goo Crew"` — the slug is derived from the name and is permanent).

```yaml
# .foundry/config.yml  (Conquest's, with the names changed)
schemaVersion: "1"
kind: game-publisher
name: Goo Crew

publisher: your-publisher-handle
gameId: goo-crew                     # the slug from the console

build:
  type: ue5
  uprojectPath: GooCrew.uproject
  ueRoot: D:/UnrealEngine            # your source build
  executableRelpath: Windows/GooCrew.exe
  clientConfig: Shipping             # real releases ship Shipping (console/debug stripped)
  chunking: true                     # fail the package if content cooks to one monolithic container

server:
  ueRoot: D:/UnrealEngine
  uprojectPath: GooCrew.uproject
  serverTarget: GooCrewServer        # the Server target from section 2.1
  dockerfile: Docker/Dockerfile
  imageName: goo-crew-server
```

The server needs a `Docker/Dockerfile` + `Docker/entrypoint.sh`. `foundry package --server`
cross-compiles and stages the Linux server to `Saved/StagedBuilds/LinuxServer/`, then runs
`docker build` with `Saved/StagedBuilds/` as the context, so the Dockerfile `COPY`s
`LinuxServer/`:

```dockerfile
# Docker/Dockerfile
FROM ubuntu:22.04
# A UE Linux server needs only glibc + TLS roots (OpenSSL is statically linked into the binary).
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates libc6 \
 && rm -rf /var/lib/apt/lists/*
COPY LinuxServer/ /server/
COPY entrypoint.sh /entrypoint.sh
# UE dedicated servers REFUSE to run as root; the ELF loses its +x bit on Windows filesystems.
RUN useradd -m -u 1000 ueserver \
 && chmod +x /server/GooCrew/Binaries/Linux/GooCrewServer /entrypoint.sh \
 && chown -R ueserver:ueserver /server
USER ueserver
EXPOSE 7777/udp
ENTRYPOINT ["/entrypoint.sh"]
```

```sh
#!/bin/sh
# Docker/entrypoint.sh - the platform sets FOUNDRY_GAME_PORT and FOUNDRY_AUTH_BASE.
exec /server/GooCrew/Binaries/Linux/GooCrewServer GooCrew_Arena \
  -Port="${FOUNDRY_GAME_PORT:-7777}" \
  -FoundryAuthBase="${FOUNDRY_AUTH_BASE:-https://auth.foundryplatform.app}" \
  -unattended -stdout -FullStdOutLogOutput
```

The Dockerfile path comes from `server.dockerfile`, but the **build context is
`Saved/StagedBuilds/`**, so `COPY entrypoint.sh` resolves there: copy `Docker/entrypoint.sh` into
`Saved/StagedBuilds/` before packaging (the CLI does not do this for you yet), LF-terminated.
Put a `.dockerignore` next to it excluding `Windows/`, `**/*.debug`, `**/*.sym`, `**/*.pdb`
and `Manifest_*.txt` — about 1.4 GB of symbols the server never reads.

Then, in order:

```sh
foundry package --server --version 0.1.0               # Linux cook + docker build + save -> <name>-0.1.0.tar
foundry fcm push <the .tar> --type server --version 0.1.0 --game goo-crew
#   -> the platform scans the image. Once it is CLEAN, set the SERVER channel in the console
#      (Content Mesh -> your game): that is what provisions the game's own server fleet.

foundry keys generate && foundry keys register          # once: you sign your own releases;
                                                        # Foundry never holds your private key
foundry package --client --version 0.1.0               # Windows cook -> Saved/StagedBuilds/Windows
foundry fcm publish Saved/StagedBuilds/Windows --version 0.1.0 --prerelease
#   -> a private test build (the "snapshot" channel) you and your testers install from the launcher.
#      Drop --prerelease when the release is public; move the pointer with `foundry fcm channel set`.
#      (--managed = platform-held KMS signing, available once Foundry has enabled it for your org.)
```

Create the matchmaking queue in the console (**Matchmaking → New queue**) or from the CLI. Its
name — `<game-slug>/<mode-slug>` — is what `FindMatchAuthenticated` takes:

```sh
foundry fmms queue create --game goo-crew --mode-slug spread --display-name "Spread" \
  --teams 4 --team-size 12 --min-players 4 --backfill
#   -> queue "goo-crew/spread": up to 4 teams of 12, a match starts at 4 players,
#      later searchers join in progress.
```

Foundry's own walkthrough of this pipeline is the **Goo Crew** series: Ep1 sign in, Ep2 host
it, Ep3 match it, Ep4 ship it.

---

## 7. Security model, in one paragraph

The client holds **no secrets**: its only credential is a 15-minute, matchmaking-scoped player
token handed over by the launcher, never a password, never the account session. The client can
call **only player-scoped routes** (sign-in probe, matchmaking, chat, social) and every check is
enforced server-side. The server endpoint a matched player receives is an **opaque rendezvous**
(`ip:port` today, a relay later) and is useless without the platform-signed **match token** the
server verifies before admitting anyone. The match-token verifier and OpenSSL are compiled
**only** into the Server target — absent from the player binary, not merely unreachable. The
full model is in `fsdk-core/SECURITY.md`.

---

## 8. Layout

```
FoundryFSDK/
  FoundryFSDK.uplugin
  Source/FoundryFSDK/
    FoundryFSDK.Build.cs                     # compiles the vendored C core; server gate (UE_SERVER)
    Public/FoundryFSDKSubsystem.h            # sign-in, low-level matchmaking, chat, social (BP + C++)
    Public/FMMSSubsystem.h                   # the matchmaking orchestrator: FindMatchAuthenticated -> travel
    Public/FoundryFSDKServer.h               # FFoundryFSDKServer: token gate + lifecycle (Server target only)
    Public/FoundryConsoleSubsystem.h         # the Shipping-safe developer console (~)
    Private/FoundryFSDKLauncherHandoff.cpp   # FOUNDRY_IPC: the launcher -> game session handoff
    Private/FoundryFSDKTransport.cpp         # engine HTTP + WebSockets behind the core's transport seams
    Private/FoundryFSDKVerifier.cpp          # OpenSSL RS256 + JWKS (server only)
    Private/FsdkCore/                        # vendored fsdk-core (C), compiled by UBT
  LICENSE                                    # Apache-2.0
```

The vendored `Private/FsdkCore/` is a copy of the `fsdk-core` translation units. The server and
token units are wrapped in `#if FOUNDRY_FSDK_SERVER`, which `FoundryFSDK.Build.cs` defines only
for `TargetType.Server`; on every other target they compile to empty translation units. When
re-vendoring from `fsdk-core`, re-apply that gate — upstream does not carry it.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `AutoLoginFromLauncher` → `NotAuthenticated` in the editor | Expected: no launcher session. Use `-DevMode` + `foundry login` (section 3.1). |
| Client joins, then immediately returns to the menu; server log says `CloseReason=Upgrade` | `ProjectVersion` mismatch in the net handshake. Section 3.4. |
| Server log `PreLogin rejected: token validation failed` on every join | The server cannot reach the JWKS at `<FoundryAuthBase>/.well-known/jwks.json`, or the token was minted for another match. Check `-FoundryAuthBase` and the container's outbound HTTPS. |
| `MyGameServer` is not a build target | The engine is a Launcher (binary) install, or the `Server.Target.cs` was added without regenerating project files. |
| Container exits at once with `Refusing to run with the root privileges` | Run the server as a non-root user (the Dockerfile above). |
| `foundry package --server`: `Dockerfile not found` / `COPY LinuxServer/` or `COPY entrypoint.sh` fails | The Dockerfile path is `server.dockerfile` in the config; the build context is `Saved/StagedBuilds/`, so `entrypoint.sh` must be copied there. |

## License

[Apache-2.0](LICENSE). Copyright 2026 Foundry Media LLC.
