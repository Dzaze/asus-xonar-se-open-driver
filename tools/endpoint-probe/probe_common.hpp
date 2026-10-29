// endpoint-probe : déclarations communes (sortie texte/JSON, COM, propriétés, formats,
// devnode, jacks). Aucune fonction déclarée ici ne modifie la configuration audio.
#pragma once

// Ordre imposé : les en-têtes COM (qui tirent cguid.h) avant ks.h, sinon la macro
// GUID_NULL de ks.h casse la déclaration de cguid.h.
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <wrl/client.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace probe {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Codes de retour communs.
// ---------------------------------------------------------------------------
inline constexpr int kExitOk = 0;
inline constexpr int kExitError = 1;
inline constexpr int kExitNoXonar = 2;

// ---------------------------------------------------------------------------
// Arbre de valeurs à double rendu : JSON (clés stables, ASCII) et texte (libellés
// français). Chaque membre d'objet porte donc une clé ET un libellé, et peut être
// limité à l'un des deux rendus.
// ---------------------------------------------------------------------------
class Json {
public:
    enum class Kind { Null, Bool, Int, UInt, Real, String, Array, Object };
    enum class Show { Both, JsonOnly, TextOnly };
    struct Member;

    Json() = default;
    Json(const char* s) : kind_(Kind::String), str_(s ? s : "") {}
    Json(std::string s) : kind_(Kind::String), str_(std::move(s)) {}
    template <class T>
        requires std::is_arithmetic_v<T>
    Json(T v) {
        if constexpr (std::is_same_v<T, bool>) {
            kind_ = Kind::Bool;
            b_ = v;
        } else if constexpr (std::is_floating_point_v<T>) {
            kind_ = Kind::Real;
            d_ = static_cast<double>(v);
        } else if constexpr (std::is_signed_v<T>) {
            kind_ = Kind::Int;
            i_ = static_cast<std::int64_t>(v);
        } else {
            kind_ = Kind::UInt;
            u_ = static_cast<std::uint64_t>(v);
        }
    }

    static Json object();
    static Json array();
    // Réel avec un nombre de décimales fixé pour le rendu texte.
    static Json real(double v, int decimals);

    Kind kind() const { return kind_; }
    bool isObject() const { return kind_ == Kind::Object; }

    // Objets.
    Json& add(std::string key, std::string label, Json value, Show show = Show::Both);
    Json& addJson(std::string key, Json value) { return add(std::move(key), {}, std::move(value), Show::JsonOnly); }
    Json& addText(std::string label, Json value) { return add({}, std::move(label), std::move(value), Show::TextOnly); }
    // Tableaux.
    Json& push(Json value);
    std::size_t size() const;

    std::string toJson(int indent = 0) const;
    // Rendu texte indenté (libellé : valeur).
    void toText(std::string& out, int indent) const;
    std::string scalarText() const;

private:
    void jsonTo(std::string& out, int indent, int level) const;

    Kind kind_ = Kind::Null;
    bool b_ = false;
    std::int64_t i_ = 0;
    std::uint64_t u_ = 0;
    double d_ = 0.0;
    int decimals_ = -1;
    std::string str_;
    std::vector<Json> items_;
    std::vector<Member> members_;
};

struct Json::Member {
    std::string key;
    std::string label;
    Json value;
    Show show = Show::Both;
};

// ---------------------------------------------------------------------------
// Chaînes et erreurs.
// ---------------------------------------------------------------------------
std::string toUtf8(const std::wstring& s);
std::string toUtf8(const wchar_t* s);
std::wstring fromUtf8(const std::string& s);
std::wstring lower(std::wstring s);
bool containsNoCase(const std::wstring& haystack, const std::wstring& needle);
std::string hrText(HRESULT hr);
std::string hex32(std::uint32_t v);
std::string guidText(const GUID& g);
std::string timestampNow();
void printOut(const std::string& s); // stdout, UTF-8, ajoute un saut de ligne
void printErr(const std::string& s); // stderr

// Initialisation COM du thread courant (MTA), libérée à la destruction.
class ComScope {
public:
    ComScope();
    ~ComScope();
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
    HRESULT hr() const { return hr_; }

private:
    HRESULT hr_;
};

// ---------------------------------------------------------------------------
// Propriétés d'un IPropertyStore.
// ---------------------------------------------------------------------------
std::optional<std::wstring> propString(IPropertyStore* store, const PROPERTYKEY& key);
std::optional<std::uint32_t> propUInt(IPropertyStore* store, const PROPERTYKEY& key);
std::optional<std::vector<std::uint8_t>> propBlob(IPropertyStore* store, const PROPERTYKEY& key);
std::string propertyKeyName(const PROPERTYKEY& key);
std::string propertyValueText(IPropertyStore* store, const PROPERTYKEY& key);

// ---------------------------------------------------------------------------
// Formats et canaux.
// ---------------------------------------------------------------------------
std::string speakerList(std::uint32_t mask);           // "FL FR FC LFE"
std::string layoutName(std::uint32_t mask);            // "5.1 (latéral)" ou ""
std::uint32_t defaultMaskFor(unsigned channels);       // masque standard pour N canaux
bool speakerFromName(const std::string& name, std::uint32_t& bit);
const char* speakerName(std::uint32_t bit);
std::string stateName(DWORD state);
std::string formFactorName(std::uint32_t ff);
std::string flowName(EDataFlow flow);

// Décodage d'un WAVEFORMATEX (ou EXTENSIBLE) en objet + résumé lisible.
struct FormatView {
    WORD tag = 0;
    std::string subformat; // "PCM", "IEEE_FLOAT", GUID sinon
    unsigned channels = 0;
    unsigned sampleRate = 0;
    unsigned containerBits = 0;
    unsigned validBits = 0;
    unsigned blockAlign = 0;
    std::uint32_t mask = 0;
    bool hasMask = false;
    bool isFloat = false;
    bool isPcm = false;
};
std::optional<FormatView> viewFormat(const WAVEFORMATEX* wfx, std::size_t bytes);
std::string formatSummary(const FormatView& f);
Json formatJson(const WAVEFORMATEX* wfx, std::size_t bytes);

// ---------------------------------------------------------------------------
// Devnode (pilote) associé à un endpoint.
// ---------------------------------------------------------------------------
struct DevnodeInfo {
    std::wstring endpointInstanceId; // PKEY_Device_InstanceId de l'endpoint (SWD\MMDEVAPI\...)
    std::wstring instanceId;         // devnode de l'adaptateur audio
    std::string source;              // méthode ayant fourni instanceId
    bool located = false;
    bool present = false;
    std::wstring friendlyName, deviceDesc, manufacturer, provider, version, date, infPath, infSection, service, hardwareId;
    ULONG status = 0, problem = 0;
    std::string error;
};
DevnodeInfo resolveDevnode(IMMDevice* device, IPropertyStore* store);
Json devnodeJson(const DevnodeInfo& d);

// ---------------------------------------------------------------------------
// Résumé d'un endpoint, utilisé pour le filtrage et la sélection.
// ---------------------------------------------------------------------------
struct EndpointBasic {
    ComPtr<IMMDevice> device;
    std::wstring id;
    EDataFlow flow = eRender;
    DWORD state = 0;
    std::wstring friendlyName;  // "Haut-parleurs (Xonar SE)"
    std::wstring deviceDesc;    // "Haut-parleurs"
    std::wstring interfaceName; // "Xonar SE"
    DevnodeInfo devnode;
    bool isXonar = false;
};
HRESULT createEnumerator(ComPtr<IMMDeviceEnumerator>& out);
HRESULT enumerateEndpoints(IMMDeviceEnumerator* en, EDataFlow flow, DWORD stateMask, std::vector<EndpointBasic>& out);
bool describeBasic(IMMDevice* device, EndpointBasic& out);
bool looksLikeXonar(const EndpointBasic& e);

// ---------------------------------------------------------------------------
// Jacks (IDeviceTopology -> IConnector -> IPart -> IKsJackDescription[2]).
// ---------------------------------------------------------------------------
struct JackAccess {
    ComPtr<IKsJackDescription> jack;
    ComPtr<IKsJackDescription2> jack2;
    std::string adapterTopologyId; // identifiant du filtre KS de l'adaptateur
    std::string connectorName;
    HRESULT hr = S_OK;
    std::string step; // étape ayant échoué
};
JackAccess openJacks(IMMDevice* device);
std::string colorName(std::uint32_t rgb);
std::string connectionTypeName(int v);
std::string geoLocationName(int v);
std::string genLocationName(int v);
std::string portConnectionName(int v);

} // namespace probe
