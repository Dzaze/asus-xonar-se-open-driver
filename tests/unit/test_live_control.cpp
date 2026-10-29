#include "test_framework.hpp"
#include "xonar/dsp/latest_value.hpp"
#include "xonar/dsp/pipeline.hpp"
#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace xonar::dsp;

TEST_CASE("Mono declare : extension sans perte vers le transport six canaux") {
    Profile profile;
    profile.mode = Mode::Upmix1to6;
    profile.input = ChannelLayout::fromMask(speakerMaskBit(Channel::FrontCenter));
    profile.output = ChannelLayout::fromMask(masks::k5Point1Side);
    profile.outputProtection = false;
    Pipeline pipeline;
    REQUIRE(pipeline.prepare(profile, 4800));
    std::vector<float> input(4800), output(4800 * 6);
    for(unsigned f = 0; f < 4800; ++f) input[f] = .01f * std::sin(2.f * 3.14159265f * 60.f * f / 48000.f);
    REQUIRE(pipeline.process({input.data(), 1, 4800}, {output.data(), 6, 4800}));
    for(unsigned f = 2400; f < 4800; ++f) {
        CHECK_NEAR(output[f * 6], input[f], 1e-6);
        CHECK_NEAR(output[f * 6 + 1], input[f], 1e-6);
        CHECK_NEAR(output[f * 6 + 2], input[f] * dbToLinear(-3.f), 1e-6);
    }
}

TEST_CASE("Transition de mode : fondu borne, sans saut ni reallocation de format") {
    auto profile = makePassThroughProfile(48000, masks::k5Point1Side);
    profile.limiter.enabled = false;
    Pipeline pipeline;
    REQUIRE(pipeline.prepare(profile, 32));
    std::vector<float> input(32 * 6, .1f), output(32 * 6);
    REQUIRE(pipeline.process({input.data(), 6, 32}, {output.data(), 6, 32}));
    float last = output[31 * 6];
    float jump = 0;
    profile.mode = Mode::DownmixStereo6to6;
    REQUIRE(pipeline.setProfile(profile));
    for(unsigned block = 0; block < 100; ++block) {
        REQUIRE(pipeline.process({input.data(), 6, 32}, {output.data(), 6, 32}));
        for(unsigned frame = 0; frame < 32; ++frame) {
            jump = std::max(jump, std::fabs(output[frame * 6] - last));
            last = output[frame * 6];
        }
    }
    CHECK(jump < .001f);
    CHECK(!pipeline.isTransitioning());
    CHECK(pipeline.profile().mode == Mode::DownmixStereo6to6);
    CHECK(output[31 * 6 + 2] == 0);
}

TEST_CASE("Etage avant mixage : aucune protection ou calibration appliquee deux fois") {
    auto profile = makePassThroughProfile(48000, masks::k5Point1Side);
    profile.outputProtection = false;
    profile.limiter.enabled = false;
    Pipeline pipeline;
    REQUIRE(pipeline.prepare(profile, 32));
    std::vector<float> input(32 * 6, 1.5f), output(32 * 6);
    REQUIRE(pipeline.process({input.data(), 6, 32}, {output.data(), 6, 32}));
    CHECK(output == input); // le GFX pourra attenuer avant son unique protection
    profile.outputProtection = true;
    REQUIRE(pipeline.setProfile(profile));
    REQUIRE(pipeline.process({input.data(), 6, 32}, {output.data(), 6, 32}));
    CHECK(output.back() == 1.f);
}

TEST_CASE("Controle live : 5.1 / upmix / 2.0 / 2.1 sans reprepare") {
    auto profile = makePassThroughProfile(48000, masks::k5Point1Side);
    profile.limiter.enabled = false;
    Pipeline pipeline;
    REQUIRE(pipeline.prepare(profile, 4800));
    std::vector<float> input(4800 * 6), output(4800 * 6);
    for (unsigned f = 0; f < 4800; ++f) {
        input[f * 6] = .01f * std::sin(2.f * 3.14159265f * 60.f * f / 48000.f);
        input[f * 6 + 1] = input[f * 6];
    }
    const auto energy = [&](unsigned channel) {
        double value = 0;
        for (unsigned f = 2400; f < 4800; ++f) value += output[f * 6 + channel] * output[f * 6 + channel];
        return value;
    };
    for (const auto mode : {Mode::PassThrough, Mode::Upmix6to6, Mode::DownmixStereo6to6, Mode::TwoPointOne, Mode::PassThrough}) {
        profile.mode = mode;
        REQUIRE(pipeline.setProfile(profile));
        REQUIRE(pipeline.process({input.data(), 6, 4800}, {output.data(), 6, 4800}));
        CHECK(energy(0) > .01);
        CHECK(energy(1) > .01);
        if (mode == Mode::Upmix6to6) {
            CHECK(energy(2) > .001);
            CHECK(energy(3) > .001);
            CHECK(energy(4) > .001);
            CHECK(energy(5) > .001);
        } else {
            CHECK(energy(2) == 0);
            CHECK(energy(4) == 0);
            CHECK(energy(5) == 0);
            CHECK(mode == Mode::TwoPointOne ? energy(3) > .001 : energy(3) == 0);
        }
    }
    // Modifier le crossover en 2.1 doit réellement filtrer le caisson à chaud.
    profile.mode = Mode::TwoPointOne;
    profile.twoPointOne.lfeLowPassHz = 200;
    REQUIRE(pipeline.setProfile(profile));
    REQUIRE(pipeline.process({input.data(), 6, 4800}, {output.data(), 6, 4800}));
    const double highCutoff = energy(3);
    profile.twoPointOne.lfeLowPassHz = 30;
    REQUIRE(pipeline.setProfile(profile));
    REQUIRE(pipeline.process({input.data(), 6, 4800}, {output.data(), 6, 4800}));
    CHECK(energy(3) < highCutoff * .02);
}

TEST_CASE("Controle live : limiteur plafond et bass management changent a chaud") {
    auto profile = makePassThroughProfile(48000, masks::k5Point1Side);
    Pipeline pipeline;
    REQUIRE(pipeline.prepare(profile, 4800));
    std::vector<float> input(4800 * 6, .5f), output(4800 * 6);
    profile.limiter.ceilingDb = -20;
    REQUIRE(pipeline.setProfile(profile));
    REQUIRE(pipeline.process({input.data(), 6, 4800}, {output.data(), 6, 4800}));
    CHECK_NEAR(output.back(), .1f, .0001);
    profile.limiter.enabled = false;
    profile.bassManagement.enabled = true;
    profile.bassManagement.highPassSatellites = true;
    REQUIRE(pipeline.setProfile(profile));
    REQUIRE(pipeline.process({input.data(), 6, 4800}, {output.data(), 6, 4800}));
    CHECK(std::fabs(output[4799 * 6]) < .001f);
    profile.bassManagement.enabled = false;
    REQUIRE(pipeline.setProfile(profile));
    REQUIRE(pipeline.process({input.data(), 6, 4800}, {output.data(), 6, 4800}));
    CHECK_NEAR(output[4799 * 6], .5f, .0001);
}

TEST_CASE("Marge : crete avant protection et ecretages mesures, remise a zero") {
    auto profile = makePassThroughProfile(48000, masks::k5Point1Side);
    profile.limiter.enabled = false;
    Pipeline pipeline;
    REQUIRE(pipeline.prepare(profile, 64));
    std::vector<float> input(64 * 6, 1.5f), output(input.size());
    REQUIRE(pipeline.process({input.data(), 6, 64}, {output.data(), 6, 64}));
    CHECK_NEAR(pipeline.stats().preLimiterPeak, 1.5f, .00001);
    CHECK(pipeline.stats().clippedSamples == input.size());
    CHECK_NEAR(output.back(), 1.f, .00001);
    pipeline.resetStats();
    std::fill(input.begin(), input.end(), .1f);
    REQUIRE(pipeline.process({input.data(), 6, 64}, {output.data(), 6, 64}));
    CHECK_NEAR(pipeline.stats().preLimiterPeak, .1f, .00001);
    CHECK(pipeline.stats().clippedSamples == 0);
}

TEST_CASE("Boite SPSC : aucun profil dechire sous publications concurrentes") {
    struct Message { std::uint64_t sequence; std::uint64_t payload[64]; };
    LatestValue<Message> box;
    std::atomic<bool> finished{false};
    std::thread producer([&] {
        Message value{};
        for (std::uint64_t sequence = 1; sequence <= 100000; ++sequence) {
            value.sequence = sequence;
            for (auto& part : value.payload) part = sequence;
            box.publish(value);
        }
        finished.store(true, std::memory_order_release);
    });
    bool valid = true;
    std::uint64_t last = 0;
    while (!finished.load(std::memory_order_acquire) || last != 100000) {
        if (const auto* value = box.consume()) {
            valid = valid && value->sequence >= last;
            last = value->sequence;
            for (const auto part : value->payload) valid = valid && part == value->sequence;
        } else std::this_thread::yield();
    }
    producer.join();
    CHECK(valid);
    CHECK(last == 100000);
}
