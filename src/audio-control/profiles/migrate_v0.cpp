#include "profile_codec.hpp"

#include <cmath>

namespace xonar::audio_control::profiles {

namespace {

using json::Object;
using json::Value;

void addMigDiag(std::vector<Diagnostic>& diags, std::string code, std::string message) {
    diags.push_back(Diagnostic{std::move(code), "$", std::move(message)});
}

const Value* findField(const Object& obj, std::string_view key) { return obj.find(key); }

bool readV0String(const Object& obj, std::string_view key, std::string& out) {
    const Value* v = findField(obj, key);
    if (v == nullptr || !v->isString()) return false;
    out = v->string;
    return true;
}

bool readV0Number(const Object& obj, std::string_view key, double& out) {
    const Value* v = findField(obj, key);
    if (v == nullptr || !v->isNumber() || std::isnan(v->number) || std::isinf(v->number)) return false;
    out = v->number;
    return true;
}

bool readV0Bool(const Object& obj, std::string_view key, bool& out) {
    const Value* v = findField(obj, key);
    if (v == nullptr || !v->isBool()) return false;
    out = v->boolean;
    return true;
}

bool validMask(std::uint32_t mask) {
    constexpr std::uint32_t known = contracts::v1::kKnownChannelBits; // 0x63F
    return mask != 0 && (mask & ~known) == 0;
}

} // namespace

MigrationOutcome migrateV0ToV1(std::string_view v0JsonUtf8) {
    MigrationOutcome outcome;
    auto parsed = json::parse(v0JsonUtf8);
    if (!parsed.ok) {
        outcome.errorCode = std::move(parsed.errorCode);
        outcome.errorMessage = std::move(parsed.errorMessage);
        return outcome;
    }

    const Value& root = parsed.root;
    if (!root.isObject()) {
        addMigDiag(outcome.diagnostics, "schema.type", "Document racine : objet attendu.");
        return outcome;
    }

    const Object& obj = root.object;

    // --- mode ----------------------------------------------------------------
    std::string mode;
    if (!readV0String(obj, "mode", mode)) {
        addMigDiag(outcome.diagnostics, "schema.required", "Champ v0 « mode » manquant ou invalide.");
        return outcome;
    }

    std::string listeningMode;
    std::string sourcePolicy;
    std::string uuidSuffix;
    if (mode == "PassThrough") { listeningMode = "5.1"; sourcePolicy = "preserve"; uuidSuffix = "03"; }
    else if (mode == "Upmix2to6") { listeningMode = "upmix"; sourcePolicy = "autoDeclaredLayout"; uuidSuffix = "04"; }
    else if (mode == "Downmix6to2") { listeningMode = "2.0"; sourcePolicy = "preserve"; uuidSuffix = "02"; }
    else if (mode == "TwoPointOne") { listeningMode = "2.1"; sourcePolicy = "preserve"; uuidSuffix = "01"; }
    else if (mode == "ChannelTest") {
        addMigDiag(outcome.diagnostics, "unsupported", "Le mode ChannelTest n'est pas persistant en v1 ; non migré.");
        return outcome;
    } else {
        addMigDiag(outcome.diagnostics, "enum", "Mode v0 inconnu.");
        return outcome;
    }

    // --- sampleRate : 88,2/176,4 jamais annoncés par le rendu Xonar ----------
    double sampleRate = 0;
    if (!readV0Number(obj, "sampleRate", sampleRate)) {
        addMigDiag(outcome.diagnostics, "schema.required", "Champ v0 « sampleRate » manquant.");
        return outcome;
    }
    const bool supported = sampleRate == 44100 || sampleRate == 48000 ||
                           sampleRate == 96000 || sampleRate == 192000;
    if (!supported) {
        addMigDiag(outcome.diagnostics, "endpoint.sampleRateUnsupported",
                   "Fréquence v0 non annoncée par le rendu Xonar (44,1/48/96/192 kHz seulement).");
        return outcome;
    }

    // --- masques -------------------------------------------------------------
    double inputMask = 0;
    double outputMask = 0;
    if (!readV0Number(obj, "inputMask", inputMask) || !readV0Number(obj, "outputMask", outputMask)) {
        addMigDiag(outcome.diagnostics, "schema.required", "Champs v0 « inputMask »/« outputMask » manquants.");
        return outcome;
    }
    if (!validMask(static_cast<std::uint32_t>(inputMask)) ||
        !validMask(static_cast<std::uint32_t>(outputMask))) {
        addMigDiag(outcome.diagnostics, "semantic.mask",
                   "Masque v0 incompatible (bits inconnus) ; non migré silencieusement.");
        return outcome;
    }

    // --- construction du document v1 ----------------------------------------
    auto& doc = outcome.document;
    doc.schemaVersion = 1;
    doc.profileId = "3f1c0a6e-5b7d-4e2a-9c8b-1d2e3f4a5b" + uuidSuffix;
    doc.revision = 1;
    doc.endpointRole = contracts::v1::EndpointRole::Speakers;
    // listeningMode/sourcePolicy proviennent du mapping contrôlé ci-dessus : la
    // conversion réussit toujours. On vérifie tout de même (fromWire est [[nodiscard]])
    // pour ne jamais retenir un défaut en silence si le mapping évoluait.
    if (!contracts::v1::fromWire(listeningMode, doc.listeningMode) ||
        !contracts::v1::fromWire(sourcePolicy, doc.sourcePolicy)) {
        addMigDiag(outcome.diagnostics, "internal.modeMapping",
                   "Mode v0 non converti en mode d'écoute/politique v1 (mapping interne).");
        return outcome;
    }

    if (mode == "Upmix2to6") {
        const Value* upmix = findField(obj, "upmix");
        if (upmix != nullptr && upmix->isObject()) {
            const Object& u = upmix->object;
            readV0Number(u, "centerGainDb", doc.routing.upmix.centerGainDb);
            readV0Number(u, "surroundGainDb", doc.routing.upmix.surroundGainDb);
            readV0Bool(u, "deriveLfe", doc.routing.upmix.deriveLfe);
            readV0Number(u, "lfeGainDb", doc.routing.upmix.lfeGainDb);
            readV0Number(u, "lfeLowPassHz", doc.routing.upmix.lfeLowPassHz);
        }
    }

    readV0String(obj, "name", doc.name.emplace());
    if (doc.name && doc.name->empty()) doc.name.reset();
    readV0String(obj, "description", doc.description.emplace());
    if (doc.description && doc.description->empty()) doc.description.reset();

    readV0Number(obj, "masterGainDb", doc.output.masterGainDb);
    // gainRampMs est un entier (std::uint32_t) côté profil ; readV0Number écrit un
    // double. On lit dans un temporaire et on convertit en bornant à >= 0.
    if (double rampMs = 0.0; readV0Number(obj, "gainRampMs", rampMs) && rampMs >= 0.0) {
        doc.output.gainRampMs = static_cast<std::uint32_t>(rampMs);
    }
    readV0Number(obj, "maxDelayMs", doc.output.maxDelayMs);

    // Le schéma v0 autorisait maxDelayMs jusqu'à 200 ; v1 borne à 50.
    if (doc.output.maxDelayMs > contracts::v1::kMaxDelayMs) {
        addMigDiag(outcome.diagnostics, "semantic.delayExceedsMax",
                   "maxDelayMs v0 dépasse la capacité v1 (50 ms) ; non migré.");
        return outcome;
    }

    const Value* channelGain = findField(obj, "channelGainDb");
    if (channelGain != nullptr && channelGain->isArray()) {
        for (std::size_t i = 0; i < channelGain->array.size() && i < 8; ++i) {
            if (channelGain->array[i].isNumber()) doc.output.channelGainDb[i] = channelGain->array[i].number;
        }
    }
    const Value* channelDelay = findField(obj, "channelDelayMs");
    if (channelDelay != nullptr && channelDelay->isArray()) {
        for (std::size_t i = 0; i < channelDelay->array.size() && i < 8; ++i) {
            if (channelDelay->array[i].isNumber()) doc.output.channelDelayMs[i] = channelDelay->array[i].number;
        }
    }
    const Value* channelMute = findField(obj, "channelMute");
    if (channelMute != nullptr && channelMute->isArray()) {
        for (std::size_t i = 0; i < channelMute->array.size() && i < 8; ++i) {
            if (channelMute->array[i].isBool()) doc.output.channelMute[i] = channelMute->array[i].boolean;
        }
    }

    const Value* downmix = findField(obj, "downmix");
    if (downmix != nullptr && downmix->isObject()) {
        const Object& d = downmix->object;
        readV0Number(d, "centerGainDb", doc.output.downmix.centerGainDb);
        readV0Number(d, "surroundGainDb", doc.output.downmix.surroundGainDb);
        readV0Bool(d, "includeLfe", doc.output.downmix.includeLfe);
        readV0Number(d, "lfeGainDb", doc.output.downmix.lfeGainDb);
        readV0Number(d, "headroomDb", doc.output.downmix.headroomDb);
    }

    const Value* twoPointOne = findField(obj, "twoPointOne");
    if (twoPointOne != nullptr && twoPointOne->isObject()) {
        const Object& t = twoPointOne->object;
        readV0Number(t, "lfeGainDb", doc.output.twoPointOne.lfeGainDb);
        readV0Number(t, "lfeLowPassHz", doc.output.twoPointOne.lfeLowPassHz);
        readV0Bool(t, "includeSourceLfe", doc.output.twoPointOne.includeSourceLfe);
        readV0Number(t, "sourceLfeGainDb", doc.output.twoPointOne.sourceLfeGainDb);
    }

    const Value* bass = findField(obj, "bassManagement");
    if (bass != nullptr && bass->isObject()) {
        const Object& b = bass->object;
        readV0Bool(b, "enabled", doc.output.bassManagement.enabled);
        readV0Number(b, "crossoverHz", doc.output.bassManagement.crossoverHz);
        readV0Bool(b, "highPassSatellites", doc.output.bassManagement.highPassSatellites);
        readV0Bool(b, "sumToLfe", doc.output.bassManagement.sumToLfe);
        readV0Number(b, "sumToLfeGainDb", doc.output.bassManagement.sumToLfeGainDb);
    }

    const Value* limiter = findField(obj, "limiter");
    if (limiter != nullptr && limiter->isObject()) {
        const Object& l = limiter->object;
        readV0Bool(l, "enabled", doc.output.limiter.enabled);
        readV0Number(l, "ceilingDb", doc.output.limiter.ceilingDb);
        readV0Number(l, "releaseMs", doc.output.limiter.releaseMs);
    }

    outcome.ok = true;
    return outcome;
}

} // namespace xonar::audio_control::profiles
