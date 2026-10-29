// Limiteur de crête simple (attaque instantanée, relâchement exponentiel) suivi
// d'un écrêtage dur de sécurité avec comptage. Protège les enceintes contre les
// dépassements créés par l'upmix, le downmix ou l'égaliseur.
#pragma once

#include "xonar/dsp/buffer.hpp"
#include "xonar/dsp/smoothed_gain.hpp"

#include <cmath>
#include <cstdint>

namespace xonar::dsp {

class PeakLimiter {
public:
    void prepare(double sampleRate, float ceilingDb, float releaseMs) noexcept {
        setParameters(sampleRate, ceilingDb, releaseMs);
        gain_ = 1.f;
        minGain_ = 1.f;
        clips_ = 0;
    }

    void setParameters(double sampleRate, float ceilingDb, float releaseMs) noexcept {
        ceiling_ = dbToLinear(ceilingDb);
        const double releaseSamples = sampleRate * releaseMs / 1000.0;
        releaseCoef_ = releaseSamples > 0 ? static_cast<float>(std::exp(-1.0 / releaseSamples)) : 0.f;
    }

    void setEnabled(bool on) noexcept { enabled_ = on; }
    bool enabled() const noexcept { return enabled_; }

    // Traite le bloc en place. Retourne le nombre d'échantillons écrêtés dans ce bloc.
    std::uint32_t process(InterleavedView buf) noexcept {
        std::uint32_t clipped = 0;
        for (std::uint32_t f = 0; f < buf.frames; ++f) {
            float* frame = buf.data + static_cast<std::size_t>(f) * buf.channels;
            if (enabled_) {
                float peak = 0.f;
                for (std::uint32_t c = 0; c < buf.channels; ++c) {
                    const float a = std::fabs(frame[c]);
                    if (a > peak) peak = a;
                }
                const float needed = peak > ceiling_ ? ceiling_ / peak : 1.f;
                // Relâchement progressif vers 1, mais jamais au-dessus du gain
                // nécessaire pour la trame courante (attaque instantanée).
                const float released = gain_ + (1.f - gain_) * (1.f - releaseCoef_);
                gain_ = needed < released ? needed : released;
                if (gain_ > 1.f) gain_ = 1.f;
                if (gain_ < minGain_) minGain_ = gain_;
                for (std::uint32_t c = 0; c < buf.channels; ++c) frame[c] *= gain_;
            }
            // Écrêtage dur de sécurité : ne doit normalement jamais agir quand le limiteur est actif.
            for (std::uint32_t c = 0; c < buf.channels; ++c) {
                if (frame[c] > 1.f) { frame[c] = 1.f; ++clipped; }
                else if (frame[c] < -1.f) { frame[c] = -1.f; ++clipped; }
            }
        }
        clips_ += clipped;
        return clipped;
    }

    std::uint64_t totalClips() const noexcept { return clips_; }
    float minimumGainLinear() const noexcept { return minGain_; }
    void resetStats() noexcept { clips_ = 0; minGain_ = 1.f; }

private:
    bool enabled_ = true;
    float ceiling_ = 1.f;
    float releaseCoef_ = 0.f;
    float gain_ = 1.f;
    float minGain_ = 1.f;
    std::uint64_t clips_ = 0;
};

} // namespace xonar::dsp
