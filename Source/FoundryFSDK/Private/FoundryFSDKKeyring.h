// Copyright Foundry Media. fsdk-core <-> OS keyring bridge (refresh-token persistence).
#pragma once

/**
 * Install the OS keyring backing for fsdk-core's secret-store seam
 * (fsdk_set_secret_store). The core persists ONLY the long-lived FID refresh token
 * through this seam so a player need not re-enter a password every launch; the
 * access token and password are never given to it.
 *
 * Windows = Credential Manager (CredWrite/Read/Delete). Other platforms = a
 * session-only no-op (login still works, just no resume). Installed once at module
 * startup; cleared at shutdown.
 */
void FoundryFSDKInstallSecretStore();
void FoundryFSDKShutdownSecretStore();
