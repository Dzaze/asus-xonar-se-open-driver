#include "test_framework.hpp"
#include "xonar/common/wav.hpp"

#include <cstdio>
#include <filesystem>
#include <vector>

using namespace xonar::common;

namespace {
std::string tempPath(const char* name) {
    return (std::filesystem::temp_directory_path() / name).string();
}
} // namespace

TEST_CASE("WAV : aller-retour float32 extensible 5.1 avec masque") {
    WavInfo info;
    info.sampleRate = 48000;
    info.channels = 6;
    info.channelMask = 0x60F;
    info.format = SampleFormat::Float32;
    std::vector<float> samples(6 * 100);
    for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<float>(i % 7) * 0.1f - 0.3f;

    const auto path = tempPath("xonar_test_51.wav");
    auto r = writeWav(path, info, samples);
    REQUIRE(r.ok);
    WavData d;
    r = readWav(path, d);
    REQUIRE(r.ok);
    CHECK(d.info.channels == 6);
    CHECK(d.info.channelMask == 0x60F);
    CHECK(d.info.sampleRate == 48000);
    CHECK(d.info.frames == 100);
    CHECK(d.info.format == SampleFormat::Float32);
    REQUIRE(d.samples.size() == samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) CHECK_NEAR(d.samples[i], samples[i], 0.f);
    std::remove(path.c_str());
}

TEST_CASE("WAV : PCM 24 bits stéréo classique, précision de quantification") {
    WavInfo info;
    info.channels = 2;
    info.format = SampleFormat::Pcm24;
    std::vector<float> samples = {0.f, 0.5f, -0.5f, 0.999f, -1.f, 1.f};
    const auto path = tempPath("xonar_test_24.wav");
    REQUIRE(writeWav(path, info, samples).ok);
    WavData d;
    REQUIRE(readWav(path, d).ok);
    CHECK(d.info.format == SampleFormat::Pcm24);
    CHECK(d.info.channelMask == 0);
    REQUIRE(d.samples.size() == samples.size());
    for (std::size_t i = 0; i < samples.size(); ++i) CHECK_NEAR(d.samples[i], samples[i], 2.0 / 8388608.0);
    std::remove(path.c_str());
}

TEST_CASE("WAV : PCM 16 bits et fichier invalide") {
    WavInfo info;
    info.channels = 1;
    info.format = SampleFormat::Pcm16;
    std::vector<float> samples = {0.25f, -0.25f, 0.f};
    const auto path = tempPath("xonar_test_16.wav");
    REQUIRE(writeWav(path, info, samples).ok);
    WavData d;
    REQUIRE(readWav(path, d).ok);
    CHECK(d.info.format == SampleFormat::Pcm16);
    CHECK_NEAR(d.samples[0], 0.25f, 1.0 / 32768.0);
    std::remove(path.c_str());

    WavData bad;
    CHECK(!readWav(tempPath("fichier_qui_n_existe_pas.wav"), bad).ok);
}
