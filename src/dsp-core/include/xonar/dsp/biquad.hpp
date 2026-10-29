// Filtre biquad (forme directe II transposée) et formules de conception
// d'après le "Audio EQ Cookbook" de Robert Bristow-Johnson.
#pragma once

#include <cmath>
#include <numbers>

namespace xonar::dsp {

struct BiquadCoefficients {
    float b0 = 1.f, b1 = 0.f, b2 = 0.f; // numérateur (déjà normalisé par a0)
    float a1 = 0.f, a2 = 0.f;           // dénominateur

    static BiquadCoefficients identity() noexcept { return {}; }

    static BiquadCoefficients lowPass(double sampleRate, double frequencyHz, double q) noexcept {
        const double w0 = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        return normalize((1.0 - cw) / 2.0, 1.0 - cw, (1.0 - cw) / 2.0, a0, -2.0 * cw, 1.0 - alpha);
    }

    static BiquadCoefficients highPass(double sampleRate, double frequencyHz, double q) noexcept {
        const double w0 = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        return normalize((1.0 + cw) / 2.0, -(1.0 + cw), (1.0 + cw) / 2.0, a0, -2.0 * cw, 1.0 - alpha);
    }

    // Égaliseur paramétrique (bande "peaking").
    static BiquadCoefficients peaking(double sampleRate, double frequencyHz, double q, double gainDb) noexcept {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        return normalize(1.0 + alpha * A, -2.0 * cw, 1.0 - alpha * A,
                         1.0 + alpha / A, -2.0 * cw, 1.0 - alpha / A);
    }

    static BiquadCoefficients lowShelf(double sampleRate, double frequencyHz, double q, double gainDb) noexcept {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double k = 2.0 * std::sqrt(A) * alpha;
        return normalize(A * ((A + 1) - (A - 1) * cw + k), 2 * A * ((A - 1) - (A + 1) * cw),
                         A * ((A + 1) - (A - 1) * cw - k), (A + 1) + (A - 1) * cw + k,
                         -2 * ((A - 1) + (A + 1) * cw), (A + 1) + (A - 1) * cw - k);
    }

    static BiquadCoefficients highShelf(double sampleRate, double frequencyHz, double q, double gainDb) noexcept {
        const double A = std::pow(10.0, gainDb / 40.0);
        const double w0 = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
        const double cw = std::cos(w0), sw = std::sin(w0);
        const double alpha = sw / (2.0 * q);
        const double k = 2.0 * std::sqrt(A) * alpha;
        return normalize(A * ((A + 1) + (A - 1) * cw + k), -2 * A * ((A - 1) + (A + 1) * cw),
                         A * ((A + 1) + (A - 1) * cw - k), (A + 1) - (A - 1) * cw + k,
                         2 * ((A - 1) - (A + 1) * cw), (A + 1) - (A - 1) * cw - k);
    }

private:
    static BiquadCoefficients normalize(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
        BiquadCoefficients c;
        c.b0 = static_cast<float>(b0 / a0);
        c.b1 = static_cast<float>(b1 / a0);
        c.b2 = static_cast<float>(b2 / a0);
        c.a1 = static_cast<float>(a1 / a0);
        c.a2 = static_cast<float>(a2 / a0);
        return c;
    }
};

class Biquad {
public:
    void setCoefficients(const BiquadCoefficients& c) noexcept { coef_ = c; }
    const BiquadCoefficients& coefficients() const noexcept { return coef_; }
    void reset() noexcept { z1_ = z2_ = 0.f; }

    float process(float x) noexcept {
        const float y = coef_.b0 * x + z1_;
        z1_ = coef_.b1 * x - coef_.a1 * y + z2_;
        z2_ = coef_.b2 * x - coef_.a2 * y;
        return y;
    }

private:
    BiquadCoefficients coef_{};
    float z1_ = 0.f, z2_ = 0.f;
};

// Filtre de Linkwitz-Riley d'ordre 4 (24 dB/oct) : deux Butterworth d'ordre 2 en cascade.
// Un passe-bas et un passe-haut LR4 à la même fréquence se somment à plat en amplitude.
class LinkwitzRiley4 {
public:
    enum class Type { LowPass, HighPass };

    void configure(Type type, double sampleRate, double frequencyHz) noexcept {
        constexpr double kButterworthQ = 0.7071067811865476;
        const auto c = (type == Type::LowPass)
                           ? BiquadCoefficients::lowPass(sampleRate, frequencyHz, kButterworthQ)
                           : BiquadCoefficients::highPass(sampleRate, frequencyHz, kButterworthQ);
        first_.setCoefficients(c);
        second_.setCoefficients(c);
    }
    void reset() noexcept { first_.reset(); second_.reset(); }
    float process(float x) noexcept { return second_.process(first_.process(x)); }

private:
    Biquad first_, second_;
};

} // namespace xonar::dsp
