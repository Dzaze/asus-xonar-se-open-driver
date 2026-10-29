// Ligne à retard entière (en échantillons) pour la correction de distance des enceintes.
// La mémoire est allouée dans prepare(), jamais dans process().
#pragma once

#include <cstdint>
#include <vector>

namespace xonar::dsp {

class DelayLine {
public:
    void prepare(std::uint32_t maxDelaySamples) {
        buffer_.assign(static_cast<std::size_t>(maxDelaySamples) + 1, 0.f);
        write_ = 0;
        delay_ = 0;
    }

    void setDelay(std::uint32_t samples) noexcept {
        const auto maxDelay = static_cast<std::uint32_t>(buffer_.size()) - 1;
        delay_ = samples > maxDelay ? maxDelay : samples;
    }
    std::uint32_t delay() const noexcept { return delay_; }

    void reset() noexcept {
        for (auto& v : buffer_) v = 0.f;
        write_ = 0;
    }

    float process(float x) noexcept {
        if (buffer_.empty() || delay_ == 0) return x;
        const auto size = static_cast<std::uint32_t>(buffer_.size());
        buffer_[write_] = x;
        const std::uint32_t read = (write_ + size - delay_) % size;
        write_ = (write_ + 1) % size;
        return buffer_[read];
    }

private:
    std::vector<float> buffer_;
    std::uint32_t write_ = 0;
    std::uint32_t delay_ = 0;
};

} // namespace xonar::dsp
