// Constantes matérielles de l'ASUS Xonar SE (USB VID 0B05, PID 189D, C-Media CM6620A).
// Source : descripteurs lus sur la carte le 27/10/2026, en lecture seule
// (src/hardware-protocol/captures/2026-10-27-xonar-se-usb-descriptors.txt).
// Toute valeur ici est CONFIRMÉE par cette capture, sauf mention [H] ou [T].
#pragma once

#include <ntddk.h>

namespace xonar::hw {

constexpr USHORT kVendorId = 0x0B05;
constexpr USHORT kProductId = 0x189D;

// --- Interfaces de la fonction audio (MI_00, association d'interfaces 0 à 5) ---
constexpr UCHAR kIfControl = 0;     // AudioControl UAC2
constexpr UCHAR kIfSpeakers = 1;    // AudioStreaming, terminal 1 -> haut-parleurs
constexpr UCHAR kIfHeadphones = 2;  // AudioStreaming, terminal 2 -> casque
constexpr UCHAR kIfSpdif = 3;       // AudioStreaming, terminal 3 -> S/PDIF
constexpr UCHAR kIfCapture1 = 4;    // AudioStreaming, terminal 10 <- sélecteur 20 (micro 1 / ligne / mixage)
constexpr UCHAR kIfCapture2 = 5;    // AudioStreaming, terminal 11 <- FU 18 (micro 2)

// --- Sources d'horloge (internes programmables, Frequency RW, Validity RO) ---
constexpr UCHAR kClockSpeakers = 21;
constexpr UCHAR kClockHeadphones = 22;
constexpr UCHAR kClockSpdif = 23;
constexpr UCHAR kClockCapture1 = 24;
constexpr UCHAR kClockCapture2 = 25;

// --- Terminaux d'entrée ---
constexpr UCHAR kItUsbSpeakers = 1;
constexpr UCHAR kItUsbHeadphones = 2;
constexpr UCHAR kItUsbSpdif = 3;
constexpr UCHAR kItMic1 = 4;        // Microphone, Connector RO
constexpr UCHAR kItLine = 5;        // Line connector, Connector RO
constexpr UCHAR kItMic2 = 6;        // Microphone, Connector RO

// --- Terminaux de sortie ---
constexpr UCHAR kOtSpeaker = 7;     // Speaker (0x0301), Connector RO, bits 14-15 bmControls non standard
constexpr UCHAR kOtHeadphones = 8;  // Headphones (0x0302), Connector RO
constexpr UCHAR kOtSpdif = 9;       // S/PDIF (0x0605), Connector RO
constexpr UCHAR kOtUsbCapture1 = 10;
constexpr UCHAR kOtUsbCapture2 = 11;

// --- Unités de fonction (Feature Units) ---
// FU12 : haut-parleurs. Maître : Mute RW, InputGainPad RW. Canaux 1 à 6 : Volume RW.
constexpr UCHAR kFuSpeakers = 12;
// FU13 : casque. Maître : Mute RW. Canaux 1 et 2 : Volume RW, InputGainPad RW.
constexpr UCHAR kFuHeadphones = 13;
// FU14 : S/PDIF. Maître : Mute RW. Pas de volume.
constexpr UCHAR kFuSpdif = 14;
constexpr UCHAR kFuMic1 = 15;       // Maître : Mute, AGC. Canaux : Volume.
constexpr UCHAR kFuLine = 16;       // Maître : Mute. Canaux : Volume.
constexpr UCHAR kFuMix = 17;        // Maître : Mute. Canaux : Volume.
constexpr UCHAR kFuMic2 = 18;       // Maître : Mute, AGC. Canaux : Volume.
constexpr UCHAR kMuCapture = 19;    // Mixer : entrées 4 (micro 1) et 5 (ligne)
constexpr UCHAR kSuCapture = 20;    // Selector RW : entrées 15, 16, 17

// Canal 0 = contrôle maître dans les requêtes UAC2 ; canaux 1..N = canaux logiques.
constexpr UCHAR kMasterChannel = 0;

// --- Endpoints ---
constexpr UCHAR kEpInterrupt = 0x8F;      // Interrupt IN, 6 octets, bInterval 4 (statut UAC2)
constexpr UCHAR kEpSpeakersOut = 0x05;    // Iso OUT asynchrone, bInterval 1 (chaque microtrame)
constexpr UCHAR kEpSpeakersFb = 0x85;     // Iso IN feedback, 4 octets, bInterval 4 (toutes les 8 microtrames)
constexpr UCHAR kEpHeadphonesOut = 0x07;
constexpr UCHAR kEpHeadphonesFb = 0x87;
constexpr UCHAR kEpSpdifOut = 0x06;
constexpr UCHAR kEpSpdifFb = 0x86;
constexpr UCHAR kEpCapture1In = 0x88;     // Iso IN asynchrone, pas de feedback
constexpr UCHAR kEpCapture2In = 0x89;
constexpr UCHAR kEpHid = 0x84;            // interface 6 (HID), hors de notre fonction MI_00

// --- Formats par altsetting ---
struct AltFormat {
    UCHAR Interface;
    UCHAR AltSetting;
    UCHAR Channels;
    ULONG ChannelMask;      // bmChannelConfig UAC2 = masque KSAUDIO_SPEAKER_* pour ces bits
    UCHAR SubslotBytes;     // octets par échantillon dans le paquet
    UCHAR BitResolution;
    USHORT MaxPacketBytes;  // wMaxPacketSize de l'endpoint de données
    UCHAR DataEndpoint;
    UCHAR FeedbackEndpoint; // 0 si aucun (capture)
    bool Iec61937;          // altsetting Type III (passthrough compressé)
};

// Les MaxPacketBytes correspondent à 26 trames par microtrame, soit jusqu'à 208 kHz :
// la carte transporte donc 192 kHz sur tous les formats (fréquences confirmées plus bas).
inline constexpr AltFormat kAltFormats[] = {
    // Haut-parleurs (interface 1)
    {kIfSpeakers, 1, 6, 0x60F, 2, 16, 312, kEpSpeakersOut, kEpSpeakersFb, false},
    {kIfSpeakers, 2, 6, 0x60F, 3, 24, 468, kEpSpeakersOut, kEpSpeakersFb, false},
    {kIfSpeakers, 3, 2, 0x003, 2, 16, 104, kEpSpeakersOut, kEpSpeakersFb, false},
    {kIfSpeakers, 4, 2, 0x003, 3, 24, 156, kEpSpeakersOut, kEpSpeakersFb, false},
    {kIfSpeakers, 5, 4, 0x033, 2, 16, 208, kEpSpeakersOut, kEpSpeakersFb, false},
    {kIfSpeakers, 6, 4, 0x033, 3, 24, 312, kEpSpeakersOut, kEpSpeakersFb, false},
    // Casque (interface 2)
    {kIfHeadphones, 1, 2, 0x003, 2, 16, 104, kEpHeadphonesOut, kEpHeadphonesFb, false},
    {kIfHeadphones, 2, 2, 0x003, 3, 24, 156, kEpHeadphonesOut, kEpHeadphonesFb, false},
    // S/PDIF (interface 3)
    {kIfSpdif, 1, 2, 0x003, 2, 16, 104, kEpSpdifOut, kEpSpdifFb, false},
    {kIfSpdif, 2, 2, 0x003, 3, 24, 156, kEpSpdifOut, kEpSpdifFb, false},
    {kIfSpdif, 3, 2, 0x003, 2, 16, 104, kEpSpdifOut, kEpSpdifFb, true},
    // Capture (interfaces 4 et 5)
    {kIfCapture1, 1, 2, 0x003, 2, 16, 104, kEpCapture1In, 0, false},
    {kIfCapture1, 2, 2, 0x003, 3, 24, 156, kEpCapture1In, 0, false},
    {kIfCapture2, 1, 2, 0x003, 2, 16, 104, kEpCapture2In, 0, false},
    {kIfCapture2, 2, 2, 0x003, 3, 24, 156, kEpCapture2In, 0, false},
};

// Recherche d'un altsetting PCM pour une interface, un nombre de canaux et une résolution.
inline const AltFormat* FindAltFormat(UCHAR iface, UCHAR channels, UCHAR bits) {
    for (const auto& f : kAltFormats) {
        if (f.Interface == iface && f.Channels == channels && f.BitResolution == bits && !f.Iec61937) return &f;
    }
    return nullptr;
}

// Fréquences supportées : CONFIRMÉES le 27/10/2026 par la requête RANGE sur les
// horloges 21, 22 et 23 (quatre valeurs discrètes, pas de 88,2 ni de 176,4 kHz).
inline constexpr ULONG kCandidateRates[] = {44100, 48000, 96000, 192000};
constexpr ULONG kDefaultRate = 48000;

} // namespace xonar::hw
