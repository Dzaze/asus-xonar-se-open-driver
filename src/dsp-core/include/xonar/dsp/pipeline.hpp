// Pipeline de traitement complet :
//   entrée -> matrice de routage (mode) -> bass management -> EQ -> délais
//          -> gains par canal (lissés) -> gain maître -> limiteur -> sortie
//
// Contrat temps réel : après prepare(), process() n'alloue pas, ne verrouille pas
// et ne fait aucune E/S. setProfile() change les paramètres et les modes d'écoute
// partageant le transport 6->6 ; un changement de format/capacité exige prepare().
#pragma once

#include "xonar/dsp/biquad.hpp"
#include "xonar/dsp/buffer.hpp"
#include "xonar/dsp/delay_line.hpp"
#include "xonar/dsp/limiter.hpp"
#include "xonar/dsp/profile.hpp"
#include "xonar/dsp/routing_matrix.hpp"
#include "xonar/dsp/smoothed_gain.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace xonar::dsp {

struct PipelineStats {
    std::array<float, kMaxChannels> outputPeak{}; // crête absolue par position de sortie
    float preLimiterPeak = 0.f; // maximum avant protection, pour mesurer la marge réelle
    std::uint64_t clippedSamples = 0;
    float limiterMinGainDb = 0.f;
    std::uint64_t framesProcessed = 0;
};

class Pipeline {
public:
    // Alloue tout ce qui est nécessaire pour traiter des blocs de maxFrames trames.
    // Retourne false si le profil est incohérent (ex. upmix sans entrée stéréo).
    bool prepare(const Profile& profile, std::uint32_t maxFrames);

    // Mise à jour à chaud. Retourne false (et ne change rien) si le profil n'est
    // pas structurellement compatible avec celui préparé.
    bool setProfile(const Profile& profile) noexcept;
    bool isTransitioning() const noexcept { return modeSwitchPending_ || modeFade_.isRamping(); }

    const Profile& profile() const noexcept { return profile_; }
    bool isPrepared() const noexcept { return prepared_; }

    // Traite un bloc. in.channels doit valoir profile.input.count et
    // out.channels profile.output.count ; frames <= maxFrames. Les tampons ne
    // doivent pas se recouvrir. Retourne false si les dimensions sont mauvaises.
    bool process(ConstInterleavedView in, InterleavedView out) noexcept;

    const PipelineStats& stats() const noexcept { return stats_; }
    void resetStats() noexcept;

    // Pour inspection et tests : la matrice de routage effective.
    const RoutingMatrix& routing() const noexcept { return routing_; }

private:
    bool applyProfile(const Profile& profile) noexcept;
    bool processBlock(ConstInterleavedView in, InterleavedView out) noexcept;
    void buildRouting() noexcept;
    void applyHotParameters() noexcept;
    void applyEq(std::uint32_t position) noexcept;

    Profile profile_{};
    bool prepared_ = false;
    std::uint32_t maxFrames_ = 0;
    Profile queuedProfile_{};
    SmoothedGain modeFade_;
    bool modeSwitchPending_ = false;
    std::uint32_t fadeOutRemaining_ = 0;

    RoutingMatrix routing_;
    std::vector<float> scratch_; // bloc intermédiaire au layout de sortie

    // Dérivation LFE (upmix et 2.1) : passe-bas sur la somme pondérée.
    LinkwitzRiley4 derivedLfeLowPass_;
    int derivedLfePosition_ = -1;

    // Bass management.
    std::array<LinkwitzRiley4, kMaxChannels> satelliteHighPass_{};
    std::array<LinkwitzRiley4, kMaxChannels> satelliteLowPass_{};
    std::array<bool, kMaxChannels> isSatellite_{};
    int lfePosition_ = -1;
    bool sumToLfeAllowed_ = true; // faux en 2.1 : le mode dérive déjà le LFE
    SmoothedGain sumToLfeGain_;

    std::array<std::array<Biquad, kEqBandsPerChannel>, kMaxChannels> eq_{};
    std::array<std::array<bool, kEqBandsPerChannel>, kMaxChannels> eqActive_{};
    std::array<std::array<EqBand, kEqBandsPerChannel>, kMaxChannels> appliedEq_{};
    std::array<bool, kMaxChannels> eqSettingsValid_{};
    std::array<DelayLine, kMaxChannels> delays_{};
    std::array<SmoothedGain, kMaxChannels> channelGain_{};
    SmoothedGain masterGain_;
    PeakLimiter limiter_;

    // Générateur de test.
    double testPhase_ = 0.0;
    double testPhaseIncrement_ = 0.0;
    int testPosition_ = -1;
    float testLevel_ = 0.f;

    PipelineStats stats_{};
};

} // namespace xonar::dsp
