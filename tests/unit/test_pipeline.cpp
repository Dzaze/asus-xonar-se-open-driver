// Tests minimaux du cahier des charges (§5) sur le pipeline complet.
#include "test_framework.hpp"
#include "xonar/dsp/pipeline.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using namespace xonar::dsp;

namespace {

constexpr std::uint32_t kRate = 48000;

std::vector<float> zeros(std::uint32_t frames, std::uint32_t channels) {
    return std::vector<float>(static_cast<std::size_t>(frames) * channels, 0.f);
}

// Amplitude crête d'un canal de sortie sur la seconde moitié du bloc (régime établi).
float peakSecondHalf(const std::vector<float>& buf, std::uint32_t channels, std::uint32_t ch) {
    const auto frames = static_cast<std::uint32_t>(buf.size() / channels);
    float peak = 0.f;
    for (std::uint32_t f = frames / 2; f < frames; ++f) peak = std::max(peak, std::fabs(buf[f * channels + ch]));
    return peak;
}

void fillSine(std::vector<float>& buf, std::uint32_t channels, std::uint32_t ch, double freq, float amp) {
    const auto frames = static_cast<std::uint32_t>(buf.size() / channels);
    for (std::uint32_t f = 0; f < frames; ++f)
        buf[f * channels + ch] = amp * static_cast<float>(std::sin(2.0 * std::numbers::pi * freq * f / kRate));
}

} // namespace

TEST_CASE("Test 1 : impulsion unité par canal en pass-through 5.1 : rien d'inversé, perdu ou échangé") {
    Profile prof = makePassThroughProfile(kRate, masks::k5Point1Side);
    prof.limiter.enabled = false; // une impulsion à 1.0 serait sinon ramenée au plafond de -0.3 dB
    Pipeline p;
    REQUIRE(p.prepare(prof, 64));
    for (std::uint32_t ch = 0; ch < 6; ++ch) {
        auto in = zeros(64, 6);
        auto out = zeros(64, 6);
        in[ch] = 1.f; // impulsion à la trame 0 sur ce canal
        REQUIRE(p.process({in.data(), 6, 64}, {out.data(), 6, 64}));
        for (std::uint32_t c = 0; c < 6; ++c) {
            CHECK_NEAR(out[c], c == ch ? 1.f : 0.f, 1e-6);   // trame 0
        }
        int tail = 0;
        for (std::size_t i = 6; i < out.size(); ++i) if (std::fabs(out[i]) > 1e-6f) ++tail;
        CHECK(tail == 0); // pas de traîne
    }
}

TEST_CASE("Test 2 : sinus stéréo en upmix : gains et affectation en 5.1") {
    Profile prof = makeUpmixProfile(kRate, masks::k5Point1Side);
    prof.limiter.enabled = false;
    Pipeline p;
    REQUIRE(p.prepare(prof, 4800));
    auto in = zeros(4800, 2);
    auto out = zeros(4800, 6);
    fillSine(in, 2, 0, 1000.0, 0.5f); // L seul
    REQUIRE(p.process({in.data(), 2, 4800}, {out.data(), 6, 4800}));
    const auto& o = prof.output;
    CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontLeft)), 0.5f, 0.01);
    CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontRight)), 0.f, 1e-6);
    CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontCenter)), 0.5f * 0.5f * dbToLinear(-3.f), 0.01);
    CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::SideLeft)), 0.5f * dbToLinear(-6.f), 0.01);
    CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::SideRight)), 0.f, 1e-6);
    // LFE dérivé passe-bas à 120 Hz : un 1 kHz doit être fortement atténué.
    CHECK(peakSecondHalf(out, 6, o.indexOf(Channel::LowFrequency)) < 0.5f * 0.5f * dbToLinear(-6.f) * 0.01f);
}

TEST_CASE("Test 2 bis : upmix avec un signal basse fréquence alimente le LFE") {
    Profile prof = makeUpmixProfile(kRate, masks::k5Point1Side);
    prof.limiter.enabled = false;
    Pipeline p;
    REQUIRE(p.prepare(prof, 48000));
    auto in = zeros(48000, 2);
    auto out = zeros(48000, 6);
    fillSine(in, 2, 0, 40.0, 0.5f);
    fillSine(in, 2, 1, 40.0, 0.5f);
    REQUIRE(p.process({in.data(), 2, 48000}, {out.data(), 6, 48000}));
    const float expected = 0.5f * dbToLinear(-6.f); // (L+R)/2 = 0.5 puis -6 dB, passe-bas transparent à 40 Hz
    CHECK_NEAR(peakSecondHalf(out, 6, prof.output.indexOf(Channel::LowFrequency)), expected, 0.02);
}

TEST_CASE("Test 3 : un vrai signal 5.1 en pass-through n'est pas upmixé ni modifié") {
    Pipeline p;
    REQUIRE(p.prepare(makePassThroughProfile(kRate, masks::k5Point1Side), 2400));
    auto in = zeros(2400, 6);
    auto out = zeros(2400, 6);
    const double freqs[6] = {200, 300, 400, 60, 500, 600};
    for (std::uint32_t c = 0; c < 6; ++c) fillSine(in, 6, c, freqs[c], 0.1f * (c + 1));
    REQUIRE(p.process({in.data(), 6, 2400}, {out.data(), 6, 2400}));
    int diffs = 0;
    for (std::size_t i = 0; i < in.size(); ++i) if (std::fabs(out[i] - in[i]) > 1e-6f) ++diffs;
    CHECK(diffs == 0);
    CHECK(p.stats().clippedSamples == 0);
}

TEST_CASE("Test 4 : clipping contrôlé, aucun dépassement non maîtrisé") {
    Profile prof = makeUpmixProfile(kRate, masks::k5Point1Side);
    prof.masterGainDb = +12.f;
    Pipeline p;
    REQUIRE(p.prepare(prof, 4800));
    auto in = zeros(4800, 2);
    auto out = zeros(4800, 6);
    fillSine(in, 2, 0, 1000.0, 0.9f);
    fillSine(in, 2, 1, 1000.0, 0.9f);
    REQUIRE(p.process({in.data(), 2, 4800}, {out.data(), 6, 4800}));
    const float ceiling = dbToLinear(prof.limiter.ceilingDb);
    int above = 0;
    for (float v : out) if (std::fabs(v) > ceiling + 1e-4f) ++above;
    CHECK(above == 0);
    CHECK(p.stats().clippedSamples == 0);
    CHECK(p.stats().limiterMinGainDb < -6.f);

    // Sans limiteur : l'écrêtage dur agit et est compté.
    prof.limiter.enabled = false;
    Pipeline q;
    REQUIRE(q.prepare(prof, 4800));
    auto out2 = zeros(4800, 6);
    REQUIRE(q.process({in.data(), 2, 4800}, {out2.data(), 6, 4800}));
    CHECK(q.stats().clippedSamples > 0);
    int over = 0;
    for (float v : out2) if (std::fabs(v) > 1.f) ++over;
    CHECK(over == 0);
}

TEST_CASE("Test 5 : bass management, sweep BF : satellites coupés sous fc, somme vers LFE") {
    Profile prof = makePassThroughProfile(kRate, masks::k5Point1Side);
    prof.bassManagement.enabled = true;
    prof.bassManagement.crossoverHz = 80.f;
    prof.limiter.enabled = false;
    Pipeline p;
    REQUIRE(p.prepare(prof, 48000));
    const auto fl = static_cast<std::uint32_t>(prof.output.indexOf(Channel::FrontLeft));
    const auto lfe = static_cast<std::uint32_t>(prof.output.indexOf(Channel::LowFrequency));

    // 30 Hz sur FL : FL très atténué, LFE reçoit ~tout.
    {
        auto in = zeros(48000, 6), out = zeros(48000, 6);
        fillSine(in, 6, fl, 30.0, 0.5f);
        REQUIRE(p.process({in.data(), 6, 48000}, {out.data(), 6, 48000}));
        CHECK(peakSecondHalf(out, 6, fl) < 0.5f * 0.1f);
        CHECK_NEAR(peakSecondHalf(out, 6, lfe), 0.5f, 0.03);
    }
    // 1 kHz sur FL : FL intact, LFE quasi nul.
    {
        REQUIRE(p.prepare(prof, 48000));
        auto in = zeros(48000, 6), out = zeros(48000, 6);
        fillSine(in, 6, fl, 1000.0, 0.5f);
        REQUIRE(p.process({in.data(), 6, 48000}, {out.data(), 6, 48000}));
        CHECK_NEAR(peakSecondHalf(out, 6, fl), 0.5f, 0.01);
        CHECK(peakSecondHalf(out, 6, lfe) < 0.5f * 0.001f);
    }
}

TEST_CASE("Downmix 5.1 vers 2.0 : coefficients ITU-R BS.775 avec headroom, LFE ignoré par défaut") {
    Profile prof = makeDownmixProfile(kRate, masks::k5Point1Side);
    prof.limiter.enabled = false;
    Pipeline p;
    REQUIRE(p.prepare(prof, 16));
    const auto& in = prof.input;
    const float head = dbToLinear(prof.downmix.headroomDb);
    const auto& m = p.routing();
    const auto L = 0u, R = 1u;
    CHECK_NEAR(m.get(L, in.indexOf(Channel::FrontLeft)), head, 1e-6);
    CHECK_NEAR(m.get(R, in.indexOf(Channel::FrontRight)), head, 1e-6);
    CHECK_NEAR(m.get(L, in.indexOf(Channel::FrontCenter)), head * dbToLinear(-3.f), 1e-6);
    CHECK_NEAR(m.get(R, in.indexOf(Channel::FrontCenter)), head * dbToLinear(-3.f), 1e-6);
    CHECK_NEAR(m.get(L, in.indexOf(Channel::SideLeft)), head * dbToLinear(-3.f), 1e-6);
    CHECK_NEAR(m.get(R, in.indexOf(Channel::SideLeft)), 0.f, 0.f);
    CHECK_NEAR(m.get(L, in.indexOf(Channel::LowFrequency)), 0.f, 0.f);
    CHECK_NEAR(m.get(R, in.indexOf(Channel::LowFrequency)), 0.f, 0.f);
    // Pire cas : tous les canaux à pleine échelle en phase -> somme bornée.
    const float worst = head * (1.f + 2.f * dbToLinear(-3.f));
    CHECK(worst < 1.5f);
}

TEST_CASE("Mode 2.1 depuis stéréo : avant intact, caisson passe-bas, centre et surrounds muets") {
    Profile prof = makeTwoPointOneProfile(kRate, masks::kStereo, masks::k5Point1Side);
    prof.limiter.enabled = false;
    Pipeline p;
    REQUIRE(p.prepare(prof, 48000));
    const auto& o = prof.output;

    // 20 Hz sur L et R : FL/FR intacts, LFE = (L+R)/2 = 0.5 (le LR4 à 80 Hz laisse passer 99,6 % à 20 Hz ;
    // à 40 Hz il atténuerait déjà de 0,5 dB, ce qui est le comportement voulu d'un passe-bas).
    {
        auto in = zeros(48000, 2), out = zeros(48000, 6);
        fillSine(in, 2, 0, 20.0, 0.5f);
        fillSine(in, 2, 1, 20.0, 0.5f);
        REQUIRE(p.process({in.data(), 2, 48000}, {out.data(), 6, 48000}));
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontLeft)), 0.5f, 0.01);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontRight)), 0.5f, 0.01);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::LowFrequency)), 0.5f, 0.02);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontCenter)), 0.f, 0.f);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::SideLeft)), 0.f, 0.f);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::SideRight)), 0.f, 0.f);
    }
    // 1 kHz : le caisson ne reçoit quasiment rien.
    {
        REQUIRE(p.prepare(prof, 48000));
        auto in = zeros(48000, 2), out = zeros(48000, 6);
        fillSine(in, 2, 0, 1000.0, 0.5f);
        fillSine(in, 2, 1, 1000.0, 0.5f);
        REQUIRE(p.process({in.data(), 2, 48000}, {out.data(), 6, 48000}));
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontLeft)), 0.5f, 0.01);
        CHECK(peakSecondHalf(out, 6, o.indexOf(Channel::LowFrequency)) < 0.5f * 0.001f);
    }
}

TEST_CASE("Mode 2.1 depuis 5.1 : downmix vers l'avant, LFE d'origine conservé, surrounds muets") {
    Profile prof = makeTwoPointOneProfile(kRate, masks::k5Point1Side, masks::k5Point1Side);
    prof.limiter.enabled = false;
    Pipeline p;
    REQUIRE(p.prepare(prof, 48000));
    const auto& o = prof.output;
    const float head = dbToLinear(prof.downmix.headroomDb);

    // Centre seul à 1 kHz : apparaît sur FL et FR à gC x headroom, pas au caisson.
    {
        auto in = zeros(48000, 6), out = zeros(48000, 6);
        fillSine(in, 6, static_cast<std::uint32_t>(o.indexOf(Channel::FrontCenter)), 1000.0, 0.5f);
        REQUIRE(p.process({in.data(), 6, 48000}, {out.data(), 6, 48000}));
        const float expected = 0.5f * dbToLinear(-3.f) * head;
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontLeft)), expected, 0.01);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontRight)), expected, 0.01);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontCenter)), 0.f, 0.f);
        CHECK(peakSecondHalf(out, 6, o.indexOf(Channel::LowFrequency)) < 0.001f);
    }
    // LFE d'origine à 20 Hz : passe au caisson, rien à l'avant.
    {
        REQUIRE(p.prepare(prof, 48000));
        auto in = zeros(48000, 6), out = zeros(48000, 6);
        fillSine(in, 6, static_cast<std::uint32_t>(o.indexOf(Channel::LowFrequency)), 20.0, 0.5f);
        REQUIRE(p.process({in.data(), 6, 48000}, {out.data(), 6, 48000}));
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::LowFrequency)), 0.5f, 0.02);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::FrontLeft)), 0.f, 0.f);
        CHECK_NEAR(peakSecondHalf(out, 6, o.indexOf(Channel::SideLeft)), 0.f, 0.f);
    }
    // Refus : sortie stéréo en 2.1 n'a pas de sens (pas de canal LFE physique).
    Profile bad = makeTwoPointOneProfile(kRate, masks::kStereo, masks::kStereo);
    Pipeline q;
    CHECK(!q.prepare(bad, 64));
}

TEST_CASE("Changement de profil à chaud : rampe sans saut, refus si incompatible") {
    Profile prof = makePassThroughProfile(kRate, masks::kStereo);
    prof.gainRampMs = 10;
    prof.limiter.enabled = false;
    Pipeline p;
    REQUIRE(p.prepare(prof, 4800));

    // Signal continu à 1.0 pour observer le gain appliqué directement.
    auto in = std::vector<float>(4800 * 2, 1.f);
    auto out = zeros(4800, 2);
    prof.channelGainDb[static_cast<std::size_t>(Channel::FrontLeft)] = -20.f;
    CHECK(p.setProfile(prof));
    REQUIRE(p.process({in.data(), 2, 4800}, {out.data(), 2, 4800}));
    float maxJump = 0.f;
    for (std::uint32_t f = 1; f < 4800; ++f) maxJump = std::max(maxJump, std::fabs(out[f * 2] - out[(f - 1) * 2]));
    CHECK(maxJump < 0.005f);                 // 0.9 de variation répartie sur 480 échantillons
    CHECK_NEAR(out[4799 * 2], dbToLinear(-20.f), 1e-4);
    CHECK_NEAR(out[4799 * 2 + 1], 1.f, 1e-6);

    Profile other = makeUpmixProfile(kRate);
    CHECK(!p.setProfile(other));
    CHECK(p.profile().mode == Mode::PassThrough);
}

TEST_CASE("Mode test : signal sur le seul canal demandé, niveau calibré") {
    Pipeline p;
    REQUIRE(p.prepare(makeChannelTestProfile(kRate, masks::k5Point1Side, Channel::SideRight, 440.f, -20.f), 48000));
    auto in = zeros(48000, 2), out = zeros(48000, 6);
    REQUIRE(p.process({in.data(), 2, 48000}, {out.data(), 6, 48000}));
    for (std::uint32_t c = 0; c < 6; ++c) {
        const float peak = peakSecondHalf(out, 6, c);
        if (c == 5) CHECK_NEAR(peak, dbToLinear(-20.f), 0.002);
        else CHECK_NEAR(peak, 0.f, 0.f);
    }
}

TEST_CASE("Préparation refusée pour profils incohérents") {
    Pipeline p;
    Profile bad = makeUpmixProfile(kRate);
    bad.input = ChannelLayout::fromMask(masks::k5Point1Side); // upmix depuis du 5.1 : non
    CHECK(!p.prepare(bad, 64));
    Profile bad2 = makePassThroughProfile(kRate, masks::kStereo);
    bad2.output = ChannelLayout::fromMask(masks::k5Point1Side);
    CHECK(!p.prepare(bad2, 64));
    CHECK(!p.isPrepared());
}
