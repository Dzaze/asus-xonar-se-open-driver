// Codec de profils v1 (C3) : analyse stricte, validation schéma + sémantique,
// normalisation aux défauts C++, migration v0 non destructive et compilation
// vers les profils DSP distincts (routage SFX / sortie EFX).
#pragma once

#include "json_value.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <xonar/contracts/v1/contracts.hpp>
#include <xonar/dsp/profile.hpp>

namespace xonar::audio_control::profiles {

struct Diagnostic {
    std::string code;
    std::string path;
    std::string message;
};

// ---------------------------------------------------------------------------
// Analyse + validation + normalisation d'un document v1
// ---------------------------------------------------------------------------

struct ProfileParseOutcome {
    bool ok = false;
    std::string errorCode;   // code JSON (json.*) en cas d'échec d'analyse
    std::string errorMessage;
    std::vector<Diagnostic> diagnostics; // diagnostics de champ (schéma/sémantique)
    contracts::v1::ProfileDocument document; // document normalisé quand ok
};

// Valide un document JSON v1. Si `sampleRate` est fourni, applique la règle
// Nyquist ; sinon validation DSP portable uniquement.
ProfileParseOutcome parseAndValidateProfile(std::string_view jsonUtf8,
                                            std::optional<std::uint32_t> sampleRate = std::nullopt);

// Sérialise un document normalisé vers sa forme filaire JSON v1.
std::string serializeProfile(const contracts::v1::ProfileDocument& document);

// ---------------------------------------------------------------------------
// Migration v0 → v1 (non destructive : l'entrée n'est jamais modifiée)
// ---------------------------------------------------------------------------

struct MigrationOutcome {
    bool ok = false;
    std::string errorCode;
    std::string errorMessage;
    std::vector<Diagnostic> diagnostics;
    contracts::v1::ProfileDocument document;
};

MigrationOutcome migrateV0ToV1(std::string_view v0JsonUtf8);

// ---------------------------------------------------------------------------
// Compilation vers les profils DSP (routage SFX / sortie EFX)
// ---------------------------------------------------------------------------

struct CompileInputs {
    std::uint32_t sampleRate = 48000;
    std::uint32_t sfxInputMask = contracts::v1::kMask51Side;  // masque de flux négocié
    std::uint32_t sfxOutputMask = contracts::v1::kMask51Side; // sortie SFX 6 canaux
    std::uint32_t efxInputMask = contracts::v1::kMask51Side;  // mix 5.1
    std::uint32_t efxOutputMask = contracts::v1::kMask51Side; // sortie finale 5.1
};

struct CompiledProfiles {
    dsp::Profile sfx; // routage avant mixage (RoutingStage)
    dsp::Profile efx; // compensation de sortie (Pipeline)
};

// Compile un document v1 normalisé. Le document doit avoir été validé au
// préalable ; la fréquence et les layouts viennent de la connexion négociée.
CompiledProfiles compileProfiles(const contracts::v1::ProfileDocument& document,
                                 const CompileInputs& inputs);

} // namespace xonar::audio_control::profiles
