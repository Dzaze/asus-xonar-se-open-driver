// SPDX-License-Identifier: Apache-2.0
// Réglages d'instance APO + construction du profil dsp-core + journal de cycle de
// vie. TOUT ce qui touche au disque (lecture de la configuration, journal) se fait
// hors de la callback temps réel APOProcess : au chargement et dans Lock/Unlock.
// Même le premier bloc n'effectue aucune E/S disque (contrat C7).
#pragma once

#include "xonar/dsp/profile.hpp"

#include <cstdint>
#include <string>

namespace xonar::apo {

// Étage de l'APO (contrat C2 : SFX par flux avant mixage, EFX après mixage).
enum class Stage : std::uint8_t { Sfx, Efx };

const char* stageName(Stage s) noexcept;

// Réglages du prototype lus depuis %ProgramData%\XonarSE\apo-<sfx|efx>.cfg (format key=value, une
// paire par ligne, '#' = commentaire). Fichier absent ou illisible -> valeurs par
// défaut = IDENTITÉ sûre (le son n'est pas modifié). Les gains/mutes sont indexés par
// la valeur de xonar::dsp::Channel (FL FR FC LFE BL BR SL SR), pas par position.
struct ApoSettings {
    bool enabled = true;                                   // false -> copie identité, DSP contourné
    float masterGainDb = 0.f;
    float channelGainDb[xonar::dsp::kMaxChannels] = {0, 0, 0, 0, 0, 0, 0, 0};
    bool channelMute[xonar::dsp::kMaxChannels] = {false, false, false, false, false, false, false, false};
    bool limiterEnabled = true;
    float limiterCeilingDb = -0.3f;
    bool bassEnabled = false;
    float crossoverHz = 80.f;
    // Politique SFX pour une entrée 2.0 : true -> Upmix2to6 ; false -> sera traité en
    // extension neutre (FL/FR conservés). N'affecte pas l'EFX.
    bool sfxUpmixStereo = false; // Auto explicite seulement, identité par défaut
    bool streamUpmix = false; // Upmix manuel délégué au LFX, calibration GFX unique
    // EFX uniquement : true -> mode Upmix6to6 (étale FL/FR d'un mix 6 canaux sur le centre
    // et les surrounds). N'affecte pas le SFX. Défaut false (PassThrough 6->6).
    // REPLI DE COMPAT : utilisé seulement si la clé 'mode' est absente.
    bool upmix = false;

    // ----------------------------------------------------------------------------------
    // Extensions du format .cfg (étage EFX). Défauts = comportement actuel (identité /
    // réglages dsp-core), pour qu'un fichier qui n'en contient aucune ne change rien.
    // ----------------------------------------------------------------------------------

    // Mode d'écoute (clé 'mode'), PRIORITAIRE sur 'upmix'. Unset -> repli sur 'upmix'.
    // Mapping EFX (sortie 5.1) : Surround51->PassThrough, Upmix->Upmix6to6,
    // TwoPointOne->TwoPointOne, Stereo20->DownmixStereo6to6.
    enum class EfxMode : std::uint8_t { Unset, Surround51, Upmix, TwoPointOne, Stereo20 };
    EfxMode mode = EfxMode::Unset;

    // Délai par canal en ms (0..50), indexé par valeur de xonar::dsp::Channel. 0 = aucun retard.
    float channelDelayMs[xonar::dsp::kMaxChannels] = {0, 0, 0, 0, 0, 0, 0, 0};

    // Égaliseur paramétrique : 10 bandes par canal (index canal = valeur de Channel ;
    // bande 0..9). Défaut = toutes désactivées (gain 0 dB) = identité.
    xonar::dsp::EqBand eq[xonar::dsp::kMaxChannels][xonar::dsp::kEqBandsPerChannel] = {};

    // Limiteur : temps de relâchement (ms). Défaut = valeur dsp-core.
    float limiterReleaseMs = 80.f;

    // Bass management (compléments de bassEnabled / crossoverHz). Défauts = dsp-core.
    bool bassHighPass = true;        // Passe-haut LR4 sur les satellites.
    bool bassSumToLfe = true;        // Somme passe-bas des satellites ajoutée au LFE.
    float bassSumToLfeGainDb = 0.f;

    // Sous-réglages de l'upmix (Routing.Upmix). Défauts = UpmixSettings de dsp-core.
    float upmixCenterDb = -3.f;
    float upmixSurroundDb = -6.f;
    float upmixLfeDb = -6.f;
    float upmixLfeHz = 120.f;
    bool upmixDeriveLfe = true;
};

// Charge les réglages de l'étage. Jamais d'exception ; échec silencieux -> défauts.
ApoSettings loadApoSettings(Stage stage) noexcept;

// Construit le profil dsp-core pour l'étage, à partir du format négocié et des
// réglages. Pour l'EFX : PassThrough 6->6 + compensation de sortie. Pour le SFX :
// Upmix2to6 (entrée stéréo) ou PassThrough (entrée 6 canaux) vers 6 canaux / 0x60F.
xonar::dsp::Profile buildProfile(Stage stage, std::uint32_t inputMask, std::uint32_t outputMask,
                                 std::uint32_t sampleRate, const ApoSettings& s) noexcept;

// Journal de cycle de vie (%ProgramData%\XonarSE\apo-log.txt ; repli OutputDebugString).
// À N'APPELER QUE hors callback audio (construct, init, négociation, lock/unlock).
void apoLog(Stage stage, int instanceId, const std::string& line) noexcept;
void apoTraceUnregister() noexcept;
// Acquittement du contrôle, écrit par le veilleur hors callback audio.
struct RuntimeMeters {
    float peaks[xonar::dsp::kMaxChannels]{};
    float preLimiterPeak = 0;
    float limiterReductionDb = 0;
    std::uint64_t clippedSamples = 0;
    bool enabled = true;
};
void writeApoStatus(Stage stage, int instanceId, std::uint64_t fileTime,
                    const xonar::dsp::Profile& profile, const float* inputRms,
                    const float* outputRms, const float* correlation = nullptr, bool active = true,
                    const RuntimeMeters& meters = {}) noexcept;

} // namespace xonar::apo
