// Flux WaveRT de rendu : tampon cyclique, machine d'états KS, notifications.
// Structure inspirée de SYSVAD (minwavertstream.cpp ; licence MIT, Copyright (c)
// Microsoft Corporation), le transport étant ici le moteur isochrone USB.
#pragma once

#include "iso_engine.h"
#include "minwavert.h"

class CXonarWaveRtStream : public IMiniportWaveRTStreamNotification, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    CXonarWaveRtStream(_In_ CXonarWaveRtMiniport* Miniport, _In_ PPORTWAVERTSTREAM PortStream,
                       _In_ const xonar::wave::StreamFormat& Format);
    ~CXonarWaveRtStream() override;

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;

private:
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Transition(KSSTATE From, KSSTATE To);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AcquireHardware();
    _IRQL_requires_(PASSIVE_LEVEL) void ReleaseHardware();
    // Programmation de l'horloge (SetSampleRate puis contrôle GetSampleRate).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS ProgramClock();
    // Sélection de l'altsetting, contrôle des pipes et préparation du moteur.
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SelectAltAndPrepare();
    // PAUSE -> RUN : horloge reprogrammée et altsetting revérifié (reprise après veille).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS PrepareRun();
    // Altsetting courant de l'interface vu par la carte (requête standard GET_INTERFACE).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS QueryCurrentAltSetting(_Out_ UCHAR* AltSetting);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AllocateBufferCommon(ULONG NotificationCount, ULONG RequestedSize,
                                                                 _Out_ PMDL* AudioBufferMdl, _Out_ ULONG* ActualSize,
                                                                 _Out_ ULONG* OffsetFromFirstPage,
                                                                 _Out_ MEMORY_CACHING_TYPE* CacheType);
    _IRQL_requires_(PASSIVE_LEVEL) void FreeBufferCommon(_In_opt_ PMDL AudioBufferMdl);

    CXonarWaveRtMiniport* m_Miniport;
    PPORTWAVERTSTREAM m_PortStream; // non référencé, valide pendant toute la vie du flux (comme SYSVAD)
    xonar::wave::StreamFormat m_Format;
    KSSTATE m_State = KSSTATE_STOP;

    PMDL m_BufferMdl = nullptr;
    PUCHAR m_Buffer = nullptr;
    ULONG m_BufferSize = 0;
    ULONG m_NotificationCount = 0;

    bool m_AltSelected = false;
    XONAR_AS_PIPES m_Pipes = {};
    XonarIsoOutEngine m_Engine;
};
