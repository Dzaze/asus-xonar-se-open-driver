// Miniport et flux WaveRT de capture (voir minwavertcapture.h).
//
// Reprend la structure du rendu (minwavert.cpp, minwavertstream.cpp ; inspirés de SYSVAD,
// licence MIT, Copyright (c) Microsoft Corporation) en inversant le sens du flux et en
// branchant le moteur isochrone IN. Le format est figé à la création (comme SYSVAD).
#include "minwavertcapture.h"

namespace {

constexpr ULONG kRateOrder[] = {48000, 44100, 96000, 192000};
static_assert(ARRAYSIZE(kRateOrder) == XONAR_CAP_RATE_COUNT, "kRateOrder doit couvrir kCandidateRates");
constexpr ULONG kVtIllegal = 0xFFFF;

KSDATARANGE g_CaptureBridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE),
};

const GUID g_CaptureFilterCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_CAPTURE),
    STATICGUIDOF(KSCATEGORY_REALTIME),
};

NTSTATUS PropertyHandlerCaptureWaveFilter(_In_ PPCPROPERTY_REQUEST Request);

const PCPROPERTY_ITEM g_CaptureFilterProperties[] = {
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT,
     PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT,
     PropertyHandlerCaptureWaveFilter},
};

DEFINE_PCAUTOMATION_TABLE_PROP(g_CaptureFilterAutomation, g_CaptureFilterProperties);

bool GuidIsOrWildcard(const GUID& Value, const GUID& Expected)
{
    return IsEqualGUIDAligned(Value, Expected) || IsEqualGUIDAligned(Value, GUID_NULL);
}

} // namespace

#pragma code_seg("PAGE")

namespace {

NTSTATUS PropertyHandlerCaptureWaveFilter(PPCPROPERTY_REQUEST Request)
{
    PAGED_CODE();
    if (Request == nullptr || Request->MajorTarget == nullptr || Request->PropertyItem == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!IsEqualGUIDAligned(*Request->PropertyItem->Set, KSPROPSETID_Pin) ||
        Request->PropertyItem->Id != KSPROPERTY_PIN_PROPOSEDATAFORMAT) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    auto* miniport =
        static_cast<CXonarWaveRtCaptureMiniport*>(static_cast<PMINIPORTWAVERT>(Request->MajorTarget));
    return miniport->PropertyProposedFormat(Request);
}

} // namespace

NTSTATUS CreateXonarWaveRtCaptureMiniport(_Out_ PUNKNOWN* Unknown, _In_ XonarUsbDevice* Usb,
                                          _In_ XONARSE_ENDPOINT Endpoint)
{
    PAGED_CODE();
    if (Unknown == nullptr) return STATUS_INVALID_PARAMETER;
    *Unknown = nullptr;
    if (Usb == nullptr || Endpoint >= XonarEndpointCount) return STATUS_INVALID_PARAMETER;

    auto* miniport = new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) CXonarWaveRtCaptureMiniport(Usb, Endpoint);
    if (miniport == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    if (miniport->StreamRangeCount() == 0) {
        XONARSE_ERROR("capture: endpoint %lu sans altsetting PCM", static_cast<ULONG>(Endpoint));
        delete miniport;
        return STATUS_NOT_SUPPORTED;
    }
    miniport->AddRef();
    *Unknown = PUNKNOWN(PMINIPORTWAVERT(miniport));
    XONARSE_INFO("capture: miniport WaveRT créé pour l'endpoint %lu (%lu plages)", static_cast<ULONG>(Endpoint),
                 miniport->StreamRangeCount());
    return STATUS_SUCCESS;
}

CXonarWaveRtCaptureMiniport::CXonarWaveRtCaptureMiniport(XonarUsbDevice* Usb, XONARSE_ENDPOINT Endpoint)
    : CUnknown(nullptr), m_Usb(Usb), m_Endpoint(Endpoint), m_Hw(&XonarSeEndpointHw(Endpoint))
{
    PAGED_CODE();
    m_Usb->AddRef();
    BuildDescriptors();
}

CXonarWaveRtCaptureMiniport::~CXonarWaveRtCaptureMiniport()
{
    PAGED_CODE();
    NT_ASSERT(m_StreamOpen == 0);
    if (m_Usb != nullptr) m_Usb->Release();
}

void CXonarWaveRtCaptureMiniport::BuildDescriptors()
{
    PAGED_CODE();
    // Altsettings PCM de l'interface de capture (feedback absent : pas de filtre dessus),
    // du plus riche au plus simple.
    const xonar::hw::AltFormat* alts[ARRAYSIZE(xonar::hw::kAltFormats)] = {};
    ULONG altCount = 0;
    for (const auto& f : xonar::hw::kAltFormats) {
        if (f.Interface != m_Hw->Interface || f.Iec61937) continue;
        ULONG pos = altCount++;
        while (pos > 0 && (alts[pos - 1]->Channels < f.Channels ||
                           (alts[pos - 1]->Channels == f.Channels && alts[pos - 1]->BitResolution < f.BitResolution))) {
            alts[pos] = alts[pos - 1];
            --pos;
        }
        alts[pos] = &f;
    }

    m_StreamRangeCount = 0;
    for (ULONG a = 0; a < altCount; ++a) {
        for (const ULONG rate : kRateOrder) {
            if (m_StreamRangeCount >= XONAR_CAP_MAX_RANGES) break;
            KSDATARANGE_AUDIO& r = m_RangeStorage[m_StreamRangeCount];
            r.DataRange.FormatSize = sizeof(KSDATARANGE_AUDIO);
            r.DataRange.Flags = 0;
            r.DataRange.SampleSize = 0;
            r.DataRange.Reserved = 0;
            r.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
            r.DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
            r.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
            r.MaximumChannels = alts[a]->Channels;
            r.MinimumBitsPerSample = alts[a]->SubslotBytes * 8u;
            r.MaximumBitsPerSample = alts[a]->SubslotBytes * 8u;
            r.MinimumSampleFrequency = rate;
            r.MaximumSampleFrequency = rate;
            m_StreamRanges[m_StreamRangeCount] = &r.DataRange;
            ++m_StreamRangeCount;
        }
    }
    m_BridgeRanges[0] = &g_CaptureBridgeRange;

    // Broche hôte (sortie du filtre vers le moteur audio) : capture = KSPIN_DATAFLOW_OUT.
    PCPIN_DESCRIPTOR& host = m_Pins[XONARSE_WAVE_CAPTURE_PIN_HOST];
    host.MaxGlobalInstanceCount = 1;
    host.MaxFilterInstanceCount = 1;
    host.MinFilterInstanceCount = 0;
    host.AutomationTable = nullptr;
    host.KsPinDescriptor.InterfacesCount = 0;
    host.KsPinDescriptor.Interfaces = nullptr;
    host.KsPinDescriptor.MediumsCount = 0;
    host.KsPinDescriptor.Mediums = nullptr;
    host.KsPinDescriptor.DataRangesCount = m_StreamRangeCount;
    host.KsPinDescriptor.DataRanges = m_StreamRanges;
    host.KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
    host.KsPinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
    host.KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    host.KsPinDescriptor.Name = nullptr;

    // Broche pont (entrée analogique depuis le filtre Topology) : capture = KSPIN_DATAFLOW_IN.
    PCPIN_DESCRIPTOR& bridge = m_Pins[XONARSE_WAVE_CAPTURE_PIN_BRIDGE];
    bridge.MaxGlobalInstanceCount = 0;
    bridge.MaxFilterInstanceCount = 0;
    bridge.MinFilterInstanceCount = 0;
    bridge.AutomationTable = nullptr;
    bridge.KsPinDescriptor.DataRangesCount = ARRAYSIZE(m_BridgeRanges);
    bridge.KsPinDescriptor.DataRanges = m_BridgeRanges;
    bridge.KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
    bridge.KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    bridge.KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    bridge.KsPinDescriptor.Name = nullptr;

    // Flux interne : pont (1) -> hôte (0).
    m_Connections[0].FromNode = PCFILTER_NODE;
    m_Connections[0].FromNodePin = XONARSE_WAVE_CAPTURE_PIN_BRIDGE;
    m_Connections[0].ToNode = PCFILTER_NODE;
    m_Connections[0].ToNodePin = XONARSE_WAVE_CAPTURE_PIN_HOST;

    m_Filter.Version = 0;
    m_Filter.AutomationTable = &g_CaptureFilterAutomation;
    m_Filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
    m_Filter.PinCount = ARRAYSIZE(m_Pins);
    m_Filter.Pins = m_Pins;
    m_Filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
    m_Filter.NodeCount = 0;
    m_Filter.Nodes = nullptr;
    m_Filter.ConnectionCount = ARRAYSIZE(m_Connections);
    m_Filter.Connections = m_Connections;
    m_Filter.CategoryCount = ARRAYSIZE(g_CaptureFilterCategories);
    m_Filter.Categories = g_CaptureFilterCategories;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureMiniport::NonDelegatingQueryInterface(REFIID Interface, PVOID* Object)
{
    PAGED_CODE();
    if (Object == nullptr) return STATUS_INVALID_PARAMETER;
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERT(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) {
        *Object = PVOID(PMINIPORT(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRT)) {
        *Object = PVOID(PMINIPORTWAVERT(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS)
CXonarWaveRtCaptureMiniport::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTWAVERT Port)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureMiniport::GetDescription(PPCFILTER_DESCRIPTOR* Description)
{
    PAGED_CODE();
    if (Description == nullptr) return STATUS_INVALID_PARAMETER;
    *Description = &m_Filter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureMiniport::GetDeviceDescription(PDEVICE_DESCRIPTION DeviceDescription)
{
    PAGED_CODE();
    if (DeviceDescription == nullptr) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(DeviceDescription, sizeof(DEVICE_DESCRIPTION));
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = PCIBus;
    DeviceDescription->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureMiniport::DataRangeIntersection(ULONG PinId, PKSDATARANGE DataRange,
                                                                           PKSDATARANGE MatchingDataRange,
                                                                           ULONG OutputBufferLength,
                                                                           PVOID ResultantFormat,
                                                                           PULONG ResultantFormatLength)
{
    PAGED_CODE();
    if (PinId != XONARSE_WAVE_CAPTURE_PIN_HOST) return STATUS_NOT_IMPLEMENTED;
    if (DataRange == nullptr || MatchingDataRange == nullptr || ResultantFormatLength == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (MatchingDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO)) return STATUS_NOT_IMPLEMENTED;
    if (!GuidIsOrWildcard(DataRange->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        !GuidIsOrWildcard(DataRange->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
        !GuidIsOrWildcard(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        return STATUS_NO_MATCH;
    }

    const auto* ours = reinterpret_cast<const KSDATARANGE_AUDIO*>(MatchingDataRange);
    const ULONG channels = ours->MaximumChannels;
    const ULONG bits = ours->MaximumBitsPerSample;
    const ULONG rate = ours->MaximumSampleFrequency;

    if (IsEqualGUIDAligned(DataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX) &&
        DataRange->FormatSize >= sizeof(KSDATARANGE_AUDIO)) {
        const auto* client = reinterpret_cast<const KSDATARANGE_AUDIO*>(DataRange);
        if (client->MaximumChannels != static_cast<ULONG>(-1) && client->MaximumChannels < channels) return STATUS_NO_MATCH;
        if (bits < client->MinimumBitsPerSample || bits > client->MaximumBitsPerSample) return STATUS_NO_MATCH;
        if (rate < client->MinimumSampleFrequency || rate > client->MaximumSampleFrequency) return STATUS_NO_MATCH;
    }

    const xonar::hw::AltFormat* alt = xonar::hw::FindAltFormat(m_Hw->Interface, static_cast<UCHAR>(channels),
                                                               static_cast<UCHAR>(bits));
    if (alt == nullptr) return STATUS_NO_MATCH;

    constexpr ULONG size = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    if (OutputBufferLength == 0) {
        *ResultantFormatLength = size;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (OutputBufferLength < size || ResultantFormat == nullptr) return STATUS_BUFFER_TOO_SMALL;
    xonar::wave::BuildFormat(*alt, rate, static_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(ResultantFormat));
    *ResultantFormatLength = size;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureMiniport::NewStream(PMINIPORTWAVERTSTREAM* Stream,
                                                               PPORTWAVERTSTREAM PortStream, ULONG Pin, BOOLEAN Capture,
                                                               PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    if (Stream == nullptr) return STATUS_INVALID_PARAMETER;
    *Stream = nullptr;
    if (PortStream == nullptr || DataFormat == nullptr) return STATUS_INVALID_PARAMETER;
    // Endpoint de capture : PortCls ouvre la broche hôte avec Capture = TRUE.
    if (Pin != XONARSE_WAVE_CAPTURE_PIN_HOST || !Capture) return STATUS_INVALID_PARAMETER;

    xonar::wave::StreamFormat format;
    NTSTATUS status = xonar::wave::ValidateFormat(*m_Hw, DataFormat, DataFormat->FormatSize, &format);
    if (!NT_SUCCESS(status)) {
        XONARSE_WARN("capture: endpoint %lu, format refusé", static_cast<ULONG>(m_Endpoint));
        return status;
    }
    if (InterlockedCompareExchange(&m_StreamOpen, 1, 0) != 0) {
        XONARSE_WARN("capture: endpoint %lu, flux déjà ouvert", static_cast<ULONG>(m_Endpoint));
        return STATUS_DEVICE_BUSY;
    }

    auto* stream = new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) CXonarWaveRtCaptureStream(this, PortStream, format);
    if (stream == nullptr) {
        InterlockedExchange(&m_StreamOpen, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    stream->AddRef();
    *Stream = PMINIPORTWAVERTSTREAM(stream);
    XONARSE_INFO("capture: endpoint %lu, flux %u canaux %u bits %lu Hz masque 0x%lX (alt %u)",
                 static_cast<ULONG>(m_Endpoint), format.Channels, format.BitsPerSample, format.SampleRate,
                 format.ChannelMask, format.Alt->AltSetting);
    return STATUS_SUCCESS;
}

void CXonarWaveRtCaptureMiniport::OnStreamClosed()
{
    PAGED_CODE();
    InterlockedExchange(&m_StreamOpen, 0);
}

NTSTATUS CXonarWaveRtCaptureMiniport::PropertyProposedFormat(PPCPROPERTY_REQUEST Request)
{
    PAGED_CODE();
    if (Request->Instance == nullptr ||
        Request->InstanceSize < sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)) {
        return STATUS_INVALID_PARAMETER;
    }
    const KSP_PIN* pin = CONTAINING_RECORD(Request->Instance, KSP_PIN, PinId);
    if (pin->PinId != XONARSE_WAVE_CAPTURE_PIN_HOST) return STATUS_NOT_SUPPORTED;

    constexpr ULONG access = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        if (Request->ValueSize == 0) {
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            return STATUS_BUFFER_OVERFLOW;
        }
        if (Request->Value == nullptr) return STATUS_INVALID_PARAMETER;
        if (Request->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
            auto* d = static_cast<PKSPROPERTY_DESCRIPTION>(Request->Value);
            d->AccessFlags = access;
            d->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
            d->PropTypeSet.Set = KSPROPTYPESETID_General;
            d->PropTypeSet.Id = kVtIllegal;
            d->PropTypeSet.Flags = 0;
            d->MembersListCount = 0;
            d->Reserved = 0;
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            return STATUS_SUCCESS;
        }
        if (Request->ValueSize >= sizeof(ULONG)) {
            *static_cast<PULONG>(Request->Value) = access;
            Request->ValueSize = sizeof(ULONG);
            return STATUS_SUCCESS;
        }
        Request->ValueSize = 0;
        return STATUS_BUFFER_TOO_SMALL;
    }

    if (Request->Verb & KSPROPERTY_TYPE_SET) {
        if (Request->Value == nullptr || Request->ValueSize < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
            return STATUS_INVALID_PARAMETER;
        }
        xonar::wave::StreamFormat format;
        return xonar::wave::ValidateFormat(*m_Hw, static_cast<const KSDATAFORMAT*>(Request->Value), Request->ValueSize,
                                           &format);
    }

    if (Request->Verb & KSPROPERTY_TYPE_GET) {
        constexpr ULONG size = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
        if (Request->ValueSize == 0) {
            Request->ValueSize = size;
            return STATUS_BUFFER_OVERFLOW;
        }
        if (Request->ValueSize < size || Request->Value == nullptr) return STATUS_BUFFER_TOO_SMALL;
        const NTSTATUS status =
            xonar::wave::BuildDefaultFormat(*m_Hw, static_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(Request->Value));
        if (NT_SUCCESS(status)) Request->ValueSize = size;
        return status;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

// =======================================================================================
//  Flux de capture
// =======================================================================================

CXonarWaveRtCaptureStream::CXonarWaveRtCaptureStream(CXonarWaveRtCaptureMiniport* Miniport,
                                                     PPORTWAVERTSTREAM PortStream,
                                                     const xonar::wave::StreamFormat& Format)
    : CUnknown(nullptr), m_Miniport(Miniport), m_PortStream(PortStream), m_Format(Format)
{
    PAGED_CODE();
    m_Miniport->AddRef();
}

CXonarWaveRtCaptureStream::~CXonarWaveRtCaptureStream()
{
    PAGED_CODE();
    if (m_State == KSSTATE_RUN) m_Engine.Stop();
    if (m_State != KSSTATE_STOP) {
        ReleaseHardware();
        m_State = KSSTATE_STOP;
    }
    if (m_BufferMdl != nullptr) FreeBufferCommon(m_BufferMdl);
    m_Miniport->OnStreamClosed();
    m_Miniport->Release();
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::NonDelegatingQueryInterface(REFIID Interface, PVOID* Object)
{
    PAGED_CODE();
    if (Object == nullptr) return STATUS_INVALID_PARAMETER;
    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(PMINIPORTWAVERTSTREAM(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStream)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportWaveRTStreamNotification)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAMNOTIFICATION(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::SetFormat(PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DataFormat);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS CXonarWaveRtCaptureStream::ProgramClock()
{
    PAGED_CODE();
    XonarUsbDevice* usb = m_Miniport->Usb();
    const XONARSE_ENDPOINT_HW& hw = m_Miniport->Hw();
    if (usb == nullptr) return STATUS_DEVICE_NOT_READY;

    NTSTATUS status = usb->SetSampleRate(hw.ClockId, m_Format.SampleRate);
    ULONG actual = 0;
    const NTSTATUS getStatus = usb->GetSampleRate(hw.ClockId, &actual);
    if (!NT_SUCCESS(status)) {
        if (NT_SUCCESS(getStatus) && actual == m_Format.SampleRate) {
            XONARSE_WARN("capture: SetSampleRate(%u) 0x%08X mais l'horloge est déjà à %lu Hz", hw.ClockId, status, actual);
        } else {
            XONARSE_ERROR("capture: SetSampleRate(horloge %u, %lu Hz) : 0x%08X", hw.ClockId, m_Format.SampleRate, status);
            return status;
        }
    } else if (NT_SUCCESS(getStatus) && actual != m_Format.SampleRate) {
        XONARSE_WARN("capture: horloge %u à %lu Hz au lieu de %lu Hz", hw.ClockId, actual, m_Format.SampleRate);
    }
    return STATUS_SUCCESS;
}

void CXonarWaveRtCaptureStream::ProgramSelector()
{
    PAGED_CODE();
    XonarUsbDevice* usb = m_Miniport->Usb();
    const XONARSE_ENDPOINT_HW& hw = m_Miniport->Hw();
    if (usb == nullptr) return;
    // Endpoint sans sélecteur (ex. micro 2 sur l'interface 5 dédiée) : la source est câblée,
    // rien à programmer.
    if (hw.SelectorId == 0) return;

    // UAC2 SET CUR sur le sélecteur d'entrée hw.SelectorId (SU20) : active l'entrée hw.SelectorInput.
    //   micro 1 -> entrée 1 (FU15 <- IT4) ; ligne -> entrée 2 (FU16 <- IT5).
    // ATTENTION : SU20 n'alimente qu'un seul endpoint ISO IN (0x88) partagé entre micro 1, ligne et
    // mixage. Programmer le sélecteur bascule la source UNIQUE : micro 1 et ligne sont donc mutuellement
    // exclusifs (ouvrir la ligne pendant une capture micro bascule SU20 sur l'entrée 2).
    // Requête de classe vers l'interface AudioControl (recipient interface, OUT, 1 octet).
    struct SelReq {
        struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST Urb;
        UCHAR Value;
    };
    auto* req = static_cast<SelReq*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(SelReq), XONARSE_POOL_TAG));
    if (req == nullptr) return;
    req->Value = hw.SelectorInput; // entrée à activer sur le sélecteur
    req->Urb.Hdr.Length = sizeof(req->Urb);
    req->Urb.Hdr.Function = URB_FUNCTION_CLASS_INTERFACE;
    req->Urb.TransferFlags = USBD_TRANSFER_DIRECTION_OUT; // hôte -> périphérique
    req->Urb.TransferBufferLength = 1;
    req->Urb.TransferBuffer = &req->Value;
    req->Urb.TransferBufferMDL = nullptr;
    req->Urb.RequestTypeReservedBits = 0;
    req->Urb.Request = xonar::uac2::kRequestCur;                                     // 0x01
    req->Urb.Value = static_cast<USHORT>(0x01 << 8);                                 // CS = Selector Control, CN = 0
    req->Urb.Index = static_cast<USHORT>((hw.SelectorId << 8) | xonar::hw::kIfControl); // (SU<<8)|0
    const NTSTATUS status = usb->SubmitUrbSync(reinterpret_cast<PURB>(&req->Urb), 1000);
    if (!NT_SUCCESS(status)) {
        XONARSE_WARN("capture: sélection de l'entrée (SU%u=%u) échouée : 0x%08X (source par défaut conservée)",
                     hw.SelectorId, hw.SelectorInput, status);
    } else {
        XONARSE_INFO("capture: source = sélecteur SU%u entrée %u", hw.SelectorId, hw.SelectorInput);
    }
    ExFreePoolWithTag(req, XONARSE_POOL_TAG);
}

NTSTATUS CXonarWaveRtCaptureStream::SelectAltAndPrepare()
{
    PAGED_CODE();
    XonarUsbDevice* usb = m_Miniport->Usb();
    const XONARSE_ENDPOINT_HW& hw = m_Miniport->Hw();
    const xonar::hw::AltFormat& alt = *m_Format.Alt;
    if (usb == nullptr) return STATUS_DEVICE_NOT_READY;
    NT_ASSERT(!m_Engine.IsPrepared());

    RtlZeroMemory(&m_Pipes, sizeof(m_Pipes));
    NTSTATUS status = usb->SelectAltSetting(hw.Interface, alt.AltSetting, &m_Pipes);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("capture: SelectAltSetting(%u, %u) : 0x%08X", hw.Interface, alt.AltSetting, status);
        return status;
    }
    if (!m_AltSelected) {
        m_AltSelected = true;
        (void)usb->StreamOpened();
    }
    if (m_Pipes.DataPipe == nullptr) {
        XONARSE_ERROR("capture: altsetting %u sans pipe de données", alt.AltSetting);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (m_Pipes.DataEndpoint != alt.DataEndpoint) {
        XONARSE_WARN("capture: pipe de données 0x%02X (attendu 0x%02X)", m_Pipes.DataEndpoint, alt.DataEndpoint);
    }

    XonarIsoInConfig config = {};
    config.SampleRate = m_Format.SampleRate;
    config.BlockAlign = m_Format.BlockAlign;
    config.MaxPacketBytes = alt.MaxPacketBytes;
    if (m_Pipes.DataMaxPacketBytes != 0 && m_Pipes.DataMaxPacketBytes < config.MaxPacketBytes) {
        config.MaxPacketBytes = m_Pipes.DataMaxPacketBytes;
    }
    config.DataPipe = m_Pipes.DataPipe;
    config.DataInterval = m_Pipes.DataInterval;
    return m_Engine.Prepare(usb, config);
}

// Exclusion de l'interface physique partagee : micro 1 et ligne partagent l'interface 4
// (UN seul endpoint ISO IN 0x88 + le selecteur SU20). Un bit par interface empeche deux
// flux d'accaparer la meme interface simultanement : sinon le second reprogramme le
// selecteur et reselectionne l'altsetting, ce qui casse silencieusement le flux en cours.
// Le micro 2 (interface 5, endpoint 0x89 dedie) a son propre bit et reste independant.
// Etat global process (audiodg) : la carte Xonar est unique sur ce poste.
static volatile LONG g_captureIfaceBusy = 0;

NTSTATUS CXonarWaveRtCaptureStream::AcquireHardware()
{
    PAGED_CODE();
    const LONG ifaceBit = 1L << (m_Miniport->Hw().Interface & 31);
    if ((InterlockedOr(&g_captureIfaceBusy, ifaceBit) & ifaceBit) != 0) {
        XONARSE_WARN("capture: interface %u deja en capture (exclusion micro/ligne) : flux refuse",
                     m_Miniport->Hw().Interface);
        return STATUS_DEVICE_BUSY;
    }
    m_ifaceAcquired = true;

    NTSTATUS status = ProgramClock();
    if (!NT_SUCCESS(status)) {
        ReleaseHardware(); // libere le bit d'interface
        return status;
    }
    ProgramSelector(); // non fatal
    status = SelectAltAndPrepare();
    if (!NT_SUCCESS(status)) {
        ReleaseHardware();
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CXonarWaveRtCaptureStream::QueryCurrentAltSetting(UCHAR* AltSetting)
{
    PAGED_CODE();
    *AltSetting = 0;
    XonarUsbDevice* usb = m_Miniport->Usb();
    if (usb == nullptr) return STATUS_DEVICE_NOT_READY;

    struct GetInterfaceRequest {
        struct _URB_CONTROL_GET_INTERFACE_REQUEST Urb;
        UCHAR Value;
    };
    auto* req = static_cast<GetInterfaceRequest*>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(GetInterfaceRequest), XONARSE_POOL_TAG));
    if (req == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    req->Urb.Hdr.Length = sizeof(req->Urb);
    req->Urb.Hdr.Function = URB_FUNCTION_GET_INTERFACE;
    req->Urb.TransferBufferLength = 1;
    req->Urb.TransferBuffer = &req->Value;
    req->Urb.Interface = m_Miniport->Hw().Interface;
    NTSTATUS status = usb->SubmitUrbSync(reinterpret_cast<PURB>(&req->Urb), 1000);
    if (NT_SUCCESS(status) && req->Urb.TransferBufferLength < 1) status = STATUS_DEVICE_PROTOCOL_ERROR;
    if (NT_SUCCESS(status)) *AltSetting = req->Value;
    ExFreePoolWithTag(req, XONARSE_POOL_TAG);
    return status;
}

NTSTATUS CXonarWaveRtCaptureStream::PrepareRun()
{
    PAGED_CODE();
    NTSTATUS status = ProgramClock();
    if (!NT_SUCCESS(status)) return status;

    const UCHAR expected = m_Format.Alt->AltSetting;
    bool reselect = !m_Engine.IsPrepared();
    if (!reselect) {
        UCHAR current = 0;
        status = QueryCurrentAltSetting(&current);
        if (!NT_SUCCESS(status)) {
            XONARSE_WARN("capture: GET_INTERFACE(%u) : 0x%08X, altsetting supposé inchangé", m_Miniport->Hw().Interface,
                         status);
        } else if (current != expected) {
            XONARSE_WARN("capture: interface %u en altsetting %u au lieu de %u, resélection",
                         m_Miniport->Hw().Interface, current, expected);
            reselect = true;
        }
    }
    if (reselect) {
        m_Engine.Unprepare();
        ProgramSelector(); // la carte a pu perdre la sélection (veille)
        status = SelectAltAndPrepare();
        if (!NT_SUCCESS(status)) {
            XONARSE_ERROR("capture: resélection de l'altsetting %u : 0x%08X", expected, status);
            m_Engine.Unprepare();
            return status;
        }
    }
    return STATUS_SUCCESS;
}

void CXonarWaveRtCaptureStream::ReleaseHardware()
{
    PAGED_CODE();
    m_Engine.Unprepare();
    if (m_AltSelected) {
        const XONARSE_ENDPOINT_HW& hw = m_Miniport->Hw();
        XonarUsbDevice* usb = m_Miniport->Usb();
        const NTSTATUS status = usb->SelectAltSetting(hw.Interface, 0, nullptr);
        if (!NT_SUCCESS(status)) XONARSE_WARN("capture: retour à l'altsetting 0 : 0x%08X", status);
        m_AltSelected = false;
        (void)usb->StreamClosed();
    }
    RtlZeroMemory(&m_Pipes, sizeof(m_Pipes));
    if (m_ifaceAcquired) {
        const LONG ifaceBit = 1L << (m_Miniport->Hw().Interface & 31);
        InterlockedAnd(&g_captureIfaceBusy, ~ifaceBit);
        m_ifaceAcquired = false;
    }
}

NTSTATUS CXonarWaveRtCaptureStream::Transition(KSSTATE From, KSSTATE To)
{
    PAGED_CODE();
    if (From == KSSTATE_STOP && To == KSSTATE_ACQUIRE) return AcquireHardware();
    if (From == KSSTATE_PAUSE && To == KSSTATE_RUN) {
        if (m_Buffer == nullptr) {
            XONARSE_ERROR("capture: RUN sans tampon cyclique");
            return STATUS_INVALID_DEVICE_STATE;
        }
        const NTSTATUS status = PrepareRun();
        if (!NT_SUCCESS(status)) return status;
        return m_Engine.Start();
    }
    if (From == KSSTATE_RUN && To == KSSTATE_PAUSE) {
        m_Engine.Stop();
        return STATUS_SUCCESS;
    }
    if (From == KSSTATE_ACQUIRE && To == KSSTATE_STOP) {
        ReleaseHardware();
        m_Engine.ResetPosition();
        return STATUS_SUCCESS;
    }
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::SetState(KSSTATE State)
{
    PAGED_CODE();
    if (State < KSSTATE_STOP || State > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    while (m_State != State) {
        const KSSTATE next = static_cast<KSSTATE>(State > m_State ? m_State + 1 : m_State - 1);
        const NTSTATUS status = Transition(m_State, next);
        if (!NT_SUCCESS(status)) {
            XONARSE_ERROR("capture: endpoint %lu, transition %d -> %d : 0x%08X",
                          static_cast<ULONG>(m_Miniport->Endpoint()), m_State, next, status);
            return status;
        }
        m_State = next;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CXonarWaveRtCaptureStream::AllocateBufferCommon(ULONG NotificationCount, ULONG RequestedSize,
                                                         PMDL* AudioBufferMdl, ULONG* ActualSize,
                                                         ULONG* OffsetFromFirstPage, MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    *AudioBufferMdl = nullptr;
    *ActualSize = 0;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    if (m_BufferMdl != nullptr || m_State == KSSTATE_RUN) return STATUS_INVALID_DEVICE_STATE;

    const ULONG block = m_Format.BlockAlign;
    if (block == 0) return STATUS_INVALID_PARAMETER;
    // Avance USB : octets reçus en vol (Prepare a déjà couru à l'ACQUISITION).
    ULONG inFlight = m_Engine.InFlightBytes();
    if (inFlight == 0) inFlight = XonarIsoInEngine::MaxInFlightBytes(m_Format.SampleRate, m_Format.BlockAlign);
    auto roundUp = [block](ULONGLONG Bytes) -> ULONGLONG { return (Bytes + block - 1) / block * block; };
    ULONGLONG wanted;
    if (NotificationCount != 0) {
        ULONGLONG period = roundUp(RequestedSize / NotificationCount);
        if (period == 0) period = block;
        wanted = period * NotificationCount;
        if (wanted < 2ULL * inFlight) {
            // Tampon trop court pour l'avance USB : on garde la période demandée mais on
            // ajoute des périodes jusqu'à couvrir 2x l'avance (le curseur d'écriture ne doit
            // pas rattraper la lecture).
            const ULONGLONG periods = (2ULL * inFlight + period - 1) / period;
            wanted = period * (periods != 0 ? periods : 1);
            XONARSE_INFO("capture: tampon porté à %llu octets (%llu périodes, avance USB %lu)", wanted,
                         wanted / period, inFlight);
        }
    } else {
        wanted = RequestedSize / block * block;
        const ULONGLONG minSize = 2ULL * inFlight;
        if (wanted < minSize) {
            XONARSE_INFO("capture: tampon porté de %llu à %llu octets (%lu octets en vol)", wanted, minSize, inFlight);
            wanted = minSize;
        }
    }
    if (wanted == 0 || wanted > MAXULONG / 2) return STATUS_INVALID_PARAMETER;
    const ULONG size = static_cast<ULONG>(wanted);

    PHYSICAL_ADDRESS high;
    high.HighPart = 0;
    high.LowPart = MAXULONG;
    PMDL mdl = m_PortStream->AllocatePagesForMdl(high, size);
    if (mdl == nullptr) {
        XONARSE_ERROR("capture: AllocatePagesForMdl(%lu) a échoué", size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (MmGetMdlByteCount(mdl) < size) {
        m_PortStream->FreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    PUCHAR va = static_cast<PUCHAR>(m_PortStream->MapAllocatedPages(mdl, MmCached));
    if (va == nullptr) {
        m_PortStream->FreePagesFromMdl(mdl);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(va, size);

    m_BufferMdl = mdl;
    m_Buffer = va;
    m_BufferSize = size;
    m_NotificationCount = NotificationCount;
    m_Engine.SetAudioBuffer(va, size, NotificationCount);

    *AudioBufferMdl = mdl;
    *ActualSize = size;
    XONARSE_INFO("capture: tampon cyclique %lu octets (%lu notifications)", size, NotificationCount);
    return STATUS_SUCCESS;
}

void CXonarWaveRtCaptureStream::FreeBufferCommon(PMDL AudioBufferMdl)
{
    PAGED_CODE();
    if (m_BufferMdl == nullptr) return;
    if (AudioBufferMdl != nullptr && AudioBufferMdl != m_BufferMdl) {
        XONARSE_WARN("capture: FreeAudioBuffer avec un MDL inconnu");
    }
    m_Engine.SetAudioBuffer(nullptr, 0, 0);
    m_PortStream->UnmapAllocatedPages(m_Buffer, m_BufferMdl);
    m_PortStream->FreePagesFromMdl(m_BufferMdl);
    m_BufferMdl = nullptr;
    m_Buffer = nullptr;
    m_BufferSize = 0;
    m_NotificationCount = 0;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::AllocateAudioBuffer(ULONG RequestedSize, PMDL* AudioBufferMdl,
                                                                       ULONG* ActualSize, ULONG* OffsetFromFirstPage,
                                                                       MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    return AllocateBufferCommon(0, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(VOID) CXonarWaveRtCaptureStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(BufferSize);
    FreeBufferCommon(AudioBufferMdl);
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::AllocateBufferWithNotification(ULONG NotificationCount,
                                                                                 ULONG RequestedSize,
                                                                                 PMDL* AudioBufferMdl, ULONG* ActualSize,
                                                                                 ULONG* OffsetFromFirstPage,
                                                                                 MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    if (NotificationCount == 0) return STATUS_INVALID_PARAMETER;
    return AllocateBufferCommon(NotificationCount, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage,
                                CacheType);
}

STDMETHODIMP_(VOID) CXonarWaveRtCaptureStream::FreeBufferWithNotification(PMDL AudioBufferMdl, ULONG BufferSize)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(BufferSize);
    FreeBufferCommon(AudioBufferMdl);
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::GetPositionRegister(KSRTAUDIO_HWREGISTER* Register)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::GetClockRegister(KSRTAUDIO_HWREGISTER* Register)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

#pragma code_seg()

// --- Non paginé : peut être appelé à DISPATCH_LEVEL et prend le verrou du moteur ---

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::GetPosition(PKSAUDIO_POSITION Position)
{
    if (Position == nullptr) return STATUS_INVALID_PARAMETER;
    ULONG writePos = 0;
    m_Engine.GetPosition(&writePos, nullptr);
    // Capture : PlayOffset = position juste après le dernier échantillon capturé (curseur
    // d'écriture) ; le moteur audio lit en deçà. Pas de registre matériel séparé.
    Position->PlayOffset = writePos;
    Position->WriteOffset = writePos;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CXonarWaveRtCaptureStream::GetHWLatency(KSRTAUDIO_HWLATENCY* Latency)
{
    if (Latency == nullptr) return;
    Latency->FifoSize = m_Engine.InFlightBytes();
    Latency->ChipsetDelay = 0;
    Latency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::RegisterNotificationEvent(PKEVENT NotificationEvent)
{
    return m_Engine.RegisterNotificationEvent(NotificationEvent);
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtCaptureStream::UnregisterNotificationEvent(PKEVENT NotificationEvent)
{
    return m_Engine.UnregisterNotificationEvent(NotificationEvent);
}
