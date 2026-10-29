// Validation et construction des formats PCM du filtre WaveRT (voir wave_formats.h).
#include "wave_formats.h"

#ifndef WAVE_FORMAT_PCM
#define WAVE_FORMAT_PCM 1
#endif

namespace xonar::wave {

bool IsCandidateRate(ULONG Rate)
{
    for (const ULONG r : xonar::hw::kCandidateRates) {
        if (r == Rate) return true;
    }
    return false;
}

bool IsChannelMaskAccepted(USHORT Channels, ULONG Mask, ULONG AltMask)
{
    if (Mask == AltMask) return true;
    switch (Channels) {
    case 6:
        // 0x60F = FL FR FC LFE SL SR (descripteur USB) ; 0x3F = FL FR FC LFE BL BR
        // (KSAUDIO_SPEAKER_5POINT1). Même ordre de trame, seule l'étiquette change.
        return Mask == 0x60F || Mask == 0x3F;
    case 4:
        // 0x33 = FL FR BL BR (KSAUDIO_SPEAKER_QUAD, descripteur USB) ; 0x603 = FL FR SL SR.
        return Mask == 0x33 || Mask == 0x603;
    case 2:
        return Mask == 0x3;
    default:
        return false;
    }
}

#pragma code_seg("PAGE")

NTSTATUS ValidateFormat(const XONARSE_ENDPOINT_HW& Hw, const KSDATAFORMAT* Format, ULONG BufferSize, StreamFormat* Out)
{
    PAGED_CODE();
    RtlZeroMemory(Out, sizeof(*Out));
    if (Format == nullptr || BufferSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) return STATUS_NO_MATCH;
    if (Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX) || Format->FormatSize > BufferSize) return STATUS_NO_MATCH;
    if (!IsEqualGUIDAligned(Format->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !IsEqualGUIDAligned(Format->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !IsEqualGUIDAligned(Format->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    const WAVEFORMATEX* wfx = &reinterpret_cast<const KSDATAFORMAT_WAVEFORMATEX*>(Format)->WaveFormatEx;
    ULONG mask = 0;
    USHORT validBits = 0;
    if (wfx->wFormatTag == WAVE_FORMAT_PCM) {
        // Un WAVEFORMATEX simple n'a pas de masque : on ne l'accepte qu'en stéréo,
        // où l'interprétation (FL FR) est sans ambiguïté.
        if (wfx->nChannels != 2) return STATUS_NO_MATCH;
        mask = 0x3;
        validBits = wfx->wBitsPerSample;
    } else if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (Format->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE) ||
            wfx->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
            return STATUS_NO_MATCH;
        }
        const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        if (!IsEqualGUIDAligned(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) return STATUS_NO_MATCH;
        mask = ext->dwChannelMask;
        validBits = ext->Samples.wValidBitsPerSample;
    } else {
        return STATUS_NO_MATCH;
    }

    // Pas de 24 bits dans un conteneur 32 : le sous-slot USB fait 3 octets.
    if (validBits != wfx->wBitsPerSample) return STATUS_NO_MATCH;
    if (wfx->nChannels == 0 || wfx->nChannels > Hw.MaxChannels) return STATUS_NO_MATCH;
    if (wfx->wBitsPerSample != 16 && wfx->wBitsPerSample != 24) return STATUS_NO_MATCH;

    const xonar::hw::AltFormat* alt = xonar::hw::FindAltFormat(Hw.Interface, static_cast<UCHAR>(wfx->nChannels),
                                                               static_cast<UCHAR>(wfx->wBitsPerSample));
    if (alt == nullptr) return STATUS_NO_MATCH;
    if (static_cast<ULONG>(alt->SubslotBytes) * 8 != wfx->wBitsPerSample) return STATUS_NO_MATCH;

    const ULONG blockAlign = static_cast<ULONG>(alt->Channels) * alt->SubslotBytes;
    if (wfx->nBlockAlign != blockAlign) return STATUS_NO_MATCH;
    if (!IsCandidateRate(wfx->nSamplesPerSec)) return STATUS_NO_MATCH;
    if (wfx->nAvgBytesPerSec != wfx->nSamplesPerSec * blockAlign) return STATUS_NO_MATCH;
    if (!IsChannelMaskAccepted(wfx->nChannels, mask, alt->ChannelMask)) return STATUS_NO_MATCH;

    Out->Alt = alt;
    Out->SampleRate = wfx->nSamplesPerSec;
    Out->ChannelMask = mask;
    Out->Channels = wfx->nChannels;
    Out->BitsPerSample = wfx->wBitsPerSample;
    Out->BlockAlign = static_cast<USHORT>(blockAlign);
    return STATUS_SUCCESS;
}

void BuildFormat(const xonar::hw::AltFormat& Alt, ULONG Rate, KSDATAFORMAT_WAVEFORMATEXTENSIBLE* Out)
{
    PAGED_CODE();
    RtlZeroMemory(Out, sizeof(*Out));
    const USHORT blockAlign = static_cast<USHORT>(Alt.Channels * Alt.SubslotBytes);

    Out->DataFormat.FormatSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    Out->DataFormat.Flags = 0;
    Out->DataFormat.SampleSize = blockAlign;
    Out->DataFormat.Reserved = 0;
    Out->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    Out->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    Out->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    WAVEFORMATEXTENSIBLE& w = Out->WaveFormatExt;
    w.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    w.Format.nChannels = Alt.Channels;
    w.Format.nSamplesPerSec = Rate;
    w.Format.nBlockAlign = blockAlign;
    w.Format.nAvgBytesPerSec = Rate * blockAlign;
    w.Format.wBitsPerSample = static_cast<WORD>(Alt.SubslotBytes * 8);
    w.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    w.Samples.wValidBitsPerSample = Alt.BitResolution;
    w.dwChannelMask = Alt.ChannelMask;
    w.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
}

NTSTATUS BuildDefaultFormat(const XONARSE_ENDPOINT_HW& Hw, KSDATAFORMAT_WAVEFORMATEXTENSIBLE* Out)
{
    PAGED_CODE();
    const xonar::hw::AltFormat* alt = xonar::hw::FindAltFormat(Hw.Interface, Hw.MaxChannels, 24);
    if (alt == nullptr) alt = xonar::hw::FindAltFormat(Hw.Interface, Hw.MaxChannels, 16);
    if (alt == nullptr) {
        RtlZeroMemory(Out, sizeof(*Out));
        return STATUS_NOT_SUPPORTED;
    }
    ULONG rate = XonarSeSettings().DefaultSampleRate;
    if (!IsCandidateRate(rate)) rate = xonar::hw::kDefaultRate;
    BuildFormat(*alt, rate, Out);
    return STATUS_SUCCESS;
}

#pragma code_seg()

} // namespace xonar::wave
