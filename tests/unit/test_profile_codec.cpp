// Tests du codec de profils v1 (E2-01) : analyse stricte, validation schéma et
// sémantique, migration v0 non destructive, round-trip 8 canaux / 80 bandes,
// comparaison aux fabriques DSP existantes. Tout s'exécute sans carte.
#include "test_framework.hpp"

#include <cstdio>
#include <string>

#include <xonar/contracts/v1/contracts.hpp>
#include <xonar/dsp/profile.hpp>

#include "profile_codec.hpp"

using namespace xonar::audio_control::profiles;
namespace dsp = xonar::dsp;             // fabriques et constantes de référence (profile.hpp)
namespace contracts = xonar::contracts; // constantes de contrat v1 (contracts.hpp)

namespace {

// ---------------------------------------------------------------------------
// Fixtures v0 (les quatre exemples du dépôt, copiés en lecture seule)
// ---------------------------------------------------------------------------

constexpr const char* kUpmixV0 = R"({
  "$schema": "../profiles/profile.schema.json",
  "name": "Upmix stéréo vers 5.1",
  "description": "Source stéréo redistribuée sur 5.1 (layout latéral 0x60F) : avant intact, centre (L+R)/2 à -3 dB, surrounds à -6 dB, LFE dérivé par passe-bas 120 Hz à -6 dB. Réglages conservateurs, pas de prétention spatiale.",
  "mode": "Upmix2to6",
  "sampleRate": 48000,
  "inputMask": 3,
  "outputMask": 1551,
  "masterGainDb": 0,
  "channelGainDb": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelDelayMs": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelMute": [false, false, false, false, false, false, false, false],
  "upmix": { "centerGainDb": -3, "surroundGainDb": -6, "deriveLfe": true, "lfeGainDb": -6, "lfeLowPassHz": 120 },
  "bassManagement": { "enabled": false, "crossoverHz": 80, "highPassSatellites": true, "sumToLfe": true, "sumToLfeGainDb": 0 },
  "limiter": { "enabled": true, "ceilingDb": -0.3, "releaseMs": 80 },
  "gainRampMs": 20,
  "maxDelayMs": 50
})";

constexpr const char* kPassThroughV0 = R"({
  "$schema": "../profiles/profile.schema.json",
  "name": "Pass-through 5.1",
  "description": "Source 5.1 native (layout latéral Windows 0x60F) vers sortie 5.1 : canaux et niveaux préservés, limiteur de sécurité actif.",
  "mode": "PassThrough",
  "sampleRate": 48000,
  "inputMask": 1551,
  "outputMask": 1551,
  "masterGainDb": 0,
  "channelGainDb": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelDelayMs": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelMute": [false, false, false, false, false, false, false, false],
  "bassManagement": { "enabled": false, "crossoverHz": 80, "highPassSatellites": true, "sumToLfe": true, "sumToLfeGainDb": 0 },
  "limiter": { "enabled": true, "ceilingDb": -0.3, "releaseMs": 80 },
  "gainRampMs": 20,
  "maxDelayMs": 50
})";

constexpr const char* kTwoPointOneV0 = R"JSON({
  "$schema": "../profiles/profile.schema.json",
  "name": "2.1 virtuel (Windows et ampli restent en 5.1)",
  "description": "Mode 2.1 fabriqué par le DSP sur le câblage 5.1.",
  "mode": "TwoPointOne",
  "sampleRate": 48000,
  "inputMask": 3,
  "outputMask": 1551,
  "masterGainDb": 0,
  "channelGainDb": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelDelayMs": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelMute": [false, false, false, false, false, false, false, false],
  "twoPointOne": { "lfeGainDb": 0, "lfeLowPassHz": 80, "includeSourceLfe": true, "sourceLfeGainDb": 0 },
  "downmix": { "centerGainDb": -3, "surroundGainDb": -3, "includeLfe": false, "lfeGainDb": -6, "headroomDb": -4.5 },
  "bassManagement": { "enabled": false, "crossoverHz": 80, "highPassSatellites": true, "sumToLfe": false, "sumToLfeGainDb": 0 },
  "limiter": { "enabled": true, "ceilingDb": -0.3, "releaseMs": 80 },
  "gainRampMs": 20,
  "maxDelayMs": 50
})JSON";

constexpr const char* kDownmixV0 = R"({
  "$schema": "../profiles/profile.schema.json",
  "name": "Ampli en position 2.1 — downmix 5.1 vers stéréo",
  "description": "Downmix ITU-R BS.775 : centre et surrounds à -3 dB, LFE ignoré, headroom -4.5 dB, limiteur actif.",
  "mode": "Downmix6to2",
  "sampleRate": 48000,
  "inputMask": 1551,
  "outputMask": 3,
  "masterGainDb": 0,
  "channelGainDb": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelDelayMs": [0, 0, 0, 0, 0, 0, 0, 0],
  "channelMute": [false, false, false, false, false, false, false, false],
  "downmix": { "centerGainDb": -3, "surroundGainDb": -3, "includeLfe": false, "lfeGainDb": -6, "headroomDb": -4.5 },
  "bassManagement": { "enabled": false, "crossoverHz": 80, "highPassSatellites": true, "sumToLfe": true, "sumToLfeGainDb": 0 },
  "limiter": { "enabled": true, "ceilingDb": -0.3, "releaseMs": 80 },
  "gainRampMs": 20,
  "maxDelayMs": 50
})";

std::string buildFullProfile() {
    std::string s;
    s.reserve(16384);
    s += "{\"schemaVersion\":1,\"profileId\":\"a4f2240f-e1cf-45c0-9012-bdffdf283425\",\"revision\":42,";
    s += "\"endpointRole\":\"speakers\",\"listeningMode\":\"upmix\",\"sourcePolicy\":\"preserve\",";
    s += "\"routing\":{\"upmix\":{\"centerGainDb\":-4.5,\"surroundGainDb\":-9,\"deriveLfe\":false,\"lfeGainDb\":-12,\"lfeLowPassHz\":100}},";
    s += "\"output\":{\"masterGainDb\":-6,\"channelGainDb\":[1,-1,0.5,6,-60,12,-3,-3],";
    s += "\"channelDelayMs\":[0,0.1,1,2.5,50,49.99,10,10],";
    s += "\"channelMute\":[false,false,false,false,true,true,false,false],\"eq\":[";
    for (int ch = 0; ch < 8; ++ch) {
        if (ch) s += ",";
        s += "[";
        for (int b = 0; b < 10; ++b) {
            if (b) s += ",";
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "{\"enabled\":%s,\"frequencyHz\":%.2f,\"q\":%.2f,\"gainDb\":%.1f}",
                (b % 2 == 0) ? "true" : "false",
                100.0 + ch * 500 + b * 10, 0.5 + b * 0.25, -12.0 + ch + b);
            s += buf;
        }
        s += "]";
    }
    s += "],\"downmix\":{},\"twoPointOne\":{},\"bassManagement\":{},\"limiter\":{},\"gainRampMs\":20,\"maxDelayMs\":50}}";
    return s;
}

} // namespace

TEST_CASE("codec : clés dupliquées refusées")
{
    auto outcome = parseAndValidateProfile(R"({"schemaVersion":1,"schemaVersion":1})");
    CHECK(!outcome.ok);
    CHECK(outcome.errorCode == "json.duplicateKey");
}

TEST_CASE("codec : document > 128 Kio refusé")
{
    std::string big = "{\"padding\":\"";
    big.append(140000, 'x');
    big += "\"}";
    auto outcome = parseAndValidateProfile(big);
    CHECK(!outcome.ok);
    CHECK(outcome.errorCode == "json.tooLarge");
}

TEST_CASE("codec : version majeure inconnue refusée")
{
    auto outcome = parseAndValidateProfile(
        R"({"schemaVersion":2,"profileId":"a4f2240f-e1cf-45c0-9012-bdffdf283425","revision":1,"endpointRole":"speakers","listeningMode":"upmix","sourcePolicy":"preserve","routing":{},"output":{}})");
    CHECK(!outcome.ok);
    CHECK(!outcome.diagnostics.empty());
    CHECK(outcome.diagnostics.front().code == "version.unsupportedMajor");
}

TEST_CASE("codec : propriété inconnue refusée (additionalProperties false)")
{
    auto outcome = parseAndValidateProfile(
        R"({"schemaVersion":1,"profileId":"a4f2240f-e1cf-45c0-9012-bdffdf283425","revision":1,"endpointRole":"speakers","listeningMode":"upmix","sourcePolicy":"preserve","routing":{},"output":{},"cheminArbitraire":"x"})");
    CHECK(!outcome.ok);
}

TEST_CASE("codec : Nyquist et bornes refusés")
{
    // crossoverHz=24000 est refusé par la borne du crossover (40–200 Hz), ce qui le
    // garde aussi sous Nyquist par construction. NB : le contrôle Nyquist dédié
    // (code "semantic.nyquist") reste défensif et inatteignable pour un profil valide :
    // aux fréquences d'échantillonnage supportées (>= 44,1 kHz, donc Nyquist >= 22050 Hz),
    // aucune fréquence admissible (crossover <= 200, EQ <= 20000) ne l'atteint. On vérifie
    // donc ici le refus réel, par la borne max.
    auto nyquist = parseAndValidateProfile(
        R"({"schemaVersion":1,"profileId":"a4f2240f-e1cf-45c0-9012-bdffdf283425","revision":1,"endpointRole":"speakers","listeningMode":"upmix","sourcePolicy":"preserve","routing":{},"output":{"bassManagement":{"crossoverHz":24000}}})",
        std::uint32_t{48000});
    CHECK(!nyquist.ok);
    CHECK(!nyquist.diagnostics.empty());
    CHECK(nyquist.diagnostics.front().code == "schema.maximum");

    // masterGainDb hors bornes.
    auto bounds = parseAndValidateProfile(
        R"({"schemaVersion":1,"profileId":"a4f2240f-e1cf-45c0-9012-bdffdf283425","revision":1,"endpointRole":"speakers","listeningMode":"upmix","sourcePolicy":"preserve","routing":{},"output":{"masterGainDb":100}})");
    CHECK(!bounds.ok);

    // Délai 40 ms > maxDelayMs 30 ms (règle sémantique).
    auto delay = parseAndValidateProfile(
        R"({"schemaVersion":1,"profileId":"a4f2240f-e1cf-45c0-9012-bdffdf283425","revision":1,"endpointRole":"speakers","listeningMode":"upmix","sourcePolicy":"preserve","routing":{},"output":{"channelDelayMs":[40,0,0,0,0,0,0,0],"maxDelayMs":30}})");
    CHECK(!delay.ok);
}

TEST_CASE("codec : 88,2 kHz jamais annoncé par le rendu")
{
    // Les fréquences candidates sont exactement 44,1/48/96/192 kHz.
    const auto& rates = contracts::v1::kXonarRenderSampleRates;
    CHECK(rates.size() == 4);
    for (auto rate : rates) CHECK(rate != 88200 && rate != 176400);

    // La migration d'un profil v0 en 88,2 kHz est refusée.
    std::string v0 = kPassThroughV0;
    const auto pos = v0.find("48000");
    v0.replace(pos, 5, "88200");
    auto migration = migrateV0ToV1(v0);
    CHECK(!migration.ok);
}

TEST_CASE("codec : migration des quatre exemples v0 sans écraser l'original")
{
    struct Case { const char* v0; const char* expectedMode; };
    const Case cases[] = {
        {kUpmixV0, "upmix"},
        {kPassThroughV0, "5.1"},
        {kTwoPointOneV0, "2.1"},
        {kDownmixV0, "2.0"},
    };
    for (const auto& c : cases) {
        const std::string original(c.v0);
        auto migration = migrateV0ToV1(original);
        CHECK(migration.ok);
        CHECK(contracts::v1::toWire(migration.document.listeningMode) == std::string(c.expectedMode));
        CHECK(original == std::string(c.v0)); // entrée jamais modifiée
    }
}

TEST_CASE("codec : round-trip des 8 canaux et 80 bandes")
{
    const std::string json = buildFullProfile();
    auto first = parseAndValidateProfile(json, std::uint32_t{48000});
    REQUIRE(first.ok);

    const std::string serialized = serializeProfile(first.document);
    auto second = parseAndValidateProfile(serialized, std::uint32_t{48000});
    REQUIRE(second.ok);

    for (std::size_t ch = 0; ch < 8; ++ch) {
        CHECK_NEAR(first.document.output.channelGainDb[ch], second.document.output.channelGainDb[ch], 1e-9);
        CHECK_NEAR(first.document.output.channelDelayMs[ch], second.document.output.channelDelayMs[ch], 1e-9);
        CHECK(first.document.output.channelMute[ch] == second.document.output.channelMute[ch]);
        for (std::size_t b = 0; b < 10; ++b) {
            CHECK_NEAR(first.document.output.eq[ch][b].frequencyHz, second.document.output.eq[ch][b].frequencyHz, 1e-9);
            CHECK_NEAR(first.document.output.eq[ch][b].q, second.document.output.eq[ch][b].q, 1e-9);
            CHECK_NEAR(first.document.output.eq[ch][b].gainDb, second.document.output.eq[ch][b].gainDb, 1e-9);
        }
    }
}

TEST_CASE("codec : compilation DSP conforme aux fabriques existantes")
{
    CompileInputs inputs{};
    inputs.sampleRate = 48000;
    inputs.sfxInputMask = contracts::v1::kMaskStereo;

    auto compileMode = [&](const char* modeWire, CompiledProfiles& out) {
        std::string json = std::string(R"({"schemaVersion":1,"profileId":"a4f2240f-e1cf-45c0-9012-bdffdf283425","revision":1,"endpointRole":"speakers","listeningMode":")")
            + modeWire + R"(","sourcePolicy":"preserve","routing":{},"output":{}})";
        auto parsed = parseAndValidateProfile(json, std::uint32_t{48000});
        REQUIRE(parsed.ok);
        out = compileProfiles(parsed.document, inputs);
    };

    CompiledProfiles compiled;
    compileMode("5.1", compiled);
    auto pass = dsp::makePassThroughProfile(48000, dsp::masks::k5Point1Side);
    CHECK(compiled.efx.mode == pass.mode);
    CHECK(compiled.efx.output.mask == pass.output.mask);

    compileMode("upmix", compiled);
    auto upmix = dsp::makeUpmixProfile(48000, dsp::masks::k5Point1Side);
    CHECK(compiled.sfx.mode == dsp::Mode::Upmix2to6);
    CHECK(compiled.sfx.output.mask == upmix.output.mask);

    compileMode("2.1", compiled);
    auto twoPointOne = dsp::makeTwoPointOneProfile(48000, dsp::masks::k5Point1Side, dsp::masks::k5Point1Side);
    CHECK(compiled.efx.mode == twoPointOne.mode);
    CHECK(compiled.efx.output.mask == twoPointOne.output.mask);

    compileMode("2.0", compiled);
    auto downmix = dsp::makeDownmixProfile(48000, dsp::masks::k5Point1Side);
    CHECK(compiled.efx.mode == downmix.mode);
    CHECK(compiled.efx.output.mask == downmix.output.mask);
}
