// Flux WaveRT de rendu (voir minwavertstream.h).
//
// Machine d'états (PASSIVE_LEVEL, transitions pas à pas) :
//   STOP -> ACQUIRE : SetSampleRate(horloge) puis contrôle GetSampleRate, SelectAltSetting,
//                     allocation des URB (moteur préparé), StreamOpened().
//   ACQUIRE -> PAUSE : rien.
//   PAUSE -> RUN     : horloge reprogrammée, altsetting revérifié par GET_INTERFACE et
//                      resélectionné si la carte l'a perdu (reprise après veille), puis
//                      démarrage du moteur isochrone (la position reprend où elle était).
//   RUN -> PAUSE     : arrêt synchrone du moteur (attente de tous les URB).
//   PAUSE -> ACQUIRE : rien.
//   ACQUIRE -> STOP  : libération des URB, altsetting 0, StreamClosed(), position remise à zéro.
// Si la couche USB échoue (bouchon : STATUS_NOT_IMPLEMENTED), STOP -> ACQUIRE échoue
// proprement et le flux reste en STOP.
#include "minwavertstream.h"

#pragma code_seg("PAGE")

CXonarWaveRtStream::CXonarWaveRtStream(CXonarWaveRtMiniport* Miniport, PPORTWAVERTSTREAM PortStream,
                                       const xonar::wave::StreamFormat& Format)
    : CUnknown(nullptr), m_Miniport(Miniport), m_PortStream(PortStream), m_Format(Format)
{
    PAGED_CODE();
    m_Miniport->AddRef();
}

CXonarWaveRtStream::~CXonarWaveRtStream()
{
    PAGED_CODE();
    // Normalement déjà en STOP avec tampon libéré ; on se protège d'un arrêt brutal.
    if (m_State == KSSTATE_RUN) m_Engine.Stop();
    if (m_State != KSSTATE_STOP) {
        ReleaseHardware();
        m_State = KSSTATE_STOP;
    }
    if (m_BufferMdl != nullptr) FreeBufferCommon(m_BufferMdl);
    m_Miniport->OnStreamClosed();
    m_Miniport->Release();
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::NonDelegatingQueryInterface(REFIID Interface, PVOID* Object)
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

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::SetFormat(PKSDATAFORMAT DataFormat)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(DataFormat);
    // Comme SYSVAD : le format est figé à la création du flux.
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS CXonarWaveRtStream::ProgramClock()
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
            XONARSE_WARN("wave: SetSampleRate(%u) 0x%08X mais l'horloge est déjà à %lu Hz", hw.ClockId, status, actual);
        } else {
            XONARSE_ERROR("wave: SetSampleRate(horloge %u, %lu Hz) : 0x%08X", hw.ClockId, m_Format.SampleRate, status);
            return status;
        }
    } else if (!NT_SUCCESS(getStatus)) {
        XONARSE_WARN("wave: GetSampleRate(horloge %u) illisible : 0x%08X", hw.ClockId, getStatus);
    } else if (actual != m_Format.SampleRate) {
        XONARSE_WARN("wave: horloge %u à %lu Hz au lieu de %lu Hz (écart %ld Hz)", hw.ClockId, actual,
                     m_Format.SampleRate, static_cast<LONG>(actual) - static_cast<LONG>(m_Format.SampleRate));
    }
    return STATUS_SUCCESS;
}

NTSTATUS CXonarWaveRtStream::SelectAltAndPrepare()
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
        XONARSE_ERROR("wave: SelectAltSetting(%u, %u) : 0x%08X", hw.Interface, alt.AltSetting, status);
        return status;
    }
    if (!m_AltSelected) {
        // Compteur de flux du module USB : un flux compte de STOP -> ACQUIRE jusqu'au
        // retour en STOP (ou à la destruction), resélections comprises.
        m_AltSelected = true;
        (void)usb->StreamOpened();
    }
    if (m_Pipes.DataPipe == nullptr) {
        XONARSE_ERROR("wave: altsetting %u sans pipe de données", alt.AltSetting);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (m_Pipes.DataEndpoint != alt.DataEndpoint) {
        XONARSE_WARN("wave: pipe de données 0x%02X (attendu 0x%02X)", m_Pipes.DataEndpoint, alt.DataEndpoint);
    }
    if (m_Pipes.FeedbackPipe == nullptr) {
        XONARSE_WARN("wave: altsetting %u sans pipe de feedback (attendu 0x%02X)", alt.AltSetting, alt.FeedbackEndpoint);
    }

    XonarIsoEngineConfig config = {};
    config.SampleRate = m_Format.SampleRate;
    config.BlockAlign = m_Format.BlockAlign;
    config.MaxPacketBytes = alt.MaxPacketBytes;
    if (m_Pipes.DataMaxPacketBytes != 0 && m_Pipes.DataMaxPacketBytes < config.MaxPacketBytes) {
        config.MaxPacketBytes = m_Pipes.DataMaxPacketBytes;
    }
    config.DataPipe = m_Pipes.DataPipe;
    config.FeedbackPipe = m_Pipes.FeedbackPipe;
    // Le moteur refuse (STATUS_DEVICE_CONFIGURATION_ERROR) une géométrie d'endpoint
    // différente de la sienne (bInterval données 1, feedback 4).
    config.DataInterval = m_Pipes.DataInterval;
    config.FeedbackInterval = m_Pipes.FeedbackInterval;
    return m_Engine.Prepare(usb, config);
}

NTSTATUS CXonarWaveRtStream::AcquireHardware()
{
    PAGED_CODE();
    NTSTATUS status = ProgramClock();
    if (!NT_SUCCESS(status)) return status;
    status = SelectAltAndPrepare();
    if (!NT_SUCCESS(status)) {
        ReleaseHardware();
        return status;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CXonarWaveRtStream::QueryCurrentAltSetting(UCHAR* AltSetting)
{
    PAGED_CODE();
    *AltSetting = 0;
    XonarUsbDevice* usb = m_Miniport->Usb();
    if (usb == nullptr) return STATUS_DEVICE_NOT_READY;

    // URB et octet de réponse en pool non paginé (URB client : SubmitUrbSync, Argument1).
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

NTSTATUS CXonarWaveRtStream::PrepareRun()
{
    PAGED_CODE();
    // 1. Horloge : après une veille, la carte peut être revenue à sa fréquence par défaut.
    NTSTATUS status = ProgramClock();
    if (!NT_SUCCESS(status)) return status;

    // 2. Altsetting : la carte doit toujours être dans celui du flux.
    const UCHAR expected = m_Format.Alt->AltSetting;
    bool reselect = !m_Engine.IsPrepared(); // resélection précédente avortée
    if (!reselect) {
        UCHAR current = 0;
        status = QueryCurrentAltSetting(&current);
        if (!NT_SUCCESS(status)) {
            // Requête standard obligatoire ; en cas d'échec on garde la configuration connue.
            XONARSE_WARN("wave: GET_INTERFACE(%u) : 0x%08X, altsetting supposé inchangé", m_Miniport->Hw().Interface,
                         status);
        } else if (current != expected) {
            XONARSE_WARN("wave: interface %u en altsetting %u au lieu de %u (reprise après veille ?), resélection",
                         m_Miniport->Hw().Interface, current, expected);
            reselect = true;
        }
    }
    if (reselect) {
        // Moteur arrêté (PAUSE) : les poignées de pipes changent, les URB sont refaits.
        m_Engine.Unprepare();
        status = SelectAltAndPrepare();
        if (!NT_SUCCESS(status)) {
            XONARSE_ERROR("wave: resélection de l'altsetting %u : 0x%08X", expected, status);
            // m_AltSelected reste vrai : ACQUIRE -> STOP remettra l'altsetting 0.
            m_Engine.Unprepare();
            return status;
        }
    }
    return STATUS_SUCCESS;
}

void CXonarWaveRtStream::ReleaseHardware()
{
    PAGED_CODE();
    m_Engine.Unprepare();
    if (m_AltSelected) {
        const XONARSE_ENDPOINT_HW& hw = m_Miniport->Hw();
        XonarUsbDevice* usb = m_Miniport->Usb();
        const NTSTATUS status = usb->SelectAltSetting(hw.Interface, 0, nullptr);
        if (!NT_SUCCESS(status)) XONARSE_WARN("wave: retour à l'altsetting 0 : 0x%08X", status);
        m_AltSelected = false;
        (void)usb->StreamClosed();
    }
    RtlZeroMemory(&m_Pipes, sizeof(m_Pipes));
}

NTSTATUS CXonarWaveRtStream::Transition(KSSTATE From, KSSTATE To)
{
    PAGED_CODE();
    if (From == KSSTATE_STOP && To == KSSTATE_ACQUIRE) return AcquireHardware();
    if (From == KSSTATE_PAUSE && To == KSSTATE_RUN) {
        if (m_Buffer == nullptr) {
            XONARSE_ERROR("wave: RUN sans tampon cyclique");
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
    return STATUS_SUCCESS; // ACQUIRE <-> PAUSE
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::SetState(KSSTATE State)
{
    PAGED_CODE();
    if (State < KSSTATE_STOP || State > KSSTATE_RUN) return STATUS_INVALID_PARAMETER;
    // KS impose des transitions adjacentes ; on avance quand même pas à pas par sécurité.
    while (m_State != State) {
        const KSSTATE next = static_cast<KSSTATE>(State > m_State ? m_State + 1 : m_State - 1);
        const NTSTATUS status = Transition(m_State, next);
        if (!NT_SUCCESS(status)) {
            XONARSE_ERROR("wave: endpoint %lu, transition %d -> %d : 0x%08X",
                          static_cast<ULONG>(m_Miniport->Endpoint()), m_State, next, status);
            return status;
        }
        m_State = next;
    }
    return STATUS_SUCCESS;
}

NTSTATUS CXonarWaveRtStream::AllocateBufferCommon(ULONG NotificationCount, ULONG RequestedSize, PMDL* AudioBufferMdl,
                                                  ULONG* ActualSize, ULONG* OffsetFromFirstPage,
                                                  MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    *AudioBufferMdl = nullptr;
    *ActualSize = 0;
    *OffsetFromFirstPage = 0;
    *CacheType = MmCached;
    if (m_BufferMdl != nullptr || m_State == KSSTATE_RUN) return STATUS_INVALID_DEVICE_STATE;

    const ULONG block = m_Format.BlockAlign;
    if (block == 0) return STATUS_INVALID_PARAMETER;
    // Le moteur prélève d'un coup jusqu'à « inFlight » octets au démarrage (8 URB de 1 ms)
    // et garde cette avance ensuite.
    const ULONG inFlight = XonarIsoOutEngine::MaxInFlightBytes(m_Format.SampleRate, m_Format.BlockAlign);
    auto roundUp = [block](ULONGLONG Bytes) -> ULONGLONG { return (Bytes + block - 1) / block * block; };
    ULONGLONG wanted;
    if (NotificationCount != 0) {
        // Mode événementiel : la période demandée est respectée telle quelle. Le moteur
        // audio partagé refuse un tampon agrandi (IAudioClient::Initialize 0x887C0054 en
        // 0.3.1.0, constaté le 28/10/2026). Les notifications suivent la position de
        // prélèvement (iso_engine.cpp) : le client dispose toujours d'une période entière
        // pour remplir, l'avance USB n'ajoute que de la latence. Seule conséquence d'une
        // période plus courte que l'avance : l'amorçage franchit plusieurs frontières et
        // les signaux fusionnent (compté par le moteur).
        ULONGLONG period = roundUp(RequestedSize / NotificationCount);
        if (period == 0) period = block;
        if (period < inFlight) {
            XONARSE_WARN("wave: période de %llu octets plus courte que l'avance USB (%lu octets)", period, inFlight);
        }
        wanted = period * NotificationCount;
    } else {
        wanted = RequestedSize / block * block;
        const ULONGLONG minSize = 2ULL * inFlight;
        if (wanted < minSize) {
            XONARSE_INFO("wave: tampon porté de %llu à %llu octets (%lu octets en vol)", wanted, minSize, inFlight);
            wanted = minSize;
        }
    }
    if (wanted == 0 || wanted > MAXULONG / 2) return STATUS_INVALID_PARAMETER;
    const ULONG size = static_cast<ULONG>(wanted);

    PHYSICAL_ADDRESS high;
    high.HighPart = 0;
    high.LowPart = MAXULONG; // comme SYSVAD ; aucun DMA, la contrainte est sans effet
    PMDL mdl = m_PortStream->AllocatePagesForMdl(high, size);
    if (mdl == nullptr) {
        XONARSE_ERROR("wave: AllocatePagesForMdl(%lu) a échoué", size);
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
    RtlZeroMemory(va, size); // silence

    m_BufferMdl = mdl;
    m_Buffer = va;
    m_BufferSize = size;
    m_NotificationCount = NotificationCount;
    m_Engine.SetAudioBuffer(va, size, NotificationCount);

    *AudioBufferMdl = mdl;
    *ActualSize = size;
    XONARSE_INFO("wave: tampon cyclique %lu octets (%lu notifications)", size, NotificationCount);
    return STATUS_SUCCESS;
}

void CXonarWaveRtStream::FreeBufferCommon(PMDL AudioBufferMdl)
{
    PAGED_CODE();
    if (m_BufferMdl == nullptr) return;
    if (AudioBufferMdl != nullptr && AudioBufferMdl != m_BufferMdl) {
        XONARSE_WARN("wave: FreeAudioBuffer avec un MDL inconnu");
    }
    // Le moteur n'utilise plus le tampon au retour (échange sous verrou).
    m_Engine.SetAudioBuffer(nullptr, 0, 0);
    m_PortStream->UnmapAllocatedPages(m_Buffer, m_BufferMdl);
    m_PortStream->FreePagesFromMdl(m_BufferMdl);
    m_BufferMdl = nullptr;
    m_Buffer = nullptr;
    m_BufferSize = 0;
    m_NotificationCount = 0;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::AllocateAudioBuffer(ULONG RequestedSize, PMDL* AudioBufferMdl,
                                                                ULONG* ActualSize, ULONG* OffsetFromFirstPage,
                                                                MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    return AllocateBufferCommon(0, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage, CacheType);
}

STDMETHODIMP_(VOID) CXonarWaveRtStream::FreeAudioBuffer(PMDL AudioBufferMdl, ULONG BufferSize)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(BufferSize);
    FreeBufferCommon(AudioBufferMdl);
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::AllocateBufferWithNotification(ULONG NotificationCount, ULONG RequestedSize,
                                                                           PMDL* AudioBufferMdl, ULONG* ActualSize,
                                                                           ULONG* OffsetFromFirstPage,
                                                                           MEMORY_CACHING_TYPE* CacheType)
{
    PAGED_CODE();
    if (NotificationCount == 0) return STATUS_INVALID_PARAMETER;
    return AllocateBufferCommon(NotificationCount, RequestedSize, AudioBufferMdl, ActualSize, OffsetFromFirstPage,
                                CacheType);
}

STDMETHODIMP_(VOID) CXonarWaveRtStream::FreeBufferWithNotification(PMDL AudioBufferMdl, ULONG BufferSize)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(BufferSize);
    FreeBufferCommon(AudioBufferMdl);
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::GetPositionRegister(KSRTAUDIO_HWREGISTER* Register)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED; // pas de registre matériel : le moteur audio utilise GetPosition
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::GetClockRegister(KSRTAUDIO_HWREGISTER* Register)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_SUPPORTED;
}

#pragma code_seg()

// --- Non paginé : peut être appelé à DISPATCH_LEVEL et prend le verrou du moteur ---

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::GetPosition(PKSAUDIO_POSITION Position)
{
    if (Position == nullptr) return STATUS_INVALID_PARAMETER;
    ULONG played = 0;
    ULONG read = 0;
    m_Engine.GetPosition(&played, &read, nullptr);
    // PlayOffset : octets des URB terminés (au plus près du son réel, sans les ~8 ms en
    // vol). WriteOffset : position de prélèvement, équivalent de la position DMA : le
    // moteur audio ne doit plus rien écrire en deçà.
    Position->PlayOffset = played;
    Position->WriteOffset = read;
    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID) CXonarWaveRtStream::GetHWLatency(KSRTAUDIO_HWLATENCY* Latency)
{
    if (Latency == nullptr) return;
    Latency->FifoSize = m_Engine.InFlightBytes(); // octets prélevés et en vol sur le bus
    Latency->ChipsetDelay = 0;
    Latency->CodecDelay = 0;
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::RegisterNotificationEvent(PKEVENT NotificationEvent)
{
    return m_Engine.RegisterNotificationEvent(NotificationEvent);
}

STDMETHODIMP_(NTSTATUS) CXonarWaveRtStream::UnregisterNotificationEvent(PKEVENT NotificationEvent)
{
    return m_Engine.UnregisterNotificationEvent(NotificationEvent);
}
