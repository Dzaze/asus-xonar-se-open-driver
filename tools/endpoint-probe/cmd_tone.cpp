// endpoint-probe tone : joue un sinus sur un seul canal d'un endpoint de rendu Xonar,
// via WASAPI, et mesure la tenue du flux (horloge, remplissages tardifs, sous-alimentations).
//
// Garde-fous de sécurité (non contournables par option) :
//  - niveau plafonné à -20 dBFS : toute valeur supérieure est refusée avant toute
//    ouverture de périphérique ;
//  - rampes d'attaque et de relâchement de 20 ms (demi-cosinus) pour éviter les clics ;
//  - endpoint non Xonar refusé, sauf --any-device explicite ;
//  - aucune modification du périphérique par défaut, du volume ni du format : le flux
//    partagé utilise le format de mixage existant (AUDCLNT_STREAMFLAGS_NOPERSIST), le flux
//    exclusif n'existe que pendant la lecture et Windows restaure ensuite le moteur.
#include "commands.hpp"
#include "probe_common.hpp"

#include <avrt.h>

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

namespace probe {

namespace {

constexpr double kMaxLevelDbfs = -20.0;
constexpr double kMinLevelDbfs = -90.0;
constexpr double kRampSeconds = 0.020;
constexpr REFERENCE_TIME kBufferDuration = 1'000'000; // 100 ms en unités de 100 ns

struct FormatSpec {
    unsigned channels = 0;
    unsigned validBits = 0;
    unsigned containerBits = 0;
    unsigned rate = 0;
    std::uint32_t mask = 0;
    bool explicitMask = false;
    bool explicitContainer = false;
    bool isFloat = false;
};

struct ToneOptions {
    std::wstring endpoint;
    std::string channel;
    double freq = 440.0;
    double level = -20.0;
    double seconds = 2.0;
    bool exclusive = false;
    std::optional<FormatSpec> format;
    bool anyDevice = false;
};

bool parseNumber(const std::wstring& w, double& out) {
    std::string s = toUtf8(w);
    std::replace(s.begin(), s.end(), ',', '.'); // accepte "0,5" comme "0.5"
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end && *end == '\0' && std::isfinite(out);
}

bool parseUnsigned(const std::string& s, unsigned& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (!end || *end != '\0' || v > 0xFFFFFFFFul) return false;
    out = static_cast<unsigned>(v);
    return true;
}

// Format "CxBxR[:MASQUE]" : B vaut 16, 24 (24 bits compacts, repli 24 dans 32 si refusé),
// 24in32, 32 ou f32 (flottant 32 bits). Exemple : 6x24x48000:0x60F.
bool parseFormatSpec(const std::string& text, FormatSpec& f, std::string& err) {
    std::string body = text, maskPart;
    const auto colon = text.find(':');
    if (colon != std::string::npos) {
        body = text.substr(0, colon);
        maskPart = text.substr(colon + 1);
    }
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const auto x = body.find_first_of("xX", start);
        parts.push_back(body.substr(start, x == std::string::npos ? std::string::npos : x - start));
        if (x == std::string::npos) break;
        start = x + 1;
    }
    if (parts.size() != 3) {
        err = "format attendu CxBxR[:MASQUE], par exemple 6x24x48000 ou 2x16x44100:0x3";
        return false;
    }
    if (!parseUnsigned(parts[0], f.channels) || f.channels < 1 || f.channels > 8) {
        err = "nombre de canaux invalide (1 à 8)";
        return false;
    }
    const std::string& b = parts[1];
    if (b == "16") {
        f.validBits = f.containerBits = 16;
    } else if (b == "24") {
        f.validBits = f.containerBits = 24;
    } else if (b == "24in32") {
        f.validBits = 24;
        f.containerBits = 32;
        f.explicitContainer = true;
    } else if (b == "32") {
        f.validBits = f.containerBits = 32;
    } else if (b == "f32") {
        f.validBits = f.containerBits = 32;
        f.isFloat = true;
    } else {
        err = "profondeur invalide (16, 24, 24in32, 32 ou f32)";
        return false;
    }
    if (!parseUnsigned(parts[2], f.rate) || f.rate < 8000 || f.rate > 384000) {
        err = "fréquence d'échantillonnage invalide (8000 à 384000)";
        return false;
    }
    if (!maskPart.empty()) {
        unsigned m = 0;
        if (!parseUnsigned(maskPart, m) || static_cast<unsigned>(std::popcount(m)) != f.channels) {
            err = "masque invalide ou nombre de bits différent du nombre de canaux";
            return false;
        }
        f.mask = m;
        f.explicitMask = true;
    }
    return true;
}

WAVEFORMATEXTENSIBLE buildFormat(const FormatSpec& f, unsigned containerBits) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = static_cast<WORD>(f.channels);
    w.Format.nSamplesPerSec = f.rate;
    w.Format.wBitsPerSample = static_cast<WORD>(containerBits);
    w.Format.nBlockAlign = static_cast<WORD>(f.channels * containerBits / 8);
    w.Format.nAvgBytesPerSec = f.rate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = static_cast<WORD>(f.validBits);
    w.dwChannelMask = f.mask;
    w.SubFormat = f.isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT : KSDATAFORMAT_SUBTYPE_PCM;
    return w;
}

// Générateur de sinus mono avec enveloppe (rampes demi-cosinus de 20 ms).
class ToneGenerator {
public:
    ToneGenerator(double freq, double levelDbfs, unsigned rate, std::uint64_t totalFrames)
        : step_(2.0 * std::numbers::pi * freq / rate),
          amplitude_(std::pow(10.0, std::min(levelDbfs, kMaxLevelDbfs) / 20.0)), // plafond appliqué ici aussi
          total_(totalFrames),
          ramp_(std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(kRampSeconds * rate)))) {}

    double next() {
        const double v = amplitude_ * envelope(n_) * std::sin(phase_);
        phase_ += step_;
        if (phase_ > 2.0 * std::numbers::pi) phase_ -= 2.0 * std::numbers::pi;
        ++n_;
        return v;
    }
    double amplitude() const { return amplitude_; }

private:
    double envelope(std::uint64_t n) const {
        double g = 1.0;
        if (n < ramp_) g = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(n) / static_cast<double>(ramp_)));
        const std::uint64_t remaining = n < total_ ? total_ - 1 - n : 0;
        if (remaining < ramp_) {
            const double r = 0.5 * (1.0 - std::cos(std::numbers::pi * static_cast<double>(remaining) / static_cast<double>(ramp_)));
            g = std::min(g, r);
        }
        return n >= total_ ? 0.0 : g;
    }

    double step_;
    double amplitude_;
    std::uint64_t total_;
    std::uint64_t ramp_;
    std::uint64_t n_ = 0;
    double phase_ = 0.0;
};

// Écrit `frames` trames : silence partout sauf sur le canal `index`.
void fillFrames(BYTE* dst, UINT32 frames, const FormatView& f, unsigned index, ToneGenerator& gen) {
    const unsigned bytesPerSample = f.containerBits / 8;
    std::memset(dst, 0, static_cast<std::size_t>(frames) * f.blockAlign);
    for (UINT32 i = 0; i < frames; ++i) {
        const double s = gen.next();
        BYTE* p = dst + static_cast<std::size_t>(i) * f.blockAlign + static_cast<std::size_t>(index) * bytesPerSample;
        if (f.isFloat && f.containerBits == 32) {
            const float v = static_cast<float>(s);
            std::memcpy(p, &v, sizeof(v));
        } else if (f.containerBits == 16) {
            const auto v = static_cast<std::int16_t>(std::lround(s * 32767.0));
            std::memcpy(p, &v, sizeof(v));
        } else if (f.containerBits == 24) {
            const auto v = static_cast<std::int32_t>(std::lround(s * 8388607.0));
            p[0] = static_cast<BYTE>(v & 0xFF);
            p[1] = static_cast<BYTE>((v >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((v >> 16) & 0xFF);
        } else if (f.containerBits == 32) {
            // Entier 32 bits pleine échelle : valable aussi pour 24 bits utiles cadrés à gauche.
            const auto v = static_cast<std::int32_t>(std::llround(s * 2147483647.0));
            std::memcpy(p, &v, sizeof(v));
        }
    }
}

double qpcSeconds(LONGLONG from, LONGLONG to) {
    static const LONGLONG freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return static_cast<double>(to - from) / static_cast<double>(freq);
}

LONGLONG qpcNow() {
    LARGE_INTEGER t{};
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

struct RunResult {
    bool ok = false;
    bool channelAbsent = false; // canal absent du masque du format (sauté avec "all")
    Json report = Json::object();
};

HRESULT activateClient(IMMDevice* device, ComPtr<IAudioClient>& client) {
    client.Reset();
    return device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
}

// Joue le canal `bit`. Retourne un rapport détaillé ; ok = false en cas d'erreur.
RunResult playChannel(IMMDevice* device, IPropertyStore* store, const ToneOptions& opt, std::uint32_t bit, bool json) {
    RunResult res;
    Json& r = res.report;
    r.add("channel", "Canal", speakerName(bit));
    const auto fail = [&](const std::string& msg) {
        r.add("error", "Erreur", msg);
        res.ok = false;
        return res;
    };

    ComPtr<IAudioClient> client;
    HRESULT hr = activateClient(device, client);
    if (FAILED(hr)) return fail("Activate(IAudioClient) : " + hrText(hr));

    // --- Choix du format -------------------------------------------------
    std::vector<BYTE> fmt;
    if (!opt.exclusive) {
        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) return fail("GetMixFormat : " + hrText(hr));
        fmt.assign(reinterpret_cast<BYTE*>(mix), reinterpret_cast<BYTE*>(mix) + sizeof(WAVEFORMATEX) + mix->cbSize);
        CoTaskMemFree(mix);
    } else {
        FormatSpec spec;
        if (opt.format) {
            spec = *opt.format;
        } else {
            // Sans --format : format du moteur (PKEY_AudioEngine_DeviceFormat), que le
            // pilote a déjà accepté et qui ne change donc rien côté matériel.
            const auto blob = propBlob(store, PKEY_AudioEngine_DeviceFormat);
            const auto v = blob ? viewFormat(reinterpret_cast<const WAVEFORMATEX*>(blob->data()), blob->size()) : std::nullopt;
            if (!v) return fail("aucun format du moteur lisible ; préciser --format CxBxR");
            spec.channels = v->channels;
            spec.validBits = v->validBits;
            spec.containerBits = v->containerBits;
            spec.rate = v->sampleRate;
            spec.isFloat = v->isFloat;
            spec.explicitContainer = true;
            if (v->hasMask) {
                spec.mask = v->mask;
                spec.explicitMask = true;
            }
        }
        if (!spec.explicitMask) {
            // Masque : haut-parleurs physiques déclarés s'ils correspondent, sinon masque standard.
            const auto ps = propUInt(store, PKEY_AudioEndpoint_PhysicalSpeakers);
            spec.mask = (ps && static_cast<unsigned>(std::popcount(*ps)) == spec.channels) ? *ps : defaultMaskFor(spec.channels);
        }
        WAVEFORMATEXTENSIBLE w = buildFormat(spec, spec.containerBits);
        hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&w), nullptr);
        if (hr != S_OK && spec.validBits == 24 && spec.containerBits == 24 && !spec.explicitContainer) {
            // Beaucoup de pilotes USB n'exposent que 24 bits dans un conteneur de 32.
            WAVEFORMATEXTENSIBLE w32 = buildFormat(spec, 32);
            const HRESULT hr32 = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&w32), nullptr);
            if (hr32 == S_OK) {
                r.add("format_note", "Note", "24 bits compacts refusés, repli sur 24 bits dans 32");
                w = w32;
                hr = hr32;
            }
        }
        if (hr != S_OK) return fail("format exclusif refusé (IsFormatSupported) : " + hrText(hr));
        fmt.assign(reinterpret_cast<BYTE*>(&w), reinterpret_cast<BYTE*>(&w) + sizeof(w));
    }
    const auto* wfx = reinterpret_cast<const WAVEFORMATEX*>(fmt.data());
    const auto view = viewFormat(wfx, fmt.size());
    if (!view) return fail("format illisible");
    r.add("format", "Format du flux", formatJson(wfx, fmt.size()));
    const bool writable = (view->isFloat && view->containerBits == 32) ||
                          (view->isPcm && (view->containerBits == 16 || view->containerBits == 24 || view->containerBits == 32));
    if (!writable) return fail("type d'échantillon non géré par l'outil : " + formatSummary(*view));

    const std::uint32_t mask = view->hasMask ? view->mask : defaultMaskFor(view->channels);
    if (!(mask & bit)) {
        res.channelAbsent = true; // aucun flux n'a encore été initialisé
        return fail(std::format("canal {} absent du format (masque {} : {})", speakerName(bit), hex32(mask), speakerList(mask)));
    }
    const auto index = static_cast<unsigned>(std::popcount(mask & (bit - 1)));
    if (index >= view->channels) return fail("indice de canal hors format");
    r.add("channel_index", "Indice dans la trame", index);

    const double nyquist = view->sampleRate / 2.0;
    if (opt.freq >= nyquist * 0.9) return fail(std::format("fréquence {} Hz trop proche de Nyquist ({} Hz)", opt.freq, nyquist));

    // --- Initialisation du flux -----------------------------------------
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defPeriod, &minPeriod);
    REFERENCE_TIME bufferDuration = kBufferDuration;
    const AUDCLNT_SHAREMODE mode = opt.exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED;
    const DWORD flags = opt.exclusive ? 0 : AUDCLNT_STREAMFLAGS_NOPERSIST;
    const REFERENCE_TIME periodicity = opt.exclusive ? defPeriod : 0;
    hr = client->Initialize(mode, flags, bufferDuration, periodicity, wfx, nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Procédure documentée : relire la taille alignée, recréer le client, réessayer.
        UINT32 aligned = 0;
        client->GetBufferSize(&aligned);
        bufferDuration = static_cast<REFERENCE_TIME>(10'000'000.0 * aligned / view->sampleRate + 0.5);
        hr = activateClient(device, client);
        if (SUCCEEDED(hr)) hr = client->Initialize(mode, flags, bufferDuration, periodicity, wfx, nullptr);
        r.add("buffer_realigned", "Tampon réaligné", true);
    }
    if (FAILED(hr)) return fail("IAudioClient::Initialize : " + hrText(hr));

    UINT32 bufferFrames = 0;
    hr = client->GetBufferSize(&bufferFrames);
    if (FAILED(hr) || bufferFrames == 0) return fail("GetBufferSize : " + hrText(hr));
    ComPtr<IAudioRenderClient> render;
    hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) return fail("GetService(IAudioRenderClient) : " + hrText(hr));
    ComPtr<IAudioClock> clock;
    hr = client->GetService(IID_PPV_ARGS(&clock));
    if (FAILED(hr)) return fail("GetService(IAudioClock) : " + hrText(hr));
    UINT64 clockFreq = 0;
    clock->GetFrequency(&clockFreq);

    r.add("buffer_frames", "Tampon (trames)", bufferFrames);
    r.add("buffer_ms", "Tampon (ms)", Json::real(1000.0 * bufferFrames / view->sampleRate, 2));
    r.add("device_period_ms", "Période du périphérique (ms)", Json::real(static_cast<double>(defPeriod) / 10000.0, 3));

    const auto totalFrames = static_cast<std::uint64_t>(std::llround(opt.seconds * view->sampleRate));
    ToneGenerator gen(opt.freq, opt.level, view->sampleRate, totalFrames);
    std::uint64_t written = 0;
    const auto writeFrames = [&](UINT32 n) -> HRESULT {
        BYTE* data = nullptr;
        HRESULT h = render->GetBuffer(n, &data);
        if (FAILED(h)) return h;
        fillFrames(data, n, *view, index, gen);
        h = render->ReleaseBuffer(n, 0);
        if (SUCCEEDED(h)) written += n;
        return h;
    };

    // Préremplissage complet avant Start().
    hr = writeFrames(static_cast<UINT32>(std::min<std::uint64_t>(bufferFrames, totalFrames)));
    if (FAILED(hr)) return fail("préremplissage : " + hrText(hr));

    const std::string announce = std::format("Canal joué : {} (indice {} sur {}, masque {}), {} Hz, {} dBFS, {} s, mode {}",
                                             speakerName(bit), index, view->channels, hex32(mask), opt.freq, opt.level,
                                             opt.seconds, opt.exclusive ? "exclusif" : "partagé");
    if (json) printErr(announce);
    else printOut(announce);

    // MMCSS : priorité temps réel du planificateur multimédia pour ce thread.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    // Réveil toutes les 5 ms (ou un quart du tampon s'il est plus court).
    const double bufferMs = 1000.0 * bufferFrames / view->sampleRate;
    const double wakeMs = std::min(5.0, bufferMs / 4.0);
    unsigned late = 0, glitches = 0, wakes = 0;
    double maxIntervalMs = 0.0;
    std::string loopError;

    hr = client->Start();
    if (FAILED(hr)) {
        if (timer) CloseHandle(timer);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        return fail("IAudioClient::Start : " + hrText(hr));
    }
    const LONGLONG t0 = qpcNow();
    LONGLONG prev = t0;
    LONGLONG drainedAt = 0;
    const double timeoutS = opt.seconds + 3.0;
    for (;;) {
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(wakeMs * 10000.0);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        } else {
            Sleep(static_cast<DWORD>(wakeMs));
        }
        const LONGLONG now = qpcNow();
        maxIntervalMs = std::max(maxIntervalMs, 1000.0 * qpcSeconds(prev, now));
        prev = now;
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            loopError = "GetCurrentPadding : " + hrText(hr);
            break;
        }
        if (written < totalFrames) {
            ++wakes;
            if (padding == 0) ++glitches;                 // tampon vidé : sous-alimentation
            else if (padding < bufferFrames / 4) ++late; // réserve inférieure à 25 % du tampon
            const auto room = static_cast<UINT32>(std::min<std::uint64_t>(bufferFrames - padding, totalFrames - written));
            if (room > 0) {
                hr = writeFrames(room);
                if (FAILED(hr)) {
                    loopError = "GetBuffer/ReleaseBuffer : " + hrText(hr);
                    break;
                }
            }
        } else if (padding == 0) {
            drainedAt = now; // tout le signal a été consommé par le moteur
            break;
        }
        if (qpcSeconds(t0, now) > timeoutS) {
            loopError = "délai dépassé en attendant la fin du flux";
            break;
        }
    }
    const LONGLONG tEnd = drainedAt ? drainedAt : qpcNow();
    UINT64 pos = 0, posQpc = 0;
    const HRESULT hrPos = clock->GetPosition(&pos, &posQpc);
    client->Stop();
    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);

    const double expectedS = static_cast<double>(totalFrames) / view->sampleRate;
    r.add("frames_expected", "Trames attendues", totalFrames);
    r.add("frames_written", "Trames écrites", written);
    if (SUCCEEDED(hrPos) && clockFreq) {
        const double posS = static_cast<double>(pos) / static_cast<double>(clockFreq);
        const double posFrames = posS * view->sampleRate;
        r.add("clock_position_frames", "Position IAudioClock (trames)", Json::real(posFrames, 0));
        r.add("clock_offset_ms", "Écart position / attendu (ms)", Json::real(1000.0 * (posS - expectedS), 2));
        r.addJson("clock_frequency", clockFreq);
    } else {
        r.add("clock_position_frames", "Position IAudioClock (trames)", "GetPosition : " + hrText(hrPos));
    }
    r.add("late_fills", "Remplissages tardifs (réserve < 25 %)", late);
    r.add("glitches", "Glitches (padding à zéro en cours de flux)", glitches);
    r.add("wakes", "Réveils de remplissage", wakes);
    r.add("max_wake_interval_ms", "Intervalle de réveil maximal (ms)", Json::real(maxIntervalMs, 2));
    r.add("duration_expected_s", "Durée attendue (s)", Json::real(expectedS, 3));
    r.add("duration_real_s", "Durée réelle Start -> fin du flux (s)", Json::real(qpcSeconds(t0, tEnd), 3));
    r.add("mmcss", "MMCSS (Pro Audio)", mmcss != nullptr);
    if (!loopError.empty()) return fail(loopError);
    res.ok = true;
    r.add("result", "Résultat", glitches == 0 ? "OK" : "OK avec glitches");
    return res;
}

bool parseToneArgs(const std::vector<std::wstring>& args, ToneOptions& o, std::string& err) {
    for (std::size_t i = 0; i < args.size(); ++i) {
        const std::wstring& a = args[i];
        const auto value = [&](std::wstring& out) {
            if (i + 1 >= args.size()) {
                err = "valeur manquante après " + toUtf8(a);
                return false;
            }
            out = args[++i];
            return true;
        };
        std::wstring v;
        if (a == L"--endpoint") {
            if (!value(o.endpoint)) return false;
        } else if (a == L"--channel") {
            if (!value(v)) return false;
            o.channel = toUtf8(v);
        } else if (a == L"--freq" || a == L"--level" || a == L"--seconds") {
            if (!value(v)) return false;
            double& target = a == L"--freq" ? o.freq : (a == L"--level" ? o.level : o.seconds);
            if (!parseNumber(v, target)) {
                err = toUtf8(a) + " : nombre invalide « " + toUtf8(v) + " »";
                return false;
            }
        } else if (a == L"--mode") {
            if (!value(v)) return false;
            if (v == L"shared") o.exclusive = false;
            else if (v == L"exclusive") o.exclusive = true;
            else {
                err = "--mode attend shared ou exclusive";
                return false;
            }
        } else if (a == L"--format") {
            if (!value(v)) return false;
            FormatSpec f;
            if (!parseFormatSpec(toUtf8(v), f, err)) {
                err = "--format : " + err;
                return false;
            }
            o.format = f;
        } else if (a == L"--any-device") {
            o.anyDevice = true;
        } else {
            err = "option inconnue : " + toUtf8(a);
            return false;
        }
    }
    if (o.endpoint.empty()) {
        err = "--endpoint est obligatoire";
        return false;
    }
    if (o.channel.empty()) {
        err = "--channel est obligatoire (FL, FR, FC, LFE, SL, SR, BL, BR ou all)";
        return false;
    }
    // Plafond de sécurité : refus net, pas d'écrêtage silencieux de la valeur demandée.
    if (o.level > kMaxLevelDbfs) {
        err = std::format("niveau refusé : {} dBFS dépasse le plafond de sécurité de {} dBFS", o.level, kMaxLevelDbfs);
        return false;
    }
    if (o.level < kMinLevelDbfs) {
        err = std::format("niveau trop faible : {} dBFS (minimum {} dBFS)", o.level, kMinLevelDbfs);
        return false;
    }
    if (o.freq < 20.0 || o.freq > 20000.0) {
        err = "--freq doit être compris entre 20 et 20000 Hz";
        return false;
    }
    if (o.seconds < 0.1 || o.seconds > 30.0) {
        err = "--seconds doit être compris entre 0,1 et 30 s";
        return false;
    }
    if (o.format && !o.exclusive) {
        err = "--format n'a de sens qu'avec --mode exclusive (en partagé, le format de mixage est imposé)";
        return false;
    }
    return true;
}

} // namespace

void printToneHelp() {
    printOut(
        "endpoint-probe tone : joue un sinus sur UN canal d'un endpoint de rendu Xonar (WASAPI).\n\n"
        "Usage : endpoint-probe tone --endpoint <nom partiel ou id> --channel <canal|all>\n"
        "                            [--freq 440] [--level -20] [--seconds 2]\n"
        "                            [--mode shared|exclusive] [--format CxBxR[:MASQUE]]\n"
        "                            [--any-device] [--json]\n\n"
        "  --endpoint     Identifiant exact ou partie du nom (insensible à la casse) d'un endpoint\n"
        "                 de rendu ACTIF.\n"
        "  --channel      FL, FR, FC, LFE, SL, SR, BL, BR, ou all (chaque canal du format, l'un\n"
        "                 après l'autre, avec annonce du canal joué).\n"
        "  --freq         Fréquence en Hz (20 à 20000, défaut 440).\n"
        "  --level        Niveau crête en dBFS (défaut -20). Toute valeur > -20 est REFUSÉE.\n"
        "  --seconds      Durée par canal (0,1 à 30 s, défaut 2).\n"
        "  --mode         shared (défaut, format de mixage du moteur) ou exclusive.\n"
        "  --format       Exclusif seulement. C canaux, B = 16, 24, 24in32, 32 ou f32, R en Hz,\n"
        "                 masque facultatif. Ex. 6x24x48000 ou 6x24x48000:0x60F. Défaut : format\n"
        "                 du moteur (PKEY_AudioEngine_DeviceFormat).\n"
        "  --any-device   Autorise un endpoint qui n'est pas une Xonar (déconseillé).\n\n"
        "Sécurité : plafond -20 dBFS, rampes d'attaque et de relâchement de 20 ms, aucun\n"
        "changement du périphérique par défaut, du volume ni du format du moteur.\n\n"
        "Mesures : position IAudioClock en fin de flux contre position attendue, remplissages\n"
        "tardifs (réserve < 25 % du tampon au réveil), glitches (padding à zéro alors que le\n"
        "signal n'est pas fini), durée réelle.\n\n"
        "Codes de retour : 0 OK, 2 aucun endpoint Xonar correspondant (ou endpoint non Xonar\n"
        "refusé), 1 erreur.");
}

int cmdTone(const CommandArgs& a) {
    if (a.help) {
        printToneHelp();
        return kExitOk;
    }
    ToneOptions opt;
    std::string err;
    if (!parseToneArgs(a.args, opt, err)) {
        printErr("tone : " + err);
        return kExitError;
    }
    std::vector<std::uint32_t> channels;
    if (opt.channel == "all" || opt.channel == "ALL") {
        // Ordre de présentation : avant, centre, caisson, latéraux, arrière.
        channels = {SPEAKER_FRONT_LEFT, SPEAKER_FRONT_RIGHT, SPEAKER_FRONT_CENTER, SPEAKER_LOW_FREQUENCY,
                    SPEAKER_SIDE_LEFT,  SPEAKER_SIDE_RIGHT,  SPEAKER_BACK_LEFT,    SPEAKER_BACK_RIGHT};
    } else {
        std::uint32_t bit = 0;
        static const char* allowed[] = {"FL", "FR", "FC", "LFE", "SL", "SR", "BL", "BR"};
        std::string up = opt.channel;
        for (auto& c : up) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        if (std::find_if(std::begin(allowed), std::end(allowed), [&](const char* s) { return up == s; }) == std::end(allowed) ||
            !speakerFromName(up, bit)) {
            printErr("tone : canal inconnu « " + opt.channel + " » (FL, FR, FC, LFE, SL, SR, BL, BR ou all)");
            return kExitError;
        }
        channels = {bit};
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
    hr = enumerateEndpoints(en.Get(), eRender, DEVICE_STATE_ACTIVE, eps);
    if (FAILED(hr)) {
        printErr("EnumAudioEndpoints : " + hrText(hr));
        return kExitError;
    }

    // Sélection : identifiant exact prioritaire, sinon sous-chaîne du nom ou de l'id.
    std::vector<EndpointBasic*> matches;
    for (auto& e : eps) {
        if (e.id == opt.endpoint) {
            matches = {&e};
            break;
        }
        if (containsNoCase(e.friendlyName, opt.endpoint) || containsNoCase(e.deviceDesc, opt.endpoint) ||
            containsNoCase(e.interfaceName, opt.endpoint) || containsNoCase(e.id, opt.endpoint))
            matches.push_back(&e);
    }
    if (matches.empty()) {
        printErr("tone : aucun endpoint de rendu actif ne correspond à « " + toUtf8(opt.endpoint) + " ».");
        return kExitNoXonar;
    }
    if (matches.size() > 1) {
        std::string msg = "tone : plusieurs endpoints correspondent, préciser l'identifiant :";
        for (const auto* m : matches) msg += "\n  " + toUtf8(m->friendlyName) + "  " + toUtf8(m->id);
        printErr(msg);
        return kExitError;
    }
    EndpointBasic& target = *matches.front();
    if (!target.isXonar && !opt.anyDevice) {
        printErr("tone : refus, « " + toUtf8(target.friendlyName) +
                 " » n'est pas un endpoint Xonar. Rien n'a été joué (utiliser --any-device pour forcer).");
        return kExitNoXonar;
    }

    ComPtr<IPropertyStore> store;
    target.device->OpenPropertyStore(STGM_READ, &store);

    // Rappel informatif du volume de l'endpoint, qui n'est jamais modifié par l'outil.
    std::string volumeNote;
    ComPtr<IAudioEndpointVolume> vol;
    if (SUCCEEDED(target.device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                          reinterpret_cast<void**>(vol.GetAddressOf())))) {
        float db = 0;
        BOOL mute = FALSE;
        if (SUCCEEDED(vol->GetMasterVolumeLevel(&db))) volumeNote = std::format("{:.2f} dB", db);
        if (SUCCEEDED(vol->GetMute(&mute)) && mute) volumeNote += " (muet)";
    }
    const std::string header = std::format("Endpoint : {}\nIdentifiant : {}\nVolume maître (non modifié) : {}",
                                           toUtf8(target.friendlyName), toUtf8(target.id), volumeNote.empty() ? "n/d" : volumeNote);
    if (a.json) printErr(header);
    else printOut(header);

    // Pour "all", on ne joue que les canaux présents dans le format effectif ; on le
    // détermine en jouant : un canal absent produit une erreur "absent du format" qu'on
    // transforme en simple saut annoncé.
    Json runs = Json::array();
    bool allOk = true;
    unsigned played = 0;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        RunResult rr = playChannel(target.device.Get(), store.Get(), opt, channels[i], a.json);
        const bool skipped = channels.size() > 1 && rr.channelAbsent;
        if (skipped) {
            const std::string msg = std::format("Canal {} absent du format, ignoré.", speakerName(channels[i]));
            if (a.json) printErr(msg);
            else printOut(msg);
            continue;
        }
        ++played;
        if (!rr.ok) allOk = false;
        if (!a.json) {
            std::string text;
            rr.report.toText(text, 2);
            printOut(text.substr(0, text.empty() ? 0 : text.size() - 1));
        }
        runs.push(std::move(rr.report));
        if (i + 1 < channels.size()) Sleep(400); // courte pause audible entre deux canaux
    }
    if (played == 0) allOk = false;

    if (a.json) {
        Json root = Json::object();
        root.addJson("tool", "endpoint-probe");
        root.addJson("version", kToolVersion);
        root.addJson("command", "tone");
        root.addJson("timestamp", timestampNow());
        root.addJson("endpoint", Json::object().addJson("name", toUtf8(target.friendlyName)).addJson("id", toUtf8(target.id)));
        root.addJson("mode", opt.exclusive ? "exclusive" : "shared");
        root.addJson("freq_hz", Json::real(opt.freq, 2));
        root.addJson("level_dbfs", Json::real(opt.level, 2));
        root.addJson("seconds", Json::real(opt.seconds, 3));
        root.addJson("runs", std::move(runs));
        root.addJson("ok", allOk);
        printOut(root.toJson(2));
    }
    return allOk ? kExitOk : kExitError;
}

} // namespace probe
