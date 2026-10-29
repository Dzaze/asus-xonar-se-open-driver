#include "xonar/dsp/pipeline.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace xonar::dsp {

namespace {

constexpr float kMuteLinear = 0.f;

float channelLinearGain(const Profile& p, Channel c) noexcept {
    const auto i = static_cast<std::size_t>(c);
    return p.channelMute[i] ? kMuteLinear : dbToLinear(p.channelGainDb[i]);
}

std::uint32_t msToSamples(float ms, std::uint32_t sampleRate) noexcept {
    const float s = ms * static_cast<float>(sampleRate) / 1000.f;
    return s <= 0.f ? 0u : static_cast<std::uint32_t>(s + 0.5f);
}

} // namespace

bool Pipeline::prepare(const Profile& profile, std::uint32_t maxFrames) {
    prepared_ = false;
    if (maxFrames == 0 || profile.sampleRate == 0) return false;
    if (profile.input.count == 0 || profile.output.count == 0) return false;
    if (profile.input.count > kMaxChannels || profile.output.count > kMaxChannels) return false;

    switch (profile.mode) {
    case Mode::PassThrough:
        if (profile.input != profile.output) return false;
        break;
    case Mode::Upmix2to6:
        if (!profile.input.isStereo() || !profile.output.is5Point1()) return false;
        break;
    case Mode::Upmix1to6:
        if(profile.input.count != 1 || !profile.output.is5Point1()) return false;
        break;
    case Mode::Downmix6to2:
        if (!profile.input.is5Point1() || !profile.output.isStereo()) return false;
        break;
    case Mode::TwoPointOne:
        if (!(profile.input.isStereo() || profile.input.is5Point1()) || !profile.output.is5Point1()) return false;
        break;
    case Mode::ChannelTest:
        if (profile.output.indexOf(profile.test.channel) < 0) return false;
        break;
    case Mode::Upmix6to6:
        // Même layout 5.1 en entrée et en sortie : on étale FL/FR sur le centre/surrounds.
        if (!profile.input.is5Point1() || profile.input != profile.output) return false;
        break;
    case Mode::DownmixStereo6to6:
        // Même layout 5.1 en entrée et en sortie : downmix stéréo replié sur FL/FR.
        if (!profile.input.is5Point1() || profile.input != profile.output) return false;
        break;
    }

    profile_ = profile;
    maxFrames_ = maxFrames;
    eqSettingsValid_.fill(false);
    modeFade_.prepare(1.f, std::max(1u, profile.sampleRate / 100));
    modeSwitchPending_ = false;
    fadeOutRemaining_ = 0;

    routing_.prepare(profile_.input.count, profile_.output.count);
    scratch_.assign(static_cast<std::size_t>(maxFrames) * profile_.output.count, 0.f);
    buildRouting();

    // Dérivation LFE (upmix et 2.1) : la somme pondérée écrite par la matrice sur la
    // position LFE est filtrée passe-bas.
    derivedLfePosition_ = -1;
    if ((profile_.mode == Mode::Upmix1to6 || profile_.mode == Mode::Upmix2to6 || profile_.mode == Mode::Upmix6to6) && profile_.upmix.deriveLfe) {
        derivedLfePosition_ = profile_.output.indexOf(Channel::LowFrequency);
        derivedLfeLowPass_.configure(LinkwitzRiley4::Type::LowPass, profile_.sampleRate,
                                     profile_.upmix.lfeLowPassHz);
        derivedLfeLowPass_.reset();
    } else if (profile_.mode == Mode::TwoPointOne) {
        derivedLfePosition_ = profile_.output.indexOf(Channel::LowFrequency);
        derivedLfeLowPass_.configure(LinkwitzRiley4::Type::LowPass, profile_.sampleRate,
                                     profile_.twoPointOne.lfeLowPassHz);
        derivedLfeLowPass_.reset();
    }

    // Bass management. En 2.1 la somme vers le LFE est déjà faite par le mode :
    // on ne la double pas, seul le passe-haut des satellites reste disponible.
    sumToLfeAllowed_ = profile_.mode != Mode::TwoPointOne;
    lfePosition_ = profile_.output.indexOf(Channel::LowFrequency);
    for (std::uint32_t pos = 0; pos < profile_.output.count; ++pos) {
        const Channel c = profile_.output.order[pos];
        isSatellite_[pos] = (c != Channel::LowFrequency);
        satelliteHighPass_[pos].configure(LinkwitzRiley4::Type::HighPass, profile_.sampleRate,
                                          profile_.bassManagement.crossoverHz);
        satelliteLowPass_[pos].configure(LinkwitzRiley4::Type::LowPass, profile_.sampleRate,
                                         profile_.bassManagement.crossoverHz);
        satelliteHighPass_[pos].reset();
        satelliteLowPass_[pos].reset();
    }

    const std::uint32_t rampSamples = msToSamples(static_cast<float>(profile_.gainRampMs), profile_.sampleRate);
    sumToLfeGain_.prepare(dbToLinear(profile_.bassManagement.sumToLfeGainDb), rampSamples);

    // EQ, délais, gains.
    const std::uint32_t maxDelay = msToSamples(profile_.maxDelayMs, profile_.sampleRate);
    for (std::uint32_t pos = 0; pos < profile_.output.count; ++pos) {
        const Channel c = profile_.output.order[pos];
        for (auto& b : eq_[pos]) b.reset();
        delays_[pos].prepare(maxDelay);
        channelGain_[pos].prepare(channelLinearGain(profile_, c), rampSamples);
    }
    masterGain_.prepare(dbToLinear(profile_.masterGainDb), rampSamples);
    limiter_.prepare(profile_.sampleRate, profile_.limiter.ceilingDb, profile_.limiter.releaseMs);

    // Générateur de test.
    testPhase_ = 0.0;
    testPosition_ = profile_.output.indexOf(profile_.test.channel);

    applyHotParameters();
    resetStats();
    prepared_ = true;
    return true;
}

bool Pipeline::setProfile(const Profile& profile) noexcept {
    if (!prepared_ || !profile_.isStructurallyCompatibleWith(profile)) return false;
    if(modeSwitchPending_) { queuedProfile_ = profile;return true; }
    if(profile_.mode != profile.mode) {
        queuedProfile_ = profile;
        modeSwitchPending_ = true;
        fadeOutRemaining_ = std::max(1u, profile_.sampleRate / 100);
        modeFade_.setTarget(0.f);
        return true;
    }
    return applyProfile(profile);
}

bool Pipeline::applyProfile(const Profile& profile) noexcept {
    const bool modeChanged = profile_.mode != profile.mode;
    const bool routingChanged = modeChanged || profile_.upmix != profile.upmix ||
        profile_.downmix != profile.downmix || profile_.twoPointOne != profile.twoPointOne;
    const bool derivedChanged = modeChanged || profile_.upmix.deriveLfe != profile.upmix.deriveLfe ||
        profile_.upmix.lfeLowPassHz != profile.upmix.lfeLowPassHz ||
        profile_.twoPointOne.lfeLowPassHz != profile.twoPointOne.lfeLowPassHz;
    const bool bassChanged = modeChanged || profile_.bassManagement.enabled != profile.bassManagement.enabled ||
        profile_.bassManagement.crossoverHz != profile.bassManagement.crossoverHz ||
        profile_.bassManagement.highPassSatellites != profile.bassManagement.highPassSatellites ||
        profile_.bassManagement.sumToLfe != profile.bassManagement.sumToLfe;
    profile_ = profile;
    // La matrice est déjà dimensionnée : clear/set ne réallouent jamais.
    if(routingChanged) buildRouting();
    if (derivedChanged) {
        derivedLfePosition_ = -1;
        float cutoff = profile_.upmix.lfeLowPassHz;
        if ((profile_.mode == Mode::Upmix1to6 || profile_.mode == Mode::Upmix2to6 || profile_.mode == Mode::Upmix6to6) && profile_.upmix.deriveLfe)
            derivedLfePosition_ = profile_.output.indexOf(Channel::LowFrequency);
        else if (profile_.mode == Mode::TwoPointOne) {
            derivedLfePosition_ = profile_.output.indexOf(Channel::LowFrequency);
            cutoff = profile_.twoPointOne.lfeLowPassHz;
        }
        if (derivedLfePosition_ >= 0) {
            derivedLfeLowPass_.configure(LinkwitzRiley4::Type::LowPass, profile_.sampleRate, cutoff);
            derivedLfeLowPass_.reset();
        }
    }
    sumToLfeAllowed_ = profile_.mode != Mode::TwoPointOne;
    if (bassChanged) {
        for (std::uint32_t pos = 0; pos < profile_.output.count; ++pos) {
            satelliteHighPass_[pos].configure(LinkwitzRiley4::Type::HighPass, profile_.sampleRate, profile_.bassManagement.crossoverHz);
            satelliteLowPass_[pos].configure(LinkwitzRiley4::Type::LowPass, profile_.sampleRate, profile_.bassManagement.crossoverHz);
            satelliteHighPass_[pos].reset();
            satelliteLowPass_[pos].reset();
        }
    }
    applyHotParameters();
    return true;
}

void Pipeline::applyHotParameters() noexcept {
    for (std::uint32_t pos = 0; pos < profile_.output.count; ++pos) {
        const Channel c = profile_.output.order[pos];
        const auto ci = static_cast<std::size_t>(c);
        channelGain_[pos].setTarget(channelLinearGain(profile_, c));
        delays_[pos].setDelay(msToSamples(profile_.channelDelayMs[ci], profile_.sampleRate));
        applyEq(pos);
    }
    masterGain_.setTarget(dbToLinear(profile_.masterGainDb));
    limiter_.setEnabled(profile_.limiter.enabled);
    limiter_.setParameters(profile_.sampleRate, profile_.limiter.ceilingDb, profile_.limiter.releaseMs);
    sumToLfeGain_.setTarget(dbToLinear(profile_.bassManagement.sumToLfeGainDb));

    testPhaseIncrement_ = 2.0 * std::numbers::pi * profile_.test.frequencyHz / profile_.sampleRate;
    testLevel_ = dbToLinear(profile_.test.levelDb);
    testPosition_ = profile_.output.indexOf(profile_.test.channel);
}

void Pipeline::applyEq(std::uint32_t pos) noexcept {
    const Channel c = profile_.output.order[pos];
    const auto& bands = profile_.eq[static_cast<std::size_t>(c)];
    for (std::size_t b = 0; b < kEqBandsPerChannel; ++b) {
        const EqBand& band = bands[b];
        if(eqSettingsValid_[pos] && appliedEq_[pos][b] == band) continue;
        appliedEq_[pos][b] = band;
        const bool active = band.enabled && band.gainDb != 0.f && band.frequencyHz > 0.f &&
                            band.frequencyHz < profile_.sampleRate * 0.5f && band.q > 0.f;
        if (active) {
            eq_[pos][b].setCoefficients(BiquadCoefficients::peaking(
                profile_.sampleRate, band.frequencyHz, band.q, band.gainDb));
        } else if (eqActive_[pos][b]) {
            eq_[pos][b].setCoefficients(BiquadCoefficients::identity());
            eq_[pos][b].reset();
        }
        eqActive_[pos][b] = active;
    }
    eqSettingsValid_[pos] = true;
}

void Pipeline::buildRouting() noexcept {
    routing_.clear();
    const auto& in = profile_.input;
    const auto& out = profile_.output;

    switch (profile_.mode) {
    case Mode::PassThrough:
        routing_.setIdentity();
        break;

    case Mode::Upmix1to6:
    case Mode::Upmix2to6: {
        const auto L = profile_.mode == Mode::Upmix1to6 ? 0u : static_cast<std::uint32_t>(in.indexOf(Channel::FrontLeft));
        const auto R = profile_.mode == Mode::Upmix1to6 ? 0u : static_cast<std::uint32_t>(in.indexOf(Channel::FrontRight));
        const float gC = dbToLinear(profile_.upmix.centerGainDb);
        const float gS = dbToLinear(profile_.upmix.surroundGainDb);
        const float gLfe = profile_.upmix.deriveLfe ? dbToLinear(profile_.upmix.lfeGainDb) : 0.f;

        auto setOut = [&](Channel c, std::uint32_t input, float g) {
            const int pos = out.indexOf(c);
            if (pos >= 0) routing_.set(static_cast<std::uint32_t>(pos), input, routing_.get(static_cast<std::uint32_t>(pos), input) + g);
        };
        setOut(Channel::FrontLeft, L, 1.f);
        setOut(Channel::FrontRight, R, 1.f);
        setOut(Channel::FrontCenter, L, 0.5f * gC);
        setOut(Channel::FrontCenter, R, 0.5f * gC);
        setOut(Channel::LowFrequency, L, 0.5f * gLfe); // filtré passe-bas ensuite
        setOut(Channel::LowFrequency, R, 0.5f * gLfe);
        setOut(out.surroundLeft(), L, gS);
        setOut(out.surroundRight(), R, gS);
        break;
    }

    case Mode::Downmix6to2: {
        const auto Lo = static_cast<std::uint32_t>(out.indexOf(Channel::FrontLeft));
        const auto Ro = static_cast<std::uint32_t>(out.indexOf(Channel::FrontRight));
        const float head = dbToLinear(profile_.downmix.headroomDb);
        const float gC = dbToLinear(profile_.downmix.centerGainDb) * head;
        const float gS = dbToLinear(profile_.downmix.surroundGainDb) * head;
        const float gLfe = profile_.downmix.includeLfe ? dbToLinear(profile_.downmix.lfeGainDb) * head : 0.f;

        auto add = [&](std::uint32_t o, Channel c, float g) {
            const int i = in.indexOf(c);
            if (i >= 0) routing_.set(o, static_cast<std::uint32_t>(i), g);
        };
        add(Lo, Channel::FrontLeft, head);
        add(Ro, Channel::FrontRight, head);
        add(Lo, Channel::FrontCenter, gC);
        add(Ro, Channel::FrontCenter, gC);
        add(Lo, in.surroundLeft(), gS);
        add(Ro, in.surroundRight(), gS);
        add(Lo, Channel::LowFrequency, gLfe);
        add(Ro, Channel::LowFrequency, gLfe);
        break;
    }

    case Mode::TwoPointOne: {
        const auto Lo = static_cast<std::uint32_t>(out.indexOf(Channel::FrontLeft));
        const auto Ro = static_cast<std::uint32_t>(out.indexOf(Channel::FrontRight));
        const auto LfeOut = static_cast<std::uint32_t>(out.indexOf(Channel::LowFrequency));
        const float gLfe = dbToLinear(profile_.twoPointOne.lfeGainDb);

        if (in.isStereo()) {
            const auto L = static_cast<std::uint32_t>(in.indexOf(Channel::FrontLeft));
            const auto R = static_cast<std::uint32_t>(in.indexOf(Channel::FrontRight));
            routing_.set(Lo, L, 1.f);
            routing_.set(Ro, R, 1.f);
            routing_.set(LfeOut, L, 0.5f * gLfe); // filtré passe-bas ensuite
            routing_.set(LfeOut, R, 0.5f * gLfe);
        } else {
            // Entrée 5.1 : downmix BS.775 vers l'avant, puis LFE dérivé de ce downmix,
            // plus le LFE d'origine si demandé.
            const float head = dbToLinear(profile_.downmix.headroomDb);
            const float gC = dbToLinear(profile_.downmix.centerGainDb) * head;
            const float gS = dbToLinear(profile_.downmix.surroundGainDb) * head;
            auto add = [&](std::uint32_t o, Channel c, float g) {
                const int i = in.indexOf(c);
                if (i < 0) return;
                const auto idx = static_cast<std::uint32_t>(i);
                routing_.set(o, idx, routing_.get(o, idx) + g);
            };
            add(Lo, Channel::FrontLeft, head);
            add(Ro, Channel::FrontRight, head);
            add(Lo, Channel::FrontCenter, gC);
            add(Ro, Channel::FrontCenter, gC);
            add(Lo, in.surroundLeft(), gS);
            add(Ro, in.surroundRight(), gS);
            // LFE dérivé = 0.5 * (Ldm + Rdm) * gLfe
            add(LfeOut, Channel::FrontLeft, 0.5f * head * gLfe);
            add(LfeOut, Channel::FrontRight, 0.5f * head * gLfe);
            add(LfeOut, Channel::FrontCenter, gC * gLfe); // 2 x 0.5 x gC
            add(LfeOut, in.surroundLeft(), 0.5f * gS * gLfe);
            add(LfeOut, in.surroundRight(), 0.5f * gS * gLfe);
            if (profile_.twoPointOne.includeSourceLfe)
                add(LfeOut, Channel::LowFrequency, dbToLinear(profile_.twoPointOne.sourceLfeGainDb));
        }
        // Centre et surrounds : lignes laissées à zéro.
        break;
    }

    case Mode::ChannelTest:
        // Aucune entrée utilisée.
        break;

    case Mode::Upmix6to6: {
        // Préserve tous les canaux (identité) puis ajoute l'étalement des avants vers le
        // centre et les surrounds. Une source stéréo mixée en 6 canaux (FC/SL/SR vides)
        // voit donc son centre et ses surrounds se remplir ; un vrai 5.1 est préservé + élargi.
        routing_.setIdentity();
        const float gC = dbToLinear(profile_.upmix.centerGainDb);
        const float gS = dbToLinear(profile_.upmix.surroundGainDb);
        const float gLfe = profile_.upmix.deriveLfe ? dbToLinear(profile_.upmix.lfeGainDb) : 0.f;
        auto spread = [&](Channel oc, Channel ic, float g) {
            const int o = out.indexOf(oc);
            const int i = in.indexOf(ic);
            if (o >= 0 && i >= 0) {
                const auto ou = static_cast<std::uint32_t>(o);
                const auto iu = static_cast<std::uint32_t>(i);
                routing_.set(ou, iu, routing_.get(ou, iu) + g);
            }
        };
        spread(Channel::FrontCenter, Channel::FrontLeft, 0.5f * gC);
        spread(Channel::FrontCenter, Channel::FrontRight, 0.5f * gC);
        spread(out.surroundLeft(), Channel::FrontLeft, gS);
        spread(out.surroundRight(), Channel::FrontRight, gS);
        // Caisson : (FL+FR) vers le LFE, filtré passe-bas ensuite (lfeLowPassHz).
        spread(Channel::LowFrequency, Channel::FrontLeft, 0.5f * gLfe);
        spread(Channel::LowFrequency, Channel::FrontRight, 0.5f * gLfe);
        break;
    }

    case Mode::DownmixStereo6to6: {
        // Downmix stéréo ITU-R BS.775 replié sur l'avant gauche/droite : miroir 6->6 de
        // Downmix6to2, mais la sortie reste au layout 5.1. Centre, LFE et surrounds restent
        // à zéro (lignes laissées vides par clear()) : l'ampli câblé en 5.1 n'entend que
        // ses enceintes avant, comme un vrai 2.0.
        const auto Lo = static_cast<std::uint32_t>(out.indexOf(Channel::FrontLeft));
        const auto Ro = static_cast<std::uint32_t>(out.indexOf(Channel::FrontRight));
        const float head = dbToLinear(profile_.downmix.headroomDb);
        const float gC = dbToLinear(profile_.downmix.centerGainDb) * head;
        const float gS = dbToLinear(profile_.downmix.surroundGainDb) * head;
        const float gLfe = profile_.downmix.includeLfe ? dbToLinear(profile_.downmix.lfeGainDb) * head : 0.f;

        auto add = [&](std::uint32_t o, Channel c, float g) {
            const int i = in.indexOf(c);
            if (i >= 0) routing_.set(o, static_cast<std::uint32_t>(i), g);
        };
        // FL = head*FL + gC*FC + gS*SL (+ gLfe*LFE) ; FR symétrique (FR, FC, SR, LFE).
        add(Lo, Channel::FrontLeft, head);
        add(Ro, Channel::FrontRight, head);
        add(Lo, Channel::FrontCenter, gC);
        add(Ro, Channel::FrontCenter, gC);
        add(Lo, in.surroundLeft(), gS);
        add(Ro, in.surroundRight(), gS);
        add(Lo, Channel::LowFrequency, gLfe);
        add(Ro, Channel::LowFrequency, gLfe);
        break;
    }
    }
}

void Pipeline::resetStats() noexcept {
    stats_ = PipelineStats{};
    limiter_.resetStats();
}

bool Pipeline::process(ConstInterleavedView in, InterleavedView out) noexcept {
    if(!prepared_ || !in.data || !out.data || in.channels != profile_.input.count ||
       out.channels != profile_.output.count || in.frames != out.frames || in.frames > maxFrames_) return false;
    std::uint32_t offset = 0;
    while(offset < in.frames) {
        if(modeSwitchPending_ && fadeOutRemaining_ == 0) {
            applyProfile(queuedProfile_);
            modeSwitchPending_ = false;
            modeFade_.setTarget(1.f);
        }
        const auto count = modeSwitchPending_ ? std::min(in.frames - offset, fadeOutRemaining_) : in.frames - offset;
        ConstInterleavedView input{in.data + static_cast<std::size_t>(offset) * in.channels, in.channels, count};
        InterleavedView output{out.data + static_cast<std::size_t>(offset) * out.channels, out.channels, count};
        if(!processBlock(input, output)) return false;
        if(modeFade_.isRamping() || modeFade_.current() != 1.f) {
            for(std::uint32_t frame = 0; frame < count; ++frame) {
                const float gain = modeFade_.next();
                for(std::uint32_t channel = 0; channel < out.channels; ++channel) output.at(frame, channel) *= gain;
            }
        }
        if(modeSwitchPending_) fadeOutRemaining_ -= count;
        offset += count;
    }
    // Crêtes réelles, après le fondu de transition.
    for(std::uint32_t frame = 0; frame < out.frames; ++frame)
        for(std::uint32_t channel = 0; channel < out.channels; ++channel) {
            const float peak = std::fabs(out.at(frame, channel));
            if(peak > stats_.outputPeak[channel]) stats_.outputPeak[channel] = peak;
        }
    return true;
}

bool Pipeline::processBlock(ConstInterleavedView in, InterleavedView out) noexcept {
    if (!prepared_) return false;
    if (in.channels != profile_.input.count || out.channels != profile_.output.count) return false;
    if (in.frames != out.frames || in.frames > maxFrames_) return false;
    if (in.data == nullptr || out.data == nullptr) return false;

    const std::uint32_t frames = in.frames;
    const std::uint32_t outCh = out.channels;

    // 1. Routage vers le layout de sortie (ou génération de test).
    if (profile_.mode == Mode::ChannelTest) {
        for (std::uint32_t f = 0; f < frames; ++f) {
            float* dst = out.data + static_cast<std::size_t>(f) * outCh;
            for (std::uint32_t c = 0; c < outCh; ++c) dst[c] = 0.f;
            if (testPosition_ >= 0) {
                dst[testPosition_] = testLevel_ * static_cast<float>(std::sin(testPhase_));
                testPhase_ += testPhaseIncrement_;
                if (testPhase_ >= 2.0 * std::numbers::pi) testPhase_ -= 2.0 * std::numbers::pi;
            }
        }
    } else {
        routing_.process(in, out);
    }

    // 2. Dérivation LFE (upmix, 2.1) : passe-bas sur la somme déjà pondérée.
    if (derivedLfePosition_ >= 0) {
        for (std::uint32_t f = 0; f < frames; ++f) {
            float& s = out.at(f, static_cast<std::uint32_t>(derivedLfePosition_));
            s = derivedLfeLowPass_.process(s);
        }
    }

    // 3. Bass management.
    if (profile_.bassManagement.enabled) {
        const bool sum = sumToLfeAllowed_ && profile_.bassManagement.sumToLfe && lfePosition_ >= 0;
        const bool hp = profile_.bassManagement.highPassSatellites;
        for (std::uint32_t f = 0; f < frames; ++f) {
            float* frame = out.data + static_cast<std::size_t>(f) * outCh;
            float low = 0.f;
            for (std::uint32_t c = 0; c < outCh; ++c) {
                if (!isSatellite_[c]) continue;
                const float x = frame[c];
                if (sum) low += satelliteLowPass_[c].process(x);
                if (hp) frame[c] = satelliteHighPass_[c].process(x);
            }
            if (sum) frame[lfePosition_] += low * sumToLfeGain_.next();
        }
    }

    // 4. EQ, 5. délais, 6. gains par canal, 7. gain maître.
    for (std::uint32_t f = 0; f < frames; ++f) {
        float* frame = out.data + static_cast<std::size_t>(f) * outCh;
        const float master = masterGain_.next();
        for (std::uint32_t c = 0; c < outCh; ++c) {
            float x = frame[c];
            for (std::size_t b = 0; b < kEqBandsPerChannel; ++b) {
                if (eqActive_[c][b]) x = eq_[c][b].process(x);
            }
            x = delays_[c].process(x);
            x *= channelGain_[c].next() * master;
            frame[c] = x;
            stats_.preLimiterPeak = std::max(stats_.preLimiterPeak, std::fabs(x));
        }
    }

    // 8. Limiteur + écrêtage de sécurité.
    if(profile_.outputProtection) limiter_.process(out);

    // Statistiques.
    stats_.clippedSamples = limiter_.totalClips();
    stats_.limiterMinGainDb = linearToDb(limiter_.minimumGainLinear());
    stats_.framesProcessed += frames;
    return true;
}

} // namespace xonar::dsp
