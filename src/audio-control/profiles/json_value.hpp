// Analyseur JSON strict pour les documents de contrat (C3/C4).
// Propriétés exigées par le backlog :
//   - UTF-8 valide, sans BOM ;
//   - refus des clés dupliquées dans un objet ;
//   - refus de NaN/Infinity et de tout contenu après la valeur racine ;
//   - taille maximale 128 Kio et profondeur maximale 16 (vérifiées par le codec).
// Implémentation maison sans dépendance externe, licence Apache-2.0 comme le dépôt.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xonar::audio_control::profiles::json {

// Types mutuellement récursifs (un objet contient des membres, un membre
// contient une valeur, une valeur peut contenir un objet). L'ordre de
// définition est contraint : un membre de type incomplet par valeur est
// interdit (C2079), mais std::vector<T> admet un T incomplet (C++17+). On
// définit donc Object (vecteur d'ObjectMember incomplet) puis Value (qui a
// Object complet par valeur) puis ObjectMember en dernier (Value complet).
struct Value;
struct ObjectMember;

struct Object {
    std::vector<ObjectMember> members;

    const Value* find(std::string_view key) const noexcept;
    Value* find(std::string_view key) noexcept;
};

struct Value {
    enum class Type : std::uint8_t { Null, Bool, Number, String, Array, Object };

    Type type = Type::Null;
    bool boolean = false;
    double number = 0.0;       // valide quand type == Number
    bool isInteger = false;    // nombre sans partie fractionnaire
    std::uint64_t uintValue = 0; // valide si isInteger et 0 <= number <= 2^64-1
    std::string string;        // valide quand type == String
    std::vector<Value> array;  // valide quand type == Array
    Object object;             // valide quand type == Object

    bool isNull() const noexcept { return type == Type::Null; }
    bool isObject() const noexcept { return type == Type::Object; }
    bool isArray() const noexcept { return type == Type::Array; }
    bool isString() const noexcept { return type == Type::String; }
    bool isNumber() const noexcept { return type == Type::Number; }
    bool isBool() const noexcept { return type == Type::Bool; }
};

struct ObjectMember {
    std::string key;
    Value value;
};

struct ParseResult {
    bool ok = false;
    std::string errorCode;
    std::string errorMessage;
    Value root;
};

// Analyse une chaîne UTF-8. La taille et la profondeur sont bornées ici pour
// que l'analyseur soit sûr même utilisé directement.
ParseResult parse(std::string_view text);

// Vérifie qu'un buffer est de l'UTF-8 valide, sans BOM. Retourne un code
// d'erreur vide si valide.
std::string validateUtf8(std::string_view text);

} // namespace xonar::audio_control::profiles::json
