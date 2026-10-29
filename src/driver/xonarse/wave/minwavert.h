// Miniport WaveRT de rendu de xonarse.sys, générique pour les trois endpoints
// (haut-parleurs 5.1, casque, S/PDIF), paramétré par XonarSeEndpointHw.
//
// Structure inspirée de l'échantillon SYSVAD de Microsoft (minwavert.cpp,
// speakerwavtable.h ; licence MIT, Copyright (c) Microsoft Corporation). Réécrit pour
// un périphérique USB : pas de DMA, le tampon cyclique est lu par le moteur isochrone.
#pragma once

#include "iso_engine.h" // inclut usb_device.h avant tout autre en-tête USB
#include "wave_formats.h"

// Une plage par combinaison (canaux, résolution, fréquence) réellement transportable :
// 3 configurations de canaux x 2 résolutions x 6 fréquences.
constexpr ULONG XONAR_WAVE_RATE_COUNT = ARRAYSIZE(xonar::hw::kCandidateRates);
constexpr ULONG XONAR_WAVE_MAX_RANGES = 3 * 2 * XONAR_WAVE_RATE_COUNT;

class CXonarWaveRtMiniport : public IMiniportWaveRT, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    CXonarWaveRtMiniport(_In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint);
    ~CXonarWaveRtMiniport() override;

    IMP_IMiniportWaveRT;

    XonarUsbDevice* Usb() const { return m_Usb; }
    const XONARSE_ENDPOINT_HW& Hw() const { return *m_Hw; }
    XONARSE_ENDPOINT Endpoint() const { return m_Endpoint; }
    ULONG StreamRangeCount() const { return m_StreamRangeCount; }

    // Appelé par le destructeur du flux.
    void OnStreamClosed();

    // KSPROPERTY_PIN_PROPOSEDATAFORMAT (filtre).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS PropertyProposedFormat(_In_ PPCPROPERTY_REQUEST Request);

private:
    void BuildDescriptors();

    XonarUsbDevice* m_Usb;
    XONARSE_ENDPOINT m_Endpoint;
    const XONARSE_ENDPOINT_HW* m_Hw;
    volatile LONG m_StreamOpen = 0;

    KSDATARANGE_AUDIO m_RangeStorage[XONAR_WAVE_MAX_RANGES] = {};
    PKSDATARANGE m_StreamRanges[XONAR_WAVE_MAX_RANGES] = {};
    ULONG m_StreamRangeCount = 0;
    PKSDATARANGE m_BridgeRanges[1] = {};
    PCPIN_DESCRIPTOR m_Pins[2] = {};
    PCCONNECTION_DESCRIPTOR m_Connections[1] = {};
    PCFILTER_DESCRIPTOR m_Filter = {};
};
