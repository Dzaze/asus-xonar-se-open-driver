// Miniport WaveRT de rendu (voir minwavert.h).
//
// Filtre (convention SYSVAD, licence MIT, Copyright (c) Microsoft Corporation) :
//   broche 0 (XONARSE_WAVE_PIN_SYSTEM) : flux, KSPIN_DATAFLOW_IN, COMMUNICATION_SINK,
//            KSCATEGORY_AUDIO, 1 instance, plages PCM exactes ;
//   broche 1 (XONARSE_WAVE_PIN_BRIDGE) : pont vers la topologie, DATAFLOW_OUT,
//            COMMUNICATION_NONE, plage analogique ;
//   aucune unité, une connexion directe broche 0 -> broche 1.
#include "minwavertstream.h"

namespace {

// Ordre de préférence des fréquences dans les plages (la première est proposée en
// premier par l'intersection quand le client ne contraint rien).
// Ordre de préférence des fréquences confirmées par RANGE (27/10/2026).
constexpr ULONG kRateOrder[] = {48000, 44100, 96000, 192000};
static_assert(ARRAYSIZE(kRateOrder) == XONAR_WAVE_RATE_COUNT, "kRateOrder doit couvrir kCandidateRates");

// Valeur VT_ILLEGAL de wtypes.h (non exposée aux en-têtes noyau).
constexpr ULONG kVtIllegal = 0xFFFF;

KSDATARANGE g_BridgeRange = {
    sizeof(KSDATARANGE), 0, 0, 0,
    STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
    STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),
    STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE),
};

const GUID g_FilterCategories[] = {
    STATICGUIDOF(KSCATEGORY_AUDIO),
    STATICGUIDOF(KSCATEGORY_RENDER),
    STATICGUIDOF(KSCATEGORY_REALTIME),
};

NTSTATUS PropertyHandlerWaveFilter(_In_ PPCPROPERTY_REQUEST Request);

const PCPROPERTY_ITEM g_FilterProperties[] = {
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT,
     PCPROPERTY_ITEM_FLAG_GET | PCPROPERTY_ITEM_FLAG_SET | PCPROPERTY_ITEM_FLAG_BASICSUPPORT, PropertyHandlerWaveFilter},
};

DEFINE_PCAUTOMATION_TABLE_PROP(g_FilterAutomation, g_FilterProperties);

bool GuidIsOrWildcard(const GUID& Value, const GUID& Expected)
{
    return IsEqualGUIDAligned(Value, Expected) || IsEqualGUIDAligned(Value, GUID_NULL);
}

} // namespace

#pragma code_seg("PAGE")

namespace {

NTSTATUS PropertyHandlerWaveFilter(PPCPROPERTY_REQUEST Request)
{
    PAGED_CODE();
    if (Request == nullptr || Request->MajorTarget == nullptr || Request->PropertyItem == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!IsEqualGUIDAligned(*Request->PropertyItem->Set, KSPROPSETID_Pin) ||
        Request->PropertyItem->Id != KSPROPERTY_PIN_PROPOSEDATAFORMAT) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }
    // MajorTarget = IUnknown du miniport, c'est-à-dire son sous-objet IMiniportWaveRT
    // (voir NonDelegatingQueryInterface).
    auto* miniport = static_cast<CXonarWaveRtMiniport*>(static_cast<PMINIPORTWAVERT>(Request->MajorTarget));
    return miniport->PropertyProposedFormat(Request);
}

} // namespace

NTSTATUS CreateXonarWaveRtMiniport(_Out_ PUNKNOWN* Unknown, _In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint)
{
    PAGED_CODE();
    if (Unknown == nullptr) return STATUS_INVALID_PARAMETER;
    *Unknown = nullptr;
    if (Usb == nullptr || Endpoint >= XonarEndpointCount) return STATUS_INVALID_PARAMETER;

    // Endpoint de capture : miniport dédié (moteur ISO IN, broches inversées).
    if (XonarSeEndpointHw(Endpoint).IsCapture) {
        return CreateXonarWaveRtCaptureMiniport(Unknown, Usb, Endpoint);
    }

    auto* miniport = new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) CXonarWaveRtMiniport(Usb, Endpoint);
    if (miniport == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    if (miniport->StreamRangeCount() == 0) {
        XONARSE_ERROR("wave: endpoint %lu sans altsetting PCM", static_cast<ULONG>(Endpoint));
        delete miniport;
        return STATUS_NOT_SUPPORTED;
    }
    miniport->AddRef(); // CUnknown démarre à 0
    *Unknown = PUNKNOWN(PMINIPORTWAVERT(miniport));
    XONARSE_INFO("wave: miniport WaveRT créé pour l'endpoint %lu (%lu plages)", static_cast<ULONG>(Endpoint),
                 miniport->StreamRangeCount());
    return STATUS_SUCCESS;
}

CXonarWaveRtMiniport::CXonarWaveRtMiniport(XonarUsbDevice* Usb, XONARSE_ENDPOINT Endpoint)
    : CUnknown(nullptr), m_Usb(Usb), m_Endpoint(Endpoint), m_Hw(&XonarSeEndpointHw(Endpoint))
{
    PAGED_CODE();
    m_Usb->AddRef();
    BuildDescriptors();
}

CXonarWaveRtMiniport::~CXonarWaveRtMiniport()
{
    PAGED_CODE();
    NT_ASSERT(m_StreamOpen == 0);
    if (m_Usb != nullptr) m_Usb->Release();
}

void CXonarWaveRtMiniport::BuildDescriptors()
{
    PAGED_CODE();
    // Altsettings PCM de l'interface, du plus riche au plus simple (canaux puis bits).
    const xonar::hw::AltFormat* alts[ARRAYSIZE(xonar::hw::kAltFormats)] = {};
    ULONG altCount = 0;
    for (const auto& f : xonar::hw::kAltFormats) {
        if (f.Interface != m_Hw->Interface || f.Iec61937 || f.FeedbackEndpoint == 0) continue;
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
            if (m_StreamRangeCount >= XONAR_WAVE_MAX_RANGES) break;
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
    m_BridgeRanges[0] = &g_BridgeRange;

    // Broche de flux (entrée du filtre, côté moteur audio).
    PCPIN_DESCRIPTOR& sys = m_Pins[XONARSE_WAVE_PIN_SYSTEM];
    sys.MaxGlobalInstanceCount = 1;
    sys.MaxFilterInstanceCount = 1;
    sys.MinFilterInstanceCount = 0;
    sys.AutomationTable = nullptr;
    sys.KsPinDescriptor.InterfacesCount = 0;
    sys.KsPinDescriptor.Interfaces = nullptr;
    sys.KsPinDescriptor.MediumsCount = 0;
    sys.KsPinDescriptor.Mediums = nullptr;
    sys.KsPinDescriptor.DataRangesCount = m_StreamRangeCount;
    sys.KsPinDescriptor.DataRanges = m_StreamRanges;
    sys.KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_IN;
    sys.KsPinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
    sys.KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    sys.KsPinDescriptor.Name = nullptr;

    // Broche pont (sortie analogique vers le filtre Topology).
    PCPIN_DESCRIPTOR& bridge = m_Pins[XONARSE_WAVE_PIN_BRIDGE];
    bridge.MaxGlobalInstanceCount = 0;
    bridge.MaxFilterInstanceCount = 0;
    bridge.MinFilterInstanceCount = 0;
    bridge.AutomationTable = nullptr;
    bridge.KsPinDescriptor.DataRangesCount = ARRAYSIZE(m_BridgeRanges);
    bridge.KsPinDescriptor.DataRanges = m_BridgeRanges;
    bridge.KsPinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
    bridge.KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    bridge.KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    bridge.KsPinDescriptor.Name = nullptr;

    m_Connections[0].FromNode = PCFILTER_NODE;
    m_Connections[0].FromNodePin = XONARSE_WAVE_PIN_SYSTEM;
    m_Connections[0].ToNode = PCFILTER_NODE;
    m_Connections[0].ToNodePin = XONARSE_WAVE_PIN_BRIDGE;

    m_Filter.Version = 0;
    m_Filter.AutomationTable = &g_FilterAutomation;
    m_Filter.PinSize = sizeof(PCPIN_DESCRIPTOR);
    m_Filter.PinCount = ARRAYSIZE(m_Pins);
    m_Filter.Pins = m_Pins;
    m_Filter.NodeSize = sizeof(PCNODE_DESCRIPTOR);
    m_Filter.NodeCount = 0;
    m_Filter.Nodes = nullptr;
    m_Filter.ConnectionCount = ARRAYSIZE(m_Connections);
    m_Filter.Connections = m_Connections;
    m_Filter.CategoryCount = ARRAYSIZE(g_FilterCategories);
    m_Filter.Categories = g_FilterCategories;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtMiniport::NonDelegatingQueryInterface(REFIID Interface, PVOID* Object)
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

STDMETHODIMP_(NTSTATUS) CXonarWaveRtMiniport::Init(PUNKNOWN UnknownAdapter, PRESOURCELIST ResourceList, PPORTWAVERT Port)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    // Le port n'est pas conservé : aucun service du port n'est nécessaire (pas de DMA,
    // pas d'événement KS), et ne pas le référencer évite un cycle port <-> miniport.
    UNREFERENCED_PARAMETER(Port);
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtMiniport::GetDescription(PPCFILTER_DESCRIPTOR* Description)
{
    PAGED_CODE();
    if (Description == nullptr) return STATUS_INVALID_PARAMETER;
    *Description = &m_Filter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtMiniport::GetDeviceDescription(PDEVICE_DESCRIPTION DeviceDescription)
{
    PAGED_CODE();
    if (DeviceDescription == nullptr) return STATUS_INVALID_PARAMETER;
    // Valeurs de SYSVAD : aucun DMA réel, le port ne s'en sert pas pour ce miniport.
    RtlZeroMemory(DeviceDescription, sizeof(DEVICE_DESCRIPTION));
    DeviceDescription->Master = TRUE;
    DeviceDescription->ScatterGather = TRUE;
    DeviceDescription->Dma32BitAddresses = TRUE;
    DeviceDescription->InterfaceType = PCIBus;
    DeviceDescription->MaximumLength = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

// Intersection : nos plages sont exactes (canaux, bits, fréquence uniques), le résultat
// est donc le format de la plage si la plage du client l'autorise, toujours en
// WAVEFORMATEXTENSIBLE avec le masque de l'altsetting.
STDMETHODIMP_(NTSTATUS) CXonarWaveRtMiniport::DataRangeIntersection(ULONG PinId, PKSDATARANGE DataRange,
                                                                    PKSDATARANGE MatchingDataRange,
                                                                    ULONG OutputBufferLength, PVOID ResultantFormat,
                                                                    PULONG ResultantFormatLength)
{
    PAGED_CODE();
    if (PinId != XONARSE_WAVE_PIN_SYSTEM) return STATUS_NOT_IMPLEMENTED; // pont : traitement par défaut de PortCls
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

STDMETHODIMP_(NTSTATUS) CXonarWaveRtMiniport::NewStream(PMINIPORTWAVERTSTREAM* Stream, PPORTWAVERTSTREAM PortStream,
                                                        ULONG Pin, BOOLEAN Capture, PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    if (Stream == nullptr) return STATUS_INVALID_PARAMETER;
    *Stream = nullptr;
    if (PortStream == nullptr || DataFormat == nullptr) return STATUS_INVALID_PARAMETER;
    if (Pin != XONARSE_WAVE_PIN_SYSTEM || Capture) return STATUS_INVALID_PARAMETER;

    xonar::wave::StreamFormat format;
    NTSTATUS status = xonar::wave::ValidateFormat(*m_Hw, DataFormat, DataFormat->FormatSize, &format);
    if (!NT_SUCCESS(status)) {
        XONARSE_WARN("wave: endpoint %lu, format refusé", static_cast<ULONG>(m_Endpoint));
        return status;
    }
    if (InterlockedCompareExchange(&m_StreamOpen, 1, 0) != 0) {
        XONARSE_WARN("wave: endpoint %lu, flux déjà ouvert", static_cast<ULONG>(m_Endpoint));
        return STATUS_DEVICE_BUSY;
    }

    auto* stream = new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) CXonarWaveRtStream(this, PortStream, format);
    if (stream == nullptr) {
        InterlockedExchange(&m_StreamOpen, 0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    stream->AddRef();
    *Stream = PMINIPORTWAVERTSTREAM(stream);
    XONARSE_INFO("wave: endpoint %lu, flux %u canaux %u bits %lu Hz masque 0x%lX (alt %u)",
                 static_cast<ULONG>(m_Endpoint), format.Channels, format.BitsPerSample, format.SampleRate,
                 format.ChannelMask, format.Alt->AltSetting);
    return STATUS_SUCCESS;
}

void CXonarWaveRtMiniport::OnStreamClosed()
{
    PAGED_CODE();
    InterlockedExchange(&m_StreamOpen, 0);
}

NTSTATUS CXonarWaveRtMiniport::PropertyProposedFormat(PPCPROPERTY_REQUEST Request)
{
    PAGED_CODE();
    // Instance = KSP_PIN privé de son en-tête KSPROPERTY (même lecture que SYSVAD).
    if (Request->Instance == nullptr ||
        Request->InstanceSize < sizeof(KSP_PIN) - RTL_SIZEOF_THROUGH_FIELD(KSP_PIN, Property)) {
        return STATUS_INVALID_PARAMETER;
    }
    const KSP_PIN* pin = CONTAINING_RECORD(Request->Instance, KSP_PIN, PinId);
    if (pin->PinId != XONARSE_WAVE_PIN_SYSTEM) return STATUS_NOT_SUPPORTED;

    constexpr ULONG access = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT;
    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        // Requête de taille : on annonce la description complète.
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
        // Format par défaut de l'endpoint (identique à celui que l'INF impose).
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

#pragma code_seg()
