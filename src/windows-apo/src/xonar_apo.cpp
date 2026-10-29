// SPDX-License-Identifier: Apache-2.0
#include "xonar_apo.hpp"

#include "media_type_min.hpp"

#include "xonar/dsp/channels.hpp"

#include <shlobj.h>

#include <cstring>
#include <cmath>
#include <sstream>
#include <string>

namespace xonar::apo {

// Verrou de module (défini dans dllmain.cpp) : empêche le déchargement de la DLL
// tant qu'une instance vit.
void XonarApoModuleLock() noexcept;
void XonarApoModuleUnlock() noexcept;

namespace {
std::atomic<int> g_nextInstanceId{1};

constexpr std::uint16_t kPhysChannels = 6;
constexpr std::uint32_t kPhysMask = 0x60Fu; // FL FR FC LFE SL SR

bool isXonarRate(std::uint32_t r) noexcept {
    return r == 44100u || r == 48000u || r == 96000u || r == 192000u;
}

const WAVEFORMATEXTENSIBLE* asExt(const WAVEFORMATEX* f) noexcept {
    if (!f || f->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        f->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return nullptr;
    }
    return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(f);
}

// Chemin du .cfg lu par cet étage (miroir de apo_settings::configDir), pour la veille.
std::wstring cfgPath(Stage s) {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &p))) {
        dir = p;
        CoTaskMemFree(p);
    }
    if (dir.empty()) return {};
    dir += L"\\XonarSE\\";
    (void)s;
    dir += L"apo-efx.cfg";
    return dir;
}

bool fileWriteTime(const std::wstring& path, FILETIME& out) noexcept {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return false;
    out = d.ftLastWriteTime;
    return true;
}
} // namespace

XonarApo::XonarApo(Stage stage, IUnknown* outer)
    : outer_(outer), stage_(stage), instanceId_(g_nextInstanceId.fetch_add(1)) {
    XonarApoModuleLock();
    apoLog(stage_, instanceId_, "Construct");
}

XonarApo::~XonarApo() {
    stopSettingsWatcher();
    apoLog(stage_, instanceId_, "Destruct");
}

const CLSID& XonarApo::clsid() const noexcept {
    return stage_ == Stage::Efx ? CLSID_XonarEfxApo : CLSID_XonarSfxApo;
}

// --- IUnknown --------------------------------------------------------------
HRESULT STDMETHODCALLTYPE XonarApo::QueryInterface(REFIID riid, void** ppv) {
    return outer_ ? outer_->QueryInterface(riid, ppv) : innerQueryInterface(riid, ppv);
}

HRESULT XonarApo::innerQueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown)) {
        *ppv = &inner_;
        innerAddRef();
        return S_OK;
    } else if (riid == __uuidof(IAudioProcessingObject)) {
        *ppv = static_cast<IAudioProcessingObject*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectRT)) {
        *ppv = static_cast<IAudioProcessingObjectRT*>(this);
    } else if (riid == __uuidof(IAudioProcessingObjectConfiguration)) {
        *ppv = static_cast<IAudioProcessingObjectConfiguration*>(this);
    } else if (riid == __uuidof(IAudioSystemEffects)) {
        *ppv = static_cast<IAudioSystemEffects*>(static_cast<IAudioSystemEffects2*>(this));
    } else if (riid == __uuidof(IAudioSystemEffects2)) {
        *ppv = static_cast<IAudioSystemEffects2*>(this);
    } else {
        *ppv = nullptr;
        wchar_t iid[40] = {};
        StringFromGUID2(riid, iid, 40);
        std::string text;
        for (const wchar_t c : iid) { if (!c) break; text.push_back(static_cast<char>(c)); }
        apoLog(stage_, instanceId_, "QueryInterface E_NOINTERFACE " + text);
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

ULONG STDMETHODCALLTYPE XonarApo::AddRef() { return outer_ ? outer_->AddRef() : innerAddRef(); }

ULONG STDMETHODCALLTYPE XonarApo::Release() {
    return outer_ ? outer_->Release() : innerRelease();
}

HRESULT STDMETHODCALLTYPE XonarApo::InnerUnknown::QueryInterface(REFIID iid, void** out) {
    return owner_.innerQueryInterface(iid, out);
}
ULONG STDMETHODCALLTYPE XonarApo::InnerUnknown::AddRef() { return owner_.innerAddRef(); }
ULONG STDMETHODCALLTYPE XonarApo::InnerUnknown::Release() { return owner_.innerRelease(); }
ULONG XonarApo::innerAddRef() noexcept { return ++ref_; }
ULONG XonarApo::innerRelease() noexcept {
    const ULONG r = --ref_;
    if (r == 0) {
        delete this;
        XonarApoModuleUnlock(); // le veilleur doit être joint avant de permettre l'unload
    }
    return r;
}

// --- IAudioProcessingObject ------------------------------------------------
HRESULT STDMETHODCALLTYPE XonarApo::Reset() {
    pipeline_.resetStats();
    processCalls_ = processedFrames_ = silentFrames_ = 0;
    inputEnergy_ = outputEnergy_ = 0;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE XonarApo::GetLatency(HNSTIME* pTime) {
    if (!pTime) return E_POINTER;
    *pTime = 0; // le retard éventuel (délais par canal) est interne au bloc
    return S_OK;
}

HRESULT STDMETHODCALLTYPE XonarApo::GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) {
    if (!ppRegProps) return E_POINTER;
    *ppRegProps = nullptr;
    auto* p = static_cast<APO_REG_PROPERTIES*>(CoTaskMemAlloc(sizeof(APO_REG_PROPERTIES)));
    if (!p) return E_OUTOFMEMORY;
    ZeroMemory(p, sizeof(*p));
    p->clsid = clsid();
    // EFX : 6->6, le nombre de canaux doit correspondre. SFX : 2->6, il ne doit PAS.
    DWORD flags = APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH;
    if (stage_ == Stage::Efx) {
        flags |= APO_FLAG_SAMPLESPERFRAME_MUST_MATCH;
    }
    p->Flags = static_cast<APO_FLAG>(flags);
    wcscpy_s(p->szFriendlyName,
             stage_ == Stage::Efx ? L"Xonar SE EFX (compensation de sortie 6->6)"
                                  : L"Xonar SE SFX (routage par flux 2->6 / 6->6)");
    wcscpy_s(p->szCopyrightInfo, L"Xonar SE Open Driver");
    p->u32MajorVersion = 1;
    p->u32MinorVersion = 0;
    p->u32MinInputConnections = 1;
    p->u32MaxInputConnections = 1;
    p->u32MinOutputConnections = 1;
    p->u32MaxOutputConnections = 1;
    p->u32MaxInstances = 0xFFFFFFFF;
    // sizeof(APO_REG_PROPERTIES) réserve UN IID : le compte et la liste doivent
    // correspondre, comme NumAPOInterfaces/APOInterface0 dans le registre.
    p->u32NumAPOInterfaces = 1;
    p->iidAPOInterfaceList[0] = __uuidof(IAudioProcessingObject);
    apoLog(stage_, instanceId_, "GetRegistrationProperties interfaces=1 IID=IAudioProcessingObject");
    *ppRegProps = p;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE XonarApo::Initialize(UINT32 cbDataSize, BYTE* pbyData) {
    std::ostringstream os;
    os << "Initialize cbDataSize=" << cbDataSize;
    if (pbyData && cbDataSize >= sizeof(APOInitBaseStruct)) {
        if (cbDataSize >= sizeof(APOInitSystemEffects2)) {
            auto* se2 = reinterpret_cast<APOInitSystemEffects2*>(pbyData);
            os << " discoveryOnly=" << (se2->InitializeForDiscoveryOnly ? "true" : "false")
               << " endpointProps=" << (se2->pAPOEndpointProperties ? "present" : "null")
               << " sysFxProps=" << (se2->pAPOSystemEffectsProperties ? "present" : "null");
        }
    }
    settings_ = loadApoSettings(stage_);
    os << " | settings: enabled=" << (settings_.enabled ? "1" : "0")
       << " masterGainDb=" << settings_.masterGainDb;
    apoLog(stage_, instanceId_, os.str());
    initialized_ = true;
    return S_OK;
}

HRESULT STDMETHODCALLTYPE XonarApo::GetInputChannelCount(UINT32* pu32ChannelCount) {
    if (!pu32ChannelCount) return E_POINTER;
    *pu32ChannelCount = inputChannels_ ? inputChannels_ : (stage_ == Stage::Efx ? kPhysChannels : 2u);
    return S_OK;
}

bool XonarApo::isSupportedInputFormat(const WAVEFORMATEXTENSIBLE& w) const noexcept {
    if (w.SubFormat != kSubtypeIeeeFloat || w.Format.wBitsPerSample != 32) return false;
    if (!isXonarRate(w.Format.nSamplesPerSec)) return false;
    const std::uint32_t m = w.dwChannelMask;
    if (stage_ == Stage::Efx) {
        return w.Format.nChannels == kPhysChannels && m == kPhysMask;
    }
    // SFX : stéréo, 5.1 arrière ou 5.1 latéral.
    return (w.Format.nChannels == 1 && (m == 4 || m == 1)) ||
           (w.Format.nChannels == 2 && m == xonar::dsp::masks::kStereo) ||
           (w.Format.nChannels == 6 && (m == xonar::dsp::masks::k5Point1Back || m == xonar::dsp::masks::k5Point1Side));
}

bool XonarApo::isSupportedOutputFormat(const WAVEFORMATEXTENSIBLE& w) const noexcept {
    if (w.SubFormat != kSubtypeIeeeFloat || w.Format.wBitsPerSample != 32) return false;
    if (!isXonarRate(w.Format.nSamplesPerSec)) return false;
    return w.Format.nChannels == kPhysChannels && w.dwChannelMask == kPhysMask; // toujours 6 / 0x60F
}

HRESULT STDMETHODCALLTYPE XonarApo::IsInputFormatSupported(IAudioMediaType* /*pOpposite*/,
                                                           IAudioMediaType* pReq,
                                                           IAudioMediaType** ppSupported) {
    if (ppSupported) *ppSupported = nullptr;
    if (!pReq) return E_POINTER;
    const WAVEFORMATEXTENSIBLE* w = asExt(pReq->GetAudioFormat());
    if (w) {
        std::ostringstream os;
        os << "IsInputFormatSupported ch=" << w->Format.nChannels << " rate=" << w->Format.nSamplesPerSec
           << " bits=" << w->Format.wBitsPerSample << " mask=0x" << std::hex << w->dwChannelMask
           << " supported=" << isSupportedInputFormat(*w);
        apoLog(stage_, instanceId_, os.str());
    }
    if (!w) return APOERR_FORMAT_NOT_SUPPORTED;
    if (isSupportedInputFormat(*w)) {
        if (ppSupported) { *ppSupported = pReq; pReq->AddRef(); }
        return S_OK;
    }
    // Proposer un format valide à la même fréquence (float32), sinon refuser.
    if (ppSupported && w->SubFormat == kSubtypeIeeeFloat && isXonarRate(w->Format.nSamplesPerSec)) {
        const WAVEFORMATEXTENSIBLE proposed =
            stage_ == Stage::Efx
                ? makeFloatFormat(w->Format.nSamplesPerSec, kPhysChannels, kPhysMask)
                : w->Format.nChannels >= 6 ? makeFloatFormat(w->Format.nSamplesPerSec, kPhysChannels, kPhysMask)
                                          : makeFloatFormat(w->Format.nSamplesPerSec, 2, xonar::dsp::masks::kStereo);
        *ppSupported = new (std::nothrow) MediaType(proposed);
        return *ppSupported ? S_FALSE : E_OUTOFMEMORY;
    }
    return APOERR_FORMAT_NOT_SUPPORTED;
}

HRESULT STDMETHODCALLTYPE XonarApo::IsOutputFormatSupported(IAudioMediaType* /*pOpposite*/,
                                                            IAudioMediaType* pReq,
                                                            IAudioMediaType** ppSupported) {
    if (ppSupported) *ppSupported = nullptr;
    if (!pReq) return E_POINTER;
    const WAVEFORMATEXTENSIBLE* w = asExt(pReq->GetAudioFormat());
    if (w) {
        std::ostringstream os;
        os << "IsOutputFormatSupported ch=" << w->Format.nChannels << " rate=" << w->Format.nSamplesPerSec
           << " bits=" << w->Format.wBitsPerSample << " mask=0x" << std::hex << w->dwChannelMask
           << " supported=" << isSupportedOutputFormat(*w);
        apoLog(stage_, instanceId_, os.str());
    }
    if (!w) return APOERR_FORMAT_NOT_SUPPORTED;
    if (isSupportedOutputFormat(*w)) {
        if (ppSupported) { *ppSupported = pReq; pReq->AddRef(); }
        return S_OK;
    }
    if (ppSupported && w->SubFormat == kSubtypeIeeeFloat && isXonarRate(w->Format.nSamplesPerSec)) {
        const WAVEFORMATEXTENSIBLE proposed =
            makeFloatFormat(w->Format.nSamplesPerSec, kPhysChannels, kPhysMask);
        *ppSupported = new (std::nothrow) MediaType(proposed);
        return *ppSupported ? S_FALSE : E_OUTOFMEMORY;
    }
    return APOERR_FORMAT_NOT_SUPPORTED;
}

// --- IAudioProcessingObjectConfiguration -----------------------------------
HRESULT STDMETHODCALLTYPE XonarApo::LockForProcess(UINT32 nIn, APO_CONNECTION_DESCRIPTOR** ppIn,
                                                   UINT32 nOut, APO_CONNECTION_DESCRIPTOR** ppOut) {
    if (!initialized_) return APOERR_NOT_INITIALIZED;
    if (nIn != 1 || nOut != 1) return APOERR_NUM_CONNECTIONS_INVALID;
    if (!ppIn || !ppOut || !ppIn[0] || !ppOut[0]) return E_POINTER;

    const WAVEFORMATEXTENSIBLE* in = asExt(ppIn[0]->pFormat->GetAudioFormat());
    const WAVEFORMATEXTENSIBLE* out = asExt(ppOut[0]->pFormat->GetAudioFormat());
    if (!in || !out) return APOERR_INVALID_CONNECTION_FORMAT;
    if (!isSupportedInputFormat(*in) || !isSupportedOutputFormat(*out)) return APOERR_FORMAT_NOT_SUPPORTED;
    if (in->Format.nSamplesPerSec != out->Format.nSamplesPerSec) return APOERR_INVALID_CONNECTION_FORMAT;

    inputMask_ = in->dwChannelMask;
    inputChannels_ = in->Format.nChannels;
    outputChannels_ = out->Format.nChannels;
    sampleRate_ = in->Format.nSamplesPerSec;

    UINT32 maxFrames = ppIn[0]->u32MaxFrameCount;
    if (ppOut[0]->u32MaxFrameCount > maxFrames) maxFrames = ppOut[0]->u32MaxFrameCount;
    if (maxFrames == 0) maxFrames = 4096; // garde-fou

    const xonar::dsp::Profile profile =
        buildProfile(stage_, inputMask_, kPhysMask, sampleRate_, settings_);
    pipelineReady_ = pipeline_.prepare(profile, maxFrames);
    try {
        scratchIn_.assign(static_cast<std::size_t>(maxFrames) * inputChannels_, 0.f);
    } catch (...) {
        return E_OUTOFMEMORY;
    }

    std::ostringstream os;
    os << "LockForProcess FORMAT REEL PRESENTE PAR WINDOWS : entree ch=" << inputChannels_
       << " mask=0x" << std::hex << inputMask_ << std::dec << " rate=" << sampleRate_
       << " bits=" << in->Format.wBitsPerSample
       << " | sortie ch=" << outputChannels_ << " mask=0x" << std::hex << out->dwChannelMask << std::dec
       << " | maxFrames=" << maxFrames
       << " | pipeline=" << (pipelineReady_ ? "pret" : "ECHEC->repli identite");
    apoLog(stage_, instanceId_, os.str());

    locked_ = true;
    processCalls_ = processedFrames_ = silentFrames_ = 0;
    inputEnergy_ = outputEnergy_ = 0;
    for (auto& e : outChEnergy_) e = 0.0;
    startSettingsWatcher(); // relecture à chaud du .cfg pendant le flux
    return S_OK;
}

HRESULT STDMETHODCALLTYPE XonarApo::UnlockForProcess() {
    if (!locked_) return APOERR_ALREADY_UNLOCKED;
    stopSettingsWatcher();
    std::ostringstream os;
    os << "UnlockForProcess calls=" << processCalls_ << " dspFrames=" << processedFrames_
       << " silentFrames=" << silentFrames_
       << " inputEnergy=" << inputEnergy_ << " outputEnergy=" << outputEnergy_;
    if (inputEnergy_ > 0 && outputEnergy_ > 0)
        os << " measuredGainDb=" << 10.0 * std::log10(outputEnergy_ / inputEnergy_);
    // Énergie de sortie par canal : montre quels haut-parleurs reçoivent du signal
    // (preuve objective de l'upmix : une source sur FL seul doit allumer FC et SL).
    {
        static const char* kLbl6[6] = {"FL", "FR", "FC", "LFE", "SL", "SR"};
        os << " perCh[";
        for (std::uint32_t c = 0; c < outputChannels_ && c < 8; ++c) {
            if (c) os << ' ';
            if (outputChannels_ == 6) os << kLbl6[c]; else os << 'c' << c;
            os << '=' << outChEnergy_[c];
        }
        os << ']';
    }
    apoLog(stage_, instanceId_, os.str());
    writeApoStatus(stage_, instanceId_, currentFileTime_, pipeline_.profile(), nullptr, nullptr, nullptr, false);
    locked_ = false;
    pipelineReady_ = false;
    return S_OK;
}

// --- Relecture à chaud du .cfg (contrôle live) -----------------------------
void XonarApo::startSettingsWatcher() {
    if (settingsWatcher_.joinable()) return;
    watchStop_.store(false, std::memory_order_relaxed);
    pendingProfile_.reset();
    appliedFileTime_.store(0, std::memory_order_relaxed);
    rejectedFileTime_.store(0, std::memory_order_relaxed);
    currentFileTime_ = 0;
    meterFrames_ = 0;
    meterSequence_.store(0);
    callbackSerial_.store(0); preLimiterPeak_.store(0); limiterReductionDb_.store(0); clippedSamples_.store(0);
    clippedTotal_ = 0;
    for (unsigned c = 0; c < xonar::dsp::kMaxChannels; ++c) {
        meterInEnergy_[c] = meterOutEnergy_[c] = meterCross_[c] = 0;
        inputRms_[c].store(0); outputRms_[c].store(0); correlation_[c].store(0); outputPeak_[c].store(0);
    }
    if (!watchWake_) watchWake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset
    if (!watchWake_) return;
    try {
        settingsWatcher_ = std::thread(&XonarApo::settingsWatchLoop, this);
    } catch (...) {
        // Sans thread de veille, l'APO reste fonctionnel (relecture au prochain flux).
    }
}

void XonarApo::stopSettingsWatcher() noexcept {
    watchStop_.store(true, std::memory_order_relaxed);
    if (watchWake_) SetEvent(watchWake_);
    if (settingsWatcher_.joinable()) {
        try { settingsWatcher_.join(); } catch (...) {}
    }
    if (watchWake_) {
        CloseHandle(watchWake_);
        watchWake_ = nullptr;
    }
}

void XonarApo::settingsWatchLoop() {
    try {
        const std::wstring path = cfgPath(stage_);
        FILETIME last{};
        // Publier aussi le fichier initial : ne perdre aucun changement entre
        // Initialize et LockForProcess.
        std::uint64_t reported = 0;
        ProfileUpdate latest{};
        std::uint64_t previousSerial = 0;
        ULONGLONG lastCallback = 0;
        while (!watchStop_.load(std::memory_order_relaxed)) {
            WaitForSingleObject(watchWake_, 250); // réveil périodique, ou arrêt immédiat
            if (watchStop_.load(std::memory_order_relaxed)) break;
            const auto applied = appliedFileTime_.load(std::memory_order_acquire);
            if (applied && applied != reported) {
                reported = applied;
                apoLog(stage_, instanceId_, "Hot reload APPLIED fileTime=" + std::to_string(applied) +
                    " mode=" + std::to_string(static_cast<unsigned>(latest.profile.mode)));
            }
            if (applied && applied == latest.fileTime) {
                const auto serial = callbackSerial_.load();
                const auto nowTick = GetTickCount64();
                if(serial != previousSerial) { lastCallback = nowTick; previousSerial = serial; }
                const bool active = lastCallback && nowTick - lastCallback < 1000;
                RuntimeMeters meters{};
                meters.enabled = latest.enabled;
                meters.preLimiterPeak = preLimiterPeak_.load();
                meters.limiterReductionDb = limiterReductionDb_.load();
                meters.clippedSamples = clippedSamples_.load();
                float input[xonar::dsp::kMaxChannels]{}, output[xonar::dsp::kMaxChannels]{}, correlation[xonar::dsp::kMaxChannels]{};
                const auto sequence = meterSequence_.load();
                if (!(sequence & 1u)) {
                    for (unsigned c = 0; c < xonar::dsp::kMaxChannels; ++c) {
                        input[c] = inputRms_[c].load(); output[c] = outputRms_[c].load(); correlation[c] = correlation_[c].load();
                        meters.peaks[c] = outputPeak_[c].load();
                    }
                    if (sequence == meterSequence_.load())
                        writeApoStatus(stage_, instanceId_, applied, latest.profile, input, output, correlation, active, meters);
                }
            }
            FILETIME now{};
            // Une suppression restaure aussi les défauts pendant le flux actif.
            if (!fileWriteTime(path, now) && last.dwHighDateTime == 0 && last.dwLowDateTime == 0) continue;
            if (CompareFileTime(&now, &last) == 0) continue; // inchangé
            last = now;
            const ApoSettings s = loadApoSettings(stage_);
            latest.profile = buildProfile(stage_, inputMask_, kPhysMask, sampleRate_, s);
            latest.fileTime = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
            latest.enabled = s.enabled;
            latest.sfxUpmixStereo = s.sfxUpmixStereo;
            pendingProfile_.publish(latest);
            apoLog(stage_, instanceId_,
                   "Hot reload: profil republie (masterGainDb=" + std::to_string(s.masterGainDb) + ")");
        }
    } catch (...) {
        // Le veilleur ne doit JAMAIS faire tomber audiodg.
    }
}

void XonarApo::applyPendingProfileRt() noexcept {
    const auto* update = pendingProfile_.consume();
    if (!update) return;
    if (pipeline_.setProfile(update->profile)) {
        settings_.enabled = update->enabled;
        settings_.sfxUpmixStereo = update->sfxUpmixStereo;
        currentFileTime_ = update->fileTime;
    } else {
        rejectedFileTime_.store(update->fileTime, std::memory_order_release);
    }
}

// --- IAudioProcessingObjectRT (temps réel : pas d'alloc, verrou, E/S) ------
void STDMETHODCALLTYPE XonarApo::APOProcess(UINT32 nIn, APO_CONNECTION_PROPERTY** ppIn, UINT32 nOut,
                                            APO_CONNECTION_PROPERTY** ppOut) {
    if (!locked_ || nIn != 1 || nOut != 1) return;
    if (!ppIn || !ppOut) return;
    APO_CONNECTION_PROPERTY* inc = ppIn[0];
    APO_CONNECTION_PROPERTY* outc = ppOut[0];
    if (!inc || !outc) return;
    applyPendingProfileRt(); // prend en compte un profil republié (contrôle live)
    appliedFileTime_.store(currentFileTime_, std::memory_order_release);

    const UINT32 frames = inc->u32ValidFrameCount;
    ++processCalls_;
    callbackSerial_.fetch_add(1, std::memory_order_relaxed);
    auto* dst = reinterpret_cast<float*>(outc->pBuffer);
    const auto* src = reinterpret_cast<const float*>(inc->pBuffer);

    if (inc->u32BufferFlags == BUFFER_SILENT) {
        meterSequence_.fetch_add(1);
        preLimiterPeak_.store(0); limiterReductionDb_.store(0); pipeline_.resetStats();
        for (unsigned c = 0; c < outputChannels_; ++c) {
            outputPeak_[c].store(0);
            inputRms_[c].store(0); outputRms_[c].store(0); correlation_[c].store(0);
            meterInEnergy_[c] = meterOutEnergy_[c] = meterCross_[c] = 0;
        }
        meterFrames_ = 0;
        meterSequence_.fetch_add(1);
        for (UINT32 i = 0; i < frames * outputChannels_; ++i) dst[i] = 0.f;
        outc->u32ValidFrameCount = frames;
        outc->u32BufferFlags = BUFFER_SILENT;
        silentFrames_ += frames;
        return;
    }

    bool processed = false;
    const bool bypass = !settings_.enabled || (stage_ == Stage::Sfx && inputChannels_ == outputChannels_ &&
        pipeline_.profile().mode == xonar::dsp::Mode::PassThrough && !pipeline_.isTransitioning());
    if (pipelineReady_ && !bypass) {
        const std::size_t n = static_cast<std::size_t>(frames) * inputChannels_;
        if (scratchIn_.size() >= n) {
            std::memcpy(scratchIn_.data(), src, n * sizeof(float));
            xonar::dsp::ConstInterleavedView iv{scratchIn_.data(), inputChannels_, frames};
            xonar::dsp::InterleavedView ov{dst, outputChannels_, frames};
            processed = pipeline_.process(iv, ov);
            // Mesure bornée sans E/S ni allocation. scratchIn_ préserve l'entrée
            // même lorsque le moteur Windows fournit le même tampon en sortie.
            if (processed && inputChannels_ == outputChannels_) {
                std::uint32_t ch = 0;
                for (std::size_t sample = 0; sample < n; ++sample) {
                    const double input = scratchIn_[sample], output = dst[sample];
                    inputEnergy_ += input * input;
                    outputEnergy_ += output * output;
                    outChEnergy_[ch] += output * output; // énergie par canal (diagnostic upmix)
                    meterInEnergy_[ch] += input * input;
                    meterOutEnergy_[ch] += output * output;
                    meterCross_[ch] += input * output;
                    if (++ch >= outputChannels_) ch = 0;
                }
                meterFrames_ += frames;
                if (meterFrames_ >= sampleRate_ / 20) {
                    meterSequence_.fetch_add(1);
                    const auto& stats = pipeline_.stats();
                    preLimiterPeak_.store(stats.preLimiterPeak, std::memory_order_relaxed);
                    limiterReductionDb_.store(-stats.limiterMinGainDb, std::memory_order_relaxed);
                    clippedTotal_ += stats.clippedSamples;
                    clippedSamples_.store(clippedTotal_, std::memory_order_relaxed);
                    for(std::uint32_t c = 0; c < outputChannels_; ++c)
                        outputPeak_[c].store(stats.outputPeak[c], std::memory_order_relaxed);
                    for (std::uint32_t c = 0; c < outputChannels_; ++c) {
                        inputRms_[c].store(static_cast<float>(std::sqrt(meterInEnergy_[c] / meterFrames_)));
                        outputRms_[c].store(static_cast<float>(std::sqrt(meterOutEnergy_[c] / meterFrames_)));
                        const double product = meterInEnergy_[c] * meterOutEnergy_[c];
                        correlation_[c].store(product > 0 ? static_cast<float>(meterCross_[c] / std::sqrt(product)) : 0.f);
                        meterInEnergy_[c] = meterOutEnergy_[c] = meterCross_[c] = 0;
                    }
                    meterFrames_ = 0;
                    pipeline_.resetStats();
                    meterSequence_.fetch_add(1);
                }
            }
        }
    }

    if (!processed) {
        // Repli identité / sûr : mêmes canaux -> copie ; 2->6 -> FL/FR puis zéros.
        if (inputChannels_ == outputChannels_) {
            if (dst != src) std::memcpy(dst, src, static_cast<std::size_t>(frames) * outputChannels_ * sizeof(float));
        } else {
            for (UINT32 f = 0; f < frames; ++f) {
                for (UINT32 c = 0; c < outputChannels_; ++c) {
                    dst[static_cast<std::size_t>(f) * outputChannels_ + c] =
                        (c < 2 && c < inputChannels_) ? src[static_cast<std::size_t>(f) * inputChannels_ + c] : 0.f;
                }
            }
        }
    }

    outc->u32ValidFrameCount = frames;
    outc->u32BufferFlags = BUFFER_VALID;
    if (processed) processedFrames_ += frames;
}

// --- IAudioSystemEffects2 ---------------------------------------------------
HRESULT STDMETHODCALLTYPE XonarApo::GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE /*Event*/) {
    if (!ppEffectsIds || !pcEffects) return E_POINTER;
    auto* ids = static_cast<LPGUID>(CoTaskMemAlloc(sizeof(GUID)));
    if (!ids) {
        *ppEffectsIds = nullptr;
        *pcEffects = 0;
        return E_OUTOFMEMORY;
    }
    ids[0] = clsid();
    *ppEffectsIds = ids;
    *pcEffects = 1;
    return S_OK;
}

} // namespace xonar::apo
