// Vues sur des blocs PCM float32 entrelacés. Aucune propriété mémoire :
// le code appelant (APO, outil de test) fournit les tampons.
#pragma once

#include <cstddef>
#include <cstdint>

namespace xonar::dsp {

struct InterleavedView {
    float* data = nullptr;
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;

    float& at(std::uint32_t frame, std::uint32_t channel) noexcept {
        return data[static_cast<std::size_t>(frame) * channels + channel];
    }
    float at(std::uint32_t frame, std::uint32_t channel) const noexcept {
        return data[static_cast<std::size_t>(frame) * channels + channel];
    }
    std::size_t sampleCount() const noexcept {
        return static_cast<std::size_t>(frames) * channels;
    }
};

struct ConstInterleavedView {
    const float* data = nullptr;
    std::uint32_t channels = 0;
    std::uint32_t frames = 0;

    ConstInterleavedView() = default;
    ConstInterleavedView(const float* d, std::uint32_t ch, std::uint32_t fr) noexcept
        : data(d), channels(ch), frames(fr) {}
    ConstInterleavedView(const InterleavedView& v) noexcept // NOLINT(google-explicit-constructor)
        : data(v.data), channels(v.channels), frames(v.frames) {}

    float at(std::uint32_t frame, std::uint32_t channel) const noexcept {
        return data[static_cast<std::size_t>(frame) * channels + channel];
    }
    std::size_t sampleCount() const noexcept {
        return static_cast<std::size_t>(frames) * channels;
    }
};

} // namespace xonar::dsp
