// Copyright Foundry Media. FoundryFSDKUI module's single log category.

#pragma once

#include "Logging/LogMacros.h"

/**
 * The ONE LogFoundryFSDKUI category for this whole module. Every file that logs under it includes
 * THIS header and nothing else declares it - a per-file DEFINE_LOG_CATEGORY_STATIC(LogFoundryFSDKUI,
 * ...) compiles fine with separate translation units (the macro's "static" gives each file its own
 * internal-linkage symbol), but a Unity build concatenates several of this module's .cpp files into
 * ONE translation unit, and five identically-named static variables in that one unit is a
 * redefinition error. The actual category is defined once, in FoundryFSDKUIModule.cpp (DEFINE_LOG_
 * CATEGORY(LogFoundryFSDKUI);), beside this module's IMPLEMENT_MODULE.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogFoundryFSDKUI, Log, All);
