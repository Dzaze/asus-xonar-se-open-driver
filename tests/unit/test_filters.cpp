#include "test_framework.hpp"
#include "xonar/dsp/biquad.hpp"
#include "xonar/dsp/delay_line.hpp"
#include "xonar/dsp/limiter.hpp"
#include "xonar/dsp/smoothed_gain.hpp"

#include <cmath>
#include <numbers>
#include <vector>

using namespace xonar::dsp;

namespace {

// Amplitude RMS en régime établi d'un sinus traversant un filtre.
template <typename Filter>
double steadyStateGain(Filter& f, double sampleRate, double freq) {
    const int warm = static_cast<int>(sampleRate);     // 1 s de mise en régime
    const int measure = static_cast<int>(sampleRate);  // 1 s de mesure
    double sumSq = 0.0;
    for (int n = 0; n < warm + measure; ++n) {
        const double x = std::sin(2.0 * std::numbers::pi * freq * n / sampleRate);
        const double y = f.process(static_cast<float>(x));
        if (n >= warm) sumSq += y * y;
    }
    return std::sqrt(sumSq / measure) * std::numbers::sqrt2; // RMS -> amplitude
}

} // namespace

TEST_CASE("Biquad passe-bas : -3 dB à fc, transparent en bas, atténué en haut") {
    Biquad f;
    f.setCoefficients(BiquadCoefficients::lowPass(48000, 1000, 0.7071));
    CHECK_NEAR(linearToDb(static_cast<float>(steadyStateGain(f, 48000, 1000))), -3.0, 0.2);
    f.reset();
    CHECK_NEAR(linearToDb(static_cast<float>(steadyStateGain(f, 48000, 50))), 0.0, 0.05);
    f.reset();
    CHECK(linearToDb(static_cast<float>(steadyStateGain(f, 48000, 10000))) < -35.0);
}

TEST_CASE("LR4 : passe-bas + passe-haut se somment à plat") {
    for (double freq : {40.0, 80.0, 160.0, 1000.0}) {
        LinkwitzRiley4 lp, hp;
        lp.configure(LinkwitzRiley4::Type::LowPass, 48000, 80);
        hp.configure(LinkwitzRiley4::Type::HighPass, 48000, 80);
        const int warm = 48000, measure = 48000;
        double sumSq = 0.0;
        for (int n = 0; n < warm + measure; ++n) {
            const float x = static_cast<float>(std::sin(2.0 * std::numbers::pi * freq * n / 48000.0));
            const double y = lp.process(x) + hp.process(x);
            if (n >= warm) sumSq += y * y;
        }
        const double amp = std::sqrt(sumSq / measure) * std::numbers::sqrt2;
        CHECK_NEAR(amp, 1.0, 0.02);
    }
}

TEST_CASE("LR4 passe-bas 80 Hz : -6 dB à fc, -24 dB/oct au-delà") {
    LinkwitzRiley4 lp;
    lp.configure(LinkwitzRiley4::Type::LowPass, 48000, 80);
    CHECK_NEAR(linearToDb(static_cast<float>(steadyStateGain(lp, 48000, 80))), -6.0, 0.3);
    lp.reset();
    const double g160 = linearToDb(static_cast<float>(steadyStateGain(lp, 48000, 160)));
    lp.reset();
    const double g320 = linearToDb(static_cast<float>(steadyStateGain(lp, 48000, 320)));
    CHECK(g160 < -20.0);
    CHECK_NEAR(g160 - g320, 24.0, 2.5);
}

TEST_CASE("EQ peaking : gain au centre, neutre loin de la bande") {
    Biquad f;
    f.setCoefficients(BiquadCoefficients::peaking(48000, 1000, 1.0, 6.0));
    CHECK_NEAR(linearToDb(static_cast<float>(steadyStateGain(f, 48000, 1000))), 6.0, 0.1);
    f.reset();
    CHECK_NEAR(linearToDb(static_cast<float>(steadyStateGain(f, 48000, 50))), 0.0, 0.2);
}

TEST_CASE("Gain lissé : atteint la cible sans saut") {
    SmoothedGain g;
    g.prepare(1.f, 100);
    g.setTarget(0.f);
    float prev = 1.f;
    float maxStep = 0.f;
    for (int i = 0; i < 100; ++i) {
        const float v = g.next();
        maxStep = std::max(maxStep, std::fabs(prev - v));
        prev = v;
    }
    CHECK_NEAR(g.current(), 0.f, 1e-6);
    CHECK(!g.isRamping());
    CHECK(maxStep <= 0.0101f);
}

TEST_CASE("Ligne à retard : décalage exact, bornée par la capacité") {
    DelayLine d;
    d.prepare(10);
    d.setDelay(3);
    std::vector<float> out;
    for (int i = 0; i < 8; ++i) out.push_back(d.process(i == 0 ? 1.f : 0.f));
    CHECK_NEAR(out[2], 0.f, 0.f);
    CHECK_NEAR(out[3], 1.f, 0.f);
    CHECK_NEAR(out[4], 0.f, 0.f);
    d.setDelay(50);
    CHECK(d.delay() == 10);
}

TEST_CASE("Limiteur : aucun échantillon ne dépasse le plafond, écrêtage compté si désactivé") {
    PeakLimiter lim;
    lim.prepare(48000, -0.3f, 50.f);
    std::vector<float> buf(2 * 480, 0.f);
    for (std::size_t f = 0; f < 480; ++f) { buf[2 * f] = 2.0f; buf[2 * f + 1] = -1.5f; }
    const auto clipped = lim.process({buf.data(), 2, 480});
    CHECK(clipped == 0);
    const float ceiling = dbToLinear(-0.3f);
    int above = 0;
    for (float v : buf) if (std::fabs(v) > ceiling + 1e-5f) ++above;
    CHECK(above == 0);
    CHECK(lim.minimumGainLinear() < 0.5f);

    PeakLimiter off;
    off.prepare(48000, 0.f, 50.f);
    off.setEnabled(false);
    std::vector<float> hot(10, 1.7f);
    const auto n = off.process({hot.data(), 1, 10});
    CHECK(n == 10);
    for (float v : hot) CHECK_NEAR(v, 1.f, 0.f);
}
