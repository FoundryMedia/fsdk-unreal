// Copyright Foundry Media. fsdk-core <-> Unreal host bridges (HTTP + logging).
#pragma once

/**
 * fsdk-core is engine-agnostic and bakes in NO network stack or log sink; the
 * host installs them. These wire the core's transport seam
 * (fsdk_set_http_transport) to the engine's HTTP module - which owns TLS - and
 * the core's log sink (fsdk_set_log_sink) to UE_LOG. Installed once at module
 * startup; cleared at shutdown.
 */
void FoundryFSDKInstallHttpTransport();
void FoundryFSDKInstallLogSink();
void FoundryFSDKShutdownBridges();
