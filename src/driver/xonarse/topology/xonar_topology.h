// Miniport Topology de xonarse.sys : un objet par endpoint (haut-parleurs 5.1, casque, S/PDIF).
//
// Filtre exposé (voir xonar_topology_tables.h) :
//   broche 0 (XONARSE_TOPO_PIN_BRIDGE_IN) : pont depuis le filtre WaveRT, analogique ;
//   broche 1 (XONARSE_TOPO_PIN_LINEOUT)   : connecteur physique (haut-parleurs, casque, S/PDIF) ;
//   nœuds : volume (KSNODETYPE_VOLUME) puis mute (KSNODETYPE_MUTE) ; S/PDIF : mute seul.
// Les contrôles sont ceux de la Feature Unit UAC2 de l'endpoint (FU12, FU13, FU14).
//
// Le modèle de classe (CUnknown + IMiniportTopology, gestionnaires de propriétés libres
// qui redirigent vers l'objet) suit l'échantillon SYSVAD de Microsoft
// (Windows-driver-samples, licence MIT, Copyright (c) Microsoft Corporation).
#pragma once

#include "../common.h"
#include "../usb/usb_device.h"

// Nombre maximal de canaux d'une Feature Unit de sortie (FU12 : 6 canaux).
constexpr ULONG XONARSE_TOPO_MAX_CHANNELS = 8;
// Identifiant de nœud absent.
constexpr ULONG XONARSE_TOPO_NO_NODE = ULONG(-1);

// Description d'un jack physique de la broche 1 (sans IsConnected, calculé à la volée).
struct XONAR_TOPO_JACK {
    DWORD ChannelMapping;      // masque KSAUDIO_SPEAKER_* porté par ce jack
    DWORD Color;               // 0x00RRGGBB
    EPcxConnectionType ConnectionType;
    EPcxGeoLocation GeoLocation;
    EPcxGenLocation GenLocation;
    EPxcPortConnection PortConnection;
};

// Description statique d'un filtre Topology pour un endpoint.
struct XONAR_TOPO_INFO {
    PCFILTER_DESCRIPTOR* Filter;
    const XONAR_TOPO_JACK* Jacks;
    ULONG JackCount;
    ULONG VolumeNode;          // XONARSE_TOPO_NO_NODE si la FU n'a pas de volume
    ULONG MuteNode;
    bool HasChannelConfig;     // KSPROPERTY_AUDIO_CHANNEL_CONFIG (haut-parleurs seulement)
};

// Gestionnaires référencés par les tables d'automatisation (redirigent vers l'objet).
NTSTATUS XonarTopoPropertyFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS XonarTopoPropertyVolume(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS XonarTopoPropertyMute(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS XonarTopoPropertyChannelConfig(_In_ PPCPROPERTY_REQUEST PropertyRequest);
NTSTATUS XonarTopoEventHandler(_In_ PPCEVENT_REQUEST EventRequest);

class CXonarMiniportTopology : public IMiniportTopology, public CUnknown {
public:
    DECLARE_STD_UNKNOWN();
    DEFINE_STD_CONSTRUCTOR(CXonarMiniportTopology);
    ~CXonarMiniportTopology();

    IMP_IMiniportTopology;

    // Associe l'objet à la couche USB et à l'endpoint (appelé une fois par la fabrique).
    void Bind(_In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint);

    // Gestionnaires appelés par les fonctions libres ci-dessus (PASSIVE_LEVEL).
    NTSTATUS HandleJackDescription(_In_ PPCPROPERTY_REQUEST PropertyRequest);
    NTSTATUS HandleJackDescription2(_In_ PPCPROPERTY_REQUEST PropertyRequest);
    NTSTATUS HandleVolume(_In_ PPCPROPERTY_REQUEST PropertyRequest);
    NTSTATUS HandleMute(_In_ PPCPROPERTY_REQUEST PropertyRequest);
    NTSTATUS HandleChannelConfig(_In_ PPCPROPERTY_REQUEST PropertyRequest);
    NTSTATUS HandleEvent(_In_ PPCEVENT_REQUEST EventRequest);

private:
    static void InterruptCallback(_In_opt_ PVOID Context, _In_ const XONAR_UAC2_INTERRUPT* Message);
    void OnInterrupt(_In_ const XONAR_UAC2_INTERRUPT* Message);

    void Lock();
    void Unlock();

    void LoadVolumeRanges();
    void RefreshVolume(ULONG KsChannel);   // relit un canal (verrou tenu)
    void RefreshMute();                    // relit le mute maître (verrou tenu)
    // Relit le connecteur ; retourne true si l'état a changé (verrou tenu).
    bool RefreshJackState(_In_z_ const char* Origin);

    LONG UacToKs(ULONG KsChannel, SHORT Value) const;
    SHORT KsToUac(ULONG KsChannel, LONG Value) const;
    BOOL JackIsConnected(ULONG JackIndex) const;
    bool HonorJackDetection() const;

    void GenerateNodeEvent(ULONG NodeId);
    void GeneratePinEvent(ULONG PinId);

    XonarUsbDevice* m_Usb = nullptr;
    XONARSE_ENDPOINT m_Endpoint = XonarEndpointSpeakers;
    const XONARSE_ENDPOINT_HW* m_Hw = nullptr;
    const XONAR_TOPO_INFO* m_Info = nullptr;
    PPORTEVENTS m_PortEvents = nullptr;
    bool m_CallbackRegistered = false;

    // Verrou des appels USB et du cache (PASSIVE_LEVEL, ne jamais le tenir pendant
    // UnregisterInterruptCallback).
    KMUTEX m_Lock;

    ULONG m_Channels = 0;
    XONAR_RANGE16 m_Range[XONARSE_TOPO_MAX_CHANNELS] = {};
    LONG m_VolumeKs[XONARSE_TOPO_MAX_CHANNELS] = {};   // cache, 1/65536 dB
    bool m_Muted = false;                              // cache du mute maître

    bool m_JackStateKnown = false;
    bool m_JackConnected = true;
    UCHAR m_JackNrChannels = 0;
    ULONG m_JackMask = 0;

    ULONG m_ChannelConfig = 0;
};
