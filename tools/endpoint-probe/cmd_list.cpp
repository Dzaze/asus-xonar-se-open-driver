// endpoint-probe list : inventaire en lecture seule des endpoints audio.
//
// Toutes les interfaces utilisées ici sont interrogées sans modification : aucun
// Initialize() de flux, aucun Set* de volume, aucun changement de périphérique par
// défaut. IAudioClient::IsFormatSupported en mode exclusif ne fait qu'une négociation
// de format (intersection KS) et n'ouvre pas de flux.
#include "commands.hpp"
#include "probe_common.hpp"

#include <algorithm>
#include <format>

namespace probe {

namespace {

struct ListOptions {
    bool all = false;
    bool formats = true;
};

Json describeProperties(IPropertyStore* store) {
    Json j = Json::object();
    if (auto ff = propUInt(store, PKEY_AudioEndpoint_FormFactor)) {
        j.add("form_factor", "Facteur de forme", std::format("{} ({})", formFactorName(*ff), *ff), Json::Show::TextOnly);
        j.addJson("form_factor", formFactorName(*ff));
        j.addJson("form_factor_value", *ff);
    } else {
        j.add("form_factor", "Facteur de forme", Json());
    }
    if (auto ps = propUInt(store, PKEY_AudioEndpoint_PhysicalSpeakers)) {
        std::string text = std::format("{} ({}", hex32(*ps), speakerList(*ps));
        const auto layout = layoutName(*ps);
        if (!layout.empty()) text += ", " + layout;
        text += ')';
        j.addText("Haut-parleurs physiques", text);
        j.addJson("physical_speakers", hex32(*ps));
        j.addJson("physical_speakers_names", speakerList(*ps));
    } else {
        j.add("physical_speakers", "Haut-parleurs physiques", Json());
    }
    if (auto js = propString(store, PKEY_AudioEndpoint_JackSubType)) {
        j.add("jack_subtype", "Sous-type de jack (KSNODETYPE)", toUtf8(*js));
    }
    if (auto ev = propUInt(store, PKEY_AudioEndpoint_Supports_EventDriven_Mode)) {
        j.add("supports_event_driven", "Mode événementiel déclaré", *ev != 0);
    }
    const auto addFormat = [&](const PROPERTYKEY& key, const char* jsonKey, const char* label) {
        if (auto blob = propBlob(store, key)) {
            j.add(jsonKey, label, formatJson(reinterpret_cast<const WAVEFORMATEX*>(blob->data()), blob->size()));
        } else {
            j.add(jsonKey, label, Json());
        }
    };
    addFormat(PKEY_AudioEngine_DeviceFormat, "device_format", "Format du moteur (DeviceFormat)");
    addFormat(PKEY_AudioEngine_OEMFormat, "oem_format", "Format OEM (OEMFormat)");
    return j;
}

Json jackJson(const KSJACK_DESCRIPTION& d, const KSJACK_DESCRIPTION2* d2) {
    Json j = Json::object();
    j.add("color", "Couleur", colorName(static_cast<std::uint32_t>(d.Color)));
    j.add("channel_mapping", "Canaux", std::format("{} ({})", hex32(d.ChannelMapping), speakerList(d.ChannelMapping)),
          Json::Show::TextOnly);
    j.addJson("channel_mapping", hex32(d.ChannelMapping));
    j.addJson("channel_mapping_names", speakerList(d.ChannelMapping));
    j.add("connection_type", "Type de connexion", connectionTypeName(static_cast<int>(d.ConnectionType)));
    j.add("geo_location", "Emplacement géométrique", geoLocationName(static_cast<int>(d.GeoLocation)));
    j.add("gen_location", "Emplacement général", genLocationName(static_cast<int>(d.GenLocation)));
    j.add("port_connection", "Raccordement", portConnectionName(static_cast<int>(d.PortConnection)));
    j.add("is_connected", "Connecté (IsConnected)", d.IsConnected != FALSE);
    if (d2) {
        j.add("jack_capabilities", "Capacités (JackCapabilities)",
              std::format("{} (détection de présence : {}, changement de format dynamique : {})", hex32(d2->JackCapabilities),
                          (d2->JackCapabilities & JACKDESC2_PRESENCE_DETECT_CAPABILITY) ? "oui" : "non",
                          (d2->JackCapabilities & JACKDESC2_DYNAMIC_FORMAT_CHANGE_CAPABILITY) ? "oui" : "non"),
              Json::Show::TextOnly);
        j.addJson("jack_capabilities", hex32(d2->JackCapabilities));
        j.addJson("presence_detect", (d2->JackCapabilities & JACKDESC2_PRESENCE_DETECT_CAPABILITY) != 0);
        j.addJson("dynamic_format_change", (d2->JackCapabilities & JACKDESC2_DYNAMIC_FORMAT_CHANGE_CAPABILITY) != 0);
        j.add("device_state_info", "DeviceStateInfo", hex32(d2->DeviceStateInfo));
    }
    return j;
}

Json describeJacks(IMMDevice* device) {
    Json j = Json::object();
    const JackAccess ja = openJacks(device);
    if (!ja.connectorName.empty()) j.add("adapter_connector", "Connecteur côté adaptateur", ja.connectorName);
    if (!ja.adapterTopologyId.empty()) j.add("adapter_topology", "Filtre de topologie", ja.adapterTopologyId);
    if (!ja.jack) {
        j.add("error", "Indisponible", std::format("{} : {}", ja.step, hrText(ja.hr)));
        return j;
    }
    UINT count = 0;
    HRESULT hr = ja.jack->GetJackCount(&count);
    if (FAILED(hr)) {
        j.add("error", "Erreur", "GetJackCount : " + hrText(hr));
        return j;
    }
    j.add("count", "Nombre de jacks", count);
    j.add("jack_description2", "IKsJackDescription2", ja.jack2 ? "disponible" : "absente");
    Json list = Json::array();
    for (UINT i = 0; i < count; ++i) {
        KSJACK_DESCRIPTION d{};
        hr = ja.jack->GetJackDescription(i, &d);
        if (FAILED(hr)) {
            list.push(Json::object().add("error", "Erreur", "GetJackDescription : " + hrText(hr)));
            continue;
        }
        KSJACK_DESCRIPTION2 d2{};
        const bool has2 = ja.jack2 && SUCCEEDED(ja.jack2->GetJackDescription2(i, &d2));
        list.push(jackJson(d, has2 ? &d2 : nullptr));
    }
    j.add("jacks", "Jacks", std::move(list));
    return j;
}

Json describeVolume(IMMDevice* device) {
    Json j = Json::object();
    ComPtr<IAudioEndpointVolume> vol;
    HRESULT hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void**>(vol.GetAddressOf()));
    if (FAILED(hr)) {
        j.add("error", "Indisponible", "Activate(IAudioEndpointVolume) : " + hrText(hr));
        return j;
    }
    float mn = 0, mx = 0, inc = 0;
    if (SUCCEEDED(vol->GetVolumeRange(&mn, &mx, &inc))) {
        j.add("range", "Plage", std::format("{:.2f} .. {:.2f} dB, pas {:.4f} dB", mn, mx, inc), Json::Show::TextOnly);
        j.addJson("min_db", Json::real(mn, 4));
        j.addJson("max_db", Json::real(mx, 4));
        j.addJson("increment_db", Json::real(inc, 4));
    }
    UINT step = 0, steps = 0;
    if (SUCCEEDED(vol->GetVolumeStepInfo(&step, &steps))) {
        j.add("step", "Pas courant", std::format("{} / {}", step, steps), Json::Show::TextOnly);
        j.addJson("step", step);
        j.addJson("step_count", steps);
    }
    float db = 0, scalar = 0;
    if (SUCCEEDED(vol->GetMasterVolumeLevel(&db))) j.add("master_db", "Volume maître (dB)", Json::real(db, 2));
    if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&scalar))) j.add("master_scalar", "Volume maître (0..1)", Json::real(scalar, 3));
    BOOL mute = FALSE;
    if (SUCCEEDED(vol->GetMute(&mute))) j.add("mute", "Muet", mute != FALSE);
    UINT ch = 0;
    if (SUCCEEDED(vol->GetChannelCount(&ch))) {
        j.add("channel_count", "Canaux de volume", ch);
        Json per = Json::array();
        for (UINT i = 0; i < ch; ++i) {
            float c = 0;
            if (SUCCEEDED(vol->GetChannelVolumeLevel(i, &c))) per.push(Json::real(c, 2));
            else per.push(Json());
        }
        j.add("channels_db", "Volume par canal (dB)", std::move(per));
    }
    DWORD hw = 0;
    hr = vol->QueryHardwareSupport(&hw);
    if (SUCCEEDED(hr)) {
        j.add("hardware_support", "Support matériel",
              std::format("{} (volume : {}, muet : {}, crête-mètre : {})", hex32(hw),
                          (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? "oui" : "non",
                          (hw & ENDPOINT_HARDWARE_SUPPORT_MUTE) ? "oui" : "non",
                          (hw & ENDPOINT_HARDWARE_SUPPORT_METER) ? "oui" : "non"),
              Json::Show::TextOnly);
        j.addJson("hardware_support", hex32(hw));
        j.addJson("hw_volume", (hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) != 0);
        j.addJson("hw_mute", (hw & ENDPOINT_HARDWARE_SUPPORT_MUTE) != 0);
        j.addJson("hw_meter", (hw & ENDPOINT_HARDWARE_SUPPORT_METER) != 0);
    } else {
        j.add("hardware_support", "Support matériel", "QueryHardwareSupport : " + hrText(hr));
    }
    return j;
}

// Construit un WAVEFORMATEXTENSIBLE PCM entier.
WAVEFORMATEXTENSIBLE makePcm(unsigned channels, unsigned validBits, unsigned containerBits, unsigned rate, std::uint32_t mask) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = static_cast<WORD>(channels);
    w.Format.nSamplesPerSec = rate;
    w.Format.wBitsPerSample = static_cast<WORD>(containerBits);
    w.Format.nBlockAlign = static_cast<WORD>(channels * containerBits / 8);
    w.Format.nAvgBytesPerSec = rate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = static_cast<WORD>(validBits);
    w.dwChannelMask = mask;
    w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

Json describeExclusiveFormats(IAudioClient* client, EDataFlow flow) {
    struct Layout {
        unsigned channels;
        std::uint32_t mask;
    };
    struct Sample {
        unsigned valid, container;
    };
    static const Layout renderLayouts[] = {{2, KSAUDIO_SPEAKER_STEREO},
                                           {4, KSAUDIO_SPEAKER_QUAD},
                                           {6, KSAUDIO_SPEAKER_5POINT1},
                                           {6, KSAUDIO_SPEAKER_5POINT1_SURROUND}};
    static const Layout captureLayouts[] = {{1, KSAUDIO_SPEAKER_MONO}, {2, KSAUDIO_SPEAKER_STEREO}};
    static const Sample samples[] = {{16, 16}, {24, 24}, {24, 32}};
    static const unsigned rates[] = {44100, 48000, 96000, 192000};

    Json j = Json::object();
    Json results = Json::array();
    unsigned accepted = 0, tested = 0;
    std::string otherErrors;
    const auto run = [&](const Layout* layouts, std::size_t nLayouts) {
        for (std::size_t li = 0; li < nLayouts; ++li) {
            const Layout& l = layouts[li];
            for (const Sample& s : samples) {
                std::string ok;
                for (unsigned r : rates) {
                    WAVEFORMATEXTENSIBLE w = makePcm(l.channels, s.valid, s.container, r, l.mask);
                    const HRESULT hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                                 reinterpret_cast<WAVEFORMATEX*>(&w), nullptr);
                    ++tested;
                    const bool good = hr == S_OK;
                    if (good) {
                        ++accepted;
                        if (!ok.empty()) ok += ' ';
                        ok += std::to_string(r);
                    } else if (hr != AUDCLNT_E_UNSUPPORTED_FORMAT && otherErrors.find(hrText(hr)) == std::string::npos) {
                        if (!otherErrors.empty()) otherErrors += " ; ";
                        otherErrors += hrText(hr);
                    }
                    Json e = Json::object();
                    e.addJson("channels", l.channels);
                    e.addJson("channel_mask", hex32(l.mask));
                    e.addJson("valid_bits", s.valid);
                    e.addJson("container_bits", s.container);
                    e.addJson("sample_rate", r);
                    e.addJson("supported", good);
                    e.addJson("hr", hrText(hr));
                    results.push(std::move(e));
                }
                const std::string bits = s.valid == s.container ? std::format("{} bits", s.valid)
                                                                : std::format("{}/{} bits", s.valid, s.container);
                j.addText(std::format("{} canaux {} ({}), {}", l.channels, hex32(l.mask), layoutName(l.mask), bits),
                          ok.empty() ? std::string("aucune fréquence") : ok + " Hz");
            }
        }
    };
    if (flow == eCapture) run(captureLayouts, std::size(captureLayouts));
    else run(renderLayouts, std::size(renderLayouts));
    j.add("accepted", "Acceptés", std::format("{} / {}", accepted, tested), Json::Show::TextOnly);
    j.addJson("accepted_count", accepted);
    j.addJson("tested_count", tested);
    if (!otherErrors.empty()) j.add("other_errors", "Autres réponses", otherErrors);
    j.addJson("results", std::move(results));
    return j;
}

Json describeAudioClient(IMMDevice* device, EDataFlow flow, bool testFormats) {
    Json j = Json::object();
    ComPtr<IAudioClient> client;
    HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) {
        j.add("error", "Indisponible", "Activate(IAudioClient) : " + hrText(hr));
        return j;
    }
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (SUCCEEDED(hr) && mix) {
        j.add("mix_format", "Format de mixage partagé", formatJson(mix, sizeof(WAVEFORMATEX) + mix->cbSize));
    } else {
        j.add("mix_format", "Format de mixage partagé", "GetMixFormat : " + hrText(hr));
    }
    REFERENCE_TIME def = 0, mn = 0;
    if (SUCCEEDED(client->GetDevicePeriod(&def, &mn))) {
        j.add("device_period", "Période (défaut / minimum)",
              std::format("{:.3f} ms / {:.3f} ms", static_cast<double>(def) / 10000.0, static_cast<double>(mn) / 10000.0),
              Json::Show::TextOnly);
        j.addJson("default_period_ms", Json::real(static_cast<double>(def) / 10000.0, 4));
        j.addJson("minimum_period_ms", Json::real(static_cast<double>(mn) / 10000.0, 4));
    }
    // IAudioClient3 : périodes du moteur partagé (latence faible), en trames.
    ComPtr<IAudioClient3> client3;
    if (mix && SUCCEEDED(client.As(&client3))) {
        UINT32 dflt = 0, fund = 0, pmin = 0, pmax = 0;
        if (SUCCEEDED(client3->GetSharedModeEnginePeriod(mix, &dflt, &fund, &pmin, &pmax))) {
            j.add("engine_period", "Période moteur partagé (trames : défaut, fondamentale, min, max)",
                  std::format("{}, {}, {}, {}", dflt, fund, pmin, pmax), Json::Show::TextOnly);
            j.addJson("engine_period_frames",
                      Json::object().addJson("default", dflt).addJson("fundamental", fund).addJson("min", pmin).addJson("max", pmax));
        }
    }
    if (mix) CoTaskMemFree(mix);
    if (testFormats) j.add("exclusive_formats", "Formats exclusifs (IsFormatSupported)", describeExclusiveFormats(client.Get(), flow));
    return j;
}

Json describeEndpoint(EndpointBasic& e, const ListOptions& opt) {
    Json j = Json::object();
    j.add("name", "Nom", toUtf8(e.friendlyName));
    j.add("description", "Description", toUtf8(e.deviceDesc));
    j.add("adapter", "Périphérique", toUtf8(e.interfaceName));
    j.add("id", "Identifiant", toUtf8(e.id));
    j.addJson("flow", flowName(e.flow));
    j.add("state", "État", stateName(e.state));
    j.add("is_xonar", "Xonar", e.isXonar);

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(e.device->OpenPropertyStore(STGM_READ, &store))) {
        j.add("properties", "Propriétés", describeProperties(store.Get()));
    }
    j.add("driver", "Pilote (devnode)", devnodeJson(e.devnode));

    const bool topologyUsable = e.state == DEVICE_STATE_ACTIVE || e.state == DEVICE_STATE_UNPLUGGED;
    if (topologyUsable) {
        j.add("jack_info", "Jacks (IKsJackDescription)", describeJacks(e.device.Get()));
    } else {
        j.add("jack_info", "Jacks (IKsJackDescription)", "non interrogés (endpoint " + stateName(e.state) + ")");
    }
    if (e.state == DEVICE_STATE_ACTIVE) {
        j.add("volume", "Volume (IAudioEndpointVolume)", describeVolume(e.device.Get()));
        j.add("audio_client", "Client audio (IAudioClient)", describeAudioClient(e.device.Get(), e.flow, opt.formats));
    } else {
        j.add("volume", "Volume (IAudioEndpointVolume)", "non interrogé (endpoint " + stateName(e.state) + ")");
        j.add("audio_client", "Client audio (IAudioClient)", "non interrogé (endpoint " + stateName(e.state) + ")");
    }
    return j;
}

} // namespace

void printListHelp() {
    printOut(
        "endpoint-probe list : inventaire en lecture seule des endpoints audio.\n\n"
        "Usage : endpoint-probe [list] [--all] [--no-formats] [--json]\n\n"
        "  --all          Tous les endpoints (par défaut : seulement ceux dont le nom ou le\n"
        "                 périphérique contient \"Xonar\", ou dont le devnode est VID_0B05&PID_189D).\n"
        "  --no-formats   Ne teste pas les formats exclusifs (IsFormatSupported).\n"
        "  --json         Sortie JSON sur stdout.\n\n"
        "Rendu puis capture, tous états (ACTIVE, DISABLED, NOTPRESENT, UNPLUGGED). Pour chaque\n"
        "endpoint : propriétés, formats du moteur, pilote du devnode, jacks, volume, format de\n"
        "mixage et formats exclusifs acceptés.\n\n"
        "Codes de retour : 0 OK, 2 aucun endpoint Xonar (sans --all), 1 erreur.");
}

int cmdList(const CommandArgs& a) {
    if (a.help) {
        printListHelp();
        return kExitOk;
    }
    ListOptions opt;
    for (const auto& arg : a.args) {
        if (arg == L"--all") opt.all = true;
        else if (arg == L"--no-formats") opt.formats = false;
        else {
            printErr("Option inconnue pour list : " + toUtf8(arg) + " (voir endpoint-probe list --help)");
            return kExitError;
        }
    }

    ComScope com;
    if (FAILED(com.hr())) {
        printErr("CoInitializeEx : " + hrText(com.hr()));
        return kExitError;
    }
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = createEnumerator(en);
    if (FAILED(hr)) {
        printErr("Création de MMDeviceEnumerator impossible : " + hrText(hr));
        return kExitError;
    }

    std::vector<EndpointBasic> eps;
    for (EDataFlow flow : {eRender, eCapture}) {
        hr = enumerateEndpoints(en.Get(), flow, DEVICE_STATEMASK_ALL, eps);
        if (FAILED(hr)) {
            printErr("EnumAudioEndpoints(" + flowName(flow) + ") : " + hrText(hr));
            return kExitError;
        }
    }

    // Tri stable : rendu puis capture, et dans chaque sens ACTIVE, UNPLUGGED, DISABLED, NOTPRESENT.
    const auto rank = [](DWORD s) {
        switch (s) {
        case DEVICE_STATE_ACTIVE: return 0;
        case DEVICE_STATE_UNPLUGGED: return 1;
        case DEVICE_STATE_DISABLED: return 2;
        default: return 3;
        }
    };
    std::stable_sort(eps.begin(), eps.end(), [&](const EndpointBasic& x, const EndpointBasic& y) {
        if (x.flow != y.flow) return x.flow == eRender;
        return rank(x.state) < rank(y.state);
    });

    std::size_t xonarCount = 0, shown = 0;
    for (const auto& e : eps) {
        if (e.isXonar) ++xonarCount;
    }

    Json list = Json::array();
    std::string text;
    for (auto& e : eps) {
        if (!opt.all && !e.isXonar) continue;
        ++shown;
        Json d = describeEndpoint(e, opt);
        if (!a.json) {
            text += std::format("=== [{}] {}  [{}]{} ===\n", e.flow == eRender ? "rendu" : "capture", toUtf8(e.friendlyName),
                                stateName(e.state), e.isXonar ? "  <Xonar>" : "");
            d.toText(text, 2);
            text += '\n';
        }
        list.push(std::move(d));
    }

    if (a.json) {
        Json root = Json::object();
        root.addJson("tool", "endpoint-probe");
        root.addJson("version", kToolVersion);
        root.addJson("command", "list");
        root.addJson("timestamp", timestampNow());
        root.addJson("filter", opt.all ? "all" : "xonar");
        root.addJson("endpoint_total", static_cast<std::uint64_t>(eps.size()));
        root.addJson("xonar_count", static_cast<std::uint64_t>(xonarCount));
        root.addJson("endpoints", std::move(list));
        printOut(root.toJson(2));
    } else {
        if (!text.empty()) printOut(text.substr(0, text.size() - 1));
        printOut(std::format("{} endpoint(s) affiché(s) sur {}, dont {} Xonar.", shown, eps.size(), xonarCount));
    }
    if (!opt.all && xonarCount == 0) {
        printErr("Aucun endpoint Xonar trouvé (utiliser --all pour lister tous les endpoints).");
        return kExitNoXonar;
    }
    return kExitOk;
}

} // namespace probe
