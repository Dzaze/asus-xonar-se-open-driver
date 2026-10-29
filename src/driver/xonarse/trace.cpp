// Implémentation de la journalisation (voir trace.h). Les sources sont en UTF-8 : les
// messages sont donc émis en UTF-8 (TraceLoggingUtf8String) pour garder les accents.
#include <ntddk.h>
#include <ntstrsafe.h>
#include <TraceLoggingProvider.h>

#include "trace.h"

// Fournisseur TraceLogging. Le nom et le GUID sont publics : ils servent à la capture.
TRACELOGGING_DEFINE_PROVIDER(g_XonarSeProvider, "XonarSeOpenDriver",
                             (0xcf2eae29, 0xedd2, 0x4840, 0x94, 0x0f, 0x41, 0x17, 0x58, 0x98, 0x47, 0x2f));

static volatile LONG g_Registered = 0;

extern "C" void XonarSeTraceRegister()
{
    // PASSIVE_LEVEL, appelé depuis DriverEntry.
    if (NT_SUCCESS(TraceLoggingRegister(g_XonarSeProvider))) {
        InterlockedExchange(&g_Registered, 1);
    }
}

extern "C" void XonarSeTraceUnregister()
{
    if (InterlockedExchange(&g_Registered, 0) != 0) {
        TraceLoggingUnregister(g_XonarSeProvider);
    }
}

extern "C" void XonarSeTrace(ULONG Level, _Printf_format_string_ const char* Format, ...)
{
    char buffer[256];
    va_list args;
    va_start(args, Format);
    // Formatage borné ; un message trop long est simplement tronqué.
    (void)RtlStringCbVPrintfA(buffer, sizeof(buffer), Format, args);
    va_end(args);

    const ULONG dpfLevel = (Level <= 1) ? DPFLTR_ERROR_LEVEL : (Level == 2) ? DPFLTR_WARNING_LEVEL : DPFLTR_INFO_LEVEL;
    DbgPrintEx(DPFLTR_IHVAUDIO_ID, dpfLevel, "xonarse: %s\n", buffer);

    if (g_Registered != 0) {
        // Le niveau TraceLogging doit être une constante de compilation : un appel par niveau.
        if (Level <= 1) {
            TraceLoggingWrite(g_XonarSeProvider, "Log", TraceLoggingLevel(1),
                              TraceLoggingUInt32(Level, "Niveau"), TraceLoggingUtf8String(buffer, "Message"));
        } else if (Level == 2) {
            TraceLoggingWrite(g_XonarSeProvider, "Log", TraceLoggingLevel(2),
                              TraceLoggingUInt32(Level, "Niveau"), TraceLoggingUtf8String(buffer, "Message"));
        } else {
            TraceLoggingWrite(g_XonarSeProvider, "Log", TraceLoggingLevel(4),
                              TraceLoggingUInt32(Level, "Niveau"), TraceLoggingUtf8String(buffer, "Message"));
        }
    }
}
