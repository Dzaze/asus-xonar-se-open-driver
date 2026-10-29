// endpoint-probe : utilitaires communs. Tout ce qui est ici est en lecture seule.
#include "probe_common.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <format>

namespace probe {

// ===========================================================================
// Json
// ===========================================================================
Json Json::object() {
    Json j;
    j.kind_ = Kind::Object;
    return j;
}

Json Json::array() {
    Json j;
    j.kind_ = Kind::Array;
    return j;
}

Json Json::real(double v, int decimals) {
    Json j(v);
    j.decimals_ = decimals;
    return j;
}

Json& Json::add(std::string key, std::string label, Json value, Show show) {
    if (kind_ != Kind::Object) {
        kind_ = Kind::Object;
        members_.clear();
    }
    members_.push_back(Member{std::move(key), std::move(label), std::move(value), show});
    return *this;
}

Json& Json::push(Json value) {
    if (kind_ != Kind::Array) {
        kind_ = Kind::Array;
        items_.clear();
    }
    items_.push_back(std::move(value));
    return *this;
}

std::size_t Json::size() const {
    if (kind_ == Kind::Array) return items_.size();
    if (kind_ == Kind::Object) return members_.size();
    return 0;
}

namespace {

void jsonEscape(std::string& out, const std::string& s) {
    out += '"';
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (u < 0x20) out += std::format("\\u{:04x}", u);
            else out += c; // UTF-8 transmis tel quel (JSON valide)
        }
    }
    out += '"';
}

std::string realText(double d, int decimals) {
    if (!std::isfinite(d)) return "n/d";
    if (decimals >= 0) return std::format("{:.{}f}", d, decimals);
    return std::format("{}", d);
}

} // namespace

std::string Json::scalarText() const {
    switch (kind_) {
    case Kind::Null: return "n/d";
    case Kind::Bool: return b_ ? "oui" : "non";
    case Kind::Int: return std::to_string(i_);
    case Kind::UInt: return std::to_string(u_);
    case Kind::Real: return realText(d_, decimals_);
    case Kind::String: return str_;
    case Kind::Array: {
        std::string s;
        for (const auto& it : items_) {
            if (!s.empty()) s += ", ";
            s += it.scalarText();
        }
        return s.empty() ? "(vide)" : s;
    }
    case Kind::Object: return "{...}";
    }
    return {};
}

void Json::jsonTo(std::string& out, int indent, int level) const {
    const auto nl = [&](int lv) {
        if (indent <= 0) return;
        out += '\n';
        out.append(static_cast<std::size_t>(indent * lv), ' ');
    };
    switch (kind_) {
    case Kind::Null: out += "null"; break;
    case Kind::Bool: out += b_ ? "true" : "false"; break;
    case Kind::Int: out += std::to_string(i_); break;
    case Kind::UInt: out += std::to_string(u_); break;
    case Kind::Real:
        if (std::isfinite(d_)) out += std::format("{}", d_);
        else out += "null";
        break;
    case Kind::String: jsonEscape(out, str_); break;
    case Kind::Array: {
        out += '[';
        bool first = true;
        for (const auto& it : items_) {
            if (!first) out += ',';
            first = false;
            nl(level + 1);
            it.jsonTo(out, indent, level + 1);
        }
        if (!first) nl(level);
        out += ']';
        break;
    }
    case Kind::Object: {
        out += '{';
        bool first = true;
        for (const auto& m : members_) {
            if (m.show == Show::TextOnly) continue;
            if (!first) out += ',';
            first = false;
            nl(level + 1);
            jsonEscape(out, m.key);
            out += indent > 0 ? ": " : ":";
            m.value.jsonTo(out, indent, level + 1);
        }
        if (!first) nl(level);
        out += '}';
        break;
    }
    }
}

std::string Json::toJson(int indent) const {
    std::string out;
    jsonTo(out, indent, 0);
    return out;
}

void Json::toText(std::string& out, int indent) const {
    const std::string pad(static_cast<std::size_t>(indent), ' ');
    if (kind_ == Kind::Object) {
        for (const auto& m : members_) {
            if (m.show == Show::JsonOnly) continue;
            const bool nested = m.value.kind_ == Kind::Object ||
                                (m.value.kind_ == Kind::Array &&
                                 std::any_of(m.value.items_.begin(), m.value.items_.end(),
                                             [](const Json& j) { return j.kind_ == Kind::Object; }));
            if (!nested) {
                out += pad + m.label + " : " + m.value.scalarText() + '\n';
            } else {
                out += pad + m.label + " :\n";
                m.value.toText(out, indent + 2);
            }
        }
    } else if (kind_ == Kind::Array) {
        std::size_t n = 0;
        for (const auto& it : items_) {
            ++n;
            if (it.kind_ == Kind::Object) {
                out += pad + std::format("[{}]\n", n);
                it.toText(out, indent + 2);
            } else {
                out += pad + "- " + it.scalarText() + '\n';
            }
        }
    } else {
        out += pad + scalarText() + '\n';
    }
}

// ===========================================================================
// Chaînes, erreurs, sortie
// ===========================================================================
std::string toUtf8(const wchar_t* s) {
    if (!s || !*s) return {};
    const int len = static_cast<int>(wcslen(s));
    const int needed = WideCharToMultiByte(CP_UTF8, 0, s, len, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s, len, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string toUtf8(const std::wstring& s) { return toUtf8(s.c_str()); }

std::wstring fromUtf8(const std::string& s) {
    if (s.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}

std::wstring lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool containsNoCase(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return true;
    return lower(haystack).find(lower(needle)) != std::wstring::npos;
}

std::string hex32(std::uint32_t v) { return std::format("0x{:08X}", v); }

std::string guidText(const GUID& g) {
    return std::format("{{{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}}}", g.Data1, g.Data2,
                       g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6],
                       g.Data4[7]);
}

std::string hrText(HRESULT hr) {
    struct Name {
        HRESULT hr;
        const char* name;
    };
    // Codes WASAPI et COM les plus fréquents, pour un diagnostic lisible sans recherche.
    static const Name names[] = {
        {AUDCLNT_E_NOT_INITIALIZED, "AUDCLNT_E_NOT_INITIALIZED"},
        {AUDCLNT_E_ALREADY_INITIALIZED, "AUDCLNT_E_ALREADY_INITIALIZED"},
        {AUDCLNT_E_WRONG_ENDPOINT_TYPE, "AUDCLNT_E_WRONG_ENDPOINT_TYPE"},
        {AUDCLNT_E_DEVICE_INVALIDATED, "AUDCLNT_E_DEVICE_INVALIDATED"},
        {AUDCLNT_E_NOT_STOPPED, "AUDCLNT_E_NOT_STOPPED"},
        {AUDCLNT_E_BUFFER_TOO_LARGE, "AUDCLNT_E_BUFFER_TOO_LARGE"},
        {AUDCLNT_E_OUT_OF_ORDER, "AUDCLNT_E_OUT_OF_ORDER"},
        {AUDCLNT_E_UNSUPPORTED_FORMAT, "AUDCLNT_E_UNSUPPORTED_FORMAT"},
        {AUDCLNT_E_INVALID_SIZE, "AUDCLNT_E_INVALID_SIZE"},
        {AUDCLNT_E_DEVICE_IN_USE, "AUDCLNT_E_DEVICE_IN_USE"},
        {AUDCLNT_E_BUFFER_OPERATION_PENDING, "AUDCLNT_E_BUFFER_OPERATION_PENDING"},
        {AUDCLNT_E_THREAD_NOT_REGISTERED, "AUDCLNT_E_THREAD_NOT_REGISTERED"},
        {AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED"},
        {AUDCLNT_E_ENDPOINT_CREATE_FAILED, "AUDCLNT_E_ENDPOINT_CREATE_FAILED"},
        {AUDCLNT_E_SERVICE_NOT_RUNNING, "AUDCLNT_E_SERVICE_NOT_RUNNING"},
        {AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED, "AUDCLNT_E_EVENTHANDLE_NOT_EXPECTED"},
        {AUDCLNT_E_EXCLUSIVE_MODE_ONLY, "AUDCLNT_E_EXCLUSIVE_MODE_ONLY"},
        {AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL, "AUDCLNT_E_BUFDURATION_PERIOD_NOT_EQUAL"},
        {AUDCLNT_E_EVENTHANDLE_NOT_SET, "AUDCLNT_E_EVENTHANDLE_NOT_SET"},
        {AUDCLNT_E_INCORRECT_BUFFER_SIZE, "AUDCLNT_E_INCORRECT_BUFFER_SIZE"},
        {AUDCLNT_E_BUFFER_SIZE_ERROR, "AUDCLNT_E_BUFFER_SIZE_ERROR"},
        {AUDCLNT_E_CPUUSAGE_EXCEEDED, "AUDCLNT_E_CPUUSAGE_EXCEEDED"},
        {AUDCLNT_E_BUFFER_ERROR, "AUDCLNT_E_BUFFER_ERROR"},
        {AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED, "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED"},
        {AUDCLNT_E_INVALID_DEVICE_PERIOD, "AUDCLNT_E_INVALID_DEVICE_PERIOD"},
        {AUDCLNT_E_RESOURCES_INVALIDATED, "AUDCLNT_E_RESOURCES_INVALIDATED"},
        {AUDCLNT_E_ENGINE_FORMAT_LOCKED, "AUDCLNT_E_ENGINE_FORMAT_LOCKED"},
        {AUDCLNT_S_BUFFER_EMPTY, "AUDCLNT_S_BUFFER_EMPTY"},
        {S_FALSE, "S_FALSE"},
        {E_NOTFOUND, "E_NOTFOUND"},
        {E_NOINTERFACE, "E_NOINTERFACE"},
        {E_NOTIMPL, "E_NOTIMPL"},
        {E_INVALIDARG, "E_INVALIDARG"},
        {E_POINTER, "E_POINTER"},
        {E_OUTOFMEMORY, "E_OUTOFMEMORY"},
        {E_ACCESSDENIED, "E_ACCESSDENIED"},
        {E_FAIL, "E_FAIL"},
        {HRESULT_FROM_WIN32(ERROR_NOT_FOUND), "HRESULT_FROM_WIN32(ERROR_NOT_FOUND)"},
        {HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)"},
        {HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND), "HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND)"},
    };
    for (const auto& n : names) {
        if (n.hr == hr) return std::format("{} ({})", hex32(static_cast<std::uint32_t>(hr)), n.name);
    }
    wchar_t* msg = nullptr;
    const DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::string text = len ? toUtf8(std::wstring(msg, len)) : std::string{};
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.'))
        text.pop_back();
    if (text.empty()) return hex32(static_cast<std::uint32_t>(hr));
    return std::format("{} ({})", hex32(static_cast<std::uint32_t>(hr)), text);
}

std::string timestampNow() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}.{:03}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                       st.wSecond, st.wMilliseconds);
}

void printOut(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void printErr(const std::string& s) {
    std::fwrite(s.data(), 1, s.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

ComScope::ComScope() : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
ComScope::~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
}

// ===========================================================================
// Propriétés
// ===========================================================================
namespace {

// PROPVARIANT avec libération automatique.
struct PropVar {
    PROPVARIANT v;
    PropVar() { PropVariantInit(&v); }
    ~PropVar() { PropVariantClear(&v); }
    PropVar(const PropVar&) = delete;
    PropVar& operator=(const PropVar&) = delete;
};

// Clé non documentée mais stable depuis Vista : instance du devnode de l'adaptateur,
// préfixée par "{1}." (constatée dans le registre MMDevices de cette machine).
// {b3f8fa53-0004-438e-9003-51a46e139bfc},2
constexpr PROPERTYKEY kPkeyEndpointDevnode = {{0xb3f8fa53, 0x0004, 0x438e, {0x90, 0x03, 0x51, 0xa4, 0x6e, 0x13, 0x9b, 0xfc}}, 2};

bool sameKey(const PROPERTYKEY& a, const PROPERTYKEY& b) {
    return a.pid == b.pid && IsEqualGUID(a.fmtid, b.fmtid);
}

} // namespace

std::optional<std::wstring> propString(IPropertyStore* store, const PROPERTYKEY& key) {
    if (!store) return std::nullopt;
    PropVar pv;
    if (FAILED(store->GetValue(key, &pv.v))) return std::nullopt;
    if (pv.v.vt == VT_LPWSTR && pv.v.pwszVal) return std::wstring(pv.v.pwszVal);
    return std::nullopt;
}

std::optional<std::uint32_t> propUInt(IPropertyStore* store, const PROPERTYKEY& key) {
    if (!store) return std::nullopt;
    PropVar pv;
    if (FAILED(store->GetValue(key, &pv.v))) return std::nullopt;
    if (pv.v.vt == VT_UI4) return static_cast<std::uint32_t>(pv.v.ulVal);
    if (pv.v.vt == VT_I4) return static_cast<std::uint32_t>(pv.v.lVal);
    if (pv.v.vt == VT_UINT) return static_cast<std::uint32_t>(pv.v.uintVal);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> propBlob(IPropertyStore* store, const PROPERTYKEY& key) {
    if (!store) return std::nullopt;
    PropVar pv;
    if (FAILED(store->GetValue(key, &pv.v))) return std::nullopt;
    if (pv.v.vt != VT_BLOB || !pv.v.blob.pBlobData) return std::nullopt;
    return std::vector<std::uint8_t>(pv.v.blob.pBlobData, pv.v.blob.pBlobData + pv.v.blob.cbSize);
}

std::string propertyKeyName(const PROPERTYKEY& key) {
    struct Known {
        const PROPERTYKEY* key;
        const char* name;
    };
    static const Known known[] = {
        {&PKEY_Device_FriendlyName, "PKEY_Device_FriendlyName"},
        {&PKEY_Device_DeviceDesc, "PKEY_Device_DeviceDesc"},
        {&PKEY_DeviceInterface_FriendlyName, "PKEY_DeviceInterface_FriendlyName"},
        {&PKEY_AudioEndpoint_FormFactor, "PKEY_AudioEndpoint_FormFactor"},
        {&PKEY_AudioEndpoint_PhysicalSpeakers, "PKEY_AudioEndpoint_PhysicalSpeakers"},
        {&PKEY_AudioEndpoint_JackSubType, "PKEY_AudioEndpoint_JackSubType"},
        {&PKEY_AudioEndpoint_GUID, "PKEY_AudioEndpoint_GUID"},
        {&PKEY_AudioEndpoint_Supports_EventDriven_Mode, "PKEY_AudioEndpoint_Supports_EventDriven_Mode"},
        {&PKEY_AudioEngine_DeviceFormat, "PKEY_AudioEngine_DeviceFormat"},
        {&PKEY_AudioEngine_OEMFormat, "PKEY_AudioEngine_OEMFormat"},
        {&PKEY_Device_InstanceId, "PKEY_Device_InstanceId"},
        {&kPkeyEndpointDevnode, "devnode adaptateur (non documentée)"},
    };
    for (const auto& k : known) {
        if (sameKey(*k.key, key)) return k.name;
    }
    return std::format("{},{}", guidText(key.fmtid), key.pid);
}

std::string propertyValueText(IPropertyStore* store, const PROPERTYKEY& key) {
    if (!store) return "n/d";
    PropVar pv;
    const HRESULT hr = store->GetValue(key, &pv.v);
    if (FAILED(hr)) return "illisible : " + hrText(hr);
    switch (pv.v.vt) {
    case VT_EMPTY: return "(vide)";
    case VT_LPWSTR: return pv.v.pwszVal ? toUtf8(pv.v.pwszVal) : "(null)";
    case VT_UI4: return std::format("{} ({})", pv.v.ulVal, hex32(pv.v.ulVal));
    case VT_I4: return std::to_string(pv.v.lVal);
    case VT_BOOL: return pv.v.boolVal ? "vrai" : "faux";
    case VT_BLOB: {
        if (sameKey(key, PKEY_AudioEngine_DeviceFormat) || sameKey(key, PKEY_AudioEngine_OEMFormat)) {
            const auto f = viewFormat(reinterpret_cast<const WAVEFORMATEX*>(pv.v.blob.pBlobData), pv.v.blob.cbSize);
            if (f) return formatSummary(*f);
        }
        return std::format("blob de {} octets", pv.v.blob.cbSize);
    }
    default: return std::format("type VARIANT {}", static_cast<unsigned>(pv.v.vt));
    }
}

// ===========================================================================
// Canaux, formats, états
// ===========================================================================
namespace {

struct SpeakerName {
    std::uint32_t bit;
    const char* name;
};
constexpr SpeakerName kSpeakers[] = {
    {SPEAKER_FRONT_LEFT, "FL"},          {SPEAKER_FRONT_RIGHT, "FR"},         {SPEAKER_FRONT_CENTER, "FC"},
    {SPEAKER_LOW_FREQUENCY, "LFE"},      {SPEAKER_BACK_LEFT, "BL"},           {SPEAKER_BACK_RIGHT, "BR"},
    {SPEAKER_FRONT_LEFT_OF_CENTER, "FLC"}, {SPEAKER_FRONT_RIGHT_OF_CENTER, "FRC"}, {SPEAKER_BACK_CENTER, "BC"},
    {SPEAKER_SIDE_LEFT, "SL"},           {SPEAKER_SIDE_RIGHT, "SR"},          {SPEAKER_TOP_CENTER, "TC"},
    {SPEAKER_TOP_FRONT_LEFT, "TFL"},     {SPEAKER_TOP_FRONT_CENTER, "TFC"},   {SPEAKER_TOP_FRONT_RIGHT, "TFR"},
    {SPEAKER_TOP_BACK_LEFT, "TBL"},      {SPEAKER_TOP_BACK_CENTER, "TBC"},    {SPEAKER_TOP_BACK_RIGHT, "TBR"},
};

} // namespace

const char* speakerName(std::uint32_t bit) {
    for (const auto& s : kSpeakers) {
        if (s.bit == bit) return s.name;
    }
    return "?";
}

bool speakerFromName(const std::string& name, std::uint32_t& bit) {
    std::string up = name;
    for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    for (const auto& s : kSpeakers) {
        if (up == s.name) {
            bit = s.bit;
            return true;
        }
    }
    return false;
}

std::string speakerList(std::uint32_t mask) {
    std::string out;
    for (const auto& s : kSpeakers) {
        if (mask & s.bit) {
            if (!out.empty()) out += ' ';
            out += s.name;
        }
    }
    if (mask & SPEAKER_RESERVED) out += out.empty() ? "RESERVED" : " RESERVED";
    return out.empty() ? "aucun" : out;
}

std::string layoutName(std::uint32_t mask) {
    switch (mask) {
    case KSAUDIO_SPEAKER_MONO: return "mono";
    case KSAUDIO_SPEAKER_STEREO: return "stéréo";
    case KSAUDIO_SPEAKER_2POINT1: return "2.1";
    case KSAUDIO_SPEAKER_QUAD: return "quadriphonie";
    case KSAUDIO_SPEAKER_SURROUND: return "surround (LCRS)";
    case KSAUDIO_SPEAKER_5POINT1: return "5.1 (arrière)";
    case KSAUDIO_SPEAKER_5POINT1_SURROUND: return "5.1 (latéral)";
    case KSAUDIO_SPEAKER_7POINT1: return "7.1 (large)";
    case KSAUDIO_SPEAKER_7POINT1_SURROUND: return "7.1";
    default: return {};
    }
}

std::uint32_t defaultMaskFor(unsigned channels) {
    switch (channels) {
    case 1: return KSAUDIO_SPEAKER_MONO;
    case 2: return KSAUDIO_SPEAKER_STEREO;
    case 3: return KSAUDIO_SPEAKER_2POINT1;
    case 4: return KSAUDIO_SPEAKER_QUAD;
    case 6: return KSAUDIO_SPEAKER_5POINT1;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default: return 0;
    }
}

std::string stateName(DWORD state) {
    switch (state) {
    case DEVICE_STATE_ACTIVE: return "ACTIVE";
    case DEVICE_STATE_DISABLED: return "DISABLED";
    case DEVICE_STATE_NOTPRESENT: return "NOTPRESENT";
    case DEVICE_STATE_UNPLUGGED: return "UNPLUGGED";
    default: return std::format("INCONNU({})", state);
    }
}

std::string formFactorName(std::uint32_t ff) {
    switch (ff) {
    case RemoteNetworkDevice: return "RemoteNetworkDevice";
    case Speakers: return "Speakers";
    case LineLevel: return "LineLevel";
    case Headphones: return "Headphones";
    case Microphone: return "Microphone";
    case Headset: return "Headset";
    case Handset: return "Handset";
    case UnknownDigitalPassthrough: return "UnknownDigitalPassthrough";
    case SPDIF: return "SPDIF";
    case DigitalAudioDisplayDevice: return "DigitalAudioDisplayDevice";
    case UnknownFormFactor: return "UnknownFormFactor";
    default: return std::format("valeur {}", ff);
    }
}

std::string flowName(EDataFlow flow) {
    switch (flow) {
    case eRender: return "render";
    case eCapture: return "capture";
    default: return "all";
    }
}

std::optional<FormatView> viewFormat(const WAVEFORMATEX* wfx, std::size_t bytes) {
    if (!wfx || bytes < sizeof(WAVEFORMATEX) - sizeof(WORD)) return std::nullopt; // PCMWAVEFORMAT minimal accepté
    FormatView f;
    f.tag = wfx->wFormatTag;
    f.channels = wfx->nChannels;
    f.sampleRate = wfx->nSamplesPerSec;
    f.containerBits = wfx->wBitsPerSample;
    f.validBits = wfx->wBitsPerSample;
    f.blockAlign = wfx->nBlockAlign;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE && bytes >= sizeof(WAVEFORMATEXTENSIBLE) && wfx->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        f.validBits = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : wfx->wBitsPerSample;
        f.mask = ext->dwChannelMask;
        f.hasMask = true;
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            f.subformat = "PCM";
            f.isPcm = true;
        } else if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            f.subformat = "IEEE_FLOAT";
            f.isFloat = true;
        } else {
            f.subformat = guidText(ext->SubFormat);
        }
    } else if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        f.subformat = "PCM";
        f.isPcm = true;
    } else if (wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        f.subformat = "IEEE_FLOAT";
        f.isFloat = true;
    } else {
        f.subformat = std::format("tag 0x{:04X}", wfx->wFormatTag);
    }
    return f;
}

std::string formatSummary(const FormatView& f) {
    std::string bits = f.validBits == f.containerBits ? std::format("{} bits", f.validBits)
                                                      : std::format("{} bits (conteneur {})", f.validBits, f.containerBits);
    std::string s = std::format("{} canaux, {}, {} Hz, {}", f.channels, bits, f.sampleRate,
                                f.isFloat ? "flottant" : (f.isPcm ? "PCM entier" : f.subformat));
    if (f.hasMask) {
        s += std::format(", masque {} ({}", hex32(f.mask), speakerList(f.mask));
        const auto layout = layoutName(f.mask);
        if (!layout.empty()) s += ", " + layout;
        s += ')';
    } else {
        s += ", sans masque (WAVEFORMATEX simple)";
    }
    return s;
}

Json formatJson(const WAVEFORMATEX* wfx, std::size_t bytes) {
    const auto f = viewFormat(wfx, bytes);
    if (!f) return Json::object().add("error", "Erreur", "format illisible");
    Json speakers = Json::array();
    for (const auto& sp : kSpeakers) {
        if (f->mask & sp.bit) speakers.push(sp.name);
    }
    Json j = Json::object();
    j.addText("Format", formatSummary(*f));
    j.addJson("tag", std::format("0x{:04X}", f->tag));
    j.addJson("subformat", f->subformat);
    j.addJson("channels", f->channels);
    j.addJson("sample_rate", f->sampleRate);
    j.addJson("container_bits", f->containerBits);
    j.addJson("valid_bits", f->validBits);
    j.addJson("block_align", f->blockAlign);
    j.addJson("avg_bytes_per_sec", static_cast<std::uint32_t>(wfx->nAvgBytesPerSec));
    if (f->hasMask) {
        j.addJson("channel_mask", hex32(f->mask));
        j.addJson("speakers", std::move(speakers));
        j.addJson("layout", layoutName(f->mask));
    } else {
        j.addJson("channel_mask", Json());
    }
    j.addJson("summary", formatSummary(*f));
    return j;
}

// ===========================================================================
// Devnode
// ===========================================================================
namespace {

std::wstring stripBracePrefix(const std::wstring& s) {
    // "{1}.HDAUDIO\..." ou "{2}.\\?\hdaudio#..." : on retire le préfixe "{n}."
    if (s.size() > 4 && s[0] == L'{') {
        const auto close = s.find(L"}.");
        if (close != std::wstring::npos && close < 6) return s.substr(close + 2);
    }
    return s;
}

std::optional<std::wstring> devnodeString(DEVINST inst, const DEVPROPKEY& key) {
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET cr = CM_Get_DevNode_PropertyW(inst, &key, &type, nullptr, &size, 0);
    if (cr != CR_BUFFER_SMALL || size == 0) return std::nullopt;
    std::vector<BYTE> buf(size);
    cr = CM_Get_DevNode_PropertyW(inst, &key, &type, buf.data(), &size, 0);
    if (cr != CR_SUCCESS) return std::nullopt;
    if (type == DEVPROP_TYPE_STRING || type == DEVPROP_TYPE_STRING_LIST) {
        // Pour une liste, la première chaîne suffit (ex. identifiant matériel principal).
        return std::wstring(reinterpret_cast<const wchar_t*>(buf.data()));
    }
    if (type == DEVPROP_TYPE_FILETIME && size >= sizeof(FILETIME)) {
        FILETIME ft{};
        std::memcpy(&ft, buf.data(), sizeof(ft));
        SYSTEMTIME st{};
        FileTimeToSystemTime(&ft, &st);
        return std::format(L"{:04}-{:02}-{:02}", st.wYear, st.wMonth, st.wDay);
    }
    return std::nullopt;
}

std::optional<std::wstring> interfaceInstanceId(const std::wstring& interfacePath) {
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET cr = CM_Get_Device_Interface_PropertyW(interfacePath.c_str(), &DEVPKEY_Device_InstanceId, &type, nullptr, &size, 0);
    if (cr != CR_BUFFER_SMALL || size == 0) return std::nullopt;
    std::vector<BYTE> buf(size);
    cr = CM_Get_Device_Interface_PropertyW(interfacePath.c_str(), &DEVPKEY_Device_InstanceId, &type, buf.data(), &size, 0);
    if (cr != CR_SUCCESS || type != DEVPROP_TYPE_STRING) return std::nullopt;
    return std::wstring(reinterpret_cast<const wchar_t*>(buf.data()));
}

} // namespace

DevnodeInfo resolveDevnode(IMMDevice* device, IPropertyStore* store) {
    DevnodeInfo d;
    if (auto v = propString(store, PKEY_Device_InstanceId)) {
        d.endpointInstanceId = *v;
    } else if (device) {
        // Le magasin de l'endpoint n'expose pas toujours PKEY_Device_InstanceId : on vérifie
        // alors l'existence du devnode logiciel SWD\MMDEVAPI\<id> (présent ou fantôme).
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            const std::wstring swd = std::wstring(L"SWD\\MMDEVAPI\\") + id;
            CoTaskMemFree(id);
            DEVINST inst = 0;
            if (CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(swd.c_str()), CM_LOCATE_DEVNODE_PHANTOM) == CR_SUCCESS)
                d.endpointInstanceId = swd;
        }
    }

    // 1. PKEY_Device_InstanceId de l'endpoint : depuis Windows 10 il désigne le devnode
    //    logiciel SWD\MMDEVAPI\... de l'endpoint lui-même, pas l'adaptateur. On ne le
    //    retient que s'il ne s'agit pas d'un devnode SWD\MMDEVAPI.
    if (!d.endpointInstanceId.empty() && !containsNoCase(d.endpointInstanceId, L"SWD\\MMDEVAPI")) {
        d.instanceId = d.endpointInstanceId;
        d.source = "PKEY_Device_InstanceId";
    }
    // 2. Clé {b3f8fa53-...},2 : instance du devnode de l'adaptateur.
    if (d.instanceId.empty()) {
        if (auto v = propString(store, kPkeyEndpointDevnode)) {
            d.instanceId = stripBracePrefix(*v);
            d.source = "{b3f8fa53-0004-438e-9003-51a46e139bfc},2";
        }
    }
    // 3. Topologie : identifiant du filtre KS de l'adaptateur -> interface -> devnode.
    if (d.instanceId.empty() && device) {
        const JackAccess ja = openJacks(device);
        if (!ja.adapterTopologyId.empty()) {
            if (auto v = interfaceInstanceId(stripBracePrefix(fromUtf8(ja.adapterTopologyId)))) {
                d.instanceId = *v;
                d.source = "IDeviceTopology::GetDeviceId + CM_Get_Device_Interface_Property";
            }
        }
    }
    if (d.instanceId.empty()) {
        d.error = "devnode de l'adaptateur introuvable";
        return d;
    }

    DEVINST inst = 0;
    CONFIGRET cr = CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(d.instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL);
    if (cr == CR_SUCCESS) {
        d.present = true;
    } else {
        // Périphérique absent (NOTPRESENT) : le devnode fantôme garde les infos du pilote.
        cr = CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(d.instanceId.c_str()), CM_LOCATE_DEVNODE_PHANTOM);
    }
    if (cr != CR_SUCCESS) {
        d.error = std::format("CM_Locate_DevNode a échoué (CONFIGRET {})", cr);
        return d;
    }
    d.located = true;
    const auto get = [&](const DEVPROPKEY& k, std::wstring& out) {
        if (auto v = devnodeString(inst, k)) out = *v;
    };
    get(DEVPKEY_Device_FriendlyName, d.friendlyName);
    get(DEVPKEY_Device_DeviceDesc, d.deviceDesc);
    get(DEVPKEY_Device_Manufacturer, d.manufacturer);
    get(DEVPKEY_Device_DriverProvider, d.provider);
    get(DEVPKEY_Device_DriverVersion, d.version);
    get(DEVPKEY_Device_DriverDate, d.date);
    get(DEVPKEY_Device_DriverInfPath, d.infPath);
    get(DEVPKEY_Device_DriverInfSection, d.infSection);
    get(DEVPKEY_Device_Service, d.service);
    get(DEVPKEY_Device_HardwareIds, d.hardwareId);
    if (d.present) CM_Get_DevNode_Status(&d.status, &d.problem, inst, 0);
    return d;
}

Json devnodeJson(const DevnodeInfo& d) {
    Json j = Json::object();
    j.add("endpoint_instance_id", "Instance de l'endpoint",
          d.endpointInstanceId.empty() ? Json() : Json(toUtf8(d.endpointInstanceId)));
    j.add("instance_id", "Instance de l'adaptateur", d.instanceId.empty() ? Json() : Json(toUtf8(d.instanceId)));
    if (!d.source.empty()) j.add("source", "Obtenue via", d.source);
    if (!d.error.empty()) j.add("error", "Erreur", d.error);
    if (!d.located) return j;
    j.add("present", "Présent", d.present);
    j.add("friendly_name", "Nom convivial", toUtf8(d.friendlyName));
    j.add("device_desc", "Description", toUtf8(d.deviceDesc));
    j.add("manufacturer", "Fabricant", toUtf8(d.manufacturer));
    j.add("driver_provider", "Fournisseur du pilote", toUtf8(d.provider));
    j.add("driver_version", "Version du pilote", toUtf8(d.version));
    j.add("driver_date", "Date du pilote", toUtf8(d.date));
    j.add("driver_inf", "INF", toUtf8(d.infPath) + (d.infSection.empty() ? "" : " [" + toUtf8(d.infSection) + "]"));
    j.add("service", "Service", toUtf8(d.service));
    j.add("hardware_id", "Identifiant matériel", toUtf8(d.hardwareId));
    if (d.present) {
        j.add("devnode_status", "Statut devnode", hex32(d.status));
        j.add("problem_code", "Code problème", static_cast<std::uint32_t>(d.problem));
    }
    return j;
}

// ===========================================================================
// Endpoints
// ===========================================================================
HRESULT createEnumerator(ComPtr<IMMDeviceEnumerator>& out) {
    return CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&out));
}

bool describeBasic(IMMDevice* device, EndpointBasic& e) {
    e.device = device;
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return false;
    e.id = id;
    CoTaskMemFree(id);
    device->GetState(&e.state);
    ComPtr<IMMEndpoint> ep;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&ep)))) ep->GetDataFlow(&e.flow);
    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
        e.friendlyName = propString(store.Get(), PKEY_Device_FriendlyName).value_or(L"");
        e.deviceDesc = propString(store.Get(), PKEY_Device_DeviceDesc).value_or(L"");
        e.interfaceName = propString(store.Get(), PKEY_DeviceInterface_FriendlyName).value_or(L"");
        e.devnode = resolveDevnode(device, store.Get());
    }
    e.isXonar = looksLikeXonar(e);
    return true;
}

bool looksLikeXonar(const EndpointBasic& e) {
    for (const auto* s : {&e.friendlyName, &e.deviceDesc, &e.interfaceName, &e.devnode.friendlyName, &e.devnode.deviceDesc}) {
        if (containsNoCase(*s, L"xonar")) return true;
    }
    // Identifiant USB de la Xonar SE (VID 0x0B05 / PID 0x189D), quel que soit le nom
    // affiché par le pilote (usbaudio2 ou xonarse.sys).
    return containsNoCase(e.devnode.instanceId, L"VID_0B05&PID_189D");
}

HRESULT enumerateEndpoints(IMMDeviceEnumerator* en, EDataFlow flow, DWORD stateMask, std::vector<EndpointBasic>& out) {
    ComPtr<IMMDeviceCollection> col;
    HRESULT hr = en->EnumAudioEndpoints(flow, stateMask, &col);
    if (FAILED(hr)) return hr;
    UINT count = 0;
    col->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        EndpointBasic e;
        if (describeBasic(dev.Get(), e)) out.push_back(std::move(e));
    }
    return S_OK;
}

// ===========================================================================
// Jacks
// ===========================================================================
JackAccess openJacks(IMMDevice* device) {
    JackAccess ja;
    ComPtr<IDeviceTopology> topo;
    ja.hr = device->Activate(__uuidof(IDeviceTopology), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(topo.GetAddressOf()));
    if (FAILED(ja.hr)) {
        ja.step = "IMMDevice::Activate(IDeviceTopology)";
        return ja;
    }
    // La topologie d'un endpoint n'a qu'un connecteur, relié au connecteur de l'adaptateur.
    ComPtr<IConnector> from;
    ja.hr = topo->GetConnector(0, &from);
    if (FAILED(ja.hr)) {
        ja.step = "IDeviceTopology::GetConnector(0)";
        return ja;
    }
    ComPtr<IConnector> to;
    ja.hr = from->GetConnectedTo(&to);
    if (FAILED(ja.hr)) {
        ja.step = "IConnector::GetConnectedTo";
        return ja;
    }
    ComPtr<IPart> part;
    ja.hr = to.As(&part);
    if (FAILED(ja.hr)) {
        ja.step = "IConnector -> IPart";
        return ja;
    }
    LPWSTR name = nullptr;
    if (SUCCEEDED(part->GetName(&name)) && name) {
        ja.connectorName = toUtf8(name);
        CoTaskMemFree(name);
    }
    ComPtr<IDeviceTopology> adapterTopo;
    if (SUCCEEDED(part->GetTopologyObject(&adapterTopo))) {
        LPWSTR devId = nullptr;
        if (SUCCEEDED(adapterTopo->GetDeviceId(&devId)) && devId) {
            ja.adapterTopologyId = toUtf8(devId);
            CoTaskMemFree(devId);
        }
    }
    ja.hr = part->Activate(CLSCTX_INPROC_SERVER, __uuidof(IKsJackDescription), reinterpret_cast<void**>(ja.jack.GetAddressOf()));
    if (FAILED(ja.hr)) {
        ja.step = "IPart::Activate(IKsJackDescription)";
        return ja;
    }
    // IKsJackDescription2 est facultatif (KSPROPERTY_JACK_DESCRIPTION2 non géré par tous les pilotes).
    part->Activate(CLSCTX_INPROC_SERVER, __uuidof(IKsJackDescription2), reinterpret_cast<void**>(ja.jack2.GetAddressOf()));
    ja.hr = S_OK;
    return ja;
}

std::string colorName(std::uint32_t rgb) {
    // KSJACK_DESCRIPTION::Color est au format 0x00RRGGBB (et non COLORREF 0x00BBGGRR).
    struct Ref {
        std::uint32_t rgb;
        const char* name;
    };
    static const Ref refs[] = {
        {0x000000, "noir"},  {0x808080, "gris"},   {0x0000FF, "bleu"},  {0x00FF00, "vert"},
        {0xFF0000, "rouge"}, {0xFF8000, "orange"}, {0xFFFF00, "jaune"}, {0x800080, "violet"},
        {0xFF80C0, "rose"},  {0xFFFFFF, "blanc"},  {0x8B4513, "marron"},
    };
    const auto r = static_cast<int>((rgb >> 16) & 0xFF), g = static_cast<int>((rgb >> 8) & 0xFF), b = static_cast<int>(rgb & 0xFF);
    const char* best = "?";
    long bestDist = -1;
    for (const auto& ref : refs) {
        const int dr = r - static_cast<int>((ref.rgb >> 16) & 0xFF);
        const int dg = g - static_cast<int>((ref.rgb >> 8) & 0xFF);
        const int db = b - static_cast<int>(ref.rgb & 0xFF);
        const long dist = static_cast<long>(dr) * dr + static_cast<long>(dg) * dg + static_cast<long>(db) * db;
        if (bestDist < 0 || dist < bestDist) {
            bestDist = dist;
            best = ref.name;
        }
    }
    return std::format("#{:02X}{:02X}{:02X} (~{})", r, g, b, best);
}

std::string connectionTypeName(int v) {
    static const char* names[] = {"Unknown", "3.5mm", "Quarter (6.35mm)", "AtapiInternal", "RCA", "Optical",
                                  "OtherDigital", "OtherAnalog", "MultichannelAnalogDIN", "XlrProfessional",
                                  "RJ11Modem", "Combination"};
    if (v >= 0 && v < static_cast<int>(std::size(names))) return names[v];
    return std::format("valeur {}", v);
}

std::string geoLocationName(int v) {
    static const char* names[] = {"Invalid", "Rear", "Front", "Left", "Right", "Top", "Bottom", "RearPanel",
                                  "Riser", "InsideMobileLid", "Drivebay", "HDMI", "OutsideMobileLid", "ATAPI",
                                  "NotApplicable", "Reserved6"};
    if (v >= 0 && v < static_cast<int>(std::size(names))) return names[v];
    return std::format("valeur {}", v);
}

std::string genLocationName(int v) {
    static const char* names[] = {"PrimaryBox", "Internal", "Separate", "Other"};
    if (v >= 0 && v < static_cast<int>(std::size(names))) return names[v];
    return std::format("valeur {}", v);
}

std::string portConnectionName(int v) {
    static const char* names[] = {"Jack", "IntegratedDevice", "BothIntegratedAndJack", "Unknown"};
    if (v >= 0 && v < static_cast<int>(std::size(names))) return names[v];
    return std::format("valeur {}", v);
}

} // namespace probe
