// Formats PCM du filtre WaveRT de rendu : plages de données, validation et construction
// des KSDATAFORMAT_WAVEFORMATEXTENSIBLE. Module wave/ uniquement.
//
// Règle : un format n'est accepté que s'il correspond exactement à un altsetting PCM
// existant de l'interface (nombre de canaux, résolution, sous-slot USB) et à une
// fréquence candidate (hw::kCandidateRates). Le 24 bits voyage sur 3 octets, comme
// sur le bus USB : aucune conversion n'est faite dans le chemin de complétion.
#pragma once

#include "../common.h"

namespace xonar::wave {

// Format de flux validé, prêt pour le moteur isochrone.
struct StreamFormat {
    const xonar::hw::AltFormat* Alt; // altsetting à sélectionner
    ULONG SampleRate;                // Hz
    ULONG ChannelMask;               // masque annoncé par le client (cohérent avec Alt)
    USHORT Channels;
    USHORT BitsPerSample;            // 16 ou 24 (conteneur = sous-slot USB)
    USHORT BlockAlign;               // octets par trame = canaux x sous-slot
};

// Vrai si Rate fait partie des fréquences exposées.
bool IsCandidateRate(ULONG Rate);

// Masques acceptés pour un nombre de canaux donné. AltMask = bmChannelConfig du
// descripteur USB ; les variantes « arrière » / « côté » de même cardinalité sont
// acceptées car l'ordre des échantillons dans la trame est identique.
bool IsChannelMaskAccepted(USHORT Channels, ULONG Mask, ULONG AltMask);

// Valide un KSDATAFORMAT (WAVEFORMATEX PCM stéréo ou WAVEFORMATEXTENSIBLE PCM) pour la
// broche de flux de l'endpoint. BufferSize = taille réellement lisible derrière Format.
// Retourne STATUS_SUCCESS (Out rempli) ou STATUS_NO_MATCH.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS ValidateFormat(_In_ const XONARSE_ENDPOINT_HW& Hw, _In_reads_bytes_(BufferSize) const KSDATAFORMAT* Format,
                        _In_ ULONG BufferSize, _Out_ StreamFormat* Out);

// Construit le KSDATAFORMAT_WAVEFORMATEXTENSIBLE d'un altsetting (canaux, bits) à Rate.
_IRQL_requires_(PASSIVE_LEVEL)
void BuildFormat(_In_ const xonar::hw::AltFormat& Alt, _In_ ULONG Rate,
                 _Out_ KSDATAFORMAT_WAVEFORMATEXTENSIBLE* Out);

// Format par défaut de l'endpoint : canaux maximaux, 24 bits, fréquence du registre
// (DefaultSampleRate) si elle est candidate, sinon 48000 Hz. Doit coïncider avec le
// format imposé par l'INF (6 canaux 24/24 bits 48 kHz 0x60F pour les haut-parleurs).
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS BuildDefaultFormat(_In_ const XONARSE_ENDPOINT_HW& Hw, _Out_ KSDATAFORMAT_WAVEFORMATEXTENSIBLE* Out);

} // namespace xonar::wave
