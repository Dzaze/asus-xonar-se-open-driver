#include "profile_codec.hpp"

#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_set>

namespace xonar::audio_control::profiles {

namespace {

using json::Object;
using json::Value;
using contracts::v1::Channel;
using contracts::v1::EndpointRole;
using contracts::v1::LimiterSettings;
using contracts::v1::ListeningMode;
using contracts::v1::ProfileDocument;
using contracts::v1::SourcePolicy;

constexpr std::size_t kChannels = 8;
constexpr std::size_t kBands = 10;

void addDiag(std::vector<Diagnostic>& diags, std::string code, std::string path, std::string message) {
    diags.push_back(Diagnostic{std::move(code), std::move(path), std::move(message)});
}

bool hasField(const Object& obj, std::string_view key) { return obj.find(key) != nullptr; }

const Value* requiredField(const Object& obj, std::string_view key, std::string_view path,
                           std::vector<Diagnostic>& diags) {
    const Value* v = obj.find(key);
    if (v == nullptr) {
        addDiag(diags, "schema.required", std::string(path) + "/" + std::string(key),
                "Champ obligatoire manquant.");
        return nullptr;
    }
    return v;
}

bool rejectUnknown(const Object& obj, const std::unordered_set<std::string>& allowed,
                   std::string_view path, std::vector<Diagnostic>& diags) {
    for (const auto& member : obj.members) {
        if (allowed.find(member.key) == allowed.end()) {
            addDiag(diags, "schema.additionalProperties", std::string(path) + "/" + member.key,
                    "Propriété inconnue (schéma v1 : additionalProperties false).");
            return false;
        }
    }
    return true;
}

bool readString(const Value& v, std::string_view path, std::vector<Diagnostic>& diags,
                std::string& out) {
    if (!v.isString()) {
        addDiag(diags, "schema.type", std::string(path), "Chaîne attendue.");
        return false;
    }
    out = v.string;
    return true;
}

bool readBoolField(const Object& obj, std::string_view key, std::string_view path,
                   std::vector<Diagnostic>& diags, bool defaultValue, bool& out) {
    const Value* v = obj.find(key);
    if (v == nullptr) { out = defaultValue; return true; }
    if (!v->isBool()) {
        addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Booléen attendu.");
        return false;
    }
    out = v->boolean;
    return true;
}

// Compte les points de code UTF-8 (un octet de continuation 10xxxxxx ne compte pas),
// pour mesurer name/description comme le schema (maxLength en points de code) et non
// en octets. Aligne le comptage C++ sur le validateur C# (conf#8).
std::size_t utf8CodepointCount(const std::string& str) {
    std::size_t n = 0;
    for (unsigned char c : str) {
        if ((c & 0xC0) != 0x80) ++n;
    }
    return n;
}

bool readNumberField(const Object& obj, std::string_view key, std::string_view path,
                     std::vector<Diagnostic>& diags, double defaultValue, double min, double max,
                     double& out) {
    const Value* v = obj.find(key);
    if (v == nullptr) { out = defaultValue; return true; }
    if (!v->isNumber() || std::isnan(v->number) || std::isinf(v->number)) {
        addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Nombre fini attendu.");
        return false;
    }
    if (v->number < min) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Valeur sous la borne minimale (%.3f).", min);
        addDiag(diags, "schema.minimum", std::string(path) + "/" + std::string(key), buf);
        return false;
    }
    if (v->number > max) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "Valeur au-dessus de la borne maximale (%.3f).", max);
        addDiag(diags, "schema.maximum", std::string(path) + "/" + std::string(key), buf);
        return false;
    }
    out = v->number;
    return true;
}

bool readUIntField(const Object& obj, std::string_view key, std::string_view path,
                   std::vector<Diagnostic>& diags, std::uint32_t defaultValue,
                   std::uint32_t min, std::uint32_t max, std::uint32_t& out) {
    const Value* v = obj.find(key);
    if (v == nullptr) { out = defaultValue; return true; }
    if (!v->isNumber() || !v->isInteger || v->number < 0 || v->number > 4294967295.0) {
        addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Entier non négatif attendu.");
        return false;
    }
    const auto u = static_cast<std::uint32_t>(v->uintValue);
    if (u < min) {
        addDiag(diags, "schema.minimum", std::string(path) + "/" + std::string(key), "Entier sous la borne minimale.");
        return false;
    }
    if (u > max) {
        addDiag(diags, "schema.maximum", std::string(path) + "/" + std::string(key), "Entier au-dessus de la borne maximale.");
        return false;
    }
    out = u;
    return true;
}

bool readU64Field(const Object& obj, std::string_view key, std::string_view path,
                  std::vector<Diagnostic>& diags, bool& ok, std::uint64_t& out) {
    const Value* v = obj.find(key);
    if (v == nullptr) { addDiag(diags, "schema.required", std::string(path) + "/" + std::string(key), "Champ obligatoire manquant."); return false; }
    if (!v->isNumber() || !v->isInteger || v->number < 0 || v->number > 18446744073709551615.0) {
        addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Entier non négatif attendu.");
        return false;
    }
    out = v->uintValue;
    ok = true;
    return true;
}

bool readDoubleArray(const Object& obj, std::string_view key, std::string_view path,
                     std::vector<Diagnostic>& diags, std::size_t length, double min, double max,
                     std::array<double, 8>& target) {
    const Value* v = obj.find(key);
    if (v == nullptr) return true; // défauts déjà posés
    if (!v->isArray()) {
        addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Tableau attendu.");
        return false;
    }
    if (v->array.size() != length) {
        addDiag(diags, v->array.size() < length ? "schema.minItems" : "schema.maxItems",
                std::string(path) + "/" + std::string(key), "Tableau de taille exacte attendue.");
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        const Value& item = v->array[i];
        if (!item.isNumber() || std::isnan(item.number) || std::isinf(item.number)) {
            addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Nombre fini attendu.");
            return false;
        }
        if (item.number < min) {
            addDiag(diags, "schema.minimum", std::string(path) + "/" + std::string(key), "Valeur sous la borne minimale.");
            return false;
        }
        if (item.number > max) {
            addDiag(diags, "schema.maximum", std::string(path) + "/" + std::string(key), "Valeur au-dessus de la borne maximale.");
            return false;
        }
        target[i] = item.number;
    }
    return true;
}

bool readBoolArray(const Object& obj, std::string_view key, std::string_view path,
                   std::vector<Diagnostic>& diags, std::size_t length, std::array<bool, 8>& target) {
    const Value* v = obj.find(key);
    if (v == nullptr) return true;
    if (!v->isArray()) {
        addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Tableau attendu.");
        return false;
    }
    if (v->array.size() != length) {
        addDiag(diags, v->array.size() < length ? "schema.minItems" : "schema.maxItems",
                std::string(path) + "/" + std::string(key), "Tableau de taille exacte attendue.");
        return false;
    }
    for (std::size_t i = 0; i < length; ++i) {
        if (!v->array[i].isBool()) {
            addDiag(diags, "schema.type", std::string(path) + "/" + std::string(key), "Booléen attendu.");
            return false;
        }
        target[i] = v->array[i].boolean;
    }
    return true;
}

bool belowNyquist(double frequencyHz, std::uint32_t sampleRate) {
    return frequencyHz < sampleRate / 2.0;
}

bool validateUpmix(const Object& obj, std::string_view path, std::vector<Diagnostic>& diags,
                   contracts::v1::UpmixSettings& out) {
    static const std::unordered_set<std::string> allowed{
        "centerGainDb", "surroundGainDb", "deriveLfe", "lfeGainDb", "lfeLowPassHz"};
    if (!rejectUnknown(obj, allowed, path, diags)) return false;

    return readNumberField(obj, "centerGainDb", path, diags, -3.0, -30, 0, out.centerGainDb)
        && readNumberField(obj, "surroundGainDb", path, diags, -6.0, -30, 0, out.surroundGainDb)
        && readBoolField(obj, "deriveLfe", path, diags, true, out.deriveLfe)
        && readNumberField(obj, "lfeGainDb", path, diags, -6.0, -30, 0, out.lfeGainDb)
        && readNumberField(obj, "lfeLowPassHz", path, diags, 120.0, 40, 250, out.lfeLowPassHz);
}

bool validateDownmix(const Object& obj, std::string_view path, std::vector<Diagnostic>& diags,
                     contracts::v1::DownmixSettings& out) {
    static const std::unordered_set<std::string> allowed{
        "centerGainDb", "surroundGainDb", "includeLfe", "lfeGainDb", "headroomDb"};
    if (!rejectUnknown(obj, allowed, path, diags)) return false;

    return readNumberField(obj, "centerGainDb", path, diags, -3.0, -30, 0, out.centerGainDb)
        && readNumberField(obj, "surroundGainDb", path, diags, -3.0, -30, 0, out.surroundGainDb)
        && readBoolField(obj, "includeLfe", path, diags, false, out.includeLfe)
        && readNumberField(obj, "lfeGainDb", path, diags, -6.0, -30, 0, out.lfeGainDb)
        && readNumberField(obj, "headroomDb", path, diags, -4.5, -12, 0, out.headroomDb);
}

bool validateTwoPointOne(const Object& obj, std::string_view path, std::vector<Diagnostic>& diags,
                         std::optional<std::uint32_t> sampleRate, contracts::v1::TwoPointOneSettings& out) {
    static const std::unordered_set<std::string> allowed{
        "lfeGainDb", "lfeLowPassHz", "includeSourceLfe", "sourceLfeGainDb"};
    if (!rejectUnknown(obj, allowed, path, diags)) return false;

    if (!readNumberField(obj, "lfeGainDb", path, diags, 0.0, -30, 6, out.lfeGainDb)) return false;
    if (!readNumberField(obj, "lfeLowPassHz", path, diags, 80.0, 40, 200, out.lfeLowPassHz)) return false;
    if (sampleRate && !belowNyquist(out.lfeLowPassHz, *sampleRate)) {
        addDiag(diags, "semantic.nyquist", std::string(path) + "/lfeLowPassHz", "Fréquence non strictement sous Nyquist.");
        return false;
    }
    if (!readBoolField(obj, "includeSourceLfe", path, diags, true, out.includeSourceLfe)) return false;
    if (!readNumberField(obj, "sourceLfeGainDb", path, diags, 0.0, -30, 6, out.sourceLfeGainDb)) return false;
    return true;
}

bool validateBassManagement(const Object& obj, std::string_view path, std::vector<Diagnostic>& diags,
                            std::optional<std::uint32_t> sampleRate, contracts::v1::BassManagementSettings& out) {
    static const std::unordered_set<std::string> allowed{
        "enabled", "crossoverHz", "highPassSatellites", "sumToLfe", "sumToLfeGainDb"};
    if (!rejectUnknown(obj, allowed, path, diags)) return false;

    if (!readBoolField(obj, "enabled", path, diags, false, out.enabled)) return false;
    if (!readNumberField(obj, "crossoverHz", path, diags, 80.0, 40, 200, out.crossoverHz)) return false;
    if (sampleRate && !belowNyquist(out.crossoverHz, *sampleRate)) {
        addDiag(diags, "semantic.nyquist", std::string(path) + "/crossoverHz", "Fréquence non strictement sous Nyquist.");
        return false;
    }
    if (!readBoolField(obj, "highPassSatellites", path, diags, true, out.highPassSatellites)) return false;
    if (!readBoolField(obj, "sumToLfe", path, diags, true, out.sumToLfe)) return false;
    if (!readNumberField(obj, "sumToLfeGainDb", path, diags, 0.0, -20, 6, out.sumToLfeGainDb)) return false;
    return true;
}

bool validateLimiter(const Object& obj, std::string_view path, std::vector<Diagnostic>& diags,
                     contracts::v1::LimiterSettings& out) {
    static const std::unordered_set<std::string> allowed{"enabled", "ceilingDb", "releaseMs"};
    if (!rejectUnknown(obj, allowed, path, diags)) return false;

    if (!readBoolField(obj, "enabled", path, diags, true, out.enabled)) return false;
    if (!readNumberField(obj, "ceilingDb", path, diags, -0.3, -12, 0, out.ceilingDb)) return false;
    if (!readNumberField(obj, "releaseMs", path, diags, 80.0, 5, 1000, out.releaseMs)) return false;
    return true;
}

bool validateEq(const Object& obj, std::string_view path, std::vector<Diagnostic>& diags,
                std::optional<std::uint32_t> sampleRate,
                std::array<std::array<contracts::v1::EqBand, kBands>, kChannels>& eq) {
    const Value* v = obj.find("eq");
    if (v == nullptr) return true;
    if (!v->isArray()) {
        addDiag(diags, "schema.type", std::string(path) + "/eq", "eq doit etre un tableau.");
        return false;
    }
    if (v->array.size() != kChannels) {
        addDiag(diags, v->array.size() < kChannels ? "schema.minItems" : "schema.maxItems",
                std::string(path) + "/eq", "eq doit contenir 8 canaux.");
        return false;
    }

    static const std::unordered_set<std::string> allowed{"enabled", "frequencyHz", "q", "gainDb"};
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        const Value& bandArray = v->array[ch];
        if (!bandArray.isArray()) {
            addDiag(diags, "schema.type", std::string(path) + "/eq", "Chaque canal doit etre un tableau.");
            return false;
        }
        if (bandArray.array.size() != kBands) {
            addDiag(diags, bandArray.array.size() < kBands ? "schema.minItems" : "schema.maxItems",
                    std::string(path) + "/eq", "Chaque canal doit contenir 10 bandes.");
            return false;
        }
        for (std::size_t b = 0; b < kBands; ++b) {
            const Value& bandValue = bandArray.array[b];
            if (!bandValue.isObject()) {
                addDiag(diags, "schema.type", std::string(path) + "/eq", "Bande EQ : objet attendu.");
                return false;
            }
            const Object& band = bandValue.object;
            if (!rejectUnknown(band, allowed, std::string(path) + "/eq", diags)) return false;

            auto& out = eq[ch][b];
            if (!readBoolField(band, "enabled", path, diags, false, out.enabled)) return false;
            if (!readNumberField(band, "frequencyHz", path, diags, 1000.0, 10, 20000, out.frequencyHz)) return false;
            if (sampleRate && !belowNyquist(out.frequencyHz, *sampleRate)) {
                addDiag(diags, "semantic.nyquist", std::string(path) + "/eq", "Fréquence d'EQ non strictement sous Nyquist.");
                return false;
            }
            if (!readNumberField(band, "q", path, diags, 1.0, 0.0, 20, out.q)) return false;
            if (out.q <= 0.0) {
                addDiag(diags, "schema.exclusiveMinimum", std::string(path) + "/eq", "q doit être strictement positif.");
                return false;
            }
            if (!readNumberField(band, "gainDb", path, diags, 0.0, -24, 12, out.gainDb)) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Sérialisation JSON v1
// ---------------------------------------------------------------------------

struct JsonWriter {
    std::string out;

    void raw(std::string_view s) { out.append(s); }
    void stringValue(std::string_view s) {
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
            }
        }
        out.push_back('"');
    }
    void number(double d) {
        char buf[64];
        if (std::floor(d) == d && std::isfinite(d) && std::fabs(d) < 1e15) {
            std::snprintf(buf, sizeof(buf), "%.0f", d);
        } else {
            std::snprintf(buf, sizeof(buf), "%.17g", d);
        }
        out += buf;
    }
    void boolean(bool b) { out += b ? "true" : "false"; }
};

void writeEq(JsonWriter& w, const ProfileDocument& doc) {
    w.raw("\"eq\":[");
    for (std::size_t ch = 0; ch < kChannels; ++ch) {
        if (ch) w.raw(",");
        w.raw("[");
        for (std::size_t b = 0; b < kBands; ++b) {
            if (b) w.raw(",");
            const auto& band = doc.output.eq[ch][b];
            w.raw("{\"enabled\":");
            w.boolean(band.enabled);
            w.raw(",\"frequencyHz\":");
            w.number(band.frequencyHz);
            w.raw(",\"q\":");
            w.number(band.q);
            w.raw(",\"gainDb\":");
            w.number(band.gainDb);
            w.raw("}");
        }
        w.raw("]");
    }
    w.raw("]");
}

} // namespace

// ---------------------------------------------------------------------------
// API publique
// ---------------------------------------------------------------------------

ProfileParseOutcome parseAndValidateProfile(std::string_view jsonUtf8,
                                            std::optional<std::uint32_t> sampleRate) {
    ProfileParseOutcome outcome;
    auto parsed = json::parse(jsonUtf8);
    if (!parsed.ok) {
        outcome.errorCode = std::move(parsed.errorCode);
        outcome.errorMessage = std::move(parsed.errorMessage);
        return outcome;
    }

    const Value& root = parsed.root;
    if (!root.isObject()) {
        addDiag(outcome.diagnostics, "schema.type", "$", "Le document racine doit être un objet.");
        return outcome;
    }

    const Object& obj = root.object;
    static const std::unordered_set<std::string> topAllowed{
        "schemaVersion", "profileId", "revision", "name", "description",
        "endpointRole", "listeningMode", "sourcePolicy", "routing", "output"};
    if (!rejectUnknown(obj, topAllowed, "$", outcome.diagnostics)) return outcome;

    auto& doc = outcome.document;

    // schemaVersion
    const Value* schemaVersion = requiredField(obj, "schemaVersion", "$", outcome.diagnostics);
    if (schemaVersion == nullptr) return outcome;
    if (!schemaVersion->isNumber() || !schemaVersion->isInteger || schemaVersion->uintValue != 1) {
        addDiag(outcome.diagnostics, "version.unsupportedMajor", "$/schemaVersion",
                "Version majeure inconnue : seule la version 1 est acceptée.");
        return outcome;
    }
    doc.schemaVersion = 1;

    // profileId
    const Value* profileId = requiredField(obj, "profileId", "$", outcome.diagnostics);
    if (profileId == nullptr) return outcome;
    std::string profileIdString;
    if (!readString(*profileId, "$/profileId", outcome.diagnostics, profileIdString)) return outcome;
    // Pattern UUID canonique minuscules (validation simplifiée mais stricte).
    static const std::string uuidChars = "0123456789abcdef-";
    bool uuidOk = profileIdString.size() == 36;
    for (std::size_t i = 0; uuidOk && i < profileIdString.size(); ++i) {
        if (uuidChars.find(profileIdString[i]) == std::string::npos) uuidOk = false;
    }
    if (!uuidOk) {
        addDiag(outcome.diagnostics, "schema.pattern", "$/profileId", "UUID canonique minuscule attendu.");
        return outcome;
    }
    doc.profileId = profileIdString;

    // revision
    std::uint64_t revision = 0;
    bool revisionOk = false;
    readU64Field(obj, "revision", "$", outcome.diagnostics, revisionOk, revision);
    if (!revisionOk || revision < 1) {
        addDiag(outcome.diagnostics, "schema.minimum", "$/revision", "revision doit être ≥ 1.");
        return outcome;
    }
    doc.revision = revision;

    // name / description
    const Value* name = obj.find("name");
    if (name != nullptr) {
        if (!name->isString()) {
            addDiag(outcome.diagnostics, "schema.type", "$/name", "name doit etre une chaine.");
            return outcome;
        }
        if (name->string.empty()) {
            addDiag(outcome.diagnostics, "schema.minLength", "$/name", "name : 1 caractere minimum.");
            return outcome;
        }
        if (utf8CodepointCount(name->string) > 80) {
            addDiag(outcome.diagnostics, "schema.maxLength", "$/name", "name : 80 caracteres maximum.");
            return outcome;
        }
        doc.name = name->string;
    }
    const Value* description = obj.find("description");
    if (description != nullptr) {
        if (!description->isString()) {
            addDiag(outcome.diagnostics, "schema.type", "$/description", "description doit etre une chaine.");
            return outcome;
        }
        if (utf8CodepointCount(description->string) > 500) {
            addDiag(outcome.diagnostics, "schema.maxLength", "$/description", "description : 500 caracteres maximum.");
            return outcome;
        }
        doc.description = description->string;
    }

    // endpointRole
    const Value* role = requiredField(obj, "endpointRole", "$", outcome.diagnostics);
    if (role == nullptr) return outcome;
    std::string roleString;
    if (!readString(*role, "$/endpointRole", outcome.diagnostics, roleString)) return outcome;
    if (roleString != "speakers") {
        addDiag(outcome.diagnostics, "schema.enum", "$/endpointRole", "endpointRole doit valoir « speakers ».");
        return outcome;
    }
    doc.endpointRole = EndpointRole::Speakers;

    // listeningMode
    const Value* mode = requiredField(obj, "listeningMode", "$", outcome.diagnostics);
    if (mode == nullptr) return outcome;
    std::string modeString;
    if (!readString(*mode, "$/listeningMode", outcome.diagnostics, modeString)) return outcome;
    if (!contracts::v1::fromWire(modeString, doc.listeningMode)) {
        addDiag(outcome.diagnostics, "schema.enum", "$/listeningMode", "Mode d'écoute inconnu.");
        return outcome;
    }

    // sourcePolicy
    const Value* policy = requiredField(obj, "sourcePolicy", "$", outcome.diagnostics);
    if (policy == nullptr) return outcome;
    std::string policyString;
    if (!readString(*policy, "$/sourcePolicy", outcome.diagnostics, policyString)) return outcome;
    if (!contracts::v1::fromWire(policyString, doc.sourcePolicy)) {
        addDiag(outcome.diagnostics, "schema.enum", "$/sourcePolicy", "Politique de source inconnue.");
        return outcome;
    }

    // routing
    const Value* routing = requiredField(obj, "routing", "$", outcome.diagnostics);
    if (routing == nullptr) return outcome;
    if (!routing->isObject()) {
        addDiag(outcome.diagnostics, "schema.type", "$/routing", "routing doit être un objet.");
        return outcome;
    }
    {
        static const std::unordered_set<std::string> routingAllowed{"upmix"};
        if (!rejectUnknown(routing->object, routingAllowed, "$/routing", outcome.diagnostics)) return outcome;
        const Value* upmix = routing->object.find("upmix");
        if (upmix != nullptr) {
            if (!upmix->isObject()) {
                addDiag(outcome.diagnostics, "schema.type", "$/routing/upmix", "routing.upmix doit être un objet.");
                return outcome;
            }
            if (!validateUpmix(upmix->object, "$/routing/upmix", outcome.diagnostics, doc.routing.upmix)) return outcome;
        }
    }

    // output
    const Value* output = requiredField(obj, "output", "$", outcome.diagnostics);
    if (output == nullptr) return outcome;
    if (!output->isObject()) {
        addDiag(outcome.diagnostics, "schema.type", "$/output", "output doit être un objet.");
        return outcome;
    }
    {
        const Object& outObj = output->object;
        static const std::unordered_set<std::string> outputAllowed{
            "masterGainDb", "channelGainDb", "channelDelayMs", "channelMute", "eq",
            "downmix", "twoPointOne", "bassManagement", "limiter", "gainRampMs", "maxDelayMs"};
        if (!rejectUnknown(outObj, outputAllowed, "$/output", outcome.diagnostics)) return outcome;

        if (!readNumberField(outObj, "masterGainDb", "$/output", outcome.diagnostics, 0.0, -60, 6, doc.output.masterGainDb)) return outcome;
        if (!readDoubleArray(outObj, "channelGainDb", "$/output", outcome.diagnostics, kChannels, -60, 12, doc.output.channelGainDb)) return outcome;
        if (!readDoubleArray(outObj, "channelDelayMs", "$/output", outcome.diagnostics, kChannels, 0, 50, doc.output.channelDelayMs)) return outcome;
        if (!readBoolArray(outObj, "channelMute", "$/output", outcome.diagnostics, kChannels, doc.output.channelMute)) return outcome;

        if (!readNumberField(outObj, "maxDelayMs", "$/output", outcome.diagnostics, 50.0, 0, 50, doc.output.maxDelayMs)) return outcome;
        for (std::size_t i = 0; i < kChannels; ++i) {
            if (doc.output.channelDelayMs[i] > doc.output.maxDelayMs) {
                addDiag(outcome.diagnostics, "semantic.delayExceedsMax", "$/output/channelDelayMs",
                        "Délai supérieur à maxDelayMs.");
                return outcome;
            }
        }

        if (!validateEq(outObj, "$/output", outcome.diagnostics, sampleRate, doc.output.eq)) return outcome;

        const Value* downmix = outObj.find("downmix");
        if (downmix != nullptr) {
            if (!downmix->isObject()) {
                addDiag(outcome.diagnostics, "schema.type", "$/output/downmix", "downmix doit être un objet.");
                return outcome;
            }
            if (!validateDownmix(downmix->object, "$/output/downmix", outcome.diagnostics, doc.output.downmix)) return outcome;
        }

        const Value* twoPointOne = outObj.find("twoPointOne");
        if (twoPointOne != nullptr) {
            if (!twoPointOne->isObject()) {
                addDiag(outcome.diagnostics, "schema.type", "$/output/twoPointOne", "twoPointOne doit être un objet.");
                return outcome;
            }
            if (!validateTwoPointOne(twoPointOne->object, "$/output/twoPointOne", outcome.diagnostics, sampleRate, doc.output.twoPointOne)) return outcome;
        }

        const Value* bass = outObj.find("bassManagement");
        if (bass != nullptr) {
            if (!bass->isObject()) {
                addDiag(outcome.diagnostics, "schema.type", "$/output/bassManagement", "bassManagement doit être un objet.");
                return outcome;
            }
            if (!validateBassManagement(bass->object, "$/output/bassManagement", outcome.diagnostics, sampleRate, doc.output.bassManagement)) return outcome;
        }

        const Value* limiter = outObj.find("limiter");
        if (limiter != nullptr) {
            if (!limiter->isObject()) {
                addDiag(outcome.diagnostics, "schema.type", "$/output/limiter", "limiter doit être un objet.");
                return outcome;
            }
            if (!validateLimiter(limiter->object, "$/output/limiter", outcome.diagnostics, doc.output.limiter)) return outcome;
        }

        std::uint32_t gainRamp = 20;
        if (!readUIntField(outObj, "gainRampMs", "$/output", outcome.diagnostics, 20, 1, 500, gainRamp)) return outcome;
        doc.output.gainRampMs = gainRamp;
    }

    outcome.ok = true;
    return outcome;
}

std::string serializeProfile(const contracts::v1::ProfileDocument& doc) {
    JsonWriter w;
    w.raw("{\"schemaVersion\":1");
    w.raw(",\"profileId\":");
    w.stringValue(doc.profileId);
    w.raw(",\"revision\":");
    w.number(static_cast<double>(doc.revision));
    if (doc.name) { w.raw(",\"name\":"); w.stringValue(*doc.name); }
    if (doc.description) { w.raw(",\"description\":"); w.stringValue(*doc.description); }
    w.raw(",\"endpointRole\":\"speakers\"");
    w.raw(",\"listeningMode\":");
    w.stringValue(contracts::v1::toWire(doc.listeningMode));
    w.raw(",\"sourcePolicy\":");
    w.stringValue(contracts::v1::toWire(doc.sourcePolicy));

    w.raw(",\"routing\":{\"upmix\":{");
    w.raw("\"centerGainDb\":");
    w.number(doc.routing.upmix.centerGainDb);
    w.raw(",\"surroundGainDb\":");
    w.number(doc.routing.upmix.surroundGainDb);
    w.raw(",\"deriveLfe\":");
    w.boolean(doc.routing.upmix.deriveLfe);
    w.raw(",\"lfeGainDb\":");
    w.number(doc.routing.upmix.lfeGainDb);
    w.raw(",\"lfeLowPassHz\":");
    w.number(doc.routing.upmix.lfeLowPassHz);
    w.raw("}}");

    w.raw(",\"output\":{\"masterGainDb\":");
    w.number(doc.output.masterGainDb);
    w.raw(",\"channelGainDb\":[");
    for (std::size_t i = 0; i < kChannels; ++i) { if (i) w.raw(","); w.number(doc.output.channelGainDb[i]); }
    w.raw("],\"channelDelayMs\":[");
    for (std::size_t i = 0; i < kChannels; ++i) { if (i) w.raw(","); w.number(doc.output.channelDelayMs[i]); }
    w.raw("],\"channelMute\":[");
    for (std::size_t i = 0; i < kChannels; ++i) { if (i) w.raw(","); w.boolean(doc.output.channelMute[i]); }
    w.raw("],");
    writeEq(w, doc);
    w.raw(",\"downmix\":{");
    w.raw("\"centerGainDb\":"); w.number(doc.output.downmix.centerGainDb);
    w.raw(",\"surroundGainDb\":"); w.number(doc.output.downmix.surroundGainDb);
    w.raw(",\"includeLfe\":"); w.boolean(doc.output.downmix.includeLfe);
    w.raw(",\"lfeGainDb\":"); w.number(doc.output.downmix.lfeGainDb);
    w.raw(",\"headroomDb\":"); w.number(doc.output.downmix.headroomDb);
    w.raw("},\"twoPointOne\":{");
    w.raw("\"lfeGainDb\":"); w.number(doc.output.twoPointOne.lfeGainDb);
    w.raw(",\"lfeLowPassHz\":"); w.number(doc.output.twoPointOne.lfeLowPassHz);
    w.raw(",\"includeSourceLfe\":"); w.boolean(doc.output.twoPointOne.includeSourceLfe);
    w.raw(",\"sourceLfeGainDb\":"); w.number(doc.output.twoPointOne.sourceLfeGainDb);
    w.raw("},\"bassManagement\":{");
    w.raw("\"enabled\":"); w.boolean(doc.output.bassManagement.enabled);
    w.raw(",\"crossoverHz\":"); w.number(doc.output.bassManagement.crossoverHz);
    w.raw(",\"highPassSatellites\":"); w.boolean(doc.output.bassManagement.highPassSatellites);
    w.raw(",\"sumToLfe\":"); w.boolean(doc.output.bassManagement.sumToLfe);
    w.raw(",\"sumToLfeGainDb\":"); w.number(doc.output.bassManagement.sumToLfeGainDb);
    w.raw("},\"limiter\":{");
    w.raw("\"enabled\":"); w.boolean(doc.output.limiter.enabled);
    w.raw(",\"ceilingDb\":"); w.number(doc.output.limiter.ceilingDb);
    w.raw(",\"releaseMs\":"); w.number(doc.output.limiter.releaseMs);
    w.raw("},\"gainRampMs\":");
    w.number(static_cast<double>(doc.output.gainRampMs));
    w.raw(",\"maxDelayMs\":");
    w.number(doc.output.maxDelayMs);
    w.raw("}}");
    return w.out;
}

} // namespace xonar::audio_control::profiles
