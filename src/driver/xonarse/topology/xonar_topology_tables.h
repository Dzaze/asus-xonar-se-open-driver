// Descripteurs statiques des filtres Topology de xonarse.sys (inclus par xonar_topology.cpp seulement).
//
// Structure des tables reprise de l'échantillon SYSVAD (speakertoptable.h,
// speakerhptoptable.h ; Windows-driver-samples, licence MIT, Copyright (c) Microsoft
// Corporation), adaptée : nœuds volume et mute ajoutés, une table par endpoint.
//
// Graphe de chaque filtre :
//   haut-parleurs, casque : broche 0 -> volume (nœud 0) -> mute (nœud 1) -> broche 1
//   S/PDIF                : broche 0 -> mute (nœud 0) -> broche 1 (FU14 n'a pas de volume)
#pragma once

#include "xonar_topology.h"

namespace xonar::topo {

// Nœuds des filtres avec volume.
constexpr ULONG kNodeVolume = 0;
constexpr ULONG kNodeMuteAfterVolume = 1;
// Nœud du filtre S/PDIF.
constexpr ULONG kNodeMuteOnly = 0;

// --- Plages de données : les deux broches transportent un signal analogique (convention
// des filtres Topology : pas de flux réel, la broche pont est reliée au filtre WaveRT).
static KSDATARANGE PinDataRangesAnalog[] = {
    {
        sizeof(KSDATARANGE),
        0,
        0,
        0,
        STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
        STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
        STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE),
    },
};

static PKSDATARANGE PinDataRangePointersAnalog[] = {
    &PinDataRangesAnalog[0],
};

// Broche 1 : seule la catégorie diffère d'un endpoint à l'autre.
#define XONAR_TOPO_PINS(Name, LineOutCategory)                                                   \
    static PCPIN_DESCRIPTOR Name[] = {                                                           \
        {                                                                                        \
            0, 0, 0, nullptr,                                                                    \
            {                                                                                    \
                0, nullptr, 0, nullptr,                                                          \
                SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,            \
                KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, {0}     \
            }                                                                                    \
        },                                                                                       \
        {                                                                                        \
            0, 0, 0, nullptr,                                                                    \
            {                                                                                    \
                0, nullptr, 0, nullptr,                                                          \
                SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,            \
                KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &(LineOutCategory), nullptr, {0}   \
            }                                                                                    \
        },                                                                                       \
    }

XONAR_TOPO_PINS(PinsSpeakers, KSNODETYPE_SPEAKER);
XONAR_TOPO_PINS(PinsHeadphones, KSNODETYPE_HEADPHONES);
XONAR_TOPO_PINS(PinsSpdif, KSNODETYPE_SPDIF_INTERFACE);

#undef XONAR_TOPO_PINS

// --- Événements ---
// Changement de contrôle (volume ou mute modifié par la carte, signalé par l'endpoint
// d'interruption 0x8F).
static PCEVENT_ITEM EventsControlChange[] = {
    {
        &KSEVENTSETID_AudioControlChange,
        KSEVENT_CONTROL_CHANGE,
        KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
        XonarTopoEventHandler,
    },
};

// Changement d'état d'un jack (généré seulement si HonorJackDetection = 1).
static PCEVENT_ITEM EventsJackInfoChange[] = {
    {
        &KSEVENTSETID_PinCapsChange,
        KSEVENT_PINCAPS_JACKINFOCHANGE,
        KSEVENT_TYPE_ENABLE | KSEVENT_TYPE_BASICSUPPORT,
        XonarTopoEventHandler,
    },
};

// --- Propriétés des nœuds ---
static PCPROPERTY_ITEM PropertiesVolume[] = {
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyVolume,
    },
};

// Haut-parleurs : la configuration des canaux est aussi acceptée sur le nœud de volume
// (DirectSound l'interroge comme propriété du nœud « pan »).
static PCPROPERTY_ITEM PropertiesVolumeSpeakers[] = {
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_VOLUMELEVEL,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyVolume,
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CHANNEL_CONFIG,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyChannelConfig,
    },
};

static PCPROPERTY_ITEM PropertiesMute[] = {
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_MUTE,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyMute,
    },
};

DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationVolume, PropertiesVolume, EventsControlChange);
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationVolumeSpeakers, PropertiesVolumeSpeakers, EventsControlChange);
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationMute, PropertiesMute, EventsControlChange);

// --- Propriétés du filtre (jacks de la broche 1, configuration des canaux) ---
static PCPROPERTY_ITEM PropertiesFilter[] = {
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyFilter,
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyFilter,
    },
};

static PCPROPERTY_ITEM PropertiesFilterSpeakers[] = {
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyFilter,
    },
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION2,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyFilter,
    },
    {
        &KSPROPSETID_Audio,
        KSPROPERTY_AUDIO_CHANNEL_CONFIG,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        XonarTopoPropertyChannelConfig,
    },
};

DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationFilter, PropertiesFilter, EventsJackInfoChange);
DEFINE_PCAUTOMATION_TABLE_PROP_EVENT(AutomationFilterSpeakers, PropertiesFilterSpeakers, EventsJackInfoChange);

// --- Nœuds ---
static PCNODE_DESCRIPTOR NodesSpeakers[] = {
    {0, &AutomationVolumeSpeakers, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME},
    {0, &AutomationMute, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE},
};

static PCNODE_DESCRIPTOR NodesHeadphones[] = {
    {0, &AutomationVolume, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME},
    {0, &AutomationMute, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE},
};

static PCNODE_DESCRIPTOR NodesSpdif[] = {
    {0, &AutomationMute, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE},
};

// --- Connexions (les nœuds entrent par KSNODEPIN_STANDARD_IN et sortent par _OUT) ---
static PCCONNECTION_DESCRIPTOR ConnectionsVolumeMute[] = {
    {PCFILTER_NODE, XONARSE_TOPO_PIN_BRIDGE_IN, kNodeVolume, KSNODEPIN_STANDARD_IN},
    {kNodeVolume, KSNODEPIN_STANDARD_OUT, kNodeMuteAfterVolume, KSNODEPIN_STANDARD_IN},
    {kNodeMuteAfterVolume, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, XONARSE_TOPO_PIN_LINEOUT},
};

static PCCONNECTION_DESCRIPTOR ConnectionsMuteOnly[] = {
    {PCFILTER_NODE, XONARSE_TOPO_PIN_BRIDGE_IN, kNodeMuteOnly, KSNODEPIN_STANDARD_IN},
    {kNodeMuteOnly, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, XONARSE_TOPO_PIN_LINEOUT},
};

// --- Filtres ---
static PCFILTER_DESCRIPTOR FilterSpeakers = {
    0,                                    // Version
    &AutomationFilterSpeakers,            // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),             // PinSize
    SIZEOF_ARRAY(PinsSpeakers),           // PinCount
    PinsSpeakers,                         // Pins
    sizeof(PCNODE_DESCRIPTOR),            // NodeSize
    SIZEOF_ARRAY(NodesSpeakers),          // NodeCount
    NodesSpeakers,                        // Nodes
    SIZEOF_ARRAY(ConnectionsVolumeMute),  // ConnectionCount
    ConnectionsVolumeMute,                // Connections
    0,                                    // CategoryCount
    nullptr,                              // Categories (fournies par le port Topology)
};

static PCFILTER_DESCRIPTOR FilterHeadphones = {
    0,
    &AutomationFilter,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(PinsHeadphones),
    PinsHeadphones,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(NodesHeadphones),
    NodesHeadphones,
    SIZEOF_ARRAY(ConnectionsVolumeMute),
    ConnectionsVolumeMute,
    0,
    nullptr,
};

static PCFILTER_DESCRIPTOR FilterSpdif = {
    0,
    &AutomationFilter,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(PinsSpdif),
    PinsSpdif,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(NodesSpdif),
    NodesSpdif,
    SIZEOF_ARRAY(ConnectionsMuteOnly),
    ConnectionsMuteOnly,
    0,
    nullptr,
};

// --- Jacks de la broche 1 ---
// Haut-parleurs : trois jacks 3,5 mm à l'arrière de la carte, sur le terminal OT7.
// [H] Couleurs : convention habituelle des cartes 5.1 (vert avant, orange centre/caisson,
// noir surround), à confirmer sur la carte physique.
// ChannelMapping : 0x3 = FL|FR, 0xC = FC|LFE, 0x600 = SL|SR (total 0x60F = KSAUDIO_SPEAKER_5POINT1_SURROUND).
static const XONAR_TOPO_JACK JacksSpeakers[] = {
    {SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0x0000FF00, eConnType3Point5mm, eGeoLocRear, eGenLocPrimaryBox,
     ePortConnJack},
    {SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY, 0x00FFA500, eConnType3Point5mm, eGeoLocRear, eGenLocPrimaryBox,
     ePortConnJack},
    {SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT, 0x00000000, eConnType3Point5mm, eGeoLocRear, eGenLocPrimaryBox,
     ePortConnJack},
};

// Casque : un jack 3,5 mm stéréo sur le terminal OT8.
// [H] eGeoLocFront : connecteur de façade (en-tête front panel) probable, à confirmer.
// [H] Couleur inconnue : 0 (non précisée).
static const XONAR_TOPO_JACK JacksHeadphones[] = {
    {SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0x00000000, eConnType3Point5mm, eGeoLocFront, eGenLocPrimaryBox,
     ePortConnJack},
};

// S/PDIF : sortie optique (Toslink) à l'arrière, terminal OT9.
static const XONAR_TOPO_JACK JacksSpdif[] = {
    {SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0x00000000, eConnTypeOptical, eGeoLocRear, eGenLocPrimaryBox,
     ePortConnJack},
};

// --- Capture micro (interface 4, FU15, terminal d'entrée IT4) ---
// Flux INVERSÉ : broche 0 = pont vers le filtre Wave (SORTIE), broche 1 = connecteur micro
// (ENTRÉE). Seul le sens des broches change ; nœuds volume/mute et gestionnaires identiques.
static PCPIN_DESCRIPTOR PinsCapture[] = {
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, {0}
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, nullptr, {0}
        }
    },
};

static PCNODE_DESCRIPTOR NodesCapture[] = {
    {0, &AutomationVolume, &KSNODETYPE_VOLUME, &KSAUDFNAME_MASTER_VOLUME},
    {0, &AutomationMute, &KSNODETYPE_MUTE, &KSAUDFNAME_MASTER_MUTE},
};

// Micro (broche 1) -> volume (nœud 0) -> mute (nœud 1) -> pont (broche 0).
static PCCONNECTION_DESCRIPTOR ConnectionsCapture[] = {
    {PCFILTER_NODE, XONARSE_TOPO_CAPTURE_PIN_MIC, kNodeVolume, KSNODEPIN_STANDARD_IN},
    {kNodeVolume, KSNODEPIN_STANDARD_OUT, kNodeMuteAfterVolume, KSNODEPIN_STANDARD_IN},
    {kNodeMuteAfterVolume, KSNODEPIN_STANDARD_OUT, PCFILTER_NODE, XONARSE_TOPO_CAPTURE_PIN_BRIDGE},
};

static PCFILTER_DESCRIPTOR FilterCapture = {
    0,
    &AutomationFilter,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(PinsCapture),
    PinsCapture,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(NodesCapture),
    NodesCapture,
    SIZEOF_ARRAY(ConnectionsCapture),
    ConnectionsCapture,
    0,
    nullptr,
};

// Micro : un jack 3,5 mm (terminal d'entrée IT4).
// [H] Emplacement (arrière) et couleur (rose, convention micro) à confirmer sur la carte.
static const XONAR_TOPO_JACK JacksCapture[] = {
    {SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0x00FFC0CB, eConnType3Point5mm, eGeoLocRear, eGenLocPrimaryBox,
     ePortConnJack},
};

// --- Entrée ligne (interface 4, FU16, terminal d'entrée IT5) ---
// MIROIR EXACT du micro 1 : mêmes nœuds (volume = nœud 0, mute = nœud 1) et mêmes connexions
// (NodesCapture / ConnectionsCapture réutilisés tels quels). Seuls changent le connecteur
// (ligne au lieu de micro) et le jack.
// LIMITE MATÉRIELLE : l'interface 4 n'expose qu'UN endpoint ISO IN (0x88), routé par le sélecteur
// SU20 entre micro 1 (entrée 1), ligne (entrée 2) et mixage (entrée 3). Micro 1 et ligne sont donc
// MUTUELLEMENT EXCLUSIFS : un seul flux de capture à la fois sur l'interface 4 (ouvrir la ligne
// bascule SU20 sur l'entrée 2). Le pilote n'essaie pas de les faire coexister.
static PCPIN_DESCRIPTOR PinsLine[] = {
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, {0}
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_LINE_CONNECTOR, nullptr, {0}
        }
    },
};

static PCFILTER_DESCRIPTOR FilterLine = {
    0,
    &AutomationFilter,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(PinsLine),
    PinsLine,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(NodesCapture),  // mêmes nœuds volume/mute que le micro 1
    NodesCapture,
    SIZEOF_ARRAY(ConnectionsCapture),  // mêmes connexions capture que le micro 1
    ConnectionsCapture,
    0,
    nullptr,
};

// Ligne : un jack 3,5 mm (terminal d'entrée IT5).
// [H] Emplacement (arrière) et couleur (bleu, convention entrée ligne) à confirmer sur la carte.
static const XONAR_TOPO_JACK JacksLine[] = {
    {SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0x000099FF, eConnType3Point5mm, eGeoLocRear, eGenLocPrimaryBox,
     ePortConnJack},
};

// --- Micro 2 (interface 5 dédiée, FU18, terminal d'entrée IT6) ---
// MIROIR EXACT du micro 1 (mêmes nœuds et connexions capture), connecteur micro. Contrairement à la
// ligne, l'interface 5 possède son propre endpoint ISO IN (0x89) : le micro 2 est RÉELLEMENT
// INDÉPENDANT du micro 1 et de la ligne (il peut capturer en parallèle).
static PCPIN_DESCRIPTOR PinsMic2[] = {
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,
            KSPIN_DATAFLOW_OUT, KSPIN_COMMUNICATION_NONE, &KSCATEGORY_AUDIO, nullptr, {0}
        }
    },
    {
        0, 0, 0, nullptr,
        {
            0, nullptr, 0, nullptr,
            SIZEOF_ARRAY(PinDataRangePointersAnalog), PinDataRangePointersAnalog,
            KSPIN_DATAFLOW_IN, KSPIN_COMMUNICATION_NONE, &KSNODETYPE_MICROPHONE, nullptr, {0}
        }
    },
};

static PCFILTER_DESCRIPTOR FilterMic2 = {
    0,
    &AutomationFilter,
    sizeof(PCPIN_DESCRIPTOR),
    SIZEOF_ARRAY(PinsMic2),
    PinsMic2,
    sizeof(PCNODE_DESCRIPTOR),
    SIZEOF_ARRAY(NodesCapture),  // mêmes nœuds volume/mute que le micro 1
    NodesCapture,
    SIZEOF_ARRAY(ConnectionsCapture),  // mêmes connexions capture que le micro 1
    ConnectionsCapture,
    0,
    nullptr,
};

// Micro 2 : un jack 3,5 mm (terminal d'entrée IT6).
// [H] Emplacement (façade probable) et couleur (rose, convention micro) à confirmer sur la carte.
static const XONAR_TOPO_JACK JacksMic2[] = {
    {SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT, 0x00FFC0CB, eConnType3Point5mm, eGeoLocFront, eGenLocPrimaryBox,
     ePortConnJack},
};

// --- Table par endpoint (indexée par XONARSE_ENDPOINT) ---
static const XONAR_TOPO_INFO TopoInfo[XonarEndpointCount] = {
    {&FilterSpeakers, JacksSpeakers, SIZEOF_ARRAY(JacksSpeakers), kNodeVolume, kNodeMuteAfterVolume, true},
    {&FilterHeadphones, JacksHeadphones, SIZEOF_ARRAY(JacksHeadphones), kNodeVolume, kNodeMuteAfterVolume, false},
    {&FilterSpdif, JacksSpdif, SIZEOF_ARRAY(JacksSpdif), XONARSE_TOPO_NO_NODE, kNodeMuteOnly, false},
    {&FilterCapture, JacksCapture, SIZEOF_ARRAY(JacksCapture), kNodeVolume, kNodeMuteAfterVolume, false},
    {&FilterLine, JacksLine, SIZEOF_ARRAY(JacksLine), kNodeVolume, kNodeMuteAfterVolume, false},
    {&FilterMic2, JacksMic2, SIZEOF_ARRAY(JacksMic2), kNodeVolume, kNodeMuteAfterVolume, false},
};

} // namespace xonar::topo
