// Journalisation du pilote : chaque message part à la fois vers DbgPrintEx (WinDbg) et
// vers ETW en TraceLogging, fournisseur « XonarSeOpenDriver »
// {CF2EAE29-EDD2-4840-940F-41175898472F}. La capture ETW se fait dans la VM avec les
// outils natifs de Windows (logman / tracerpt), sans débogueur ni outil tiers :
// voir scripts/driver/vm/installer-xonarse.ps1 -Action log-start / log-stop.
//
// Règles d'usage :
// - Utilisable à IRQL <= DISPATCH_LEVEL.
// - À DISPATCH_LEVEL, pas de %ls / %ws / %wZ (conversion Unicode interdite à cet IRQL).
// - Messages courts : ils sont tronqués à 256 octets.
#pragma once

#include <ntddk.h>

// Niveaux : 1 erreur, 2 avertissement, 4 information (convention ETW).
extern "C" void XonarSeTraceRegister();
extern "C" void XonarSeTraceUnregister();
extern "C" void XonarSeTrace(ULONG Level, _Printf_format_string_ const char* Format, ...);

#define XONARSE_LOG(level, fmt, ...) XonarSeTrace((level), fmt, __VA_ARGS__)

#define XONARSE_INFO(fmt, ...)  XONARSE_LOG(4, fmt, __VA_ARGS__)
#define XONARSE_WARN(fmt, ...)  XONARSE_LOG(2, fmt, __VA_ARGS__)
#define XONARSE_ERROR(fmt, ...) XONARSE_LOG(1, fmt, __VA_ARGS__)
