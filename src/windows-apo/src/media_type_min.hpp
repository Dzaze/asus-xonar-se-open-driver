// SPDX-License-Identifier: Apache-2.0
// IAudioMediaType minimal : encapsule un WAVEFORMATEXTENSIBLE float32 entrelacé, pour
// PROPOSER un format pris en charge (S_FALSE) lors des négociations IsXxxFormatSupported.
// Repris de experiments/apo-format-spike/src/media_type.hpp (mécanique déjà prouvée).
#pragma once

#include <windows.h>
#include <mmreg.h>

#include <audiomediatype.h>

#include <atomic>
#include <cstdint>

namespace xonar::apo {

// {00000003-0000-0010-8000-00AA00389B71} = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT.
inline const GUID kSubtypeIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

inline WAVEFORMATEXTENSIBLE makeFloatFormat(std::uint32_t sampleRate, std::uint16_t channels,
                                            std::uint32_t channelMask) {
    WAVEFORMATEXTENSIBLE w{};
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = channels;
    w.Format.nSamplesPerSec = sampleRate;
    w.Format.wBitsPerSample = 32;
    w.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    w.Format.nAvgBytesPerSec = sampleRate * w.Format.nBlockAlign;
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = 32;
    w.dwChannelMask = channelMask;
    w.SubFormat = kSubtypeIeeeFloat;
    return w;
}

class MediaType final : public IAudioMediaType {
public:
    explicit MediaType(const WAVEFORMATEXTENSIBLE& wfx) : wfx_(wfx) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioMediaType)) {
            *ppv = static_cast<IAudioMediaType*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG r = --ref_;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE IsCompressedFormat(BOOL* pfCompressed) override {
        if (!pfCompressed) return E_POINTER;
        *pfCompressed = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE IsEqual(IAudioMediaType* pOther, DWORD* pdwFlags) override {
        if (!pOther || !pdwFlags) return E_POINTER;
        const WAVEFORMATEX* o = pOther->GetAudioFormat();
        *pdwFlags = 0;
        if (o && o->nSamplesPerSec == wfx_.Format.nSamplesPerSec &&
            o->nChannels == wfx_.Format.nChannels && o->wBitsPerSample == wfx_.Format.wBitsPerSample) {
            return S_OK;
        }
        return S_FALSE;
    }
    const WAVEFORMATEX* STDMETHODCALLTYPE GetAudioFormat() override {
        return reinterpret_cast<const WAVEFORMATEX*>(&wfx_);
    }
    HRESULT STDMETHODCALLTYPE GetUncompressedAudioFormat(UNCOMPRESSEDAUDIOFORMAT* pFmt) override {
        if (!pFmt) return E_POINTER;
        pFmt->guidFormatType = wfx_.SubFormat;
        pFmt->dwSamplesPerFrame = wfx_.Format.nChannels;
        pFmt->dwBytesPerSampleContainer = wfx_.Format.wBitsPerSample / 8u;
        pFmt->dwValidBitsPerSample = wfx_.Samples.wValidBitsPerSample;
        pFmt->fFramesPerSecond = static_cast<float>(wfx_.Format.nSamplesPerSec);
        pFmt->dwChannelMask = wfx_.dwChannelMask;
        return S_OK;
    }

private:
    std::atomic<ULONG> ref_{1};
    WAVEFORMATEXTENSIBLE wfx_;
};

} // namespace xonar::apo
