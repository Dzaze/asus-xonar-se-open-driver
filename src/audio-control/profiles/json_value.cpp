#include "json_value.hpp"

#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace xonar::audio_control::profiles::json {

namespace {

constexpr std::size_t kMaxDepth = 16;

std::string utf8Error(std::string_view text) {
    // Validation minimale d'UTF-8 : rejet des séquences invalides, des surrogates
    // encodés et du BOM en tête.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        return "json.encoding";
    }

    std::size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) {
            i += 1;
            continue;
        }
        int needed = 0;
        std::uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { needed = 1; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { needed = 2; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { needed = 3; cp = c & 0x07; }
        else { return "json.encoding"; }

        if (i + needed >= text.size() + 1) return "json.encoding";
        for (int k = 1; k <= needed; ++k) {
            const auto cc = static_cast<unsigned char>(text[i + k]);
            if ((cc & 0xC0) != 0x80) return "json.encoding";
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) return "json.surrogate";
        i += static_cast<std::size_t>(needed) + 1;
    }
    return {};
}

struct Parser {
    const char* p;
    const char* end;
    std::string errorCode;
    std::string errorMessage;

    void fail(std::string code, std::string message) {
        if (errorCode.empty()) {
            errorCode = std::move(code);
            errorMessage = std::move(message);
        }
    }

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool atEnd() const { return p >= end; }

    bool parseValue(Value& out, std::size_t depth) {
        if (depth > kMaxDepth) { fail("json.tooDeep", "Imbrication > 16."); return false; }
        skipWs();
        if (atEnd()) { fail("json.syntax", "Fin inattendue."); return false; }
        switch (*p) {
        case '{': return parseObject(out, depth + 1);
        case '[': return parseArray(out, depth + 1);
        case '"': return parseString(out);
        case 't': case 'f': return parseBool(out);
        case 'n': return parseNull(out);
        default: return parseNumber(out);
        }
    }

    bool parseObject(Value& out, std::size_t depth) {
        ++p; // '{'
        out.type = Value::Type::Object;
        skipWs();
        if (!atEnd() && *p == '}') { ++p; return true; }

        std::unordered_set<std::string> keys;
        while (true) {
            skipWs();
            if (atEnd() || *p != '"') { fail("json.syntax", "Clé d'objet attendue."); return false; }
            Value keyValue;
            if (!parseString(keyValue)) return false;
            const std::string& key = keyValue.string;
            if (!keys.insert(key).second) {
                fail("json.duplicateKey", "Clé dupliquée : " + key);
                return false;
            }
            skipWs();
            if (atEnd() || *p != ':') { fail("json.syntax", "':' attendu."); return false; }
            ++p;
            ObjectMember member;
            member.key = key;
            if (!parseValue(member.value, depth)) return false;
            out.object.members.push_back(std::move(member));
            skipWs();
            if (atEnd()) { fail("json.syntax", "Objet non terminé."); return false; }
            if (*p == ',') { ++p; continue; }
            if (*p == '}') { ++p; return true; }
            fail("json.syntax", "',' ou '}' attendu.");
            return false;
        }
    }

    bool parseArray(Value& out, std::size_t depth) {
        ++p; // '['
        out.type = Value::Type::Array;
        skipWs();
        if (!atEnd() && *p == ']') { ++p; return true; }
        while (true) {
            Value item;
            if (!parseValue(item, depth)) return false;
            out.array.push_back(std::move(item));
            skipWs();
            if (atEnd()) { fail("json.syntax", "Tableau non terminé."); return false; }
            if (*p == ',') { ++p; continue; }
            if (*p == ']') { ++p; return true; }
            fail("json.syntax", "',' ou ']' attendu.");
            return false;
        }
    }

    bool parseString(Value& out) {
        ++p; // '"'
        out.type = Value::Type::String;
        std::string result;
        while (true) {
            if (atEnd()) { fail("json.syntax", "Chaîne non terminée."); return false; }
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"') { ++p; out.string = std::move(result); return true; }
            if (c < 0x20) { fail("json.syntax", "Caractère de contrôle interdit en chaîne."); return false; }
            if (c == '\\') {
                ++p;
                if (atEnd()) { fail("json.syntax", "Échappement incomplet."); return false; }
                switch (*p) {
                case '"': result.push_back('"'); ++p; break;
                case '\\': result.push_back('\\'); ++p; break;
                case '/': result.push_back('/'); ++p; break;
                case 'b': result.push_back('\b'); ++p; break;
                case 'f': result.push_back('\f'); ++p; break;
                case 'n': result.push_back('\n'); ++p; break;
                case 'r': result.push_back('\r'); ++p; break;
                case 't': result.push_back('\t'); ++p; break;
                case 'u': {
                    if (p + 4 >= end) { fail("json.syntax", "Échappement \\u incomplet."); return false; }
                    std::uint32_t cp = 0;
                    for (int i = 1; i <= 4; ++i) {
                        char h = p[i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= static_cast<std::uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= static_cast<std::uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= static_cast<std::uint32_t>(h - 'A' + 10);
                        else { fail("json.syntax", "Chiffre hexadécimal attendu."); return false; }
                    }
                    p += 5;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        // Demi-paire haute : exige une demi-paire basse.
                        if (p + 6 > end || p[0] != '\\' || p[1] != 'u') {
                            fail("json.surrogate", "Demi-paire de substitution isolée.");
                            return false;
                        }
                        std::uint32_t lo = 0;
                        for (int i = 2; i <= 5; ++i) {
                            char h = p[i];
                            lo <<= 4;
                            if (h >= '0' && h <= '9') lo |= static_cast<std::uint32_t>(h - '0');
                            else if (h >= 'a' && h <= 'f') lo |= static_cast<std::uint32_t>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') lo |= static_cast<std::uint32_t>(h - 'A' + 10);
                            else { fail("json.syntax", "Demi-paire basse invalide."); return false; }
                        }
                        if (lo < 0xDC00 || lo > 0xDFFF) { fail("json.surrogate", "Demi-paire basse invalide."); return false; }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                        fail("json.surrogate", "Demi-paire basse isolée.");
                        return false;
                    }
                    // Encode cp en UTF-8.
                    if (cp < 0x80) result.push_back(static_cast<char>(cp));
                    else if (cp < 0x800) {
                        result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    } else {
                        result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
                        result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                        result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default:
                    fail("json.syntax", "Échappement inconnu.");
                    return false;
                }
                continue;
            }
            result.push_back(static_cast<char>(c));
            ++p;
        }
    }

    bool parseBool(Value& out) {
        if (end - p >= 4 && std::string_view(p, 4) == "true") {
            out.type = Value::Type::Bool;
            out.boolean = true;
            p += 4;
            return true;
        }
        if (end - p >= 5 && std::string_view(p, 5) == "false") {
            out.type = Value::Type::Bool;
            out.boolean = false;
            p += 5;
            return true;
        }
        fail("json.syntax", "Littéral booléen invalide.");
        return false;
    }

    bool parseNull(Value& out) {
        if (end - p >= 4 && std::string_view(p, 4) == "null") {
            out.type = Value::Type::Null;
            p += 4;
            return true;
        }
        fail("json.syntax", "Littéral null invalide.");
        return false;
    }

    bool parseNumber(Value& out) {
        const char* start = p;
        if (!atEnd() && *p == '-') ++p;
        if (atEnd()) { fail("json.syntax", "Nombre invalide."); return false; }

        if (*p == '0') { ++p; }
        else if (*p >= '1' && *p <= '9') { while (p < end && *p >= '0' && *p <= '9') ++p; }
        else { fail("json.syntax", "Nombre invalide."); return false; }

        bool hasFraction = false;
        if (!atEnd() && *p == '.') {
            hasFraction = true;
            ++p;
            if (atEnd() || *p < '0' || *p > '9') { fail("json.syntax", "Fraction invalide."); return false; }
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }

        if (!atEnd() && (*p == 'e' || *p == 'E')) {
            ++p;
            if (!atEnd() && (*p == '+' || *p == '-')) ++p;
            if (atEnd() || *p < '0' || *p > '9') { fail("json.syntax", "Exposant invalide."); return false; }
            while (p < end && *p >= '0' && *p <= '9') ++p;
        }

        std::string token(start, p);
        char* numEnd = nullptr;
        double number = std::strtod(token.c_str(), &numEnd);
        if (numEnd != token.c_str() + token.size() || !std::isfinite(number)) {
            fail("json.nonFinite", "Nombre non fini ou hors plage.");
            return false;
        }

        out.type = Value::Type::Number;
        out.number = number;
        out.isInteger = !hasFraction && std::floor(number) == number && std::isfinite(number);
        if (out.isInteger && number >= 0.0 && number <= 18446744073709551615.0) {
            out.uintValue = static_cast<std::uint64_t>(number);
        } else {
            out.uintValue = 0;
        }
        return true;
    }
};

} // namespace

const Value* Object::find(std::string_view key) const noexcept {
    for (const auto& m : members) {
        if (m.key == key) return &m.value;
    }
    return nullptr;
}

Value* Object::find(std::string_view key) noexcept {
    for (auto& m : members) {
        if (m.key == key) return &m.value;
    }
    return nullptr;
}

std::string validateUtf8(std::string_view text) {
    return utf8Error(text);
}

ParseResult parse(std::string_view text) {
    ParseResult result;
    if (text.size() > 131072) {
        result.errorCode = "json.tooLarge";
        result.errorMessage = "Document > 128 Kio.";
        return result;
    }

    std::string utf8 = validateUtf8(text);
    if (!utf8.empty()) {
        result.errorCode = std::move(utf8);
        result.errorMessage = "Encodage UTF-8 invalide.";
        return result;
    }

    Parser parser{text.data(), text.data() + text.size(), {}, {}};
    if (!parser.parseValue(result.root, 1)) {
        result.errorCode = parser.errorCode;
        result.errorMessage = parser.errorMessage;
        return result;
    }
    parser.skipWs();
    if (!parser.atEnd()) {
        result.errorCode = "json.syntax";
        result.errorMessage = "Contenu après la valeur racine.";
        return result;
    }
    result.ok = true;
    return result;
}

} // namespace xonar::audio_control::profiles::json
