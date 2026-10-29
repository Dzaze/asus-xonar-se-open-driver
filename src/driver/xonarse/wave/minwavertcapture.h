// Miniport et flux WaveRT de CAPTURE de xonarse.sys (endpoint micro, interface 4).
//
// Miroir du rendu (minwavert.*, minwavertstream.*) au sens inversé :
//   broche 0 (XONARSE_WAVE_CAPTURE_PIN_HOST)   : flux vers le moteur audio, KSPIN_DATAFLOW_OUT,
//            COMMUNICATION_SINK, KSCATEGORY_AUDIO, plages PCM exactes ;
//   broche 1 (XONARSE_WAVE_CAPTURE_PIN_BRIDGE) : pont depuis la topologie, KSPIN_DATAFLOW_IN,
//            COMMUNICATION_NONE, plage analogique ;
//   connexion directe broche 1 (pont) -> broche 0 (hôte).
// Le transport est le moteur isochrone IN (iso_in_engine.*). Le format PCM et l'altsetting
// sont validés par xonar::wave (wave_formats.*), partagés avec le rendu.
#pragma once

#include "iso_in_engine.h"
#include "wave_formats.h"

constexpr ULONG XONAR_CAP_RATE_COUNT = ARRAYSIZE(xonar::hw::kCandidateRates);
constexpr ULONG XONAR_CAP_MAX_RANGES = 2 * XONAR_CAP_RATE_COUNT; // 1 config canaux x 2 résolutions x fréquences

class CXonarWaveRtCaptureMiniport : public IMiniportWaveRT, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    CXonarWaveRtCaptureMiniport(_In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint);
    ~CXonarWaveRtCaptureMiniport() override;

    IMP_IMiniportWaveRT;

    XonarUsbDevice* Usb() const { return m_Usb; }
    const XONARSE_ENDPOINT_HW& Hw() const { return *m_Hw; }
    XONARSE_ENDPOINT Endpoint() const { return m_Endpoint; }
    ULONG StreamRangeCount() const { return m_StreamRangeCount; }

    void OnStreamClosed();
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS PropertyProposedFormat(_In_ PPCPROPERTY_REQUEST Request);

private:
    void BuildDescriptors();

    XonarUsbDevice* m_Usb;
    XONARSE_ENDPOINT m_Endpoint;
    const XONARSE_ENDPOINT_HW* m_Hw;
    volatile LONG m_StreamOpen = 0;

    KSDATARANGE_AUDIO m_RangeStorage[XONAR_CAP_MAX_RANGES] = {};
    PKSDATARANGE m_StreamRanges[XONAR_CAP_MAX_RANGES] = {};
    ULONG m_StreamRangeCount = 0;
    PKSDATARANGE m_BridgeRanges[1] = {};
    PCPIN_DESCRIPTOR m_Pins[2] = {};
    PCCONNECTION_DESCRIPTOR m_Connections[1] = {};
    PCFILTER_DESCRIPTOR m_Filter = {};
};

class CXonarWaveRtCaptureStream : public IMiniportWaveRTStreamNotification, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    CXonarWaveRtCaptureStream(_In_ CXonarWaveRtCaptureMiniport* Miniport, _In_ PPORTWAVERTSTREAM PortStream,
                              _In_ const xonar::wave::StreamFormat& Format);
    ~CXonarWaveRtCaptureStream() override;

    IMP_IMiniportWaveRTStream;
    IMP_IMiniportWaveRTStreamNotification;

private:
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Transition(KSSTATE From, KSSTATE To);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AcquireHardware();
    _IRQL_requires_(PASSIVE_LEVEL) void ReleaseHardware();
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS ProgramClock();
    // Sélecteur d'entrée UAC2, piloté par hw.SelectorId / hw.SelectorInput :
    // micro 1 -> SU20 entrée 1, ligne -> SU20 entrée 2 ; endpoint sans sélecteur
    // (SelectorId == 0, ex. micro 2) : sans effet. Non fatal : journalisé en cas d'échec.
    _IRQL_requires_(PASSIVE_LEVEL) void ProgramSelector();
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SelectAltAndPrepare();
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS PrepareRun();
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS QueryCurrentAltSetting(_Out_ UCHAR* AltSetting);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AllocateBufferCommon(ULONG NotificationCount, ULONG RequestedSize,
                                                                 _Out_ PMDL* AudioBufferMdl, _Out_ ULONG* ActualSize,
                                                                 _Out_ ULONG* OffsetFromFirstPage,
                                                                 _Out_ MEMORY_CACHING_TYPE* CacheType);
    _IRQL_requires_(PASSIVE_LEVEL) void FreeBufferCommon(_In_opt_ PMDL AudioBufferMdl);

    CXonarWaveRtCaptureMiniport* m_Miniport;
    PPORTWAVERTSTREAM m_PortStream;
    xonar::wave::StreamFormat m_Format;
    KSSTATE m_State = KSSTATE_STOP;

    PMDL m_BufferMdl = nullptr;
    PUCHAR m_Buffer = nullptr;
    ULONG m_BufferSize = 0;
    ULONG m_NotificationCount = 0;

    bool m_AltSelected = false;
    bool m_ifaceAcquired = false; // bit d'exclusion de l'interface physique partagee (micro 1 / ligne)
    XONAR_AS_PIPES m_Pipes = {};
    XonarIsoInEngine m_Engine;
};

// Fabrique appelée par CreateXonarWaveRtMiniport quand l'endpoint est un endpoint de capture.
NTSTATUS CreateXonarWaveRtCaptureMiniport(_Out_ PUNKNOWN* Unknown, _In_ XonarUsbDevice* Usb,
                                          _In_ XONARSE_ENDPOINT Endpoint);
