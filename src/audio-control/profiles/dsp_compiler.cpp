#include "profile_codec.hpp"

namespace xonar::audio_control::profiles {

namespace {

using contracts::v1::ListeningMode;

float asFloat(double v) { return static_cast<float>(v); }

void applyOutputSettings(const contracts::v1::OutputSettings& src, dsp::Profile& dst) {
    dst.masterGainDb = asFloat(src.masterGainDb);
    for (std::size_t i = 0; i < dsp::kMaxChannels; ++i) {
        dst.channelGainDb[i] = asFloat(src.channelGainDb[i]);
        dst.channelDelayMs[i] = asFloat(src.channelDelayMs[i]);
        dst.channelMute[i] = src.channelMute[i];
        for (std::size_t b = 0; b < dsp::kEqBandsPerChannel; ++b) {
            dst.eq[i][b].enabled = src.eq[i][b].enabled;
            dst.eq[i][b].frequencyHz = asFloat(src.eq[i][b].frequencyHz);
            dst.eq[i][b].q = asFloat(src.eq[i][b].q);
            dst.eq[i][b].gainDb = asFloat(src.eq[i][b].gainDb);
        }
    }
    dst.downmix.centerGainDb = asFloat(src.downmix.centerGainDb);
    dst.downmix.surroundGainDb = asFloat(src.downmix.surroundGainDb);
    dst.downmix.includeLfe = src.downmix.includeLfe;
    dst.downmix.lfeGainDb = asFloat(src.downmix.lfeGainDb);
    dst.downmix.headroomDb = asFloat(src.downmix.headroomDb);

    dst.twoPointOne.lfeGainDb = asFloat(src.twoPointOne.lfeGainDb);
    dst.twoPointOne.lfeLowPassHz = asFloat(src.twoPointOne.lfeLowPassHz);
    dst.twoPointOne.includeSourceLfe = src.twoPointOne.includeSourceLfe;
    dst.twoPointOne.sourceLfeGainDb = asFloat(src.twoPointOne.sourceLfeGainDb);

    dst.bassManagement.enabled = src.bassManagement.enabled;
    dst.bassManagement.crossoverHz = asFloat(src.bassManagement.crossoverHz);
    dst.bassManagement.highPassSatellites = src.bassManagement.highPassSatellites;
    dst.bassManagement.sumToLfe = src.bassManagement.sumToLfe;
    dst.bassManagement.sumToLfeGainDb = asFloat(src.bassManagement.sumToLfeGainDb);

    dst.limiter.enabled = src.limiter.enabled;
    dst.limiter.ceilingDb = asFloat(src.limiter.ceilingDb);
    dst.limiter.releaseMs = asFloat(src.limiter.releaseMs);

    dst.gainRampMs = src.gainRampMs;
    dst.maxDelayMs = asFloat(src.maxDelayMs);
}

} // namespace

CompiledProfiles compileProfiles(const contracts::v1::ProfileDocument& doc, const CompileInputs& inputs) {
    CompiledProfiles out;

    const bool neutral = contracts::v1::neutralRoutingOf(doc.listeningMode);

    // --- routage SFX (avant mixage) -----------------------------------------
    if (neutral || doc.listeningMode == ListeningMode::Surround51) {
        // Transport neutre : identité/extension/remappage du RoutingStage.
        out.sfx.mode = dsp::Mode::PassThrough;
        out.sfx.input = dsp::ChannelLayout::fromMask(inputs.sfxInputMask);
        out.sfx.output = dsp::ChannelLayout::fromMask(inputs.sfxOutputMask);
    } else {
        // upmix : matrice 2→6, appliquée uniquement aux flux 2.0 déclarés.
        out.sfx.mode = dsp::Mode::Upmix2to6;
        out.sfx.input = dsp::ChannelLayout::fromMask(contracts::v1::kMaskStereo);
        out.sfx.output = dsp::ChannelLayout::fromMask(inputs.sfxOutputMask);
        out.sfx.upmix.centerGainDb = asFloat(doc.routing.upmix.centerGainDb);
        out.sfx.upmix.surroundGainDb = asFloat(doc.routing.upmix.surroundGainDb);
        out.sfx.upmix.deriveLfe = doc.routing.upmix.deriveLfe;
        out.sfx.upmix.lfeGainDb = asFloat(doc.routing.upmix.lfeGainDb);
        out.sfx.upmix.lfeLowPassHz = asFloat(doc.routing.upmix.lfeLowPassHz);
    }
    out.sfx.sampleRate = inputs.sampleRate;

    // --- sortie EFX (après mixage, 6 canaux) --------------------------------
    out.efx.sampleRate = inputs.sampleRate;
    out.efx.input = dsp::ChannelLayout::fromMask(inputs.efxInputMask);
    switch (doc.listeningMode) {
    case ListeningMode::Surround51:
    case ListeningMode::Upmix:
        out.efx.mode = dsp::Mode::PassThrough;
        out.efx.output = dsp::ChannelLayout::fromMask(inputs.efxOutputMask);
        break;
    case ListeningMode::TwoPointOne:
        out.efx.mode = dsp::Mode::TwoPointOne;
        out.efx.output = dsp::ChannelLayout::fromMask(inputs.efxOutputMask);
        break;
    case ListeningMode::Stereo20:
        out.efx.mode = dsp::Mode::Downmix6to2;
        // La banque Output réétend FL/FR + quatre zéros avant le crossfade ;
        // le dsp::Profile lui-même produit une sortie stéréo (fabrique existante).
        out.efx.output = dsp::ChannelLayout::fromMask(contracts::v1::kMaskStereo);
        break;
    }

    applyOutputSettings(doc.output, out.efx);
    return out;
}

} // namespace xonar::audio_control::profiles
