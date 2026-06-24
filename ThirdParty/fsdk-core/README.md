# ThirdParty/fsdk-core (unused — superseded)

**This directory is no longer used.** The original scaffold planned to link a
prebuilt `fsdk_core` static lib here. We instead **vendor the fsdk-core CLIENT
sources directly into the module** and let UBT compile them as C:

```
Source/FoundryFSDK/Private/FsdkCore/
  include/foundry/fsdk.h     # public C ABI header
  fsdk_internal.h
  fsdk.c                     # version / result strings / log sink / transport store
  transport.c                # builds the URL, dispatches to the host transport
  client.c                   # auth + matchmaking (the player-scoped conversation)
```

Why this instead of a prebuilt lib:

- **No CMake / no per-platform static lib build.** UBT (the engine toolchain)
  compiles the C in-module, so the plugin builds hermetically on any platform the
  engine targets.
- **The engine owns the network + TLS.** fsdk-core bakes in no HTTP stack; it
  exposes a transport seam (`fsdk_set_http_transport`). The module installs a
  transport backed by the engine's HTTP module (`FoundryFSDKTransport.cpp`).
- **Client-only.** Only the client translation units are vendored — the
  server/Agones/token-verify code is deliberately absent from the player binary.

`FoundryFSDK.Build.cs` adds `Private/FsdkCore/include` and `Private/FsdkCore` to the
include path. To refresh the vendored copy, re-copy those files from the
`fsdk-core` repo (`../fsdk-core`, sibling). Keep the C ABI — the security-critical
contract — in lockstep with `fsdk-core/SECURITY.md`.
