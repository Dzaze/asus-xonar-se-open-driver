// Profil de traitement : description complète et sérialisable de ce que fait le
// pipeline. Aucune dépendance Windows. Les valeurs par défaut sont conservatrices.
#pragma once

#include "xonar/dsp/channels.hpp"

#include <array>
#include <cstdint>

namespace xonar::dsp {

enum class Mode : std::uint8_t {
    PassThrough,   // Entrée et sortie de même layout : canaux préservés tels quels.
    Upmix2to6,     // Stéréo vers 5.1, redistribution paramétrable.
    Downmix6to2,   // 5.1 vers stéréo pour un ampli 2.0 / 2.1 (coefficients ITU-R BS.775).
    TwoPointOne,   // 2.1 "virtuel" sur le câblage 5.1 : FL/FR + LFE dérivé, centre et surrounds muets.
    ChannelTest,   // Génère un signal de test sur un seul canal de sortie.
    Upmix6to6,     // 6->6 : préserve tout + étale FL/FR sur centre et surrounds (source stéréo mixée en 6 canaux).
    DownmixStereo6to6, // 6->6 : downmix stéréo ITU-R BS.775 vers FL/FR ; centre, LFE et surrounds muets (écoute 2.0 sur câblage 5.1).
    Upmix1to6      // Mono déclaré : paire frontale, puis dérivation optionnelle du reste.
};

struct EqBand {
    bool operator==(const EqBand&) const = default;
    bool enabled = false;
    float frequencyHz = 1000.f;
    float q = 1.f;
    float gainDb = 0.f;
};

constexpr std::size_t kEqBandsPerChannel = 10;

struct UpmixSettings {
    bool operator==(const UpmixSettings&) const = default;
    float centerGainDb = -3.f;      // Centre = (L+R)/2 puis ce gain.
    float surroundGainDb = -6.f;    // SL = L, SR = R avec ce gain.
    bool deriveLfe = true;          // LFE = passe-bas de (L+R)/2.
    float lfeGainDb = -6.f;
    float lfeLowPassHz = 120.f;
};

struct DownmixSettings {
    bool operator==(const DownmixSettings&) const = default;
    float centerGainDb = -3.f;      // ITU-R BS.775 : 0.707.
    float surroundGainDb = -3.f;    // ITU-R BS.775 : 0.707.
    bool includeLfe = false;        // Par défaut l'LFE est ignoré en 2.0 (l'ampli 2.1 fait son propre bass management).
    float lfeGainDb = -6.f;
    float headroomDb = -4.5f;       // Atténuation globale pour éviter la saturation de la somme.
};

// Mode 2.1 : Windows reste en 5.1, l'ampli reste en 5.1, et c'est le DSP qui
// fabrique un vrai 2.1 : avant gauche/droite intacts, caisson alimenté par un
// passe-bas de la somme, centre et surrounds silencieux. Une entrée 5.1 est
// d'abord ramenée en stéréo (coefficients de DownmixSettings).
struct TwoPointOneSettings {
    bool operator==(const TwoPointOneSettings&) const = default;
    float lfeGainDb = 0.f;          // Gain du LFE dérivé de (L+R)/2.
    float lfeLowPassHz = 80.f;      // Coupure du passe-bas LR4 alimentant le caisson.
    bool includeSourceLfe = true;   // Entrée 5.1 : ajoute le LFE d'origine au caisson.
    float sourceLfeGainDb = 0.f;
};

struct BassManagementSettings {
    bool enabled = false;           // Désactivé par défaut : l'ampli de Benoist a son propre réglage.
    float crossoverHz = 80.f;
    bool highPassSatellites = true; // Passe-haut LR4 sur les canaux principaux.
    bool sumToLfe = true;           // Somme passe-bas des satellites ajoutée au canal LFE.
    float sumToLfeGainDb = 0.f;
};

struct LimiterSettings {
    bool enabled = true;
    float ceilingDb = -0.3f;
    float releaseMs = 80.f;
};

struct ChannelTestSettings {
    Channel channel = Channel::FrontLeft;
    float frequencyHz = 440.f;
    float levelDb = -20.f;
};

struct Profile {
    Mode mode = Mode::PassThrough;
    std::uint32_t sampleRate = 48000;
    ChannelLayout input = ChannelLayout::fromMask(masks::kStereo);
    ChannelLayout output = ChannelLayout::fromMask(masks::kStereo);

    float masterGainDb = 0.f;
    // Index par valeur de Channel (pas par position dans la trame).
    std::array<float, kMaxChannels> channelGainDb{};
    std::array<float, kMaxChannels> channelDelayMs{};
    std::array<bool, kMaxChannels> channelMute{};
    std::array<std::array<EqBand, kEqBandsPerChannel>, kMaxChannels> eq{};

    UpmixSettings upmix{};
    DownmixSettings downmix{};
    TwoPointOneSettings twoPointOne{};
    BassManagementSettings bassManagement{};
    LimiterSettings limiter{};
    bool outputProtection = true; // uniquement à l'étage de sortie, pas avant mixage
    bool upmixHandledPerStream = false;
    bool autoDeclaredLayout = false;
    ChannelTestSettings test{};

    std::uint32_t gainRampMs = 20;  // Durée de rampe pour les changements de gain.
    float maxDelayMs = 50.f;        // Capacité maximale des lignes à retard.

    // Vrai si deux profils peuvent être échangés sans re-préparer le pipeline
    // (même fréquence, layouts et capacité de retard). Les quatre modes
    // d'écoute sur transport 5.1 partagent les mêmes tampons préalloués.
    bool isStructurallyCompatibleWith(const Profile& other) const noexcept {
        const auto packed51 = [](Mode value) {
            return value == Mode::PassThrough || value == Mode::Upmix6to6 ||
                   value == Mode::TwoPointOne || value == Mode::DownmixStereo6to6;
        };
        const bool compatibleMode = mode == other.mode ||
            (input.is5Point1() && input == output && packed51(mode) && packed51(other.mode));
        return compatibleMode && sampleRate == other.sampleRate &&
               input == other.input && output == other.output &&
               maxDelayMs == other.maxDelayMs;
    }
};

// Profils prêts à l'emploi.
Profile makePassThroughProfile(std::uint32_t sampleRate, std::uint32_t mask);
Profile makeUpmixProfile(std::uint32_t sampleRate, std::uint32_t outputMask = masks::k5Point1Side);
Profile makeDownmixProfile(std::uint32_t sampleRate, std::uint32_t inputMask = masks::k5Point1Side);
Profile makeTwoPointOneProfile(std::uint32_t sampleRate, std::uint32_t inputMask = masks::kStereo,
                               std::uint32_t outputMask = masks::k5Point1Side);
Profile makeChannelTestProfile(std::uint32_t sampleRate, std::uint32_t outputMask, Channel channel,
                               float frequencyHz = 440.f, float levelDb = -20.f);

} // namespace xonar::dsp
