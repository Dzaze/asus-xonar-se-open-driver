#include "xonar/dsp/profile.hpp"

namespace xonar::dsp {

Profile makePassThroughProfile(std::uint32_t sampleRate, std::uint32_t mask) {
    Profile p;
    p.mode = Mode::PassThrough;
    p.sampleRate = sampleRate;
    p.input = ChannelLayout::fromMask(mask);
    p.output = p.input;
    return p;
}

Profile makeUpmixProfile(std::uint32_t sampleRate, std::uint32_t outputMask) {
    Profile p;
    p.mode = Mode::Upmix2to6;
    p.sampleRate = sampleRate;
    p.input = ChannelLayout::fromMask(masks::kStereo);
    p.output = ChannelLayout::fromMask(outputMask);
    return p;
}

Profile makeDownmixProfile(std::uint32_t sampleRate, std::uint32_t inputMask) {
    Profile p;
    p.mode = Mode::Downmix6to2;
    p.sampleRate = sampleRate;
    p.input = ChannelLayout::fromMask(inputMask);
    p.output = ChannelLayout::fromMask(masks::kStereo);
    return p;
}

Profile makeTwoPointOneProfile(std::uint32_t sampleRate, std::uint32_t inputMask, std::uint32_t outputMask) {
    Profile p;
    p.mode = Mode::TwoPointOne;
    p.sampleRate = sampleRate;
    p.input = ChannelLayout::fromMask(inputMask);
    p.output = ChannelLayout::fromMask(outputMask);
    return p;
}

Profile makeChannelTestProfile(std::uint32_t sampleRate, std::uint32_t outputMask, Channel channel,
                               float frequencyHz, float levelDb) {
    Profile p;
    p.mode = Mode::ChannelTest;
    p.sampleRate = sampleRate;
    p.input = ChannelLayout::fromMask(masks::kStereo);
    p.output = ChannelLayout::fromMask(outputMask);
    p.test.channel = channel;
    p.test.frequencyHz = frequencyHz;
    p.test.levelDb = levelDb;
    p.limiter.enabled = false; // Le signal de test est calibré ; on ne le modifie pas.
    return p;
}

} // namespace xonar::dsp
