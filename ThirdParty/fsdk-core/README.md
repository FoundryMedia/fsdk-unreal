# ThirdParty/fsdk-core (placeholder)

This directory vendors the shared **fsdk-core** C ABI that the FoundryFSDK Unreal
module links against. It is a **placeholder** in the scaffold - no headers or
built binaries are committed here yet.

## How this is meant to be populated

`fsdk-core` is the source-of-truth repo at `../fsdk-core` (sibling of this repo).
Vendor it here as a **git submodule** so the Unreal plugin always builds against
a pinned core revision:

```sh
# from the fsdk-unreal repo root
git submodule add ../fsdk-core ThirdParty/fsdk-core/repo
```

Then expose its artifacts in the layout `FoundryFSDK.Build.cs` expects:

```
ThirdParty/fsdk-core/
  include/foundry/fsdk.h        # the public C ABI header
  lib/Win64/fsdk_core.lib       # static lib built from fsdk-core (per platform)
  lib/Linux/libfsdk_core.a
  lib/Mac/libfsdk_core.a
```

Practically:

1. Add the submodule (above), or copy `fsdk-core/include` here as `include/`.
2. Build `fsdk-core` per target platform with its CMake:
   ```sh
   cmake -S <fsdk-core> -B build -DCMAKE_POSITION_INDEPENDENT_CODE=ON
   cmake --build build --config Release
   ```
3. Drop the resulting static lib at `lib/<Platform>/` using the names above.

`FoundryFSDK.Build.cs` adds `include/` to the include path and links
`lib/<Platform>/...` when the file is present (it guards with `File.Exists`, so
the scaffold parses without the binaries).

## Why vendored, not system-installed

The Unreal plugin must build hermetically on the engine's toolchain across
platforms. Vendoring (submodule) pins an exact, reviewed core revision and keeps
the C ABI - the security-critical contract - in lockstep with the plugin.
