// Miniport Topology de xonarse.sys (voir xonar_topology.h).
//
// Volume : KSPROPERTY_AUDIO_VOLUMELEVEL par canal ; canal KS n (0-based) = canal UAC2 n+1
//          de la Feature Unit ; KS (1/65536 dB) = UAC2 (1/256 dB) x 256.
// Mute   : KSPROPERTY_AUDIO_MUTE, appliqué au contrôle maître (canal UAC2 0) quel que soit
//          le canal KS demandé (les FU 12, 13 et 14 n'ont de mute que sur le maître).
// Jacks  : KSPROPERTY_JACK_DESCRIPTION et DESCRIPTION2 sur la broche 1. Par défaut
//          (HonorJackDetection = 0) les jacks sont TOUJOURS signalés connectés et la détection
//          de présence n'est pas annoncée : avec le pilote ASUS, la détection de jack rendait
//          les haut-parleurs « Non connecté » et inutilisables (docs/analyse-paquet-pilote-asus.md
//          §6). L'état réel du connecteur est seulement journalisé.
//
// Les gestionnaires de propriétés KS sont appelés par PortCls à PASSIVE_LEVEL : les requêtes
// UAC2 synchrones y sont permises. Toute erreur USB est tolérée (cache des dernières valeurs
// connues), l'initialisation n'échoue jamais pour une simple lecture impossible.
//
// Passages adaptés de l'échantillon SYSVAD de Microsoft (mintopo.cpp, basetopo.cpp,
// common.cpp ; Windows-driver-samples, licence MIT, Copyright (c) Microsoft Corporation) :
// QueryInterface, gestion des tailles de tampon des descriptions de jack, réponse
// « basic support » multicanal, ajout d'événements à la liste du port.
#include "xonar_topology_tables.h"

using namespace xonar::topo;

namespace {

// Canal KS « maître » (KSNODEPROPERTY_AUDIO_CHANNEL.Channel = -1).
constexpr LONG kKsMasterChannel = -1;
// Valeur UAC2 « -infini » du contrôle de volume.
constexpr SHORT kUacVolumeSilence = SHORT(-32768);
// Plage de repli si RANGE est illisible : -96 dB .. 0 dB, pas de 0,5 dB (en 1/256 dB).
constexpr SHORT kFallbackMin = SHORT(-96 * 256);
constexpr SHORT kFallbackMax = 0;
constexpr USHORT kFallbackRes = 128;
constexpr LONG kKsPerUac = 256;
// Accès complet (équivalent du KSPROPERTY_TYPE_ALL de SYSVAD, absent des en-têtes WDK).
constexpr ULONG kAccessAll = KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET;

// Réponse « basic support » minimale (sans liste de membres), adaptée de SYSVAD
// (PropertyHandler_BasicSupport, licence MIT).
#pragma code_seg("PAGE")
NTSTATUS BasicSupportSimple(_In_ PPCPROPERTY_REQUEST PropertyRequest, ULONG AccessFlags, ULONG Type)
{
    PAGED_CODE();
    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
        auto* desc = static_cast<PKSPROPERTY_DESCRIPTION>(PropertyRequest->Value);
        desc->AccessFlags = AccessFlags;
        desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = Type;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount = 0;
        desc->Reserved = 0;
        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
        *static_cast<PULONG>(PropertyRequest->Value) = AccessFlags;
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->ValueSize == 0) {
        PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        return STATUS_BUFFER_OVERFLOW;
    }
    PropertyRequest->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

// Réponse « basic support » d'une propriété multicanal (volume ou mute) : description,
// en-tête de membres, puis un KSPROPERTY_STEPPING_LONG par canal (voir « Exposing
// Multichannel Nodes » dans la documentation WDK ; forme reprise de SYSVAD,
// PropertyHandler_BasicSupportVolume, licence MIT).
NTSTATUS BasicSupportStepped(_In_ PPCPROPERTY_REQUEST PropertyRequest, ULONG Type, ULONG Channels, ULONG MemberFlags,
                             _In_reads_(Channels) const KSPROPERTY_STEPPING_LONG* Steps)
{
    PAGED_CODE();
    const ULONG full =
        sizeof(KSPROPERTY_DESCRIPTION) + sizeof(KSPROPERTY_MEMBERSHEADER) + Channels * sizeof(KSPROPERTY_STEPPING_LONG);

    if (PropertyRequest->ValueSize >= sizeof(KSPROPERTY_DESCRIPTION)) {
        auto* desc = static_cast<PKSPROPERTY_DESCRIPTION>(PropertyRequest->Value);
        desc->AccessFlags = kAccessAll;
        desc->DescriptionSize = full;
        desc->PropTypeSet.Set = KSPROPTYPESETID_General;
        desc->PropTypeSet.Id = Type;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount = 1;
        desc->Reserved = 0;
        if (PropertyRequest->ValueSize >= full) {
            auto* members = reinterpret_cast<PKSPROPERTY_MEMBERSHEADER>(desc + 1);
            members->MembersFlags = KSPROPERTY_MEMBER_STEPPEDRANGES;
            members->MembersSize = sizeof(KSPROPERTY_STEPPING_LONG);
            members->MembersCount = Channels;
            members->Flags = MemberFlags;
            auto* range = reinterpret_cast<PKSPROPERTY_STEPPING_LONG>(members + 1);
            for (ULONG i = 0; i < Channels; ++i) {
                range[i] = Steps[i];
            }
            PropertyRequest->ValueSize = full;
        } else {
            PropertyRequest->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
        }
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->ValueSize >= sizeof(ULONG)) {
        *static_cast<PULONG>(PropertyRequest->Value) = kAccessAll;
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    if (PropertyRequest->ValueSize == 0) {
        PropertyRequest->ValueSize = full;
        return STATUS_BUFFER_OVERFLOW;
    }
    PropertyRequest->ValueSize = 0;
    return STATUS_BUFFER_TOO_SMALL;
}

// Tampon de valeur trop petit : taille requise rendue si l'appelant interroge (taille 0).
NTSTATUS ValueTooSmall(_In_ PPCPROPERTY_REQUEST PropertyRequest, ULONG Needed)
{
    PAGED_CODE();
    if (PropertyRequest->ValueSize == 0) {
        PropertyRequest->ValueSize = Needed;
        return STATUS_BUFFER_OVERFLOW;
    }
    return STATUS_BUFFER_TOO_SMALL;
}

// Lit le canal KS d'une requête KSNODEPROPERTY_AUDIO_CHANNEL (PortCls place Instance
// juste après l'en-tête KSNODEPROPERTY).
bool ReadChannel(_In_ PPCPROPERTY_REQUEST PropertyRequest, _Out_ LONG* Channel)
{
    PAGED_CODE();
    *Channel = 0;
    if (PropertyRequest->Instance == nullptr || PropertyRequest->InstanceSize < sizeof(LONG)) return false;
    *Channel = *static_cast<const LONG*>(PropertyRequest->Instance);
    return true;
}

ULONG PopCount(ULONG Value)
{
    PAGED_CODE();
    ULONG n = 0;
    for (; Value != 0; Value &= Value - 1) ++n;
    return n;
}
#pragma code_seg()

} // namespace

// ---------------------------------------------------------------------------------------
// Fabrique
// ---------------------------------------------------------------------------------------
#pragma code_seg("PAGE")
NTSTATUS CreateXonarTopologyMiniport(_Out_ PUNKNOWN* Unknown, _In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint)
{
    PAGED_CODE();
    if (Unknown == nullptr) return STATUS_INVALID_PARAMETER;
    *Unknown = nullptr;
    if (Usb == nullptr || Endpoint >= XonarEndpointCount) return STATUS_INVALID_PARAMETER;

    auto* obj = new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) CXonarMiniportTopology(nullptr);
    if (obj == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    obj->Bind(Usb, Endpoint);

    // Référence +1 rendue à l'appelant (adapter.cpp la relâche après IPort::Init).
    obj->AddRef();
    *Unknown = static_cast<PUNKNOWN>(static_cast<IMiniportTopology*>(obj));
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Cycle de vie
// ---------------------------------------------------------------------------------------
void CXonarMiniportTopology::Bind(_In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint)
{
    PAGED_CODE();
    KeInitializeMutex(&m_Lock, 0);
    m_Usb = Usb;
    m_Usb->AddRef();
    m_Endpoint = Endpoint;
    m_Hw = &XonarSeEndpointHw(Endpoint);
    m_Info = &TopoInfo[Endpoint];
    m_Channels = m_Hw->MaxChannels;
    if (m_Channels == 0 || m_Channels > XONARSE_TOPO_MAX_CHANNELS) m_Channels = 2;
    m_ChannelConfig = m_Hw->ChannelMask;
    for (ULONG i = 0; i < XONARSE_TOPO_MAX_CHANNELS; ++i) {
        m_Range[i] = {kFallbackMin, kFallbackMax, kFallbackRes};
        m_VolumeKs[i] = LONG(kFallbackMax) * kKsPerUac;
    }
}

CXonarMiniportTopology::~CXonarMiniportTopology()
{
    PAGED_CODE();
    // Désinscription synchrone : au retour, aucun rappel n'est en cours sur cet objet.
    // Le verrou ne doit pas être tenu ici (le rappel le prend).
    if (m_CallbackRegistered && m_Usb != nullptr) {
        m_Usb->UnregisterInterruptCallback(&CXonarMiniportTopology::InterruptCallback, this);
        m_CallbackRegistered = false;
    }
    // m_PortEvents est une référence faible (voir Init) : rien à relâcher.
    m_PortEvents = nullptr;
    if (m_Usb != nullptr) {
        m_Usb->Release();
        m_Usb = nullptr;
    }
    XONARSE_INFO("Topology %ls détruite", XonarSeTopologyName(m_Endpoint));
}

STDMETHODIMP_(NTSTATUS) CXonarMiniportTopology::NonDelegatingQueryInterface(_In_ REFIID Interface,
                                                                          _COM_Outptr_ PVOID* Object)
{
    PAGED_CODE();
    if (Object == nullptr) return STATUS_INVALID_PARAMETER;

    if (IsEqualGUIDAligned(Interface, IID_IUnknown)) {
        *Object = PVOID(PUNKNOWN(static_cast<IMiniportTopology*>(this)));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniport)) {
        *Object = PVOID(static_cast<PMINIPORT>(this));
    } else if (IsEqualGUIDAligned(Interface, IID_IMiniportTopology)) {
        *Object = PVOID(static_cast<PMINIPORTTOPOLOGY>(this));
    } else {
        *Object = nullptr;
        return STATUS_INVALID_PARAMETER;
    }
    // Référence prise pour l'appelant.
    PUNKNOWN(*Object)->AddRef();
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR* Description)
{
    PAGED_CODE();
    *Description = m_Info->Filter;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(NTSTATUS) CXonarMiniportTopology::DataRangeIntersection(
    _In_ ULONG PinId, _In_ PKSDATARANGE DataRange, _In_ PKSDATARANGE MatchingDataRange, _In_ ULONG OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength) PVOID ResultantFormat,
    _Out_ PULONG ResultantFormatLength)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);
    UNREFERENCED_PARAMETER(MatchingDataRange);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(ResultantFormat);
    // Broches analogiques : l'intersection par défaut de PortCls suffit.
    *ResultantFormatLength = 0;
    return STATUS_NOT_IMPLEMENTED;
}

STDMETHODIMP_(NTSTATUS) CXonarMiniportTopology::Init(_In_ PUNKNOWN UnknownAdapter, _In_ PRESOURCELIST ResourceList,
                                                     _In_ PPORTTOPOLOGY Port)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);
    const PCWSTR name = XonarSeTopologyName(m_Endpoint);

    // Interface d'événements du port : sans elle, le filtre fonctionne mais ne notifie rien.
    if (Port != nullptr) {
        const NTSTATUS qi = Port->QueryInterface(IID_IPortEvents, reinterpret_cast<PVOID*>(&m_PortEvents));
        if (!NT_SUCCESS(qi)) {
            m_PortEvents = nullptr;
            XONARSE_WARN("Topology %ls : IPortEvents indisponible (0x%08X), aucune notification", name, qi);
        } else {
            // Référence FAIBLE : le port détient le miniport et lui survit ; garder une
            // référence forte créerait un cycle port <-> miniport qui fuit si
            // PcRegisterSubdevice échoue après Init (relecture du code, constat 8). Le rappel
            // d'interruption, seul autre utilisateur, est désinscrit dans le destructeur.
            m_PortEvents->Release();
        }
    }

    Lock();
    LoadVolumeRanges();
    for (ULONG ch = 0; ch < m_Channels; ++ch) {
        RefreshVolume(ch);
    }
    RefreshMute();
    RefreshJackState("init");
    Unlock();

    XONARSE_INFO("Topology %ls : FU%u OT%u, %lu canaux, volume %s, mute=%d, HonorJackDetection=%lu", name,
                 m_Hw->FeatureUnitId, m_Hw->TerminalId, m_Channels,
                 m_Info->VolumeNode != XONARSE_TOPO_NO_NODE ? "par canal" : "absent", m_Muted ? 1 : 0,
                 XonarSeSettings().HonorJackDetection);

    // Notifications de l'endpoint d'interruption 0x8F (connecteur, volume, mute).
    const NTSTATUS reg = m_Usb->RegisterInterruptCallback(&CXonarMiniportTopology::InterruptCallback, this);
    if (NT_SUCCESS(reg)) {
        m_CallbackRegistered = true;
    } else {
        XONARSE_WARN("Topology %ls : inscription aux interruptions UAC2 impossible (0x%08X)", name, reg);
    }
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Outils internes (verrou tenu sauf mention)
// ---------------------------------------------------------------------------------------
void CXonarMiniportTopology::Lock()
{
    PAGED_CODE();
    KeWaitForSingleObject(&m_Lock, Executive, KernelMode, FALSE, nullptr);
}

void CXonarMiniportTopology::Unlock()
{
    PAGED_CODE();
    KeReleaseMutex(&m_Lock, FALSE);
}

bool CXonarMiniportTopology::HonorJackDetection() const
{
    PAGED_CODE();
    return XonarSeSettings().HonorJackDetection != 0;
}

void CXonarMiniportTopology::LoadVolumeRanges()
{
    PAGED_CODE();
    if (m_Info->VolumeNode == XONARSE_TOPO_NO_NODE || !m_Hw->HasChannelVolume) return;
    const PCWSTR name = XonarSeTopologyName(m_Endpoint);
    for (ULONG ch = 0; ch < m_Channels; ++ch) {
        XONAR_RANGE16 r = {};
        const NTSTATUS status = m_Usb->GetVolumeRange(m_Hw->FeatureUnitId, static_cast<UCHAR>(ch + 1), &r);
        if (NT_SUCCESS(status) && r.Min < r.Max) {
            if (r.Res == 0) r.Res = 1;
            m_Range[ch] = r;
            XONARSE_INFO("Topology %ls : canal %lu, plage %d..%d pas %u (1/256 dB)", name, ch, r.Min, r.Max, r.Res);
        } else {
            m_Range[ch] = {kFallbackMin, kFallbackMax, kFallbackRes};
            XONARSE_WARN("Topology %ls : canal %lu, RANGE illisible (0x%08X), repli -96..0 dB pas 0,5 dB", name, ch,
                         status);
        }
    }
}

LONG CXonarMiniportTopology::UacToKs(ULONG KsChannel, SHORT Value) const
{
    PAGED_CODE();
    const XONAR_RANGE16& r = m_Range[KsChannel];
    // -infini (ou toute valeur hors plage) est ramené aux bornes annoncées.
    SHORT v = Value;
    if (v == kUacVolumeSilence || v < r.Min) v = r.Min;
    if (v > r.Max) v = r.Max;
    return LONG(v) * kKsPerUac;
}

SHORT CXonarMiniportTopology::KsToUac(ULONG KsChannel, LONG Value) const
{
    PAGED_CODE();
    const XONAR_RANGE16& r = m_Range[KsChannel];
    const LONG minKs = LONG(r.Min) * kKsPerUac;
    const LONG maxKs = LONG(r.Max) * kKsPerUac;
    const LONG stepKs = LONG(r.Res) * kKsPerUac;
    LONG v = Value;
    if (v <= minKs) return r.Min;
    if (v >= maxKs) return r.Max;
    // Arrondi au pas le plus proche à partir du minimum.
    const LONG steps = (v - minKs + stepKs / 2) / stepKs;
    v = minKs + steps * stepKs;
    if (v > maxKs) v = maxKs;
    return static_cast<SHORT>(v / kKsPerUac);
}

void CXonarMiniportTopology::RefreshVolume(ULONG KsChannel)
{
    PAGED_CODE();
    if (m_Info->VolumeNode == XONARSE_TOPO_NO_NODE || KsChannel >= m_Channels) return;
    SHORT value = 0;
    if (NT_SUCCESS(m_Usb->GetVolume(m_Hw->FeatureUnitId, static_cast<UCHAR>(KsChannel + 1), &value))) {
        m_VolumeKs[KsChannel] = UacToKs(KsChannel, value);
    }
    // Échec : on garde la dernière valeur connue.
}

void CXonarMiniportTopology::RefreshMute()
{
    PAGED_CODE();
    bool muted = false;
    if (NT_SUCCESS(m_Usb->GetMute(m_Hw->FeatureUnitId, xonar::hw::kMasterChannel, &muted))) {
        m_Muted = muted;
    }
}

bool CXonarMiniportTopology::RefreshJackState(_In_z_ const char* Origin)
{
    PAGED_CODE();
    bool connected = true;
    UCHAR nrChannels = 0;
    ULONG mask = 0;
    const NTSTATUS status = m_Usb->GetConnectorState(m_Hw->TerminalId, &connected, &nrChannels, &mask);
    const PCWSTR name = XonarSeTopologyName(m_Endpoint);
    if (!NT_SUCCESS(status)) {
        XONARSE_INFO("Topology %ls (%s) : état du connecteur OT%u illisible (0x%08X), jacks signalés connectés", name,
                     Origin, m_Hw->TerminalId, status);
        return false;
    }
    const bool changed = !m_JackStateKnown || connected != m_JackConnected || mask != m_JackMask ||
                         nrChannels != m_JackNrChannels;
    m_JackStateKnown = true;
    m_JackConnected = connected;
    m_JackNrChannels = nrChannels;
    m_JackMask = mask;
    if (changed) {
        // Journalisé seulement : l'état n'est remonté à Windows que si HonorJackDetection = 1.
        XONARSE_INFO("Topology %ls (%s) : connecteur OT%u %s, %u canaux, masque 0x%lX%s", name, Origin,
                     m_Hw->TerminalId, connected ? "branché" : "débranché", nrChannels, mask,
                     HonorJackDetection() ? "" : " (ignoré, HonorJackDetection=0)");
    }
    return changed;
}

BOOL CXonarMiniportTopology::JackIsConnected(ULONG JackIndex) const
{
    PAGED_CODE();
    // Politique par défaut : toujours connecté (voir l'en-tête du fichier).
    if (!HonorJackDetection() || !m_JackStateKnown) return TRUE;
    if (!m_JackConnected) return FALSE;
    // [H] Si la carte rend un masque de canaux non nul, on suppose qu'il indique les canaux
    // effectivement branchés et on l'applique jack par jack. Masque nul : état global.
    if (m_JackMask != 0 && JackIndex < m_Info->JackCount) {
        return (m_JackMask & m_Info->Jacks[JackIndex].ChannelMapping) != 0 ? TRUE : FALSE;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------------------
// Propriétés des jacks (broche 1)
// ---------------------------------------------------------------------------------------
NTSTATUS CXonarMiniportTopology::HandleJackDescription(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (PropertyRequest->InstanceSize < sizeof(ULONG) || PropertyRequest->Instance == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    const ULONG pin = *static_cast<const ULONG*>(PropertyRequest->Instance);
    if (pin != XONARSE_TOPO_PIN_LINEOUT) return STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        return BasicSupportSimple(PropertyRequest, KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET, VT_ILLEGAL);
    }
    if (!(PropertyRequest->Verb & KSPROPERTY_TYPE_GET)) return STATUS_INVALID_DEVICE_REQUEST;

    const ULONG count = m_Info->JackCount;
    const ULONG needed = sizeof(KSMULTIPLE_ITEM) + count * sizeof(KSJACK_DESCRIPTION);
    if (PropertyRequest->ValueSize == 0) {
        PropertyRequest->ValueSize = needed;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < needed) return STATUS_BUFFER_TOO_SMALL;

    auto* item = static_cast<PKSMULTIPLE_ITEM>(PropertyRequest->Value);
    auto* desc = reinterpret_cast<PKSJACK_DESCRIPTION>(item + 1);
    item->Size = needed;
    item->Count = count;
    Lock();
    for (ULONG i = 0; i < count; ++i) {
        const XONAR_TOPO_JACK& j = m_Info->Jacks[i];
        desc[i].ChannelMapping = j.ChannelMapping;
        desc[i].Color = j.Color;
        desc[i].ConnectionType = j.ConnectionType;
        desc[i].GeoLocation = j.GeoLocation;
        desc[i].GenLocation = j.GenLocation;
        desc[i].PortConnection = j.PortConnection;
        desc[i].IsConnected = JackIsConnected(i);
    }
    Unlock();
    PropertyRequest->ValueSize = needed;
    return STATUS_SUCCESS;
}

NTSTATUS CXonarMiniportTopology::HandleJackDescription2(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (PropertyRequest->InstanceSize < sizeof(ULONG) || PropertyRequest->Instance == nullptr) {
        return STATUS_INVALID_PARAMETER;
    }
    const ULONG pin = *static_cast<const ULONG*>(PropertyRequest->Instance);
    if (pin != XONARSE_TOPO_PIN_LINEOUT) return STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        return BasicSupportSimple(PropertyRequest, KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET, VT_ILLEGAL);
    }
    if (!(PropertyRequest->Verb & KSPROPERTY_TYPE_GET)) return STATUS_INVALID_DEVICE_REQUEST;

    const ULONG count = m_Info->JackCount;
    const ULONG needed = sizeof(KSMULTIPLE_ITEM) + count * sizeof(KSJACK_DESCRIPTION2);
    if (PropertyRequest->ValueSize == 0) {
        PropertyRequest->ValueSize = needed;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (PropertyRequest->ValueSize < needed) return STATUS_BUFFER_TOO_SMALL;

    // Sans détection honorée, la capacité de détection de présence n'est PAS annoncée :
    // Windows interprète alors IsConnected = TRUE comme « pas de détection ».
    const DWORD caps = HonorJackDetection() ? JACKDESC2_PRESENCE_DETECT_CAPABILITY : 0;
    auto* item = static_cast<PKSMULTIPLE_ITEM>(PropertyRequest->Value);
    auto* desc = reinterpret_cast<PKSJACK_DESCRIPTION2>(item + 1);
    item->Size = needed;
    item->Count = count;
    for (ULONG i = 0; i < count; ++i) {
        desc[i].DeviceStateInfo = 0;
        desc[i].JackCapabilities = caps;
    }
    PropertyRequest->ValueSize = needed;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Volume
// ---------------------------------------------------------------------------------------
NTSTATUS CXonarMiniportTopology::HandleVolume(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (m_Info->VolumeNode == XONARSE_TOPO_NO_NODE || PropertyRequest->Node != m_Info->VolumeNode) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        KSPROPERTY_STEPPING_LONG steps[XONARSE_TOPO_MAX_CHANNELS] = {};
        Lock();
        for (ULONG ch = 0; ch < m_Channels; ++ch) {
            steps[ch].SteppingDelta = ULONG(m_Range[ch].Res) * ULONG(kKsPerUac);
            steps[ch].Reserved = 0;
            steps[ch].Bounds.SignedMinimum = LONG(m_Range[ch].Min) * kKsPerUac;
            steps[ch].Bounds.SignedMaximum = LONG(m_Range[ch].Max) * kKsPerUac;
        }
        Unlock();
        return BasicSupportStepped(PropertyRequest, VT_I4, m_Channels, KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL,
                                   steps);
    }

    LONG channel = 0;
    if (!ReadChannel(PropertyRequest, &channel)) return STATUS_INVALID_PARAMETER;
    if (channel != kKsMasterChannel && (channel < 0 || ULONG(channel) >= m_Channels)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (PropertyRequest->ValueSize < sizeof(LONG) || PropertyRequest->Value == nullptr) {
        return ValueTooSmall(PropertyRequest, sizeof(LONG));
    }
    auto* value = static_cast<PLONG>(PropertyRequest->Value);

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET) {
        // Le maître n'existe pas sur la FU : on rend le canal 0 (FL).
        const ULONG ch = channel == kKsMasterChannel ? 0 : ULONG(channel);
        Lock();
        RefreshVolume(ch);
        *value = m_VolumeKs[ch];
        Unlock();
        PropertyRequest->ValueSize = sizeof(LONG);
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET) {
        // Canal maître : appliqué à tous les canaux de la FU.
        const ULONG first = channel == kKsMasterChannel ? 0 : ULONG(channel);
        const ULONG last = channel == kKsMasterChannel ? m_Channels : ULONG(channel) + 1;
        NTSTATUS result = STATUS_SUCCESS;
        Lock();
        for (ULONG ch = first; ch < last; ++ch) {
            const SHORT uac = KsToUac(ch, *value);
            const NTSTATUS status = m_Usb->SetVolume(m_Hw->FeatureUnitId, static_cast<UCHAR>(ch + 1), uac);
            if (NT_SUCCESS(status)) {
                m_VolumeKs[ch] = LONG(uac) * kKsPerUac;
            } else {
                // Le cache n'est pas modifié : il reflète ce que la carte applique réellement.
                XONARSE_WARN("Topology %ls : SET volume canal %lu = %d/256 dB échoué (0x%08X)",
                             XonarSeTopologyName(m_Endpoint), ch, uac, status);
                result = status;
            }
        }
        Unlock();
        return result;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

// ---------------------------------------------------------------------------------------
// Mute (contrôle maître de la FU)
// ---------------------------------------------------------------------------------------
NTSTATUS CXonarMiniportTopology::HandleMute(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (PropertyRequest->Node != m_Info->MuteNode) return STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        // Un seul mute pour tous les canaux : MULTICHANNEL | UNIFORM, plage booléenne 0..1.
        KSPROPERTY_STEPPING_LONG steps[XONARSE_TOPO_MAX_CHANNELS] = {};
        for (ULONG ch = 0; ch < m_Channels; ++ch) {
            steps[ch].SteppingDelta = 1;
            steps[ch].Reserved = 0;
            steps[ch].Bounds.SignedMinimum = 0;
            steps[ch].Bounds.SignedMaximum = 1;
        }
        return BasicSupportStepped(PropertyRequest, VT_BOOL, m_Channels,
                                   KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_MULTICHANNEL |
                                       KSPROPERTY_MEMBER_FLAG_BASICSUPPORT_UNIFORM,
                                   steps);
    }

    LONG channel = 0;
    if (!ReadChannel(PropertyRequest, &channel)) return STATUS_INVALID_PARAMETER;
    if (channel != kKsMasterChannel && (channel < 0 || ULONG(channel) >= m_Channels)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (PropertyRequest->ValueSize < sizeof(BOOL) || PropertyRequest->Value == nullptr) {
        return ValueTooSmall(PropertyRequest, sizeof(BOOL));
    }
    auto* value = static_cast<PBOOL>(PropertyRequest->Value);

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET) {
        Lock();
        RefreshMute();
        *value = m_Muted ? TRUE : FALSE;
        Unlock();
        PropertyRequest->ValueSize = sizeof(BOOL);
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET) {
        const bool muted = *value != FALSE;
        Lock();
        const NTSTATUS status = m_Usb->SetMute(m_Hw->FeatureUnitId, xonar::hw::kMasterChannel, muted);
        if (NT_SUCCESS(status)) {
            m_Muted = muted;
        } else {
            XONARSE_WARN("Topology %ls : SET mute=%d échoué (0x%08X)", XonarSeTopologyName(m_Endpoint), muted ? 1 : 0,
                         status);
        }
        Unlock();
        return status;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

// ---------------------------------------------------------------------------------------
// Configuration des canaux (haut-parleurs)
// ---------------------------------------------------------------------------------------
NTSTATUS CXonarMiniportTopology::HandleChannelConfig(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (!m_Info->HasChannelConfig) return STATUS_INVALID_DEVICE_REQUEST;

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        return BasicSupportSimple(PropertyRequest,
                                  KSPROPERTY_TYPE_BASICSUPPORT | KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_SET, VT_I4);
    }
    if (PropertyRequest->ValueSize < sizeof(KSAUDIO_CHANNEL_CONFIG) || PropertyRequest->Value == nullptr) {
        return ValueTooSmall(PropertyRequest, sizeof(KSAUDIO_CHANNEL_CONFIG));
    }
    auto* config = static_cast<PKSAUDIO_CHANNEL_CONFIG>(PropertyRequest->Value);

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_GET) {
        Lock();
        config->ActiveSpeakerPositions = static_cast<LONG>(m_ChannelConfig);
        Unlock();
        PropertyRequest->ValueSize = sizeof(KSAUDIO_CHANNEL_CONFIG);
        return STATUS_SUCCESS;
    }

    if (PropertyRequest->Verb & KSPROPERTY_TYPE_SET) {
        // Accepté si la configuration tient dans les canaux physiques (6). La valeur est
        // informative : le format de flux est négocié par le filtre WaveRT.
        const ULONG mask = static_cast<ULONG>(config->ActiveSpeakerPositions);
        if (mask == 0 || PopCount(mask) > m_Channels) return STATUS_INVALID_PARAMETER;
        Lock();
        m_ChannelConfig = mask;
        Unlock();
        XONARSE_INFO("Topology %ls : configuration des canaux 0x%lX", XonarSeTopologyName(m_Endpoint), mask);
        return STATUS_SUCCESS;
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

// ---------------------------------------------------------------------------------------
// Événements
// ---------------------------------------------------------------------------------------
// Événements : non paginé par prudence (PortCls peut retirer une entrée lors de la
// fermeture d'un handle, sans garantie documentée de PASSIVE_LEVEL).
#pragma code_seg()
NTSTATUS CXonarMiniportTopology::HandleEvent(_In_ PPCEVENT_REQUEST EventRequest)
{
    // Même logique que SYSVAD (CMiniportTopology_EventHandler_JackState, licence MIT) :
    // PortCls gère la liste, le miniport n'a qu'à y ajouter l'entrée.
    switch (EventRequest->Verb) {
    case PCEVENT_VERB_ADD:
        if (m_PortEvents == nullptr || EventRequest->EventEntry == nullptr) return STATUS_NOT_SUPPORTED;
        m_PortEvents->AddEventToEventList(EventRequest->EventEntry);
        return STATUS_SUCCESS;
    case PCEVENT_VERB_REMOVE:
    case PCEVENT_VERB_SUPPORT:
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}
#pragma code_seg()

// GenerateEventList est appelable jusqu'à DISPATCH_LEVEL : code non paginé.
void CXonarMiniportTopology::GenerateNodeEvent(ULONG NodeId)
{
    if (m_PortEvents == nullptr || NodeId == XONARSE_TOPO_NO_NODE) return;
    m_PortEvents->GenerateEventList(const_cast<GUID*>(&KSEVENTSETID_AudioControlChange), KSEVENT_CONTROL_CHANGE, FALSE,
                                    ULONG(-1), TRUE, NodeId);
}

void CXonarMiniportTopology::GeneratePinEvent(ULONG PinId)
{
    if (m_PortEvents == nullptr) return;
    m_PortEvents->GenerateEventList(const_cast<GUID*>(&KSEVENTSETID_PinCapsChange), KSEVENT_PINCAPS_JACKINFOCHANGE,
                                    TRUE, PinId, FALSE, ULONG(-1));
}

// ---------------------------------------------------------------------------------------
// Interruptions UAC2 (rappel à PASSIVE_LEVEL, jamais réentrant pour une inscription)
// ---------------------------------------------------------------------------------------
#pragma code_seg("PAGE")
void CXonarMiniportTopology::InterruptCallback(_In_opt_ PVOID Context, _In_ const XONAR_UAC2_INTERRUPT* Message)
{
    PAGED_CODE();
    if (Context == nullptr || Message == nullptr) return;
    static_cast<CXonarMiniportTopology*>(Context)->OnInterrupt(Message);
}

void CXonarMiniportTopology::OnInterrupt(_In_ const XONAR_UAC2_INTERRUPT* Message)
{
    PAGED_CODE();
    // bInfo bit 0 = 1 : interruption d'endpoint (pas d'une entité AudioControl) ; bit 1 : vendeur.
    if ((Message->Info & 0x03) != 0) return;

    if (Message->EntityId == m_Hw->TerminalId) {
        Lock();
        const bool changed = RefreshJackState("interruption");
        Unlock();
        if (changed && HonorJackDetection()) {
            GeneratePinEvent(XONARSE_TOPO_PIN_LINEOUT);
        }
        return;
    }

    if (Message->EntityId != m_Hw->FeatureUnitId) return;

    bool volumeChanged = false;
    bool muteChanged = false;
    Lock();
    if (Message->ControlSelector == xonar::uac2::kFuVolumeControl && m_Info->VolumeNode != XONARSE_TOPO_NO_NODE) {
        if (Message->ChannelNumber >= 1 && Message->ChannelNumber <= m_Channels) {
            RefreshVolume(ULONG(Message->ChannelNumber) - 1);
        } else {
            for (ULONG ch = 0; ch < m_Channels; ++ch) RefreshVolume(ch);
        }
        volumeChanged = true;
    } else if (Message->ControlSelector == xonar::uac2::kFuMuteControl) {
        RefreshMute();
        muteChanged = true;
    } else {
        // Sélecteur inconnu : on relit tout ce qu'on expose.
        for (ULONG ch = 0; ch < m_Channels && m_Info->VolumeNode != XONARSE_TOPO_NO_NODE; ++ch) RefreshVolume(ch);
        RefreshMute();
        volumeChanged = m_Info->VolumeNode != XONARSE_TOPO_NO_NODE;
        muteChanged = true;
    }
    Unlock();

    XONARSE_INFO("Topology %ls : interruption FU%u sélecteur %u canal %u", XonarSeTopologyName(m_Endpoint),
                 Message->EntityId, Message->ControlSelector, Message->ChannelNumber);
    if (volumeChanged) GenerateNodeEvent(m_Info->VolumeNode);
    if (muteChanged) GenerateNodeEvent(m_Info->MuteNode);
}

// ---------------------------------------------------------------------------------------
// Points d'entrée des tables d'automatisation
// ---------------------------------------------------------------------------------------
// MajorTarget est l'interface miniport remise au port (IMiniportTopology), d'où la
// conversion descendante depuis IUnknown (même procédé que SYSVAD).
#pragma code_seg()
static CXonarMiniportTopology* MiniportFrom(_In_ PUNKNOWN MajorTarget)
{
    return static_cast<CXonarMiniportTopology*>(static_cast<IMiniportTopology*>(MajorTarget));
}

NTSTATUS XonarTopoEventHandler(_In_ PPCEVENT_REQUEST EventRequest)
{
    if (EventRequest == nullptr || EventRequest->MajorTarget == nullptr) return STATUS_INVALID_PARAMETER;
    return MiniportFrom(EventRequest->MajorTarget)->HandleEvent(EventRequest);
}

#pragma code_seg("PAGE")
NTSTATUS XonarTopoPropertyFilter(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (PropertyRequest == nullptr || PropertyRequest->MajorTarget == nullptr) return STATUS_INVALID_PARAMETER;
    CXonarMiniportTopology* miniport = MiniportFrom(PropertyRequest->MajorTarget);
    if (IsEqualGUIDAligned(*PropertyRequest->PropertyItem->Set, KSPROPSETID_Jack)) {
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION) {
            return miniport->HandleJackDescription(PropertyRequest);
        }
        if (PropertyRequest->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION2) {
            return miniport->HandleJackDescription2(PropertyRequest);
        }
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

NTSTATUS XonarTopoPropertyVolume(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (PropertyRequest == nullptr || PropertyRequest->MajorTarget == nullptr) return STATUS_INVALID_PARAMETER;
    return MiniportFrom(PropertyRequest->MajorTarget)->HandleVolume(PropertyRequest);
}

NTSTATUS XonarTopoPropertyMute(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (PropertyRequest == nullptr || PropertyRequest->MajorTarget == nullptr) return STATUS_INVALID_PARAMETER;
    return MiniportFrom(PropertyRequest->MajorTarget)->HandleMute(PropertyRequest);
}

NTSTATUS XonarTopoPropertyChannelConfig(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    PAGED_CODE();
    if (PropertyRequest == nullptr || PropertyRequest->MajorTarget == nullptr) return STATUS_INVALID_PARAMETER;
    return MiniportFrom(PropertyRequest->MajorTarget)->HandleChannelConfig(PropertyRequest);
}
#pragma code_seg()
