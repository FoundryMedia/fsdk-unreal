# Foundry FSDK — Unreal Engine

The **Unreal Engine** binding for the **Foundry SDK (FSDK)** — the engine-side
client for [Foundry Gaming Services](https://foundryplatform.app). It gives a
game **FID sign-in verification**, **FMMS matchmaking**, and a **connection
hand-off** to a matched game server, exposed to C++ and Blueprints through a
single `UGameInstanceSubsystem`.

It is a thin, idiomatic-UE wrapper over the shared **`fsdk-core`** C ABI — all
auth / matchmaking / token logic lives in the core (vendored into this module and
compiled from source by the engine toolchain). One implementation of the
security-critical logic; the engine binding is thin.

> **Status: beta / experimental.** The API surface is still moving. Pin a release
> tag rather than tracking `main`.

## Requirements

| | |
|---|---|
| **Engine** | **Unreal Engine 5.7, built from source.** This is the only configuration currently verified. Other versions are unsupported for now. |
| **Project** | A **C++** project. This is a code plugin — it compiles in your project; a Blueprint-only project won't build it. |
| **Account** | A [Foundry](https://foundryplatform.app) account. The SDK talks to the Foundry platform API; every call carries the **player's own FID session token**, which your game obtains through the platform's normal sign-in and passes in. |

The engine's `HTTP` module backs the network transport (it owns TLS). No CMake,
no prebuilt static library, no external dependencies — the vendored C core is
compiled in-module by UBT.

## Install

1. Copy this plugin into your project's `Plugins/` directory:
   `<YourProject>/Plugins/FoundryFSDK/`.
2. Right-click your `.uproject` → **Generate Visual Studio project files**.
3. Build the editor target (e.g. `<YourProject>Editor`, Win64, Development).
4. The **Foundry FSDK** plugin is enabled automatically; the
   `UFoundryFSDKSubsystem` is then available from any `GameInstance`.

## Quickstart

The client flow is: **InitializeClient → Authenticate → RequestMatch → poll until
Found → GetConnection**, then hand `{Ip, Port}` to your netcode and forward
`MatchToken` to the server.

The subsystem is **asynchronous**: each call runs off the game thread and
broadcasts an `On…Complete` delegate back on the game thread, so the game thread
never blocks on the network. Branch on the `EFoundryFsdkResult` each delegate
carries.

### Blueprint

1. Get the **Foundry FSDK** subsystem from the Game Instance.
2. Bind the events you care about: `OnAuthenticateComplete`,
   `OnRequestMatchComplete`, `OnPollMatchComplete`, `OnGetConnectionComplete`.
3. Call `InitializeClient` (base URL), then `Authenticate` (the player's FID
   token). On `OnAuthenticateComplete` = OK, call `RequestMatch`. Drive
   `PollMatch` on a timer; when `OnPollMatchComplete` reports `Found`, call
   `GetConnection`, then connect with the returned `Ip`/`Port`/`MatchToken`.

### C++

```cpp
auto* Fsdk = GetGameInstance()->GetSubsystem<UFoundryFSDKSubsystem>();

Fsdk->OnAuthenticateComplete.AddDynamic(this, &AMyController::HandleAuth);
Fsdk->OnPollMatchComplete.AddDynamic(this, &AMyController::HandlePoll);
Fsdk->OnGetConnectionComplete.AddDynamic(this, &AMyController::HandleConnection);

Fsdk->InitializeClient(TEXT("https://api.foundryplatform.app"));
Fsdk->Authenticate(PlayerFidToken); // obtained via the platform's sign-in

// HandleAuth: on EFoundryFsdkResult::Ok -> Fsdk->RequestMatch(TEXT("ranked-2v2"), TEXT(""));
// then poll on a timer; HandlePoll: on EFoundryMatchStatus::Found -> Fsdk->GetConnection();
// HandleConnection: hand Connection.Ip/Port to netcode, send Connection.MatchToken to the server.
```

### Blueprint API surface

| Subsystem call            | fsdk-core C ABI       | Completes on |
|---------------------------|-----------------------|--------------|
| `InitializeClient(BaseUrl)` | `fsdk_client_create` | (synchronous — no network) |
| `Authenticate(PlayerToken)` | `fsdk_authenticate`  | `OnAuthenticateComplete` |
| `RequestMatch(Queue, AttributesJson)` | `fsdk_request_match` | `OnRequestMatchComplete` |
| `PollMatch()`             | `fsdk_poll_match`     | `OnPollMatchComplete` |
| `GetConnection()`         | `fsdk_get_connection` | `OnGetConnectionComplete` |
| `CancelMatch()`           | `fsdk_cancel_match`   | (best-effort, fire-and-forget) |

> Some FMMS routes are still rolling out platform-side; calls against an
> unfinished route report a clean "not ready" result rather than failing hard.

## Security

The player's FID token is **passed in** to `Authenticate` by game code and is
never stored, persisted, or logged by the plugin. The client SDK holds **no
secrets** and is assumed fully reverse-engineered — all authorization is enforced
**server-side**, and the client only ever calls a minimal player-scoped endpoint
set (auth + matchmaking + receive-connection; never admin/operator paths). The
returned `Ip`/`Port` is an **opaque rendezvous** (the box today, a relay endpoint
in future) and is useless without the short-lived FID-signed `MatchToken`, which
the server validates before admitting the player. The full model is in
`fsdk-core/SECURITY.md`.

## Layout

```
fsdk-unreal/
  FoundryFSDK.uplugin                          # UE plugin descriptor
  Source/FoundryFSDK/
    FoundryFSDK.Build.cs                        # module rules; compiles the in-module C core
    Public/FoundryFSDKSubsystem.h              # the Blueprint-callable facade
    Private/FoundryFSDKSubsystem.cpp           # async wrapper over the C ABI
    Private/FoundryFSDKModule.cpp              # installs the transport + log sink at startup
    Private/FoundryFSDKTransport.{h,cpp}       # FHttpModule-backed fsdk_http_fn (TLS)
    Private/FsdkCore/                          # vendored fsdk-core CLIENT sources (compiled as C by UBT)
      include/foundry/fsdk.h                   #   the C ABI (source of truth)
      fsdk.c / transport.c / client.c          #   client-only — no server/token-verify code
  LICENSE                                       # Apache-2.0
```

The vendored `FsdkCore/` is a copy of the **client** translation units from the
`fsdk-core` repo — the server/Agones/token-verification code is deliberately
absent from the player binary. To refresh it, re-copy those files from `fsdk-core`
and keep the C ABI in lockstep with `fsdk-core/SECURITY.md`.

## Multi-engine context

FSDK is multi-engine via the shared core: **Unreal (this binding)**, with Unity
(C# P/Invoke) and Godot (GDExtension) bindings to follow over the same C ABI. The
security-critical logic is implemented once in the core; engine bindings are thin.

## License

[Apache-2.0](LICENSE). Copyright 2026 Foundry Media LLC.
