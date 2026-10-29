// Gain lissé par rampe linéaire : évite les clics lors des changements de profil.
#pragma once

#include <cmath>
#include <cstdint>

namespace xonar::dsp {

inline float dbToLinear(float db) noexcept { return std::pow(10.f, db / 20.f); }
inline float linearToDb(float lin) noexcept {
    return lin > 0.f ? 20.f * std::log10(lin) : -144.f;
}

class SmoothedGain {
public:
    // rampSamples : nombre d'échantillons pour atteindre la cible.
    void prepare(float initialLinear, std::uint32_t rampSamples) noexcept {
        current_ = target_ = initialLinear;
        rampSamples_ = rampSamples == 0 ? 1 : rampSamples;
        remaining_ = 0;
        step_ = 0.f;
    }

    void setTarget(float linear) noexcept {
        if (linear == target_) return;
        target_ = linear;
        remaining_ = rampSamples_;
        step_ = (target_ - current_) / static_cast<float>(rampSamples_);
    }

    float target() const noexcept { return target_; }
    float current() const noexcept { return current_; }
    bool isRamping() const noexcept { return remaining_ > 0; }

    // Valeur de gain pour l'échantillon courant, puis avance d'un pas.
    float next() noexcept {
        if (remaining_ > 0) {
            current_ += step_;
            if (--remaining_ == 0) current_ = target_;
        }
        return current_;
    }

private:
    float current_ = 1.f;
    float target_ = 1.f;
    float step_ = 0.f;
    std::uint32_t rampSamples_ = 1;
    std::uint32_t remaining_ = 0;
};

} // namespace xonar::dsp
