// endpoint-probe watch : suivi horodaté des endpoints Xonar.
//
// Deux sources : les notifications MMDevice (IMMNotificationClient : ajout, retrait,
// changement d'état, de propriété, de périphérique par défaut) et une relecture, chaque
// seconde, de IsConnected sur les jacks (IKsJackDescription), qui révèle ce que le
// pilote remonte réellement comme détection de présence. Lecture seule.
#include "commands.hpp"
#include "probe_common.hpp"

#include <atomic>
#include <format>
#include <map>
#include <mutex>

namespace probe {

namespace {

std::atomic<bool> gStop{false};
HANDLE gWake = nullptr;

BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        gStop = true;
        if (gWake) SetEvent(gWake);
        return TRUE;
    }
    return FALSE;
}

enum class EvKind { Added, Removed, State, Default, Property };

struct Event {
    EvKind kind = EvKind::State;
    std::wstring id;
    DWORD state = 0;
    PROPERTYKEY key{};
    EDataFlow flow = eRender;
    ERole role = eConsole;
    std::string time;
};

// Les rappels arrivent sur un thread du service audio : ils se contentent d'empiler
// l'événement (horodaté à la réception) et de réveiller la boucle principale, comme le
// recommande la documentation (pas d'appel bloquant dans un rappel).
class Notifier final : public IMMNotificationClient {
public:
    explicit Notifier(HANDLE wake) : wake_(wake) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD state) override {
        Event e;
        e.kind = EvKind::State;
        e.id = id ? id : L"";
        e.state = state;
        push(std::move(e));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        Event e;
        e.kind = EvKind::Added;
        e.id = id ? id : L"";
        push(std::move(e));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        Event e;
        e.kind = EvKind::Removed;
        e.id = id ? id : L"";
        push(std::move(e));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        Event e;
        e.kind = EvKind::Default;
        e.flow = flow;
        e.role = role;
        e.id = id ? id : L"";
        push(std::move(e));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
        Event e;
        e.kind = EvKind::Property;
        e.id = id ? id : L"";
        e.key = key;
        push(std::move(e));
        return S_OK;
    }

    std::vector<Event> drain() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Event> out;
        out.swap(queue_);
        return out;
    }

private:
    ~Notifier() = default;
    void push(Event e) {
        e.time = timestampNow();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(e));
        }
        SetEvent(wake_);
    }

    LONG ref_ = 1;
    HANDLE wake_;
    std::mutex mutex_;
    std::vector<Event> queue_;
};

struct Tracked {
    std::wstring name;
    EDataFlow flow = eRender;
    DWORD state = 0;
    bool isXonar = false;
    JackAccess jacks;
    bool jacksTried = false;
    std::vector<int> connected; // -1 inconnu, 0 non, 1 oui
    std::vector<std::string> jackLabels;
};

struct WatchOptions {
    double seconds = 0.0; // 0 : jusqu'à Ctrl+C
    bool all = false;
    bool verbose = false;
};

const char* roleName(ERole r) {
    switch (r) {
    case eConsole: return "console";
    case eMultimedia: return "multimedia";
    case eCommunications: return "communications";
    default: return "?";
    }
}

class Watcher {
public:
    Watcher(IMMDeviceEnumerator* en, const WatchOptions& opt, bool json) : en_(en), opt_(opt), json_(json) {}

    void snapshot() {
        for (EDataFlow flow : {eRender, eCapture}) {
            std::vector<EndpointBasic> eps;
            enumerateEndpoints(en_, flow, DEVICE_STATEMASK_ALL, eps);
            for (auto& e : eps) {
                Tracked& t = tracked_[e.id];
                t.name = e.friendlyName;
                t.flow = e.flow;
                t.state = e.state;
                t.isXonar = e.isXonar;
                if (e.isXonar) sawXonar_ = true;
                if (!wanted(t)) continue;
                emit(Json::object()
                         .addJson("time", timestampNow())
                         .addJson("event", "initial")
                         .addJson("id", toUtf8(e.id))
                         .addJson("name", toUtf8(t.name))
                         .addJson("flow", flowName(t.flow))
                         .addJson("state", stateName(t.state)),
                     std::format("[{}] INITIAL  [{}] {} : {}", timestampNow(), flowLabel(t.flow), toUtf8(t.name), stateName(t.state)));
            }
        }
    }

    void handle(const Event& ev) {
        switch (ev.kind) {
        case EvKind::Added: {
            Tracked& t = refresh(ev.id);
            if (!wanted(t)) return;
            emit(base(ev, "added", t), std::format("[{}] AJOUT    [{}] {} ({})", ev.time, flowLabel(t.flow), toUtf8(t.name), stateName(t.state)));
            break;
        }
        case EvKind::Removed: {
            auto it = tracked_.find(ev.id);
            if (it == tracked_.end()) return;
            if (wanted(it->second))
                emit(base(ev, "removed", it->second), std::format("[{}] RETRAIT  [{}] {}", ev.time, flowLabel(it->second.flow), toUtf8(it->second.name)));
            tracked_.erase(it);
            break;
        }
        case EvKind::State: {
            const auto it = tracked_.find(ev.id);
            const DWORD before = it != tracked_.end() ? it->second.state : 0;
            Tracked& t = refresh(ev.id);
            t.state = ev.state; // l'état transmis par le rappel fait foi
            t.jacks = JackAccess{};
            t.jacksTried = false;
            t.connected.clear();
            if (!wanted(t)) return;
            Json j = base(ev, "state", t);
            j.addJson("old_state", before ? Json(stateName(before)) : Json());
            emit(std::move(j), std::format("[{}] ETAT     [{}] {} : {} -> {}", ev.time, flowLabel(t.flow), toUtf8(t.name),
                                           before ? stateName(before) : std::string("?"), stateName(ev.state)));
            break;
        }
        case EvKind::Property: {
            auto it = tracked_.find(ev.id);
            Tracked& t = it != tracked_.end() ? it->second : refresh(ev.id);
            if (!wanted(t)) return;
            std::string value = "n/d";
            ComPtr<IMMDevice> dev;
            ComPtr<IPropertyStore> store;
            if (SUCCEEDED(en_->GetDevice(ev.id.c_str(), &dev)) && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)))
                value = propertyValueText(store.Get(), ev.key);
            Json j = base(ev, "property", t);
            j.addJson("key", propertyKeyName(ev.key));
            j.addJson("key_raw", std::format("{},{}", guidText(ev.key.fmtid), ev.key.pid));
            j.addJson("value", value);
            emit(std::move(j), std::format("[{}] PROPRIETE [{}] {} : {} = {}", ev.time, flowLabel(t.flow), toUtf8(t.name),
                                           propertyKeyName(ev.key), value));
            break;
        }
        case EvKind::Default: {
            std::string name = "(aucun)";
            bool xonar = false;
            if (!ev.id.empty()) {
                Tracked& t = refresh(ev.id);
                name = toUtf8(t.name);
                xonar = t.isXonar;
            }
            if (!opt_.all && !xonar) return;
            emit(Json::object()
                     .addJson("time", ev.time)
                     .addJson("event", "default")
                     .addJson("flow", flowName(ev.flow))
                     .addJson("role", roleName(ev.role))
                     .addJson("id", toUtf8(ev.id))
                     .addJson("name", name),
                 std::format("[{}] DEFAUT   {} / {} -> {}", ev.time, flowName(ev.flow), roleName(ev.role), name));
            break;
        }
        }
    }

    // Relecture de IsConnected pour chaque jack des endpoints suivis.
    void pollJacks() {
        for (auto& [id, t] : tracked_) {
            if (!wanted(t)) continue;
            if (t.state != DEVICE_STATE_ACTIVE && t.state != DEVICE_STATE_UNPLUGGED) continue;
            if (!t.jacks.jack && !t.jacksTried) {
                t.jacksTried = true;
                ComPtr<IMMDevice> dev;
                if (SUCCEEDED(en_->GetDevice(id.c_str(), &dev))) t.jacks = openJacks(dev.Get());
                if (!t.jacks.jack) {
                    emit(Json::object()
                             .addJson("time", timestampNow())
                             .addJson("event", "jack_unavailable")
                             .addJson("id", toUtf8(id))
                             .addJson("name", toUtf8(t.name))
                             .addJson("error", std::format("{} : {}", t.jacks.step, hrText(t.jacks.hr))),
                         std::format("[{}] JACK     [{}] {} : indisponible ({} : {})", timestampNow(), flowLabel(t.flow),
                                     toUtf8(t.name), t.jacks.step, hrText(t.jacks.hr)));
                }
            }
            if (!t.jacks.jack) continue;
            UINT count = 0;
            if (FAILED(t.jacks.jack->GetJackCount(&count))) continue;
            if (t.connected.size() != count) {
                t.connected.assign(count, -1);
                t.jackLabels.assign(count, {});
            }
            for (UINT i = 0; i < count; ++i) {
                KSJACK_DESCRIPTION d{};
                const HRESULT hr = t.jacks.jack->GetJackDescription(i, &d);
                if (FAILED(hr)) {
                    // Endpoint invalidé (débranchement) : on rouvrira la topologie au prochain état.
                    t.jacks = JackAccess{};
                    break;
                }
                const int now = d.IsConnected ? 1 : 0;
                if (t.jackLabels[i].empty())
                    t.jackLabels[i] = std::format("jack {} ({}, {})", i, colorName(static_cast<std::uint32_t>(d.Color)),
                                                  speakerList(d.ChannelMapping));
                const int before = t.connected[i];
                if (before == now && !opt_.verbose) continue;
                t.connected[i] = now;
                const auto word = [](int v) { return v < 0 ? "?" : (v ? "connecté" : "déconnecté"); };
                emit(Json::object()
                         .addJson("time", timestampNow())
                         .addJson("event", before < 0 ? "jack_initial" : (before == now ? "jack_poll" : "jack_changed"))
                         .addJson("id", toUtf8(id))
                         .addJson("name", toUtf8(t.name))
                         .addJson("jack", i)
                         .addJson("is_connected", now == 1)
                         .addJson("previous", before < 0 ? Json() : Json(before == 1)),
                     before < 0 || before == now
                         ? std::format("[{}] JACK     [{}] {} : {} {}", timestampNow(), flowLabel(t.flow), toUtf8(t.name),
                                       t.jackLabels[i], word(now))
                         : std::format("[{}] JACK     [{}] {} : {} {} -> {}", timestampNow(), flowLabel(t.flow), toUtf8(t.name),
                                       t.jackLabels[i], word(before), word(now)));
            }
        }
    }

    bool sawXonar() const { return sawXonar_; }

private:
    bool wanted(const Tracked& t) const { return opt_.all || t.isXonar; }
    static const char* flowLabel(EDataFlow f) { return f == eCapture ? "capture" : "rendu"; }

    Tracked& refresh(const std::wstring& id) {
        Tracked& t = tracked_[id];
        ComPtr<IMMDevice> dev;
        if (SUCCEEDED(en_->GetDevice(id.c_str(), &dev))) {
            EndpointBasic e;
            if (describeBasic(dev.Get(), e)) {
                t.name = e.friendlyName;
                t.flow = e.flow;
                t.state = e.state;
                t.isXonar = e.isXonar;
            }
        }
        if (t.name.empty()) t.name = id;
        if (t.isXonar) sawXonar_ = true;
        return t;
    }

    Json base(const Event& ev, const char* kind, const Tracked& t) const {
        return Json::object()
            .addJson("time", ev.time)
            .addJson("event", kind)
            .addJson("id", toUtf8(ev.id))
            .addJson("name", toUtf8(t.name))
            .addJson("flow", flowName(t.flow))
            .addJson("state", stateName(t.state));
    }

    void emit(Json j, const std::string& text) const {
        if (json_) printOut(j.toJson(0)); // une ligne JSON par événement (JSON Lines)
        else printOut(text);
    }

    IMMDeviceEnumerator* en_;
    WatchOptions opt_;
    bool json_;
    bool sawXonar_ = false;
    std::map<std::wstring, Tracked> tracked_;
};

} // namespace

void printWatchHelp() {
    printOut(
        "endpoint-probe watch : suit les endpoints Xonar et leurs jacks, horodaté.\n\n"
        "Usage : endpoint-probe watch [--seconds N] [--all] [--verbose] [--json]\n\n"
        "  --seconds N   Durée d'observation (défaut : jusqu'à Ctrl+C).\n"
        "  --all         Suit tous les endpoints, pas seulement les Xonar.\n"
        "  --verbose     Affiche chaque relecture de jack, pas seulement les changements.\n"
        "  --json        Une ligne JSON par événement (JSON Lines).\n\n"
        "Événements : INITIAL, AJOUT, RETRAIT, ETAT (ACTIVE, DISABLED, NOTPRESENT, UNPLUGGED),\n"
        "PROPRIETE (clé et nouvelle valeur), DEFAUT, JACK (IsConnected relu chaque seconde).\n\n"
        "Codes de retour : 0 OK, 2 aucun endpoint Xonar vu pendant la session (sans --all), 1 erreur.");
}

int cmdWatch(const CommandArgs& a) {
    if (a.help) {
        printWatchHelp();
        return kExitOk;
    }
    WatchOptions opt;
    for (std::size_t i = 0; i < a.args.size(); ++i) {
        const auto& arg = a.args[i];
        if (arg == L"--all") {
            opt.all = true;
        } else if (arg == L"--verbose") {
            opt.verbose = true;
        } else if (arg == L"--seconds" && i + 1 < a.args.size()) {
            const std::string v = toUtf8(a.args[++i]);
            char* end = nullptr;
            opt.seconds = std::strtod(v.c_str(), &end);
            if (!end || *end != '\0' || opt.seconds < 0) {
                printErr("watch : --seconds invalide");
                return kExitError;
            }
        } else {
            printErr("Option inconnue pour watch : " + toUtf8(arg) + " (voir endpoint-probe watch --help)");
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
    gWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!gWake) {
        printErr("CreateEvent a échoué");
        return kExitError;
    }
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    Notifier* notifier = new Notifier(gWake);
    hr = en->RegisterEndpointNotificationCallback(notifier);
    if (FAILED(hr)) {
        notifier->Release();
        printErr("RegisterEndpointNotificationCallback : " + hrText(hr));
        return kExitError;
    }

    Watcher w(en.Get(), opt, a.json);
    if (!a.json) {
        printOut(opt.seconds > 0 ? std::format("Observation pendant {} s (Ctrl+C pour arrêter).", opt.seconds)
                                 : std::string("Observation jusqu'à Ctrl+C."));
    }
    w.snapshot();
    w.pollJacks();

    const ULONGLONG start = GetTickCount64();
    ULONGLONG nextPoll = start + 1000;
    while (!gStop) {
        const ULONGLONG now = GetTickCount64();
        if (opt.seconds > 0 && now - start >= static_cast<ULONGLONG>(opt.seconds * 1000.0)) break;
        const DWORD waitMs = nextPoll > now ? static_cast<DWORD>(nextPoll - now) : 0;
        WaitForSingleObject(gWake, waitMs);
        for (const Event& ev : notifier->drain()) w.handle(ev);
        if (GetTickCount64() >= nextPoll) {
            w.pollJacks();
            nextPoll += 1000;
            if (nextPoll < GetTickCount64()) nextPoll = GetTickCount64() + 1000;
        }
    }

    en->UnregisterEndpointNotificationCallback(notifier);
    notifier->Release();
    SetConsoleCtrlHandler(ctrlHandler, FALSE);
    CloseHandle(gWake);
    gWake = nullptr;
    if (!a.json) printOut("Fin de l'observation.");
    if (!opt.all && !w.sawXonar()) {
        printErr("Aucun endpoint Xonar vu pendant l'observation.");
        return kExitNoXonar;
    }
    return kExitOk;
}

} // namespace probe
