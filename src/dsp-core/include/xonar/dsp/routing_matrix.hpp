// Matrice de routage : chaque canal de sortie est une combinaison linéaire des
// canaux d'entrée. Les gains sont fixés à la préparation ; process() n'alloue rien.
#pragma once

#include "xonar/dsp/buffer.hpp"

#include <cstdint>
#include <vector>

namespace xonar::dsp {

class RoutingMatrix {
public:
    void prepare(std::uint32_t inputs, std::uint32_t outputs) {
        inputs_ = inputs;
        outputs_ = outputs;
        gains_.assign(static_cast<std::size_t>(inputs) * outputs, 0.f);
    }

    std::uint32_t inputs() const noexcept { return inputs_; }
    std::uint32_t outputs() const noexcept { return outputs_; }

    void clear() noexcept { for (auto& g : gains_) g = 0.f; }

    void set(std::uint32_t output, std::uint32_t input, float gain) noexcept {
        if (output < outputs_ && input < inputs_) gains_[index(output, input)] = gain;
    }
    float get(std::uint32_t output, std::uint32_t input) const noexcept {
        return (output < outputs_ && input < inputs_) ? gains_[index(output, input)] : 0.f;
    }

    // Diagonale unitaire sur min(inputs, outputs), zéro ailleurs.
    void setIdentity() noexcept {
        clear();
        const auto n = inputs_ < outputs_ ? inputs_ : outputs_;
        for (std::uint32_t i = 0; i < n; ++i) set(i, i, 1.f);
    }

    // in et out ne doivent pas se recouvrir.
    void process(ConstInterleavedView in, InterleavedView out) const noexcept {
        const auto frames = in.frames < out.frames ? in.frames : out.frames;
        for (std::uint32_t f = 0; f < frames; ++f) {
            const float* src = in.data + static_cast<std::size_t>(f) * in.channels;
            float* dst = out.data + static_cast<std::size_t>(f) * out.channels;
            for (std::uint32_t o = 0; o < outputs_; ++o) {
                float acc = 0.f;
                const float* row = gains_.data() + static_cast<std::size_t>(o) * inputs_;
                for (std::uint32_t i = 0; i < inputs_; ++i) acc += row[i] * src[i];
                dst[o] = acc;
            }
        }
    }

private:
    std::size_t index(std::uint32_t output, std::uint32_t input) const noexcept {
        return static_cast<std::size_t>(output) * inputs_ + input;
    }

    std::uint32_t inputs_ = 0;
    std::uint32_t outputs_ = 0;
    std::vector<float> gains_;
};

} // namespace xonar::dsp
