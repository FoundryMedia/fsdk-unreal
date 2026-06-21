# fsdk-unreal

The **Unreal Engine** binding for the **Foundry SDK (FSDK)** - the MVP engine
binding. It is a thin, idiomatic-UE wrapper over the shared
[`fsdk-core`](../fsdk-core) C ABI. All auth / matchmaking / token logic lives in
the core; this plugin just exposes it to UE C++ and Blueprints.

> **Status: SCAFFOLD.** Structurally a valid UE plugin (`.uplugin`, `Build.cs`,
> a `UGameInstanceSubsystem` facade). It cannot be engine-built here (no engine
> in this environment) and the `fsdk-core` static lib is not yet vendored - it
> wraps the C ABI **stubs**.

## What this plugin provides

`UFoundryFSDKSubsystem` (a `UGameInstanceSubsystem`) is the game-client facade,
with Blueprint-callable methods that map 1:1 onto the fsdk-core CLIENT ABI:

| Blueprint method            | fsdk-core C ABI            |
|-----------------------------|----------------------------|
| `InitializeClient(BaseUrl)` | `fsdk_client_create`       |
| `Authenticate(PlayerToken)` | `fsdk_authenticate`        |
| `RequestMatch(Queue, Attrs)`| `fsdk_request_match`       |
| `PollMatch()`               | `fsdk_poll_match`          |
| `GetConnection(out)`        | `fsdk_get_connection`      |
| `CancelMatch()`             | `fsdk_cancel_match`        |

The client flow: `InitializeClient` -> `Authenticate` (with the player's own FID
token, passed in by game code) -> `RequestMatch` -> poll `PollMatch` until
`Found` -> `GetConnection`, then hand `{Ip, Port}` to UE netcode and forward
`MatchToken` to the dedicated server.

The dedicated-server path (Agones lifecycle + match-token validation) consumes
the same core but is a separate integration; this MVP focuses on the client
subsystem.

## Security

The player's FID token is **passed in** to `Authenticate` and never stored or
logged by the plugin. `ip:port` is treated as an **opaque rendezvous** (the box
today, a relay endpoint in future) so the relay hardening can land without code
changes. The full model is in [`../fsdk-core/SECURITY.md`](../fsdk-core/SECURITY.md).

## Layout

```
fsdk-unreal/
  README.md
  FoundryFSDK.uplugin                       # UE plugin descriptor
  Source/FoundryFSDK/
    FoundryFSDK.Build.cs                     # links ThirdParty/fsdk-core + include path
    Public/FoundryFSDKSubsystem.h            # the Blueprint-callable facade
    Private/FoundryFSDKSubsystem.cpp         # wraps the C ABI
    Private/FoundryFSDKModule.cpp            # IModuleInterface (default impl)
  ThirdParty/fsdk-core/README.md             # where vendored fsdk-core goes
  .gitignore
```

## Using the plugin

1. Drop `fsdk-unreal` into your project's `Plugins/` directory (or reference it
   as an engine plugin).
2. Vendor `fsdk-core` under `ThirdParty/fsdk-core` (submodule -> `../fsdk-core`)
   and build its static lib per platform. See
   [`ThirdParty/fsdk-core/README.md`](ThirdParty/fsdk-core/README.md).
3. Enable the **Foundry FSDK** plugin and regenerate project files.
4. From game code, get the subsystem:
   ```cpp
   auto* Fsdk = GetGameInstance()->GetSubsystem<UFoundryFSDKSubsystem>();
   Fsdk->InitializeClient(TEXT("https://api.foundryplatform.app"));
   Fsdk->Authenticate(PlayerFidToken); // token obtained via platform sign-in
   Fsdk->RequestMatch(TEXT("ranked-2v2"), TEXT(""));
   ```

## Multi-engine context

FSDK is multi-engine via the shared core: **Unreal C++ (this, MVP)**, Unity C#
(future, P/Invoke over the C ABI), Godot (future, GDExtension over the C++ core).
One implementation of the security-critical logic; engines are thin bindings.
