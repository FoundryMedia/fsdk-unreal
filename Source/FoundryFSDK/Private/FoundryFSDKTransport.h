// Copyright Foundry Media. fsdk-core <-> Unreal host bridges (HTTP + WS + logging).
#pragma once

#include "Containers/UnrealString.h"
#include "Templates/Function.h"

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

/**
 * Chain an ADDITIONAL observer onto the module-owned log sink, without ever
 * replacing it (fsdk_set_log_sink is process-wide and "set once at startup" -
 * FoundryFSDKInstallLogSink owns it for the module's whole lifetime). Every
 * fsdk-core log line still reaches UE_LOG (LogFoundryFSDKCore) exactly as
 * before; when set, OnLog ALSO receives (level, message) for every line, on
 * WHATEVER THREAD the core logged from (it may be a worker already holding
 * another lock - keep this callback itself lock-free and dispatch elsewhere).
 * Pass an empty TFunction (or call FoundryFSDKClearAuxLogSink) to detach.
 * First consumer: UFoundryFSDKSubsystem::ConfigureTextChat's
 * bRouteSdkLogToSystem (fsdk_textchat.h "route WARN+ diagnostics into the
 * System channel" - textchat.c's fsdk_textchat_log_sink needs the core lock,
 * so the subscriber dispatches its own worker rather than calling it here).
 */
void FoundryFSDKSetAuxLogSink(TFunction<void(int32 Level, const FString& Message)> OnLog);
void FoundryFSDKClearAuxLogSink();

/**
 * WS bridge (the fsdk_set_ws_transport seam - FRC chat): the core asks us to
 * open a wss socket; we back it with the engine's WebSockets module. ALL
 * IWebSocket interaction happens on the GAME thread (creation, connect, sends,
 * events); the core may call the seam from a worker (fsdk_chat_join_global runs
 * its blocking room-resolve off-thread), so connect returns a wrapper handle
 * immediately and marshals the real socket work over, with outbound frames
 * queued in order until the socket opens.
 *
 * SINGLE-SESSION delivery (one chat session per process - Conquest's global
 * room): the subsystem registers a sink BEFORE joining; every inbound TEXT
 * frame / close lands there on the game thread. The subsystem queues them and
 * feeds fsdk_chat_on_ws_text / on_ws_closed from its own driver (the chat
 * handle is single-threaded by contract). Frames arriving before the sink
 * exists are buffered in order and flushed on registration.
 */
void FoundryFSDKInstallWsTransport();
void FoundryFSDKSetWsSink(TFunction<void(const FString&)> OnText, TFunction<void()> OnClosed);
void FoundryFSDKClearWsSink();
