// Définition des canaux audio et des dispositions (layouts) de haut-parleurs.
// Les bits de masque suivent la convention WAVEFORMATEXTENSIBLE / KSAUDIO_SPEAKER_*
// de Windows, qui est aussi celle du champ bmChannelConfig de l'USB Audio Class 2.0.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace xonar::dsp {

enum class Channel : std::uint8_t {
    FrontLeft = 0,
    FrontRight,
    FrontCenter,
    LowFrequency,
    BackLeft,
    BackRight,
    SideLeft,
    SideRight,
    Count
};

constexpr std::size_t kMaxChannels = static_cast<std::size_t>(Channel::Count);

// Bit de masque Windows pour chaque canal.
constexpr std::uint32_t speakerMaskBit(Channel c) noexcept {
    switch (c) {
    case Channel::FrontLeft:    return 0x1;
    case Channel::FrontRight:   return 0x2;
    case Channel::FrontCenter:  return 0x4;
    case Channel::LowFrequency: return 0x8;
    case Channel::BackLeft:     return 0x10;
    case Channel::BackRight:    return 0x20;
    case Channel::SideLeft:     return 0x200;
    case Channel::SideRight:    return 0x400;
    default:                    return 0;
    }
}

constexpr std::string_view channelName(Channel c) noexcept {
    switch (c) {
    case Channel::FrontLeft:    return "FL";
    case Channel::FrontRight:   return "FR";
    case Channel::FrontCenter:  return "FC";
    case Channel::LowFrequency: return "LFE";
    case Channel::BackLeft:     return "BL";
    case Channel::BackRight:    return "BR";
    case Channel::SideLeft:     return "SL";
    case Channel::SideRight:    return "SR";
    default:                    return "?";
    }
}

// Masques usuels (valeurs KSAUDIO_SPEAKER_* de Windows).
namespace masks {
constexpr std::uint32_t kMono        = 0x4;    // KSAUDIO_SPEAKER_MONO (FC)
constexpr std::uint32_t kStereo      = 0x3;    // FL FR
constexpr std::uint32_t kQuad        = 0x33;   // FL FR BL BR
constexpr std::uint32_t k5Point1Back = 0x3F;   // FL FR FC LFE BL BR  (KSAUDIO_SPEAKER_5POINT1)
constexpr std::uint32_t k5Point1Side = 0x60F;  // FL FR FC LFE SL SR  (KSAUDIO_SPEAKER_5POINT1_SURROUND)
constexpr std::uint32_t k7Point1     = 0x63F;  // FL FR FC LFE BL BR SL SR
} // namespace masks

// Disposition de canaux : masque + ordre des canaux dans les trames entrelacées.
// L'ordre suit les bits croissants du masque, comme l'exige WAVEFORMATEXTENSIBLE.
struct ChannelLayout {
    std::uint32_t mask = 0;
    std::uint8_t count = 0;
    std::array<Channel, kMaxChannels> order{};

    static constexpr ChannelLayout fromMask(std::uint32_t m) noexcept {
        ChannelLayout l{};
        l.mask = m;
        for (std::size_t i = 0; i < kMaxChannels; ++i) {
            const auto c = static_cast<Channel>(i);
            if (m & speakerMaskBit(c)) {
                l.order[l.count++] = c;
            }
        }
        // Les bits que nous ne gérons pas sont comptés comme canaux, mais sans rôle.
        return l;
    }

    // Index du canal dans la trame, ou -1 s'il est absent.
    constexpr int indexOf(Channel c) const noexcept {
        for (std::size_t i = 0; i < count; ++i) {
            if (order[i] == c) return static_cast<int>(i);
        }
        return -1;
    }

    constexpr bool has(Channel c) const noexcept { return (mask & speakerMaskBit(c)) != 0; }
    constexpr bool isStereo() const noexcept { return mask == masks::kStereo; }
    constexpr bool is5Point1() const noexcept {
        return mask == masks::k5Point1Back || mask == masks::k5Point1Side;
    }

    // Canaux "surround" effectifs de ce layout (arrière ou latéraux).
    constexpr Channel surroundLeft() const noexcept {
        return has(Channel::SideLeft) ? Channel::SideLeft : Channel::BackLeft;
    }
    constexpr Channel surroundRight() const noexcept {
        return has(Channel::SideRight) ? Channel::SideRight : Channel::BackRight;
    }
};

constexpr bool operator==(const ChannelLayout& a, const ChannelLayout& b) noexcept {
    return a.mask == b.mask;
}
constexpr bool operator!=(const ChannelLayout& a, const ChannelLayout& b) noexcept {
    return !(a == b);
}

} // namespace xonar::dsp
