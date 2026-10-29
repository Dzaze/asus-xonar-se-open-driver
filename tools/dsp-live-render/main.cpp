// dsp-live-render : prototype « notre application fait le rendu » (architecture B).
// Notre code rend l'audio TRAITÉ PAR xonar::dsp sur la Xonar via WASAPI, sans APO.
//
//   Sans --file : démo A/B (une source stéréo jouée telle quelle, puis upmixée en 5.1).
//   Avec --file <audio> : lit n'importe quel fichier (décodé par ffmpeg vers un WAV
//     temporaire), le passe dans dsp::Pipeline selon --mode, et le rend sur la Xonar.
//
// Modes : upmix (stéréo->5.1), two-one (2.1 : fronts + caisson dérivé), passthrough
// (5.1 tel quel), downmix (5.1->stéréo), stereo (fronts seulement, sans DSP).
// --gain <dB> : gain maître (le limiteur de dsp-core protège de la saturation).
//
// Transport WASAPI :
//   - par défaut : PARTAGÉ (format de mixage du moteur, float32 6 canaux) ;
//   - --exclusive : EXCLUSIF (bit-perfect / basse latence, qualité type ASIO). Le moteur
//     n'accepte pas le float32 en exclusif : on négocie le format du moteur
//     (PKEY_AudioEngine_DeviceFormat, typiquement 6 canaux / 24 bits / 48000 Hz) et on
//     CONVERTIT la sortie float de dsp-core vers le PCM négocié (16, 24, 24 dans 32 ou 32)
//     juste avant ReleaseBuffer.
// dsp-core sort toujours du float 6 canaux ; la conversion se fait en sortie.
// Ne change ni périphérique par défaut, ni volume, ni format du moteur.
#define NOMINMAX // garder std::min / std::max (pas les macros de windows.h)
#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include "xonar/common/wav.hpp"
#include "xonar/dsp/pipeline.hpp"
#include "xonar/dsp/profile.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace xonar::dsp;

namespace {

constexpr double kLevelDbfs = -20.0; // niveau des tonalités de démo (sécurité)
constexpr double kRampSeconds = 0.020;

// Clés et sous-formats définis à la main (pas d'INITGUID, comme kPkeyFriendlyName).
// PKEY_Device_FriendlyName : FMTID {a45c254e-...}, PID 14.
const PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
// PKEY_AudioEngine_DeviceFormat : {f19f064d-082c-4e27-bc73-6882a1bb8e4c}, PID 0.
const PROPERTYKEY kPkeyEngineDeviceFormat = {
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};
// PKEY_AudioEndpoint_PhysicalSpeakers : {1da5d803-d492-4edd-8c23-e0c0ffee7f0e}, PID 3.
const PROPERTYKEY kPkeyPhysicalSpeakers = {
    {0x1da5d803, 0xd492, 0x4edd, {0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e}}, 3};
// Sous-formats WAVEFORMATEXTENSIBLE (équivalents KSDATAFORMAT_SUBTYPE_*), sans ksmedia.h.
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string toUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// --- Format de sortie WASAPI et conversion depuis le float de dsp-core ------
// dsp-core produit toujours du float32 entrelacé (6 canaux ici). Selon le transport,
// le tampon WASAPI attend du float32 (partagé) ou du PCM (exclusif) ; on convertit juste
// avant ReleaseBuffer. La logique d'échantillons reprend fillFrames de cmd_tone.cpp.
struct OutputFormat {
    bool isFloat = true;      // float32 (partagé) ou PCM entier (exclusif)
    unsigned containerBits = 32; // 16, 24 (compact) ou 32 (PCM 32 / 24 dans 32)
    unsigned channels = 6;
};

// Convertit `frames` trames de `src` (float entrelacé, out.channels canaux) vers `dst`
// au format `out`, en appliquant un gain linéaire de lecture.
void writeConverted(BYTE* dst, const float* src, UINT32 frames, const OutputFormat& out, float gain) {
    const std::size_t n = static_cast<std::size_t>(frames) * out.channels;
    if (out.isFloat && out.containerBits == 32) {
        float* f = reinterpret_cast<float*>(dst);
        if (gain == 1.0f) std::memcpy(f, src, n * sizeof(float));
        else for (std::size_t i = 0; i < n; ++i) f[i] = src[i] * gain;
        return;
    }
    const unsigned bytes = out.containerBits / 8;
    for (std::size_t i = 0; i < n; ++i) {
        const double s = std::clamp(static_cast<double>(src[i]) * gain, -1.0, 1.0);
        BYTE* p = dst + i * bytes;
        if (out.containerBits == 16) {
            const auto v = static_cast<std::int16_t>(std::lround(s * 32767.0));
            std::memcpy(p, &v, sizeof(v));
        } else if (out.containerBits == 24) {
            const auto v = static_cast<std::int32_t>(std::lround(s * 8388607.0));
            p[0] = static_cast<BYTE>(v & 0xFF);
            p[1] = static_cast<BYTE>((v >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((v >> 16) & 0xFF);
        } else {
            // Entier 32 bits pleine échelle (vaut aussi pour 24 bits utiles cadrés à gauche).
            const auto v = static_cast<std::int32_t>(std::llround(s * 2147483647.0));
            std::memcpy(p, &v, sizeof(v));
        }
    }
}

// --- Générateur de sinus (démo) --------------------------------------------
struct Tone {
    double phase = 0.0, step = 0.0, amp = 0.0;
    std::uint64_t n = 0, total = 0, ramp = 0;
    void init(double freq, double levelDbfs, unsigned rate, double seconds) {
        step = 2.0 * std::numbers::pi * freq / rate;
        amp = std::pow(10.0, std::min(levelDbfs, kLevelDbfs) / 20.0);
        total = static_cast<std::uint64_t>(std::llround(seconds * rate));
        ramp = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::llround(kRampSeconds * rate)));
        phase = 0.0;
        n = 0;
    }
    float next() {
        if (n >= total) return 0.0f;
        double g = 1.0;
        if (n < ramp) g = 0.5 * (1.0 - std::cos(std::numbers::pi * double(n) / double(ramp)));
        const std::uint64_t rem = total - 1 - n;
        if (rem < ramp) g = std::min(g, 0.5 * (1.0 - std::cos(std::numbers::pi * double(rem) / double(ramp))));
        const double v = amp * g * std::sin(phase);
        phase += step;
        if (phase > 2.0 * std::numbers::pi) phase -= 2.0 * std::numbers::pi;
        ++n;
        return static_cast<float>(v);
    }
    bool done() const { return n >= total; }
};

// --- Sélection de l'endpoint -----------------------------------------------
HRESULT findEndpoint(IMMDeviceEnumerator* en, const std::wstring& want, ComPtr<IMMDevice>& out,
                     std::string& nameOut) {
    ComPtr<IMMDeviceCollection> col;
    HRESULT hr = en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col);
    if (FAILED(hr)) return hr;
    UINT count = 0;
    col->GetCount(&count);
    ComPtr<IMMDevice> firstXonar6;
    std::string firstXonar6Name;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(col->Item(i, &dev))) continue;
        ComPtr<IPropertyStore> store;
        if (FAILED(dev->OpenPropertyStore(STGM_READ, &store))) continue;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        std::wstring name;
        if (SUCCEEDED(store->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
        PropVariantClear(&pv);
        ComPtr<IAudioClient> client;
        unsigned ch = 0;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                    reinterpret_cast<void**>(client.GetAddressOf())))) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
                ch = mix->nChannels;
                CoTaskMemFree(mix);
            }
        }
        const bool isXonar = name.find(L"Xonar") != std::wstring::npos;
        if (!want.empty()) {
            std::wstring low = name, lw = want;
            std::transform(low.begin(), low.end(), low.begin(), ::towlower);
            std::transform(lw.begin(), lw.end(), lw.begin(), ::towlower);
            if (low.find(lw) != std::wstring::npos) {
                out = dev;
                nameOut = toUtf8(name.c_str());
                return S_OK;
            }
        } else if (isXonar && ch == 6 && !firstXonar6) {
            firstXonar6 = dev;
            firstXonar6Name = toUtf8(name.c_str());
        }
    }
    if (firstXonar6) {
        out = firstXonar6;
        nameOut = firstXonar6Name;
        return S_OK;
    }
    return E_NOTFOUND;
}

// --- Négociation du format exclusif ----------------------------------------
// Construit un WAVEFORMATEXTENSIBLE depuis les champs d'un format moteur.
WAVEFORMATEXTENSIBLE buildFormat(unsigned channels, unsigned rate, unsigned validBits,
                                 unsigned containerBits, std::uint32_t mask, bool isFloat) {
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
    w.SubFormat = isFloat ? kSubtypeFloat : kSubtypePcm;
    return w;
}

// S'inspire fortement de endpoint-probe/cmd_tone.cpp : part du format du moteur
// (PKEY_AudioEngine_DeviceFormat), tente « 24 bits compacts », replie sur « 24 dans 32 »
// (ou l'inverse) si refusé, puis valide via IsFormatSupported en EXCLUSIF. Remplit `w`
// (format à passer à Initialize) et `out` (conversion de sortie). `note` décrit un repli.
HRESULT negotiateExclusiveFormat(IMMDevice* dev, IAudioClient* client, WAVEFORMATEXTENSIBLE& w,
                                 OutputFormat& out, std::string& err, std::string& note) {
    ComPtr<IPropertyStore> store;
    HRESULT hr = dev->OpenPropertyStore(STGM_READ, &store);
    if (FAILED(hr)) {
        err = "OpenPropertyStore a echoue";
        return hr;
    }
    unsigned ch = 0, rate = 0, container = 0, valid = 0;
    std::uint32_t mask = 0;
    bool hasMask = false, isFloat = false;
    {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        hr = store->GetValue(kPkeyEngineDeviceFormat, &pv);
        if (FAILED(hr) || pv.vt != VT_BLOB || !pv.blob.pBlobData || pv.blob.cbSize < sizeof(WAVEFORMATEX)) {
            PropVariantClear(&pv);
            err = "format du moteur illisible (PKEY_AudioEngine_DeviceFormat)";
            return FAILED(hr) ? hr : E_FAIL;
        }
        const auto* ef = reinterpret_cast<const WAVEFORMATEX*>(pv.blob.pBlobData);
        ch = ef->nChannels;
        rate = ef->nSamplesPerSec;
        container = ef->wBitsPerSample;
        valid = ef->wBitsPerSample;
        if (ef->wFormatTag == WAVE_FORMAT_EXTENSIBLE && pv.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(ef);
            valid = ext->Samples.wValidBitsPerSample ? ext->Samples.wValidBitsPerSample : container;
            mask = ext->dwChannelMask;
            hasMask = true;
            isFloat = IsEqualGUID(ext->SubFormat, kSubtypeFloat) != 0;
        } else if (ef->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
            isFloat = true;
        }
        PropVariantClear(&pv);
    }

    if (ch != 6) {
        err = "format du moteur a " + std::to_string(ch) + " canaux (6 attendus pour l'endpoint 5.1)";
        return E_FAIL;
    }
    if (!hasMask || mask == 0) {
        // Repli : haut-parleurs physiques déclarés, sinon masque 5.1 standard (0x3F).
        std::uint32_t phys = 0;
        bool hasPhys = false;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(store->GetValue(kPkeyPhysicalSpeakers, &pv)) && pv.vt == VT_UI4) {
            phys = pv.ulVal;
            hasPhys = true;
        }
        PropVariantClear(&pv);
        mask = (hasPhys && std::popcount(phys) == 6) ? phys : 0x3Fu;
    }

    // Tentative 1 : le format du moteur tel quel (déjà accepté par le pilote).
    w = buildFormat(ch, rate, valid, container, mask, isFloat);
    hr = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&w), nullptr);
    if (hr != S_OK && valid == 24 && !isFloat) {
        // Repli conteneur 24 <-> 32 : beaucoup de pilotes USB n'exposent que l'un des deux.
        const unsigned alt = (container == 24) ? 32 : 24;
        WAVEFORMATEXTENSIBLE w2 = buildFormat(ch, rate, 24, alt, mask, false);
        const HRESULT hr2 = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                       reinterpret_cast<WAVEFORMATEX*>(&w2), nullptr);
        if (hr2 == S_OK) {
            note = (container == 24) ? "24 bits compacts refuses, repli sur 24 bits dans 32"
                                     : "24 bits dans 32 refuses, repli sur 24 bits compacts";
            w = w2;
            hr = hr2;
        }
    }
    if (hr != S_OK) {
        err = "format exclusif refuse (IsFormatSupported)";
        return hr;
    }

    out.channels = 6;
    out.isFloat = isFloat;
    out.containerBits = w.Format.wBitsPerSample;
    const bool writable = (out.isFloat && out.containerBits == 32) ||
                          (!out.isFloat && (out.containerBits == 16 || out.containerBits == 24 || out.containerBits == 32));
    if (!writable) {
        err = "type d'echantillon negocie non gere par l'outil";
        return E_FAIL;
    }
    return S_OK;
}

// --- Boucle de rendu WASAPI (partagé ou exclusif) --------------------------
// `fill` écrit toujours du float32 6 canaux dans un tampon de travail ; writeConverted le
// transpose au format `out` (copie float en partagé, conversion PCM en exclusif).
template <class Fill, class Until>
HRESULT renderLoop(IAudioClient* client, IAudioRenderClient* render, unsigned rate, const OutputFormat& out,
                   float levelLin, UINT32 bufferFrames, Fill&& fill, Until&& until) {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const double bufMs = 1000.0 * bufferFrames / rate;
    const double wakeMs = std::min(5.0, bufMs / 4.0);
    std::vector<float> scratch(static_cast<std::size_t>(bufferFrames) * out.channels, 0.0f);

    const auto emit = [&](UINT32 frames) -> bool {
        BYTE* data = nullptr;
        if (FAILED(render->GetBuffer(frames, &data))) return false;
        const UINT32 wrote = fill(frames, scratch.data());
        if (wrote == 0) return SUCCEEDED(render->ReleaseBuffer(0, AUDCLNT_BUFFERFLAGS_SILENT));
        writeConverted(data, scratch.data(), wrote, out, levelLin);
        return SUCCEEDED(render->ReleaseBuffer(wrote, 0));
    };

    emit(bufferFrames); // préremplissage
    HRESULT hr = client->Start();
    if (FAILED(hr)) {
        if (timer) CloseHandle(timer);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        return hr;
    }
    for (;;) {
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(wakeMs * 10000.0);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        } else {
            Sleep(static_cast<DWORD>(wakeMs));
        }
        UINT32 padding = 0;
        if (FAILED(client->GetCurrentPadding(&padding))) break;
        if (until()) {
            if (padding == 0) break;
            continue; // source terminée : ne plus remplir, laisser le tampon se vider
        }
        const UINT32 room = bufferFrames - padding;
        if (room > 0) {
            if (!emit(room)) break;
        }
    }
    client->Stop();
    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    return S_OK;
}

// --- Décodage de n'importe quel fichier audio vers un WAV float temporaire --
// Retourne le chemin du WAV temporaire, ou "" en cas d'échec.
std::string decodeToWav(const std::wstring& input, int channels, unsigned rate) {
    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp) == 0) return {};
    std::wstring wtemp = std::wstring(tmp) + L"xonar-dsp-render-in.wav";
    std::wstring cmd = L"ffmpeg -nostdin -y -v error -i \"" + input + L"\" -ar " +
                       std::to_wstring(rate) + L" -ac " + std::to_wstring(channels) +
                       L" -c:a pcm_f32le \"" + wtemp + L"\"";
    const int rc = _wsystem(cmd.c_str());
    if (rc != 0) {
        std::printf("ffmpeg a echoue (code %d). Verifier le chemin du fichier.\n", rc);
        return {};
    }
    return toUtf8(wtemp.c_str());
}

enum class PlayMode { Upmix, TwoOne, Passthrough, Downmix, Stereo };

bool parseMode(const std::wstring& s, PlayMode& m) {
    if (s == L"upmix") m = PlayMode::Upmix;
    else if (s == L"two-one" || s == L"2.1" || s == L"twoone") m = PlayMode::TwoOne;
    else if (s == L"passthrough" || s == L"5.1" || s == L"preserve") m = PlayMode::Passthrough;
    else if (s == L"downmix" || s == L"2.0") m = PlayMode::Downmix;
    else if (s == L"stereo") m = PlayMode::Stereo;
    else return false;
    return true;
}

// Nombre de canaux à demander à ffmpeg pour alimenter le mode.
int decodeChannelsFor(PlayMode m) {
    switch (m) {
    case PlayMode::Upmix:
    case PlayMode::TwoOne:
    case PlayMode::Stereo:
        return 2;
    case PlayMode::Passthrough:
    case PlayMode::Downmix:
        return 6;
    }
    return 2;
}

// --- Lecture d'un fichier à travers dsp-core -------------------------------
int playFile(IAudioClient* client, IAudioRenderClient* render, const OutputFormat& out, float levelLin,
             UINT32 bufferFrames, unsigned rate, PlayMode mode, const std::wstring& file, double gainDb) {
    const int decCh = decodeChannelsFor(mode);
    const std::string wavPath = decodeToWav(file, decCh, rate);
    if (wavPath.empty()) return 1;

    xonar::common::WavData wav;
    const auto res = xonar::common::readWav(wavPath, wav);
    if (!res.ok) {
        std::printf("Lecture du WAV decode impossible : %s\n", res.error.c_str());
        return 1;
    }
    const unsigned inCh = wav.info.channels;
    const auto totalFrames = static_cast<std::uint64_t>(wav.info.frames);
    std::printf("Fichier decode : %u canaux, %u Hz, %.1f s\n", inCh, wav.info.sampleRate,
                double(totalFrames) / (wav.info.sampleRate ? wav.info.sampleRate : 1));

    // Profil dsp-core selon le mode. La sortie WASAPI fait toujours 6 canaux (0x60F).
    Profile profile;
    unsigned dspOut = 6;
    switch (mode) {
    case PlayMode::Upmix: profile = makeUpmixProfile(rate, masks::k5Point1Side); break;
    case PlayMode::TwoOne: profile = makeTwoPointOneProfile(rate, masks::kStereo, masks::k5Point1Side); break;
    case PlayMode::Passthrough: profile = makePassThroughProfile(rate, masks::k5Point1Side); break;
    case PlayMode::Downmix: profile = makeDownmixProfile(rate, masks::k5Point1Side); dspOut = 2; break;
    case PlayMode::Stereo: profile = makePassThroughProfile(rate, masks::kStereo); dspOut = 0; break; // 0 = pas de DSP
    }
    profile.masterGainDb = static_cast<float>(gainDb);

    Pipeline pipe;
    const bool useDsp = (mode != PlayMode::Stereo);
    if (useDsp && !pipe.prepare(profile, bufferFrames)) {
        std::printf("Pipeline::prepare a echoue pour ce mode / ce fichier.\n");
        return 1;
    }

    std::vector<float> scratchIn(static_cast<size_t>(bufferFrames) * (inCh ? inCh : 1), 0.0f);
    std::vector<float> scratch2(static_cast<size_t>(bufferFrames) * 2, 0.0f);
    std::uint64_t pos = 0;

    auto fill = [&](UINT32 frames, float* out_) -> UINT32 {
        // Prépare le bloc d'entrée (zéro-padding en fin de fichier).
        for (UINT32 i = 0; i < frames; ++i) {
            const std::uint64_t f = pos + i;
            for (unsigned c = 0; c < inCh; ++c) {
                scratchIn[static_cast<size_t>(i) * inCh + c] =
                    (f < totalFrames) ? wav.samples[(f * inCh) + c] : 0.0f;
            }
        }
        if (!useDsp) {
            // Stéréo direct : L/R -> FL/FR, reste silencieux.
            for (UINT32 i = 0; i < frames; ++i) {
                float* o = out_ + static_cast<size_t>(i) * 6;
                o[0] = inCh > 0 ? scratchIn[static_cast<size_t>(i) * inCh + 0] : 0.f;
                o[1] = inCh > 1 ? scratchIn[static_cast<size_t>(i) * inCh + 1] : 0.f;
                o[2] = o[3] = o[4] = o[5] = 0.f;
            }
        } else if (dspOut == 6) {
            ConstInterleavedView in{scratchIn.data(), inCh, frames};
            InterleavedView o{out_, 6, frames};
            if (!pipe.process(in, o)) std::memset(out_, 0, static_cast<size_t>(frames) * 6 * sizeof(float));
        } else { // downmix : dsp sort 2 canaux, on les place sur FL/FR
            ConstInterleavedView in{scratchIn.data(), inCh, frames};
            InterleavedView o{scratch2.data(), 2, frames};
            if (pipe.process(in, o)) {
                for (UINT32 i = 0; i < frames; ++i) {
                    float* d = out_ + static_cast<size_t>(i) * 6;
                    d[0] = scratch2[static_cast<size_t>(i) * 2 + 0];
                    d[1] = scratch2[static_cast<size_t>(i) * 2 + 1];
                    d[2] = d[3] = d[4] = d[5] = 0.f;
                }
            } else {
                std::memset(out_, 0, static_cast<size_t>(frames) * 6 * sizeof(float));
            }
        }
        pos += frames;
        return frames;
    };

    std::puts("Lecture... (Ctrl+C pour arreter)");
    renderLoop(client, render, rate, out, levelLin, bufferFrames, fill, [&] { return pos >= totalFrames; });
    std::puts("Termine.");
    return 0;
}

// --- Démo A/B (sans fichier) -----------------------------------------------
int runDemo(IAudioClient* client, IAudioRenderClient* render, const OutputFormat& out, float levelLin,
            UINT32 bufferFrames, unsigned rate, double seconds) {
    Tone toneL, toneR;
    std::puts("\n>>> Phase 1 : source STEREO telle quelle (seulement avant-G / avant-D)");
    toneL.init(330.0, kLevelDbfs, rate, seconds);
    toneR.init(392.0, kLevelDbfs, rate, seconds);
    auto fillStereo = [&](UINT32 frames, float* o) -> UINT32 {
        for (UINT32 i = 0; i < frames; ++i) {
            const float l = toneL.next(), r = toneR.next();
            float* f = o + static_cast<size_t>(i) * 6;
            f[0] = l; f[1] = r; f[2] = 0; f[3] = 0; f[4] = 0; f[5] = 0;
        }
        return frames;
    };
    renderLoop(client, render, rate, out, levelLin, bufferFrames, fillStereo, [&] { return toneL.done(); });
    Sleep(500);
    client->Reset();

    std::puts(">>> Phase 2 : MEME source UPMIXEE en 5.1 par dsp-core (centre + surrounds + caisson)");
    Profile profile = makeUpmixProfile(rate, masks::k5Point1Side);
    Pipeline pipe;
    if (!pipe.prepare(profile, bufferFrames)) {
        std::puts("Pipeline::prepare a echoue.");
        return 1;
    }
    toneL.init(330.0, kLevelDbfs, rate, seconds);
    toneR.init(392.0, kLevelDbfs, rate, seconds);
    std::vector<float> stereoIn(static_cast<size_t>(bufferFrames) * 2, 0.0f);
    auto fillUpmix = [&](UINT32 frames, float* o) -> UINT32 {
        for (UINT32 i = 0; i < frames; ++i) {
            stereoIn[static_cast<size_t>(i) * 2 + 0] = toneL.next();
            stereoIn[static_cast<size_t>(i) * 2 + 1] = toneR.next();
        }
        ConstInterleavedView in{stereoIn.data(), 2, frames};
        InterleavedView ov{o, 6, frames};
        if (!pipe.process(in, ov)) std::memset(o, 0, static_cast<size_t>(frames) * 6 * sizeof(float));
        return frames;
    };
    renderLoop(client, render, rate, out, levelLin, bufferFrames, fillUpmix, [&] { return toneL.done(); });
    std::puts("\nTermine. Phase 2 plus large que phase 1 => dsp-core a bien traite le rendu.");
    return 0;
}

// Initialise le client en EXCLUSIF (format négocié dans `exFmt`), avec gestion du
// réalignement de tampon (AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED), comme cmd_tone.cpp.
HRESULT initExclusive(IMMDevice* dev, ComPtr<IAudioClient>& client, WAVEFORMATEXTENSIBLE& exFmt, unsigned rate) {
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defPeriod, &minPeriod);
    REFERENCE_TIME dur = defPeriod; // en exclusif, tampon = périodicité (basse latence, période par défaut du pilote)
    HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur, dur,
                                    reinterpret_cast<WAVEFORMATEX*>(&exFmt), nullptr);
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        // Procédure documentée : relire la taille alignée, recréer le client, réessayer.
        UINT32 aligned = 0;
        client->GetBufferSize(&aligned);
        dur = static_cast<REFERENCE_TIME>(10'000'000.0 * aligned / rate + 0.5);
        client.Reset();
        hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                           reinterpret_cast<void**>(client.GetAddressOf()));
        if (SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, 0, dur, dur,
                                    reinterpret_cast<WAVEFORMATEX*>(&exFmt), nullptr);
        std::puts("Tampon exclusif realigne.");
    }
    return hr;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::wstring want, file, modeStr = L"upmix";
    double seconds = 4.0, gainDb = 0.0, levelDb = 0.0;
    bool exclusive = false, levelSet = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        if (a == L"--endpoint" && i + 1 < argc) want = argv[++i];
        else if (a == L"--file" && i + 1 < argc) file = argv[++i];
        else if (a == L"--mode" && i + 1 < argc) modeStr = argv[++i];
        else if (a == L"--gain" && i + 1 < argc) gainDb = _wtof(argv[++i]);
        else if (a == L"--level" && i + 1 < argc) { levelDb = _wtof(argv[++i]); levelSet = true; }
        else if (a == L"--seconds" && i + 1 < argc) seconds = _wtof(argv[++i]);
        else if (a == L"--exclusive") exclusive = true;
        else if (a == L"--help" || a == L"-h") {
            std::puts("dsp-live-render : rend l'audio traite par dsp-core sur la Xonar (WASAPI, sans APO).\n"
                      "  (sans --file)        demo A/B : stereo puis upmix 5.1\n"
                      "  --file <audio>       joue un fichier (tout format via ffmpeg) a travers dsp-core\n"
                      "  --mode <m>           upmix | two-one | passthrough | downmix | stereo (defaut upmix)\n"
                      "  --gain <dB>          gain maitre dsp-core (limiteur actif)\n"
                      "  --level <dBFS>       gain de lecture en sortie (defaut 0 ; avert. si > -6 dBFS)\n"
                      "  --exclusive          WASAPI EXCLUSIF bit-perfect / basse latence (defaut : partage)\n"
                      "  --endpoint <nom>     sous-chaine du nom de l'endpoint (defaut : Xonar 6 canaux)\n"
                      "  --seconds <s>        duree par phase de la demo (defaut 4)");
            return 0;
        }
    }
    PlayMode mode = PlayMode::Upmix;
    if (!file.empty() && !parseMode(modeStr, mode)) {
        std::printf("Mode inconnu : %s (upmix|two-one|passthrough|downmix|stereo)\n", toUtf8(modeStr.c_str()).c_str());
        return 2;
    }

    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::puts("CoInitializeEx a echoue");
        return 1;
    }
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        std::puts("MMDeviceEnumerator indisponible");
        return 1;
    }
    ComPtr<IMMDevice> dev;
    std::string name;
    if (FAILED(findEndpoint(en.Get(), want, dev, name))) {
        std::puts("Aucun endpoint Xonar 6 canaux trouve (preciser --endpoint <nom>).");
        return 2;
    }
    ComPtr<IAudioClient> client;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                             reinterpret_cast<void**>(client.GetAddressOf())))) {
        std::puts("Activate(IAudioClient) a echoue");
        return 1;
    }

    const float levelLin = static_cast<float>(std::pow(10.0, levelDb / 20.0));
    if (levelSet && levelDb > -6.0)
        std::printf("Avertissement : niveau de lecture %+.1f dBFS (> -6 dBFS) : lecture potentiellement forte.\n", levelDb);

    OutputFormat out;
    WAVEFORMATEX* mix = nullptr;      // partagé : format de mixage (à libérer)
    WAVEFORMATEXTENSIBLE exFmt{};      // exclusif : format négocié (pile, durée de vie = wmain)
    UINT32 bufferFrames = 0;
    unsigned rate = 0;

    if (!exclusive) {
        if (FAILED(client->GetMixFormat(&mix)) || !mix) {
            std::puts("GetMixFormat a echoue");
            return 1;
        }
        rate = mix->nSamplesPerSec;
        const unsigned ch = mix->nChannels;
        const bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                             (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                              reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mix)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
        std::printf("Endpoint : %s\nMode : WASAPI partage\nFormat de mixage : %u canaux, %u Hz, %s\n", name.c_str(), ch,
                    rate, isFloat ? "float32" : "NON flottant (partage limite au float)");
        if (ch != 6 || !isFloat) {
            std::puts("Ce prototype attend un mixage 6 canaux float32 (endpoint 5.1). Abandon.");
            CoTaskMemFree(mix);
            return 2;
        }
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST, 1'000'000, 0, mix, nullptr);
        if (FAILED(hr)) {
            std::printf("Initialize a echoue : 0x%08lX\n", static_cast<unsigned long>(hr));
            CoTaskMemFree(mix);
            return 1;
        }
        out.isFloat = true;
        out.containerBits = 32;
        out.channels = 6;
    } else {
        std::string err, note;
        HRESULT hr = negotiateExclusiveFormat(dev.Get(), client.Get(), exFmt, out, err, note);
        if (hr != S_OK) {
            std::printf("Exclusif : %s (0x%08lX)\n", err.c_str(), static_cast<unsigned long>(hr));
            std::puts("Verifier que l'acces exclusif est autorise (Son > Proprietes > Avance) et "
                      "qu'aucune autre application n'occupe l'endpoint.");
            return 2;
        }
        rate = exFmt.Format.nSamplesPerSec;
        const unsigned validBits = out.isFloat ? 32 : exFmt.Samples.wValidBitsPerSample;
        const char* contNote = (!out.isFloat && out.containerBits == 32 && exFmt.Samples.wValidBitsPerSample == 24)
                                   ? " dans 32"
                                   : "";
        std::printf("Endpoint : %s\nMode : WASAPI exclusif (bit-perfect / basse latence)\n"
                    "Format negocie : %u canaux, %u Hz, %s %u bits%s\n",
                    name.c_str(), out.channels, rate, out.isFloat ? "flottant" : "PCM", validBits, contNote);
        if (!note.empty()) std::printf("Note : %s\n", note.c_str());

        hr = initExclusive(dev.Get(), client, exFmt, rate);
        if (FAILED(hr)) {
            std::printf("Initialize exclusif a echoue : 0x%08lX\n", static_cast<unsigned long>(hr));
            std::puts("Verifier que l'acces exclusif est autorise (Son > Proprietes > Avance) et "
                      "qu'aucune autre application n'occupe l'endpoint.");
            return 1;
        }
    }

    client->GetBufferSize(&bufferFrames);
    ComPtr<IAudioRenderClient> render;
    if (FAILED(client->GetService(IID_PPV_ARGS(&render)))) {
        std::puts("GetService(IAudioRenderClient) a echoue");
        CoTaskMemFree(mix);
        return 1;
    }
    std::printf("Tampon : %u trames (%.1f ms)\n", bufferFrames, 1000.0 * bufferFrames / rate);

    int code;
    if (file.empty()) {
        code = runDemo(client.Get(), render.Get(), out, levelLin, bufferFrames, rate, seconds);
    } else {
        if (std::fabs(gainDb) > 0.01) std::printf("Gain maitre : %+.1f dB\n", gainDb);
        code = playFile(client.Get(), render.Get(), out, levelLin, bufferFrames, rate, mode, file, gainDb);
    }

    CoTaskMemFree(mix);
    CoUninitialize();
    return code;
}
