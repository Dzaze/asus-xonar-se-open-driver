#include "test_framework.hpp"
#include "xonar/dsp/channels.hpp"

using namespace xonar::dsp;

TEST_CASE("Layout 5.1 side : ordre FL FR FC LFE SL SR") {
    constexpr auto l = ChannelLayout::fromMask(masks::k5Point1Side);
    CHECK(l.count == 6);
    CHECK(l.order[0] == Channel::FrontLeft);
    CHECK(l.order[1] == Channel::FrontRight);
    CHECK(l.order[2] == Channel::FrontCenter);
    CHECK(l.order[3] == Channel::LowFrequency);
    CHECK(l.order[4] == Channel::SideLeft);
    CHECK(l.order[5] == Channel::SideRight);
    CHECK(l.indexOf(Channel::BackLeft) == -1);
    CHECK(l.surroundLeft() == Channel::SideLeft);
    CHECK(l.is5Point1());
}

TEST_CASE("Layout 5.1 back : ordre FL FR FC LFE BL BR") {
    constexpr auto l = ChannelLayout::fromMask(masks::k5Point1Back);
    CHECK(l.count == 6);
    CHECK(l.order[4] == Channel::BackLeft);
    CHECK(l.order[5] == Channel::BackRight);
    CHECK(l.surroundRight() == Channel::BackRight);
    CHECK(l.is5Point1());
}

TEST_CASE("Layout stéréo et quad") {
    constexpr auto s = ChannelLayout::fromMask(masks::kStereo);
    CHECK(s.count == 2);
    CHECK(s.isStereo());
    CHECK(!s.is5Point1());
    constexpr auto q = ChannelLayout::fromMask(masks::kQuad);
    CHECK(q.count == 4);
    CHECK(q.indexOf(Channel::BackRight) == 3);
}

TEST_CASE("Bits de masque conformes à WAVEFORMATEXTENSIBLE") {
    CHECK(speakerMaskBit(Channel::FrontLeft) == 0x1);
    CHECK(speakerMaskBit(Channel::LowFrequency) == 0x8);
    CHECK(speakerMaskBit(Channel::BackRight) == 0x20);
    CHECK(speakerMaskBit(Channel::SideLeft) == 0x200);
    CHECK(speakerMaskBit(Channel::SideRight) == 0x400);
    CHECK(masks::k5Point1Side == (0x1 | 0x2 | 0x4 | 0x8 | 0x200 | 0x400));
}
