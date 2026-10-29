// Moteur isochrone OUT asynchrone + feedback (voir iso_engine.h pour le principe).
//
// IRQL : Prepare / Unprepare sont paginables. Tout ce qui prend m_Lock, tout ce qui
// tourne dans les complétions, la pompe, l'élément de travail de relance (il prend le
// verrou) et Start (qui soumet à DISPATCH_LEVEL) reste non paginé.
// Le tampon cyclique (MapAllocatedPages) et les tampons de transfert sont non paginés.
//
// Références m_Outstanding : 1 par emplacement en file ou en vol, 1 pour l'élément de
// travail en file ou en cours, 1 « garde » par complétion en cours d'exécution, plus le
// biais de Start. Chaque routine rend sa garde en tout dernier : Stop peut alors
// détruire l'objet dès le réveil.
#include "iso_engine.h"

static_assert(XONAR_ISO_DATA_URBS <= 32 && XONAR_FB_URBS <= 32, "masques de relance sur 32 bits");

namespace {

constexpr ULONG kMicroframesPerSecond = 8000;
constexpr LONGLONG kStopTimeoutMs = 500;
// Relance : nombre de tentatives consécutives sans aucun URB réussi sur le pipe, puis
// abandon tracé. Délai entre tentatives : 1, 2, 4 ... 64 ms (environ 2 s au total).
constexpr ULONG kMaxRecoveryRounds = 40;
constexpr ULONG kMaxBackoffMs = 64;
// Feedback sans valeur exploitable pendant ce nombre d'URB (4 ms chacun) : retour à la
// cadence nominale plutôt que de garder une valeur figée.
constexpr ULONG kMaxFeedbackEmptyUrbs = 25;

// Journalisation parcimonieuse : 1er, 10e, 100e, 1000e, puis tous les 10000.
bool ShouldLog(LONG Count)
{
    return Count == 1 || Count == 10 || Count == 100 || Count == 1000 || (Count % 10000) == 0;
}

bool IsLateStatus(USBD_STATUS Status)
{
    return Status == USBD_STATUS_ISO_NOT_ACCESSED_LATE || Status == USBD_STATUS_ISO_NA_LATE_USBPORT ||
           Status == USBD_STATUS_ISO_NOT_ACCESSED_BY_HW || Status == USBD_STATUS_BAD_START_FRAME;
}

// Seules causes d'abandon définitif hors arrêt : le périphérique a disparu.
bool IsDeviceGoneStatus(NTSTATUS Status)
{
    return Status == STATUS_NO_SUCH_DEVICE || Status == STATUS_DEVICE_NOT_CONNECTED ||
           Status == STATUS_DELETE_PENDING || Status == STATUS_DEVICE_REMOVED;
}

// Taille exacte d'un URB isochrone de n paquets, telle qu'allouée par USBD_IsochUrbAllocate.
// (GET_ISO_URB_SIZE compte un descripteur de paquet de plus, il sert à Hdr.Length et à
// l'allocation de repli.)
constexpr SIZE_T ExactIsoUrbSize(ULONG Packets)
{
    return sizeof(struct _URB_ISOCH_TRANSFER) + (Packets - 1) * sizeof(USBD_ISO_PACKET_DESCRIPTOR);
}

ULONG BitCount(ULONG Mask)
{
    ULONG n = 0;
    for (; Mask != 0; Mask &= Mask - 1) ++n;
    return n;
}

} // namespace

XonarIsoOutEngine::XonarIsoOutEngine()
{
    KeInitializeSpinLock(&m_Lock);
    KeInitializeEvent(&m_Idle, NotificationEvent, TRUE);
}

XonarIsoOutEngine::~XonarIsoOutEngine()
{
    // Le propriétaire (flux WaveRT) appelle Stop puis Unprepare avant destruction.
    NT_ASSERT(!m_Running);
    NT_ASSERT(!m_Prepared);
}

ULONG XonarIsoOutEngine::MaxInFlightBytes(ULONG SampleRate, USHORT BlockAlign)
{
    // Cadence maximale par microtrame = plafond nominal + 1 (voir m_MaxFrames).
    const ULONG maxFrames = (SampleRate + kMicroframesPerSecond - 1) / kMicroframesPerSecond + 1;
    return XONAR_ISO_DATA_URBS * XONAR_ISO_PACKETS_PER_URB * maxFrames * BlockAlign;
}

#pragma code_seg("PAGE")

NTSTATUS XonarIsoOutEngine::AllocateSlot(Slot* S, ULONG Index, ULONG Packets, ULONG BufferBytes, bool Feedback)
{
    PAGED_CODE();
    RtlZeroMemory(S, sizeof(*S));
    S->Engine = this;
    S->Index = Index;
    S->Packets = Packets;
    S->BufferBytes = BufferBytes;
    S->Feedback = Feedback;
    S->State = SlotIdle;

    S->Irp = IoAllocateIrp(m_Lower->StackSize, FALSE);
    if (S->Irp == nullptr) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    if (m_UsbdHandle != nullptr) {
        status = USBD_IsochUrbAllocate(m_UsbdHandle, Packets, &S->Urb);
        if (NT_SUCCESS(status)) {
            S->UsbdAllocated = true;
        } else {
            S->Urb = nullptr;
            XONARSE_WARN("wave: USBD_IsochUrbAllocate 0x%08X, repli ExAllocatePool2", status);
        }
    }
    if (S->Urb == nullptr) {
        S->Urb = static_cast<PURB>(ExAllocatePool2(POOL_FLAG_NON_PAGED, GET_ISO_URB_SIZE(Packets), XONARSE_POOL_TAG));
        if (S->Urb == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    }

    S->Buffer = static_cast<PUCHAR>(ExAllocatePool2(POOL_FLAG_NON_PAGED, BufferBytes, XONARSE_POOL_TAG));
    if (S->Buffer == nullptr) return STATUS_INSUFFICIENT_RESOURCES;

    // MDL construit une fois : évite à la pile USB d'en allouer un à chaque soumission.
    S->Mdl = IoAllocateMdl(S->Buffer, BufferBytes, FALSE, FALSE, nullptr);
    if (S->Mdl == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(S->Mdl);
    return STATUS_SUCCESS;
}

void XonarIsoOutEngine::FreeSlot(Slot* S)
{
    PAGED_CODE();
    if (S->Mdl != nullptr) IoFreeMdl(S->Mdl);
    if (S->Buffer != nullptr) ExFreePoolWithTag(S->Buffer, XONARSE_POOL_TAG);
    if (S->Urb != nullptr) {
        if (S->UsbdAllocated) {
            // Si PrepareForRemove a déjà fermé la poignée, la libérer avec une poignée
            // périmée serait pire qu'une fuite : on journalise et on abandonne l'URB.
            if (m_Usb != nullptr && m_Usb->UsbdHandle() == m_UsbdHandle) {
                USBD_UrbFree(m_UsbdHandle, S->Urb);
            } else {
                XONARSE_ERROR("wave: poignée USBD fermée avant la libération des URB (ordre de retrait)");
            }
        } else {
            ExFreePoolWithTag(S->Urb, XONARSE_POOL_TAG);
        }
    }
    if (S->Irp != nullptr) IoFreeIrp(S->Irp);
    RtlZeroMemory(S, sizeof(*S));
}

NTSTATUS XonarIsoOutEngine::Prepare(XonarUsbDevice* Usb, const XonarIsoEngineConfig& Config)
{
    PAGED_CODE();
    if (m_Prepared || m_Running) return STATUS_INVALID_DEVICE_STATE;
    if (Usb == nullptr || Config.DataPipe == nullptr || Config.BlockAlign == 0 || Config.SampleRate == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    // Géométrie des URB figée (8 paquets par microtrame, feedback 1 paquet par ms) : un
    // autre bInterval rendrait le débit faux, on refuse le flux.
    if (Config.DataInterval != XONAR_ISO_DATA_INTERVAL) {
        XONARSE_ERROR("wave: bInterval de données %u non pris en charge (attendu %u)", Config.DataInterval,
                      XONAR_ISO_DATA_INTERVAL);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (Config.FeedbackPipe != nullptr && Config.FeedbackInterval != XONAR_FB_INTERVAL) {
        XONARSE_ERROR("wave: bInterval de feedback %u non pris en charge (attendu %u)", Config.FeedbackInterval,
                      XONAR_FB_INTERVAL);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    PDEVICE_OBJECT lower = Usb->LowerDevice();
    if (lower == nullptr) {
        XONARSE_ERROR("wave: aucun objet USB inférieur, streaming impossible");
        return STATUS_DEVICE_NOT_READY;
    }

    const ULONG maxPacketFrames = Config.MaxPacketBytes / Config.BlockAlign;
    const ULONG nominalFloor = Config.SampleRate / kMicroframesPerSecond;
    const ULONG nominalCeil = (Config.SampleRate + kMicroframesPerSecond - 1) / kMicroframesPerSecond;
    if (maxPacketFrames < nominalCeil) {
        XONARSE_ERROR("wave: %lu Hz x %u octets ne tient pas dans wMaxPacketSize %u", Config.SampleRate,
                      Config.BlockAlign, Config.MaxPacketBytes);
        return STATUS_NOT_SUPPORTED;
    }

    m_Usb = Usb;
    m_Lower = lower;
    m_UsbdHandle = Usb->UsbdHandle();
    m_Config = Config;
    m_NominalQ16 = static_cast<ULONG>((static_cast<ULONGLONG>(Config.SampleRate) << 16) / kMicroframesPerSecond);
    m_MinFrames = nominalFloor > 0 ? nominalFloor - 1 : 0;
    m_MaxFrames = nominalCeil + 1 < maxPacketFrames ? nominalCeil + 1 : maxPacketFrames;
    m_InFlightBytes = static_cast<ULONG>(
        (static_cast<ULONGLONG>(Config.SampleRate) * XONAR_ISO_DATA_URBS * XONAR_ISO_PACKETS_PER_URB /
         kMicroframesPerSecond) * Config.BlockAlign);
    m_Prepared = true; // pour qu'Unprepare libère les allocations partielles

    NTSTATUS status = STATUS_SUCCESS;
    // Élément de travail de relance, attaché au FDO de PortCls (jamais à l'objet inférieur).
    PDEVICE_OBJECT fdo = Usb->FunctionalDevice();
    if (fdo != nullptr) {
        m_WorkItem = IoAllocateWorkItem(fdo);
        if (m_WorkItem == nullptr) status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        XONARSE_WARN("wave: aucun FDO, les URB perdus ne seront pas relancés");
    }

    const ULONG dataBytes = XONAR_ISO_PACKETS_PER_URB * m_MaxFrames * Config.BlockAlign;
    for (ULONG i = 0; i < XONAR_ISO_DATA_URBS && NT_SUCCESS(status); ++i) {
        status = AllocateSlot(&m_Data[i], i, XONAR_ISO_PACKETS_PER_URB, dataBytes, false);
    }
    m_FbSlotCount = 0;
    if (NT_SUCCESS(status) && Config.FeedbackPipe != nullptr) {
        for (ULONG i = 0; i < XONAR_FB_URBS && NT_SUCCESS(status); ++i) {
            status = AllocateSlot(&m_Fb[i], i, XONAR_FB_PACKETS_PER_URB, XONAR_FB_PACKETS_PER_URB * XONAR_FB_PACKET_BYTES,
                                  true);
            if (NT_SUCCESS(status)) m_FbSlotCount = i + 1;
        }
    } else if (Config.FeedbackPipe == nullptr) {
        XONARSE_WARN("wave: pas de pipe de feedback, cadence nominale seule (dérive d'horloge possible)");
    }

    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("wave: allocation des URB : 0x%08X", status);
        Unprepare();
        return status;
    }
    XONARSE_INFO("wave: moteur prêt : %lu Hz, %u o/trame, %lu..%lu trames/microtrame, %lu URB x %lu paquets, feedback %s",
                 Config.SampleRate, Config.BlockAlign, m_MinFrames, m_MaxFrames, XONAR_ISO_DATA_URBS,
                 XONAR_ISO_PACKETS_PER_URB, m_FbSlotCount ? "oui" : "non");
    return STATUS_SUCCESS;
}

void XonarIsoOutEngine::Unprepare()
{
    PAGED_CODE();
    NT_ASSERT(!m_Running);
    if (!m_Prepared) return;
    for (auto& s : m_Data) FreeSlot(&s);
    for (auto& s : m_Fb) FreeSlot(&s);
    // Stop a attendu m_Outstanding = 0 : l'élément de travail n'est plus en file.
    if (m_WorkItem != nullptr) {
        IoFreeWorkItem(m_WorkItem);
        m_WorkItem = nullptr;
    }
    m_FbSlotCount = 0;
    m_Prepared = false;
    m_Usb = nullptr;
    m_Lower = nullptr;
    m_UsbdHandle = nullptr;
}

void XonarIsoOutEngine::LogStatistics(const char* When)
{
    PAGED_CODE();
    XONARSE_INFO("wave: %s : URB %ld (err %ld, relances %ld/%ld, resets %ld), paquets err %ld (retard %ld), bornes %ld, fb %ld (rejets %ld)",
                 When, m_StatDataUrbs, m_StatUrbErrors, m_StatResubmitted, m_StatSlotsLost, m_StatPipeResets,
                 m_StatPacketErrors, m_StatLatePackets, m_StatClamped, m_StatFbValues, m_StatFbRejected);
    if (m_StatMultiPeriod != 0) {
        XONARSE_WARN("wave: %ld URB ont franchi plus d'une frontière de période", m_StatMultiPeriod);
    }
}

#pragma code_seg()

// ---------------------------------------------------------------------------
// Chemin non paginé
// ---------------------------------------------------------------------------

void XonarIsoOutEngine::SetAudioBuffer(PUCHAR Buffer, ULONG Size, ULONG NotificationCount)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Ring = (Buffer != nullptr && Size != 0) ? Buffer : nullptr;
    m_RingSize = m_Ring != nullptr ? Size : 0;
    m_PeriodBytes = (m_Ring != nullptr && NotificationCount != 0) ? Size / NotificationCount : 0;
    if (m_ReadPos >= m_RingSize) m_ReadPos = 0;
    if (m_PlayPos >= m_RingSize) m_PlayPos = 0;
    KeReleaseSpinLock(&m_Lock, irql);
}

void XonarIsoOutEngine::GetPosition(ULONG* PlayOffset, ULONG* ReadOffset, ULONGLONG* PlayedBytes)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    *PlayOffset = m_PlayPos;
    if (ReadOffset != nullptr) *ReadOffset = m_ReadPos;
    if (PlayedBytes != nullptr) *PlayedBytes = m_Played;
    KeReleaseSpinLock(&m_Lock, irql);
}

void XonarIsoOutEngine::ResetPosition()
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_ReadPos = 0;
    m_PlayPos = 0;
    m_Linear = 0;
    m_Played = 0;
    KeReleaseSpinLock(&m_Lock, irql);
}

NTSTATUS XonarIsoOutEngine::RegisterNotificationEvent(PKEVENT Event)
{
    if (Event == nullptr) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    for (const PKEVENT e : m_Events) {
        if (e == Event) {
            status = STATUS_UNSUCCESSFUL; // déjà inscrit (même comportement que SYSVAD)
            break;
        }
    }
    if (status != STATUS_UNSUCCESSFUL) {
        for (auto& e : m_Events) {
            if (e == nullptr) {
                e = Event;
                status = STATUS_SUCCESS;
                break;
            }
        }
    }
    KeReleaseSpinLock(&m_Lock, irql);
    return status;
}

NTSTATUS XonarIsoOutEngine::UnregisterNotificationEvent(PKEVENT Event)
{
    NTSTATUS status = STATUS_NOT_FOUND;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    for (auto& e : m_Events) {
        if (e != nullptr && e == Event) {
            e = nullptr;
            status = STATUS_SUCCESS;
            break;
        }
    }
    KeReleaseSpinLock(&m_Lock, irql);
    return status;
}

NTSTATUS XonarIsoOutEngine::Start()
{
    if (!m_Prepared) return STATUS_INVALID_DEVICE_STATE;
    if (m_Running) return STATUS_SUCCESS;

    m_StatDataUrbs = m_StatUrbErrors = m_StatPacketErrors = m_StatLatePackets = 0;
    m_StatClamped = m_StatFbValues = m_StatFbRejected = m_StatSlotsLost = 0;
    m_StatResubmitted = m_StatPipeResets = m_StatMultiPeriod = 0;
    // Biais rendu par Stop + une référence par emplacement amorcé (aucune complétion
    // possible avant les soumissions ci-dessous).
    m_Outstanding = 1 + XONAR_ISO_DATA_URBS + m_FbSlotCount;
    m_PumpOwner = 0;
    KeClearEvent(&m_Idle);

    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Stopping = false;
    m_DeviceGone = false;
    m_StarvedLogged = false;
    m_AccQ16 = 0;
    m_NominalAcc = 0;
    m_FeedbackQ16 = 0;
    m_FbEmptyUrbs = 0;
    m_FbShiftKnown = false;
    m_FbShift = 0;
    m_LostData = 0;
    m_LostFb = 0;
    m_WorkQueued = false;
    m_DataRounds = 0;
    m_FbRounds = 0;
    m_DataAbandonLogged = false;
    m_FbAbandonLogged = false;
    m_QueueHead = 0;
    m_QueueCount = 0;
    m_DataInFlight = 0;
    // Les K URB de données passent par la pompe, dans l'ordre : l'ordre de remplissage
    // est l'ordre de soumission.
    for (auto& s : m_Data) EnqueueLocked(&s);
    for (ULONG i = 0; i < m_FbSlotCount; ++i) m_Fb[i].State = SlotInFlight;
    m_FbInFlight = m_FbSlotCount;
    KeReleaseSpinLock(&m_Lock, irql);
    m_Running = true;

    // Amorçage à DISPATCH_LEVEL : les URB partent en quelques microsecondes.
    KIRQL oldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
    for (ULONG i = 0; i < m_FbSlotCount; ++i) {
        FormatFeedbackUrb(&m_Fb[i]);
        SubmitSlot(&m_Fb[i]);
    }
    Pump(); // le biais de Start tient l'objet pendant l'appel
    KeLowerIrql(oldIrql);
    XONARSE_INFO("wave: moteur démarré (%lu URB données, %lu URB feedback)", XONAR_ISO_DATA_URBS, m_FbSlotCount);
    return STATUS_SUCCESS;
}

void XonarIsoOutEngine::Stop()
{
    if (!m_Running) return;

    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Stopping = true;
    KeReleaseSpinLock(&m_Lock, irql);
    ReleaseOutstanding(); // rend le biais de Start

    // Les URB isochrones se terminent d'eux-mêmes en quelques millisecondes ; un élément
    // de travail de relance voit m_Stopping et sort sans resoumettre.
    LARGE_INTEGER timeout;
    timeout.QuadPart = -kStopTimeoutMs * 10 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_Idle, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        XONARSE_WARN("wave: URB encore en vol après %lld ms (%ld), AbortPipe", kStopTimeoutMs, m_Outstanding);
        (void)m_Usb->AbortPipe(m_Config.DataPipe);
        if (m_Config.FeedbackPipe != nullptr) (void)m_Usb->AbortPipe(m_Config.FeedbackPipe);
        // Impossible de libérer des URB encore possédés par la pile USB : on attend.
        timeout.QuadPart = -5000LL * 10 * 1000;
        while (KeWaitForSingleObject(&m_Idle, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
            XONARSE_ERROR("wave: %ld URB toujours en vol après AbortPipe", m_Outstanding);
        }
    }
    m_Running = false;
    LogStatistics("arrêt");
}

void XonarIsoOutEngine::ReleaseOutstanding()
{
    if (InterlockedDecrement(&m_Outstanding) == 0) {
        // Dernier accès à l'objet : Stop peut le détruire dès le réveil.
        KeSetEvent(&m_Idle, IO_NO_INCREMENT, FALSE);
    }
}

void XonarIsoOutEngine::FormatIsoUrb(Slot* S, USBD_PIPE_HANDLE Pipe, ULONG Flags, ULONG TotalBytes)
{
    // On conserve Hdr.UsbdDeviceHandle et Hdr.UsbdFlags, renseignés par
    // USBD_IsochUrbAllocate ; tout le reste est remis à zéro avant chaque soumission.
    struct _URB_ISOCH_TRANSFER* iso = &S->Urb->UrbIsochronousTransfer;
    const SIZE_T head = FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, PipeHandle);
    RtlZeroMemory(reinterpret_cast<PUCHAR>(iso) + head, ExactIsoUrbSize(S->Packets) - head);
    iso->Hdr.Length = static_cast<USHORT>(GET_ISO_URB_SIZE(S->Packets));
    iso->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    iso->Hdr.Status = USBD_STATUS_SUCCESS;
    iso->PipeHandle = Pipe;
    iso->TransferFlags = Flags;
    iso->TransferBufferLength = TotalBytes;
    iso->TransferBuffer = nullptr;
    iso->TransferBufferMDL = S->Mdl;
    iso->NumberOfPackets = S->Packets;
}

void XonarIsoOutEngine::FormatFeedbackUrb(Slot* S)
{
    FormatIsoUrb(S, m_Config.FeedbackPipe, USBD_TRANSFER_DIRECTION_IN | USBD_START_ISO_TRANSFER_ASAP,
                 S->Packets * XONAR_FB_PACKET_BYTES);
    for (ULONG i = 0; i < S->Packets; ++i) {
        S->Urb->UrbIsochronousTransfer.IsoPacket[i].Offset = i * XONAR_FB_PACKET_BYTES;
    }
}

ULONG XonarIsoOutEngine::NextPacketFramesLocked()
{
    ULONG frames;
    if (m_FeedbackQ16 != 0) {
        // Accumulateur 16.16 : la partie fractionnaire du feedback se reporte d'une
        // microtrame à l'autre, ce qui reproduit exactement la cadence de la carte.
        m_AccQ16 += m_FeedbackQ16;
        frames = m_AccQ16 >> 16;
        m_AccQ16 &= 0xFFFF;
    } else {
        // Pas de feedback valide : cadence nominale exacte (44100 Hz = 5,5125 trames).
        m_NominalAcc += m_Config.SampleRate;
        frames = m_NominalAcc / kMicroframesPerSecond;
        m_NominalAcc %= kMicroframesPerSecond;
    }
    if (frames < m_MinFrames || frames > m_MaxFrames) {
        frames = frames < m_MinFrames ? m_MinFrames : m_MaxFrames;
        InterlockedIncrement(&m_StatClamped);
    }
    return frames;
}

ULONG XonarIsoOutEngine::CopyFromRingLocked(PUCHAR Dest, ULONG Bytes)
{
    if (m_Ring == nullptr || m_RingSize == 0) {
        RtlZeroMemory(Dest, Bytes); // silence (PCM signé)
        return 0;
    }
    ULONG remaining = Bytes;
    while (remaining != 0) {
        const ULONG chunk = remaining < m_RingSize - m_ReadPos ? remaining : m_RingSize - m_ReadPos;
        RtlCopyMemory(Dest, m_Ring + m_ReadPos, chunk);
        Dest += chunk;
        remaining -= chunk;
        m_ReadPos += chunk;
        if (m_ReadPos >= m_RingSize) m_ReadPos = 0; // rebouclage
    }
    m_Linear += Bytes; // position linéaire de préchargement (diagnostic uniquement)
    // La notification WaveRT n'est PAS signalée ici : le préchargement précède la lecture
    // réelle d'environ 8 ms (8 URB en vol). Réveiller le moteur audio sur le préchargement
    // le ferait réécrire une période déjà copiée dans les URB en vol (grésillement en mode
    // partagé, constaté le 28/10/2026 sur matériel réel). L'événement part de la position
    // JOUÉE, dans AdvancePlayedLocked, cohérent avec GetPosition.
    return Bytes;
}

void XonarIsoOutEngine::FillDataUrbLocked(Slot* S)
{
    FormatIsoUrb(S, m_Config.DataPipe, USBD_TRANSFER_DIRECTION_OUT | USBD_START_ISO_TRANSFER_ASAP, 0);
    // Pour un OUT, la taille de chaque paquet découle des offsets successifs ; le
    // dernier va jusqu'à TransferBufferLength.
    ULONG offset = 0;
    for (ULONG i = 0; i < S->Packets; ++i) {
        S->Urb->UrbIsochronousTransfer.IsoPacket[i].Offset = offset;
        offset += NextPacketFramesLocked() * m_Config.BlockAlign;
    }
    NT_ASSERT(offset <= S->BufferBytes);
    S->RingBytes = CopyFromRingLocked(S->Buffer, offset);
    S->Urb->UrbIsochronousTransfer.TransferBufferLength = offset;
}

void XonarIsoOutEngine::AdvancePlayedLocked(ULONG Bytes)
{
    if (Bytes == 0) return;
    const ULONGLONG before = m_Played;
    m_Played += Bytes;
    if (m_RingSize != 0) m_PlayPos = static_cast<ULONG>((static_cast<ULONGLONG>(m_PlayPos) + Bytes) % m_RingSize);

    // Notification WaveRT : un événement par période franchie par la position JOUÉE (octets
    // des URB terminés). C'est le curseur que GetPosition rapporte en PlayOffset ; le moteur
    // audio remplit alors la période réellement libérée, jamais une période encore en vol.
    if (m_PeriodBytes != 0) {
        const ULONGLONG crossed = m_Played / m_PeriodBytes - before / m_PeriodBytes;
        if (crossed != 0) {
            for (const PKEVENT e : m_Events) {
                if (e != nullptr) KeSetEvent(e, IO_NO_INCREMENT, FALSE);
            }
            if (crossed > 1) {
                const LONG n = InterlockedIncrement(&m_StatMultiPeriod);
                if (ShouldLog(n)) {
                    XONARSE_WARN("wave: %llu périodes franchies par un URB (%ld fois)", crossed, n);
                }
            }
        }
    }
}

void XonarIsoOutEngine::RevertToNominalLocked()
{
    if (m_FeedbackQ16 == 0) return;
    m_FeedbackQ16 = 0;
    m_NominalAcc = 0;
    m_FbShiftKnown = false;
}

void XonarIsoOutEngine::ProcessFeedbackLocked(ULONG Raw)
{
    if (Raw == 0) return; // la carte n'a pas encore de mesure

    // Détection du format : la norme impose 16.16 en High Speed, mais certains
    // périphériques envoient du 10.14 (format Full Speed) ou une valeur par milliseconde.
    // On cherche le décalage qui ramène la valeur près de la nominale (+50 % / -25 %).
    if (!m_FbShiftKnown) {
        ULONGLONG f = Raw;
        LONG shift = 0;
        while (f < m_NominalQ16 - m_NominalQ16 / 4 && shift < 8) {
            f <<= 1;
            ++shift;
        }
        while (f > m_NominalQ16 + m_NominalQ16 / 2 && shift > -8) {
            f >>= 1;
            --shift;
        }
        // Message seulement quand le décalage change (pas de répétition à DISPATCH_LEVEL).
        if (shift != m_FbShift) {
            XONARSE_WARN("wave: feedback 0x%08lX, décalage %ld (16.16 attendu)", Raw, shift);
        }
        m_FbShift = shift;
        m_FbShiftKnown = true;
    }
    const ULONGLONG value = m_FbShift >= 0 ? static_cast<ULONGLONG>(Raw) << m_FbShift
                                           : static_cast<ULONGLONG>(Raw) >> (-m_FbShift);
    // Plage plausible : nominale +/- 12,5 %. Au-delà, retour à la cadence nominale (pas de
    // valeur figée) et nouvelle détection du format au prochain paquet.
    if (value >= m_NominalQ16 - m_NominalQ16 / 8 && value <= m_NominalQ16 + m_NominalQ16 / 8) {
        m_FeedbackQ16 = static_cast<ULONG>(value);
        m_FbEmptyUrbs = 0;
        InterlockedIncrement(&m_StatFbValues);
    } else {
        RevertToNominalLocked();
        m_FbShiftKnown = false;
        const LONG n = InterlockedIncrement(&m_StatFbRejected);
        if (ShouldLog(n)) {
            XONARSE_WARN("wave: feedback rejeté 0x%08lX (nominal 0x%08lX), cadence nominale, %ld rejets", Raw,
                         m_NominalQ16, n);
        }
    }
}

void XonarIsoOutEngine::EnqueueLocked(Slot* S)
{
    NT_ASSERT(m_QueueCount < XONAR_ISO_DATA_URBS);
    m_Queue[(m_QueueHead + m_QueueCount) % XONAR_ISO_DATA_URBS] = S;
    ++m_QueueCount;
    S->State = SlotQueued;
}

bool XonarIsoOutEngine::ArmRecoveryLocked()
{
    // L'appelant tient une référence : l'incrément ne peut pas repartir de zéro.
    if (m_WorkQueued || m_WorkItem == nullptr || m_Stopping) return false;
    m_WorkQueued = true;
    InterlockedIncrement(&m_Outstanding); // rendu à la fin de RunRecovery
    return true;
}

void XonarIsoOutEngine::QueueRecoveryWork()
{
    IoQueueWorkItem(m_WorkItem, RecoveryWorkRoutine, DelayedWorkQueue, this);
}

void XonarIsoOutEngine::SubmitSlot(Slot* S)
{
    IoReuseIrp(S->Irp, STATUS_SUCCESS);
    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(S->Irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    if (S->UsbdAllocated) {
        // URB alloué par USBD_IsochUrbAllocate : attachement obligatoire par la
        // bibliothèque USBD (exigence Microsoft), jamais à la main.
        USBD_AssignUrbToIoStackLocation(m_UsbdHandle, next, S->Urb);
    } else {
        // URB alloué en pool (repli) : attachement classique, USBD_Assign... interdit.
        next->Parameters.Others.Argument1 = S->Urb;
    }
    IoSetCompletionRoutine(S->Irp, S->Feedback ? FeedbackCompletion : DataCompletion, S, TRUE, TRUE, TRUE);
    // Un échec synchrone passe aussi par la routine de complétion, qui se contente alors
    // de marquer l'emplacement (aucune resoumission récursive).
    (void)IoCallDriver(m_Lower, S->Irp);
}

void XonarIsoOutEngine::Pump()
{
    // À DISPATCH_LEVEL pendant toute la propriété : un propriétaire PASSIVE (Start,
    // élément de travail) ne peut pas être préempté en laissant la file en attente.
    KIRQL oldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
    if (InterlockedCompareExchange(&m_PumpOwner, 1, 0) == 0) {
        for (;;) {
            KeAcquireSpinLockAtDpcLevel(&m_Lock);
            if (m_QueueCount == 0) {
                // Libération sous verrou : tout emplacement enfilé après ce point verra
                // le drapeau à 0 et prendra la propriété lui-même.
                InterlockedExchange(&m_PumpOwner, 0);
                KeReleaseSpinLockFromDpcLevel(&m_Lock);
                break;
            }
            Slot* s = m_Queue[m_QueueHead];
            m_QueueHead = (m_QueueHead + 1) % XONAR_ISO_DATA_URBS;
            --m_QueueCount;
            if (m_Stopping) {
                s->State = SlotIdle;
                KeReleaseSpinLockFromDpcLevel(&m_Lock);
                ReleaseOutstanding(); // jamais la dernière : l'appelant tient une référence
                continue;
            }
            FillDataUrbLocked(s);
            s->State = SlotInFlight;
            ++m_DataInFlight;
            KeReleaseSpinLockFromDpcLevel(&m_Lock);
            SubmitSlot(s); // jamais sous m_Lock
        }
    }
    KeLowerIrql(oldIrql);
}

XonarIsoOutEngine::Outcome XonarIsoOutEngine::Classify(const Slot* S, NTSTATUS IrpStatus, USBD_STATUS UrbStatus)
{
    if (IsDeviceGoneStatus(IrpStatus) || UrbStatus == USBD_STATUS_DEVICE_GONE || m_Usb->IsSurpriseRemoved()) {
        return Outcome::Gone;
    }
    if (NT_SUCCESS(IrpStatus)) return Outcome::Ok;
    // Toute autre erreur (y compris STATUS_CANCELLED hors arrêt) est passagère.
    const LONG n = InterlockedIncrement(&m_StatUrbErrors);
    if (ShouldLog(n)) {
        XONARSE_WARN("wave: URB %s en erreur : IRP 0x%08X, URB 0x%08X (%ld erreurs)", S->Feedback ? "feedback" : "donnees",
                     IrpStatus, UrbStatus, n);
    }
    if (IsLateStatus(UrbStatus)) InterlockedIncrement(&m_StatLatePackets);
    return Outcome::Transient;
}

void XonarIsoOutEngine::OnDataCompleted(Slot* S, NTSTATUS IrpStatus)
{
    InterlockedIncrement(&m_Outstanding); // garde, rendue en dernier

    const struct _URB_ISOCH_TRANSFER* iso = &S->Urb->UrbIsochronousTransfer;
    InterlockedIncrement(&m_StatDataUrbs);
    if (iso->ErrorCount != 0) {
        for (ULONG i = 0; i < iso->NumberOfPackets && i < S->Packets; ++i) {
            const USBD_STATUS ps = iso->IsoPacket[i].Status;
            if (!USBD_SUCCESS(ps)) {
                const LONG n = InterlockedIncrement(&m_StatPacketErrors);
                if (IsLateStatus(ps)) InterlockedIncrement(&m_StatLatePackets);
                if (ShouldLog(n)) XONARSE_WARN("wave: paquet OUT en erreur 0x%08X (%ld paquets)", ps, n);
            }
        }
    }
    const Outcome outcome = Classify(S, IrpStatus, iso->Hdr.Status);

    bool requeued = false;
    bool queueWork = false;
    bool starved = false;
    bool gone = false;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    NT_ASSERT(m_DataInFlight != 0);
    --m_DataInFlight;
    // Les octets prélevés pour cet URB ont quitté le tampon : joués ou perdus.
    AdvancePlayedLocked(S->RingBytes);
    S->RingBytes = 0;
    if (outcome == Outcome::Gone) m_DeviceGone = true;
    if (m_Stopping || m_DeviceGone) {
        S->State = SlotIdle;
    } else if (outcome == Outcome::Transient) {
        S->State = SlotLost;
        m_LostData |= 1UL << S->Index;
        InterlockedIncrement(&m_StatSlotsLost);
        queueWork = ArmRecoveryLocked();
    } else {
        m_DataRounds = 0;
        m_StarvedLogged = false;
        EnqueueLocked(S);
        requeued = true;
    }
    if (!m_Stopping && m_DataInFlight == 0 && m_QueueCount == 0 && !m_StarvedLogged) {
        m_StarvedLogged = true;
        starved = true;
        gone = m_DeviceGone;
    }
    KeReleaseSpinLock(&m_Lock, irql);

    if (starved) {
        XONARSE_ERROR("wave: plus aucun URB de données en vol (%s), notifications suspendues",
                      gone ? "périphérique absent" : "relance en attente");
    }
    if (queueWork) QueueRecoveryWork();
    if (requeued) {
        Pump(); // la référence de l'emplacement suit l'emplacement dans la file
    } else {
        ReleaseOutstanding(); // référence de l'emplacement (la garde est encore tenue)
    }
    ReleaseOutstanding(); // garde : dernier accès à l'objet
}

void XonarIsoOutEngine::OnFeedbackCompleted(Slot* S, NTSTATUS IrpStatus)
{
    InterlockedIncrement(&m_Outstanding); // garde, rendue en dernier

    const struct _URB_ISOCH_TRANSFER* iso = &S->Urb->UrbIsochronousTransfer;
    const Outcome outcome = Classify(S, IrpStatus, iso->Hdr.Status);

    bool resubmit = false;
    bool queueWork = false;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    NT_ASSERT(m_FbInFlight != 0);
    --m_FbInFlight;
    if (outcome == Outcome::Gone) m_DeviceGone = true;
    if (m_Stopping || m_DeviceGone) {
        S->State = SlotIdle;
        if (m_FbInFlight == 0) RevertToNominalLocked();
    } else if (outcome == Outcome::Transient) {
        S->State = SlotLost;
        m_LostFb |= 1UL << S->Index;
        InterlockedIncrement(&m_StatSlotsLost);
        // Plus aucune mesure en route : cadence nominale jusqu'au retour du feedback.
        if (m_FbInFlight == 0) RevertToNominalLocked();
        queueWork = ArmRecoveryLocked();
    } else {
        m_FbRounds = 0;
        bool valid = false;
        for (ULONG i = 0; i < iso->NumberOfPackets && i < S->Packets; ++i) {
            const USBD_ISO_PACKET_DESCRIPTOR& p = iso->IsoPacket[i];
            if (!USBD_SUCCESS(p.Status) || p.Length < 3 || p.Offset + 4 > S->BufferBytes) continue;
            const PUCHAR b = S->Buffer + p.Offset;
            // Petit-boutiste ; 3 octets = format Full Speed 10.14 (corrigé par la détection).
            ULONG raw = static_cast<ULONG>(b[0]) | (static_cast<ULONG>(b[1]) << 8) | (static_cast<ULONG>(b[2]) << 16);
            if (p.Length >= 4) raw |= static_cast<ULONG>(b[3]) << 24;
            ProcessFeedbackLocked(raw);
            valid = true;
        }
        // Feedback muet trop longtemps : on ne garde pas une valeur figée.
        if (!valid && ++m_FbEmptyUrbs >= kMaxFeedbackEmptyUrbs && m_FeedbackQ16 != 0) {
            RevertToNominalLocked();
            XONARSE_WARN("wave: feedback muet depuis %lu URB, cadence nominale", m_FbEmptyUrbs);
        }
        S->State = SlotInFlight;
        ++m_FbInFlight;
        resubmit = true;
    }
    KeReleaseSpinLock(&m_Lock, irql);

    if (queueWork) QueueRecoveryWork();
    if (resubmit) {
        // L'ordre des URB IN de feedback est sans importance : resoumission directe.
        FormatFeedbackUrb(S);
        SubmitSlot(S); // la référence de l'emplacement repart avec lui
    } else {
        ReleaseOutstanding(); // référence de l'emplacement
    }
    ReleaseOutstanding(); // garde : dernier accès à l'objet
}

NTSTATUS XonarIsoOutEngine::DataCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Slot* s = static_cast<Slot*>(Context);
    s->Engine->OnDataCompleted(s, Irp->IoStatus.Status);
    // L'IRP nous appartient (IoAllocateIrp) : il est réutilisé ou libéré par Unprepare.
    return STATUS_MORE_PROCESSING_REQUIRED;
}

NTSTATUS XonarIsoOutEngine::FeedbackCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Slot* s = static_cast<Slot*>(Context);
    s->Engine->OnFeedbackCompleted(s, Irp->IoStatus.Status);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// ---------------------------------------------------------------------------
// Relance des URB perdus (PASSIVE_LEVEL, élément de travail). Non paginé : prend m_Lock.
// ---------------------------------------------------------------------------

void XonarIsoOutEngine::RecoveryWorkRoutine(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    static_cast<XonarIsoOutEngine*>(Context)->RunRecovery();
}

void XonarIsoOutEngine::RunRecovery()
{
    // m_WorkQueued reste vrai pendant toute la routine : les pertes survenues pendant le
    // traitement sont reprises par la boucle au lieu de remettre l'élément en file.
    for (;;) {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        if (m_Stopping || m_DeviceGone) {
            m_WorkQueued = false;
            KeReleaseSpinLock(&m_Lock, irql);
            break;
        }
        const bool dataBudget = m_DataRounds < kMaxRecoveryRounds;
        const bool fbBudget = m_FbRounds < kMaxRecoveryRounds;
        const ULONG lostData = dataBudget ? m_LostData : 0;
        const ULONG lostFb = fbBudget ? m_LostFb : 0;
        // Abandon (budget épuisé sans aucun succès sur le pipe) : tracé une seule fois par RUN.
        ULONG abandonedData = 0;
        bool abandonedFb = false;
        if (!dataBudget && m_LostData != 0 && !m_DataAbandonLogged) {
            m_DataAbandonLogged = true;
            abandonedData = BitCount(m_LostData);
        }
        if (!fbBudget && m_LostFb != 0 && !m_FbAbandonLogged) {
            m_FbAbandonLogged = true;
            abandonedFb = true;
        }
        const bool done = lostData == 0 && lostFb == 0;
        if (done) m_WorkQueued = false;
        KeReleaseSpinLock(&m_Lock, irql);
        if (abandonedData != 0) {
            XONARSE_ERROR("wave: relance abandonnée après %lu tentatives : %lu URB de données perdus, flux muet",
                          kMaxRecoveryRounds, abandonedData);
        }
        if (abandonedFb) {
            XONARSE_ERROR("wave: relance du feedback abandonnée après %lu tentatives, cadence nominale",
                          kMaxRecoveryRounds);
        }
        if (done) break;
        KeAcquireSpinLock(&m_Lock, &irql);
        if (m_Stopping || m_DeviceGone) {
            m_WorkQueued = false;
            KeReleaseSpinLock(&m_Lock, irql);
            break;
        }
        ULONG round = 0;
        if (lostData != 0) round = ++m_DataRounds;
        if (lostFb != 0) {
            ++m_FbRounds;
            if (m_FbRounds > round) round = m_FbRounds;
        }
        // Remise en état du pipe seulement s'il n'a plus rien en vol (une réinitialisation
        // avec des transferts en cours les annulerait).
        const bool resetData = lostData != 0 && m_DataInFlight == 0 && m_QueueCount == 0;
        const bool resetFb = lostFb != 0 && m_FbInFlight == 0;
        KeReleaseSpinLock(&m_Lock, irql);

        if (round > 1) {
            const ULONG shift = round - 2 < 6 ? round - 2 : 6;
            ULONG delayMs = 1UL << shift;
            if (delayMs > kMaxBackoffMs) delayMs = kMaxBackoffMs;
            LARGE_INTEGER delay;
            delay.QuadPart = -static_cast<LONGLONG>(delayMs) * 10 * 1000;
            KeDelayExecutionThread(KernelMode, FALSE, &delay);
        }

        bool gone = false;
        if (resetData) {
            const NTSTATUS s = m_Usb->ResetPipe(m_Config.DataPipe);
            InterlockedIncrement(&m_StatPipeResets);
            if (IsDeviceGoneStatus(s)) gone = true;
            else if (!NT_SUCCESS(s)) XONARSE_WARN("wave: reset du pipe de données : 0x%08X", s);
        }
        if (resetFb && !gone) {
            const NTSTATUS s = m_Usb->ResetPipe(m_Config.FeedbackPipe);
            InterlockedIncrement(&m_StatPipeResets);
            if (IsDeviceGoneStatus(s)) gone = true;
            else if (!NT_SUCCESS(s)) XONARSE_WARN("wave: reset du pipe de feedback : 0x%08X", s);
        }
        if (m_Usb->IsSurpriseRemoved()) gone = true;

        ULONG fbToSubmit = 0;
        ULONG dataQueued = 0;
        KeAcquireSpinLock(&m_Lock, &irql);
        if (gone) m_DeviceGone = true;
        if (!m_Stopping && !m_DeviceGone) {
            // Données : retour dans la file de la pompe (remplissage frais, dans l'ordre).
            for (ULONG i = 0; i < XONAR_ISO_DATA_URBS; ++i) {
                const ULONG bit = 1UL << i;
                if ((lostData & bit) == 0 || (m_LostData & bit) == 0) continue;
                m_LostData &= ~bit;
                InterlockedIncrement(&m_Outstanding);
                EnqueueLocked(&m_Data[i]);
                ++dataQueued;
            }
            for (ULONG i = 0; i < m_FbSlotCount; ++i) {
                const ULONG bit = 1UL << i;
                if ((lostFb & bit) == 0 || (m_LostFb & bit) == 0) continue;
                m_LostFb &= ~bit;
                InterlockedIncrement(&m_Outstanding);
                m_Fb[i].State = SlotInFlight;
                ++m_FbInFlight;
                fbToSubmit |= bit;
            }
        }
        KeReleaseSpinLock(&m_Lock, irql);

        if (gone) {
            XONARSE_WARN("wave: périphérique absent, relance annulée");
            continue; // la boucle sort sur m_DeviceGone
        }
        if (dataQueued != 0 || fbToSubmit != 0) {
            XONARSE_INFO("wave: relance de %lu URB données, %lu URB feedback (tentative %lu%s)", dataQueued,
                         BitCount(fbToSubmit), round, resetData || resetFb ? ", pipe réinitialisé" : "");
            InterlockedExchangeAdd(&m_StatResubmitted, static_cast<LONG>(dataQueued + BitCount(fbToSubmit)));
        }
        for (ULONG i = 0; i < m_FbSlotCount; ++i) {
            if ((fbToSubmit & (1UL << i)) == 0) continue;
            FormatFeedbackUrb(&m_Fb[i]);
            SubmitSlot(&m_Fb[i]);
        }
        if (dataQueued != 0) Pump(); // la référence de l'élément de travail tient l'objet
    }
    ReleaseOutstanding(); // référence de l'élément de travail : dernier accès à l'objet
}
