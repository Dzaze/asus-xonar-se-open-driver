// En-tête commun à tout le pilote xonarse.sys. CONTRAT PARTAGÉ entre les modules :
// ne modifier qu'en coordination (adaptateur, usb/, topology/, wave/).
//
// Conventions :
// - C++20 noyau : pas d'exceptions, pas de RTTI, pas de bibliothèque standard.
// - Allocation : new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) T(...) ; delete p.
//   (opérateurs définis dans new_delete.cpp, ExAllocatePool2 => mémoire mise à zéro).
// - Code paginable dans #pragma code_seg("PAGE") + PAGED_CODE() ; tout ce qui peut
//   tourner à DISPATCH_LEVEL (complétions USB, positions) reste non paginé.
// - Journal : XONARSE_INFO / WARN / ERROR (trace.h), filtre DPFLTR_IHVAUDIO_ID.
#pragma once

#define _NEW_DELETE_OPERATORS_ // on fournit nos propres new/delete (voir stdunk.h)
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>

#include "trace.h"
#include "hw_xonarse.h"

constexpr ULONG XONARSE_POOL_TAG = 'ESnX';

// --- Allocation noyau (new_delete.cpp ; operator delete(void*) vient de stdunk.lib) ---
PVOID __cdecl operator new(size_t size, POOL_FLAGS flags, ULONG tag);
PVOID __cdecl operator new[](size_t size, POOL_FLAGS flags, ULONG tag);
void __cdecl operator delete(PVOID p);
void __cdecl operator delete(PVOID p, size_t size);
void __cdecl operator delete[](PVOID p);
void __cdecl operator delete[](PVOID p, size_t size);

// --- Endpoints exposés à Windows ---
// Chaque endpoint = un filtre Wave (WaveRT) + un filtre Topology, reliés par
// PcRegisterPhysicalConnection(wave, XONARSE_WAVE_PIN_BRIDGE, topo, XONARSE_TOPO_PIN_BRIDGE_IN).
enum XONARSE_ENDPOINT : ULONG {
    XonarEndpointSpeakers = 0,   // interface 1, FU12, OT7, horloge 21, 5.1 (0x60F)
    XonarEndpointHeadphones = 1, // interface 2, FU13, OT8, horloge 22, stéréo
    XonarEndpointSpdif = 2,      // interface 3, FU14, OT9, horloge 23, stéréo (+ IEC 61937 plus tard)
    XonarEndpointCapture = 3,    // interface 4, FU15 (micro 1), IT4, horloge 24, stéréo, CAPTURE
    XonarEndpointLine = 4,       // interface 4, FU16 (ligne), IT5, horloge 24, SU20 entrée 2, CAPTURE.
                                 // Partage l'endpoint ISO IN 0x88 avec le micro 1 via le sélecteur SU20 :
                                 // micro 1 et ligne sont MUTUELLEMENT EXCLUSIFS (un seul flux à la fois ;
                                 // ouvrir la ligne bascule SU20 sur l'entrée 2).
    XonarEndpointMic2 = 5,       // interface 5 dédiée, FU18 (micro 2), IT6, horloge 25, endpoint ISO IN
                                 // 0x89 SÉPARÉ -> réellement indépendant du micro 1 et de la ligne.
    XonarEndpointCount = 6
};

// Broches des filtres (convention SYSVAD).
constexpr ULONG XONARSE_WAVE_PIN_SYSTEM = 0;    // broche de flux (KSPIN_DATAFLOW_IN, côté moteur audio)
constexpr ULONG XONARSE_WAVE_PIN_BRIDGE = 1;    // broche pont vers la topologie (KSPIN_DATAFLOW_OUT)
constexpr ULONG XONARSE_TOPO_PIN_BRIDGE_IN = 0; // broche pont depuis le filtre Wave (KSPIN_DATAFLOW_IN)
constexpr ULONG XONARSE_TOPO_PIN_LINEOUT = 1;   // connecteur physique (KSPIN_DATAFLOW_OUT, KSNODETYPE_SPEAKER, etc.)

// Capture : le flux va dans l'AUTRE sens (périphérique -> topologie -> wave -> moteur audio).
// Les INDEX de broches restent 0 = pont, 1 = connecteur ; seul le sens (DataFlow) s'inverse.
// La broche connecteur garde l'index 1 (= XONARSE_TOPO_PIN_LINEOUT) pour que le miniport
// Topology et ses gestionnaires de jack/volume/mute se réutilisent sans modification.
constexpr ULONG XONARSE_WAVE_CAPTURE_PIN_HOST = 0;   // vers le moteur audio (KSPIN_DATAFLOW_OUT, COMMUNICATION_SINK)
constexpr ULONG XONARSE_WAVE_CAPTURE_PIN_BRIDGE = 1; // depuis la topologie (KSPIN_DATAFLOW_IN, COMMUNICATION_NONE)
constexpr ULONG XONARSE_TOPO_CAPTURE_PIN_BRIDGE = 0; // vers le filtre Wave (KSPIN_DATAFLOW_OUT)
constexpr ULONG XONARSE_TOPO_CAPTURE_PIN_MIC = 1;    // connecteur physique (KSPIN_DATAFLOW_IN, KSNODETYPE_MICROPHONE)

// Noms des sous-périphériques : DOIVENT correspondre aux AddInterface de xonarse.inx.
inline const wchar_t* XonarSeWaveName(XONARSE_ENDPOINT e) {
    switch (e) {
    case XonarEndpointSpeakers: return L"WaveSpeaker";
    case XonarEndpointHeadphones: return L"WaveHeadphone";
    case XonarEndpointSpdif: return L"WaveSpdif";
    case XonarEndpointCapture: return L"WaveCapture";
    case XonarEndpointLine: return L"WaveLine";
    case XonarEndpointMic2: return L"WaveMic2";
    default: return L"WaveUnknown";
    }
}
inline const wchar_t* XonarSeTopologyName(XONARSE_ENDPOINT e) {
    switch (e) {
    case XonarEndpointSpeakers: return L"TopologySpeaker";
    case XonarEndpointHeadphones: return L"TopologyHeadphone";
    case XonarEndpointSpdif: return L"TopologySpdif";
    case XonarEndpointCapture: return L"TopologyCapture";
    case XonarEndpointLine: return L"TopologyLine";
    case XonarEndpointMic2: return L"TopologyMic2";
    default: return L"TopologyUnknown";
    }
}

// Description matérielle d'un endpoint, partagée par Topology et Wave.
struct XONARSE_ENDPOINT_HW {
    XONARSE_ENDPOINT Endpoint;
    UCHAR Interface;      // interface AudioStreaming
    UCHAR ClockId;        // source d'horloge à programmer
    UCHAR FeatureUnitId;  // unité de volume / mute
    UCHAR TerminalId;     // terminal de sortie (état du connecteur)
    UCHAR MaxChannels;
    ULONG ChannelMask;    // configuration physique par défaut
    bool HasChannelVolume;// volume par canal disponible sur la FU
    bool IsCapture;       // true : endpoint de capture (flux périphérique -> hôte, moteur ISO IN)
    UCHAR SelectorId = 0;    // sélecteur UAC2 (SU) à programmer à l'ouverture du flux ; 0 = aucun
    UCHAR SelectorInput = 0; // entrée du sélecteur à activer lorsque SelectorId != 0
};

inline const XONARSE_ENDPOINT_HW& XonarSeEndpointHw(XONARSE_ENDPOINT e) {
    using namespace xonar::hw;
    static const XONARSE_ENDPOINT_HW table[XonarEndpointCount] = {
        {XonarEndpointSpeakers, kIfSpeakers, kClockSpeakers, kFuSpeakers, kOtSpeaker, 6, 0x60F, true, false, 0, 0},
        {XonarEndpointHeadphones, kIfHeadphones, kClockHeadphones, kFuHeadphones, kOtHeadphones, 2, 0x003, true, false, 0, 0},
        {XonarEndpointSpdif, kIfSpdif, kClockSpdif, kFuSpdif, kOtSpdif, 2, 0x003, false, false, 0, 0},
        // Capture micro 1 : interface 4, horloge 24, FU15 (volume/mute), terminal IT4, sélecteur SU20 entrée 1.
        {XonarEndpointCapture, kIfCapture1, kClockCapture1, kFuMic1, kItMic1, 2, 0x003, true, true, kSuCapture, 1},
        // Capture ligne : MÊME interface 4 et MÊME horloge 24 que le micro 1, FU16, terminal IT5, sélecteur
        // SU20 entrée 2. L'endpoint ISO IN 0x88 étant partagé via SU20, micro 1 et ligne sont mutuellement
        // exclusifs (cf. XonarEndpointLine).
        {XonarEndpointLine, kIfCapture1, kClockCapture1, kFuLine, kItLine, 2, 0x003, true, true, kSuCapture, 2},
        // Capture micro 2 : interface 5 dédiée, horloge 25, FU18, terminal IT6, AUCUN sélecteur
        // (endpoint ISO IN 0x89 séparé) -> indépendant du micro 1 et de la ligne.
        {XonarEndpointMic2, kIfCapture2, kClockCapture2, kFuMic2, kItMic2, 2, 0x003, true, true, 0, 0},
    };
    return table[e < XonarEndpointCount ? e : 0];
}

// --- Réglages lus dans le registre du périphérique (clé du pilote, sous-clé "Settings") ---
// Lus une fois par l'adaptateur au démarrage ; accessibles en lecture partout.
struct XONARSE_SETTINGS {
    // 0 (défaut) : les endpoints sont TOUJOURS signalés connectés à Windows, l'état
    // réel des jacks est seulement journalisé. Raison : le pilote ASUS rendait les
    // haut-parleurs « Non connecté » en suivant la détection de jack (incident
    // d'origine). 1 : l'état IsConnected de KSJACK_DESCRIPTION suit le connecteur.
    ULONG HonorJackDetection;
    ULONG DefaultSampleRate; // 48000 par défaut
};
const XONARSE_SETTINGS& XonarSeSettings();

// --- Couche USB (usb/) ---
class XonarUsbDevice;

// --- Fabriques des miniports, appelées par adapter.cpp ---
// Chacune retourne un objet COM (référence +1) qui répond à QueryInterface pour
// IID_IMiniportTopology (resp. IID_IMiniportWaveRT) et IID_IUnknown.
// Le miniport fait AddRef sur Usb et le relâche à sa destruction.
// Retourner STATUS_NOT_IMPLEMENTED fait ignorer l'endpoint par l'adaptateur.
NTSTATUS CreateXonarTopologyMiniport(_Out_ PUNKNOWN* Unknown, _In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint);
NTSTATUS CreateXonarWaveRtMiniport(_Out_ PUNKNOWN* Unknown, _In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint);
// Variante capture (moteur ISO IN) : appelée par CreateXonarWaveRtMiniport si IsCapture.
NTSTATUS CreateXonarWaveRtCaptureMiniport(_Out_ PUNKNOWN* Unknown, _In_ XonarUsbDevice* Usb,
                                          _In_ XONARSE_ENDPOINT Endpoint);
