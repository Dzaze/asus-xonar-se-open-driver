// SPDX-License-Identifier: Apache-2.0
// APO Windows Xonar SE : une seule implémentation, paramétrée par l'étage (SFX ou
// EFX). Implémente IAudioProcessingObject, IAudioProcessingObjectRT,
// IAudioProcessingObjectConfiguration et IAudioSystemEffects2, et enveloppe le moteur
// xonar::dsp::Pipeline.
//
// Contrat temps réel (APOProcess) : aucune allocation, aucun verrou, aucune E/S,
// aucune exception. Tout ce qui alloue (préparation du pipeline, tampon de travail,
// lecture de configuration, journal) se fait dans Initialize / LockForProcess.
#pragma once

#include "apo_settings.hpp"

#include "xonar/dsp/pipeline.hpp"
#include "xonar/dsp/latest_value.hpp"

#include <windows.h>
#include <mmreg.h>

#include <audioenginebaseapo.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace xonar::apo {

// CLSID gelés par E0-02 (contracts/manifest.json, miroir de contracts.hpp).
extern "C" const CLSID CLSID_XonarSfxApo; // {C243A8CB-D535-4BA0-BCC5-CC4338833E7A}
extern "C" const CLSID CLSID_XonarEfxApo; // {D2FBC737-00C3-4482-B3DD-02D92C06003E}

class XonarApo final : public IAudioProcessingObjectRT,
                       public IAudioProcessingObjectConfiguration,
                       public IAudioProcessingObject,
                       public IAudioSystemEffects2 {
public:
    explicit XonarApo(Stage stage, IUnknown* outer = nullptr);
    // La fabrique conserve la référence initiale de cet IUnknown interne.
    IUnknown* innerUnknown() noexcept { return &inner_; }

    // IUnknown ------------------------------------------------------------
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IAudioProcessingObject ---------------------------------------------
    HRESULT STDMETHODCALLTYPE Reset() override;
    HRESULT STDMETHODCALLTYPE GetLatency(HNSTIME* pTime) override;
    HRESULT STDMETHODCALLTYPE GetRegistrationProperties(APO_REG_PROPERTIES** ppRegProps) override;
    HRESULT STDMETHODCALLTYPE Initialize(UINT32 cbDataSize, BYTE* pbyData) override;
    HRESULT STDMETHODCALLTYPE IsInputFormatSupported(IAudioMediaType* pOppositeFormat,
                                                     IAudioMediaType* pRequestedInputFormat,
                                                     IAudioMediaType** ppSupportedInputFormat) override;
    HRESULT STDMETHODCALLTYPE IsOutputFormatSupported(IAudioMediaType* pOppositeFormat,
                                                      IAudioMediaType* pRequestedOutputFormat,
                                                      IAudioMediaType** ppSupportedOutputFormat) override;
    HRESULT STDMETHODCALLTYPE GetInputChannelCount(UINT32* pu32ChannelCount) override;

    // IAudioProcessingObjectRT -------------------------------------------
    void STDMETHODCALLTYPE APOProcess(UINT32 u32NumInputConnections,
                                      APO_CONNECTION_PROPERTY** ppInputConnections,
                                      UINT32 u32NumOutputConnections,
                                      APO_CONNECTION_PROPERTY** ppOutputConnections) override;
    UINT32 STDMETHODCALLTYPE CalcInputFrames(UINT32 u32OutputFrameCount) override { return u32OutputFrameCount; }
    UINT32 STDMETHODCALLTYPE CalcOutputFrames(UINT32 u32InputFrameCount) override { return u32InputFrameCount; }

    // IAudioProcessingObjectConfiguration --------------------------------
    HRESULT STDMETHODCALLTYPE LockForProcess(UINT32 u32NumInputConnections,
                                             APO_CONNECTION_DESCRIPTOR** ppInputConnections,
                                             UINT32 u32NumOutputConnections,
                                             APO_CONNECTION_DESCRIPTOR** ppOutputConnections) override;
    HRESULT STDMETHODCALLTYPE UnlockForProcess() override;

    // IAudioSystemEffects2 (hérite IAudioSystemEffects, marqueur) --------
    HRESULT STDMETHODCALLTYPE GetEffectsList(LPGUID* ppEffectsIds, UINT* pcEffects, HANDLE Event) override;

private:
    class InnerUnknown final : public IUnknown {
    public:
        explicit InnerUnknown(XonarApo& owner) : owner_(owner) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override;
        ULONG STDMETHODCALLTYPE AddRef() override;
        ULONG STDMETHODCALLTYPE Release() override;
    private:
        XonarApo& owner_;
    };
    HRESULT innerQueryInterface(REFIID iid, void** out);
    ULONG innerAddRef() noexcept;
    ULONG innerRelease() noexcept;
    ~XonarApo();
    const CLSID& clsid() const noexcept;
    bool isSupportedInputFormat(const WAVEFORMATEXTENSIBLE& w) const noexcept;
    bool isSupportedOutputFormat(const WAVEFORMATEXTENSIBLE& w) const noexcept;

    std::atomic<ULONG> ref_{1};
    InnerUnknown inner_{*this};
    IUnknown* const outer_; // référence faible : aucune boucle de références COM
    const Stage stage_;
    const int instanceId_;

    bool initialized_ = false;
    bool locked_ = false;
    std::uint64_t processCalls_ = 0;
    std::uint64_t processedFrames_ = 0;
    std::uint64_t silentFrames_ = 0;
    double inputEnergy_ = 0;
    double outputEnergy_ = 0;
    // Énergie de sortie par canal (position dans la trame de sortie) : diagnostic de
    // l'upmix (montre quels canaux reçoivent du signal). Non temps-critique.
    double outChEnergy_[xonar::dsp::kMaxChannels] = {};

    ApoSettings settings_{};
    std::uint32_t inputMask_ = 0;
    std::uint32_t inputChannels_ = 0;
    std::uint32_t outputChannels_ = 0;
    std::uint32_t sampleRate_ = 0;

    xonar::dsp::Pipeline pipeline_;
    bool pipelineReady_ = false;
    std::vector<float> scratchIn_; // copie d'entrée (APOProcess interdit le recouvrement)

    // Relecture à chaud du .cfg (contrôle live depuis le centre de contrôle) : un thread
    // de veille détecte les changements du fichier et publie un nouveau profil ;
    // APOProcess l'applique via une boîte SPSC atomique sans verrou. Le
    // pipeline_ n'est touché QUE par le thread audio ; le veilleur ne touche jamais
    // pipeline_. Les changements « à chaud » (gains, mute, EQ, limiteur) prennent effet
    // immédiatement, y compris les modes partageant le transport physique 6->6.
    void startSettingsWatcher();
    void stopSettingsWatcher() noexcept;
    void settingsWatchLoop();
    void applyPendingProfileRt() noexcept; // appelé depuis APOProcess (temps réel)

    std::thread settingsWatcher_;
    HANDLE watchWake_ = nullptr;
    std::atomic<bool> watchStop_{false};
    struct ProfileUpdate {
        xonar::dsp::Profile profile{};
        std::uint64_t fileTime = 0;
        bool enabled = true;
        bool sfxUpmixStereo = false;
    };
    xonar::dsp::LatestValue<ProfileUpdate> pendingProfile_;
    std::atomic<std::uint64_t> appliedFileTime_{0};
    std::atomic<std::uint64_t> rejectedFileTime_{0};
    std::uint64_t currentFileTime_ = 0; // thread audio exclusivement
    std::atomic<float> inputRms_[xonar::dsp::kMaxChannels]{};
    std::atomic<float> outputRms_[xonar::dsp::kMaxChannels]{};
    std::atomic<float> correlation_[xonar::dsp::kMaxChannels]{};
    std::atomic<std::uint32_t> meterSequence_{0};
    std::atomic<std::uint64_t> callbackSerial_{0};
    std::atomic<float> outputPeak_[xonar::dsp::kMaxChannels]{};
    std::atomic<float> preLimiterPeak_{0}, limiterReductionDb_{0};
    std::atomic<std::uint64_t> clippedSamples_{0};
    std::uint64_t clippedTotal_ = 0; // thread audio ; cumul conservé entre fenêtres de mesure
    double meterInEnergy_[xonar::dsp::kMaxChannels]{};
    double meterOutEnergy_[xonar::dsp::kMaxChannels]{};
    double meterCross_[xonar::dsp::kMaxChannels]{};
    std::uint32_t meterFrames_ = 0;
};

} // namespace xonar::apo
