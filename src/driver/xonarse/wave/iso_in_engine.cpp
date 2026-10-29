// Moteur isochrone IN asynchrone pour la capture (voir iso_in_engine.h).
//
// IRQL : Prepare / Unprepare paginables. Tout ce qui prend m_Lock, les complétions,
// l'élément de travail de relance et Start (soumission à DISPATCH_LEVEL) reste non paginé.
//
// Références m_Outstanding : 1 par emplacement en vol ou perdu, 1 pour l'élément de travail
// en file ou en cours, 1 « garde » par complétion en cours, plus le biais de Start. Chaque
// routine rend sa garde en dernier : Stop peut détruire l'objet dès le réveil de m_Idle.
#include "iso_in_engine.h"

static_assert(XONAR_ISO_IN_URBS <= 32, "masque de relance sur 32 bits");

namespace {

constexpr ULONG kMicroframesPerSecond = 8000;
constexpr LONGLONG kStopTimeoutMs = 500;
constexpr ULONG kMaxRecoveryRounds = 40;
constexpr ULONG kMaxBackoffMs = 64;

bool ShouldLog(LONG Count)
{
    return Count == 1 || Count == 10 || Count == 100 || Count == 1000 || (Count % 10000) == 0;
}

bool IsLateStatus(USBD_STATUS Status)
{
    return Status == USBD_STATUS_ISO_NOT_ACCESSED_LATE || Status == USBD_STATUS_ISO_NA_LATE_USBPORT ||
           Status == USBD_STATUS_ISO_NOT_ACCESSED_BY_HW || Status == USBD_STATUS_BAD_START_FRAME;
}

bool IsDeviceGoneStatus(NTSTATUS Status)
{
    return Status == STATUS_NO_SUCH_DEVICE || Status == STATUS_DEVICE_NOT_CONNECTED ||
           Status == STATUS_DELETE_PENDING || Status == STATUS_DEVICE_REMOVED;
}

ULONG BitCount(ULONG Mask)
{
    ULONG n = 0;
    for (; Mask != 0; Mask &= Mask - 1) ++n;
    return n;
}

} // namespace

XonarIsoInEngine::XonarIsoInEngine()
{
    KeInitializeSpinLock(&m_Lock);
    KeInitializeEvent(&m_Idle, NotificationEvent, TRUE);
}

XonarIsoInEngine::~XonarIsoInEngine()
{
    NT_ASSERT(!m_Running);
    NT_ASSERT(!m_Prepared);
}

ULONG XonarIsoInEngine::MaxInFlightBytes(ULONG SampleRate, USHORT BlockAlign)
{
    const ULONG maxFrames = (SampleRate + kMicroframesPerSecond - 1) / kMicroframesPerSecond + 1;
    return XONAR_ISO_IN_URBS * XONAR_ISO_IN_PACKETS_PER_URB * maxFrames * BlockAlign;
}

#pragma code_seg("PAGE")

NTSTATUS XonarIsoInEngine::AllocateSlot(Slot* S, ULONG Index, ULONG Packets, ULONG BufferBytes)
{
    PAGED_CODE();
    RtlZeroMemory(S, sizeof(*S));
    S->Engine = this;
    S->Index = Index;
    S->Packets = Packets;
    S->BufferBytes = BufferBytes;
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
            XONARSE_WARN("capture: USBD_IsochUrbAllocate 0x%08X, repli ExAllocatePool2", status);
        }
    }
    if (S->Urb == nullptr) {
        S->Urb = static_cast<PURB>(ExAllocatePool2(POOL_FLAG_NON_PAGED, GET_ISO_URB_SIZE(Packets), XONARSE_POOL_TAG));
        if (S->Urb == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    }

    S->Buffer = static_cast<PUCHAR>(ExAllocatePool2(POOL_FLAG_NON_PAGED, BufferBytes, XONARSE_POOL_TAG));
    if (S->Buffer == nullptr) return STATUS_INSUFFICIENT_RESOURCES;

    S->Mdl = IoAllocateMdl(S->Buffer, BufferBytes, FALSE, FALSE, nullptr);
    if (S->Mdl == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    MmBuildMdlForNonPagedPool(S->Mdl);
    return STATUS_SUCCESS;
}

void XonarIsoInEngine::FreeSlot(Slot* S)
{
    PAGED_CODE();
    if (S->Mdl != nullptr) IoFreeMdl(S->Mdl);
    if (S->Buffer != nullptr) ExFreePoolWithTag(S->Buffer, XONARSE_POOL_TAG);
    if (S->Urb != nullptr) {
        if (S->UsbdAllocated) {
            if (m_Usb != nullptr && m_Usb->UsbdHandle() == m_UsbdHandle) {
                USBD_UrbFree(m_UsbdHandle, S->Urb);
            } else {
                XONARSE_ERROR("capture: poignée USBD fermée avant la libération des URB (ordre de retrait)");
            }
        } else {
            ExFreePoolWithTag(S->Urb, XONARSE_POOL_TAG);
        }
    }
    if (S->Irp != nullptr) IoFreeIrp(S->Irp);
    RtlZeroMemory(S, sizeof(*S));
}

NTSTATUS XonarIsoInEngine::Prepare(XonarUsbDevice* Usb, const XonarIsoInConfig& Config)
{
    PAGED_CODE();
    if (m_Prepared || m_Running) return STATUS_INVALID_DEVICE_STATE;
    if (Usb == nullptr || Config.DataPipe == nullptr || Config.BlockAlign == 0 || Config.SampleRate == 0 ||
        Config.MaxPacketBytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    // Contrairement au rendu, la position de capture est pilotée par les octets RÉELLEMENT
    // reçus (chaque descripteur de paquet rend sa longueur) : un bInterval différent de 1
    // ne fausse rien, il ne change que la latence. On avertit sans refuser le flux.
    if (Config.DataInterval != XONAR_ISO_IN_DATA_INTERVAL) {
        XONARSE_WARN("capture: bInterval de données %u (attendu %u) : latence accrue, cadence inchangée",
                     Config.DataInterval, XONAR_ISO_IN_DATA_INTERVAL);
    }
    PDEVICE_OBJECT lower = Usb->LowerDevice();
    if (lower == nullptr) {
        XONARSE_ERROR("capture: aucun objet USB inférieur, streaming impossible");
        return STATUS_DEVICE_NOT_READY;
    }

    const ULONG maxPacketFrames = Config.MaxPacketBytes / Config.BlockAlign;
    const ULONG nominalCeil = (Config.SampleRate + kMicroframesPerSecond - 1) / kMicroframesPerSecond;
    if (maxPacketFrames < nominalCeil) {
        XONARSE_ERROR("capture: %lu Hz x %u octets ne tient pas dans wMaxPacketSize %u", Config.SampleRate,
                      Config.BlockAlign, Config.MaxPacketBytes);
        return STATUS_NOT_SUPPORTED;
    }

    m_Usb = Usb;
    m_Lower = lower;
    m_UsbdHandle = Usb->UsbdHandle();
    m_Config = Config;
    // Majorant EXACT des octets en vol : N URB x paquets x wMaxPacketSize (indépendant du
    // bInterval). Sert à dimensionner le plancher du tampon cyclique côté flux.
    m_InFlightBytes = XONAR_ISO_IN_URBS * XONAR_ISO_IN_PACKETS_PER_URB * Config.MaxPacketBytes;
    m_Prepared = true;

    NTSTATUS status = STATUS_SUCCESS;
    PDEVICE_OBJECT fdo = Usb->FunctionalDevice();
    if (fdo != nullptr) {
        m_WorkItem = IoAllocateWorkItem(fdo);
        if (m_WorkItem == nullptr) status = STATUS_INSUFFICIENT_RESOURCES;
    } else {
        XONARSE_WARN("capture: aucun FDO, les URB perdus ne seront pas relancés");
    }

    // Chaque URB reçoit jusqu'à MaxPacketBytes par paquet (le périphérique décide).
    const ULONG dataBytes = XONAR_ISO_IN_PACKETS_PER_URB * Config.MaxPacketBytes;
    for (ULONG i = 0; i < XONAR_ISO_IN_URBS && NT_SUCCESS(status); ++i) {
        status = AllocateSlot(&m_Data[i], i, XONAR_ISO_IN_PACKETS_PER_URB, dataBytes);
    }

    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("capture: allocation des URB : 0x%08X", status);
        Unprepare();
        return status;
    }
    XONARSE_INFO("capture: moteur prêt : %lu Hz, %u o/trame, %u o/paquet max, %lu URB x %lu paquets", Config.SampleRate,
                 Config.BlockAlign, Config.MaxPacketBytes, XONAR_ISO_IN_URBS, XONAR_ISO_IN_PACKETS_PER_URB);
    return STATUS_SUCCESS;
}

void XonarIsoInEngine::Unprepare()
{
    PAGED_CODE();
    NT_ASSERT(!m_Running);
    if (!m_Prepared) return;
    for (auto& s : m_Data) FreeSlot(&s);
    if (m_WorkItem != nullptr) {
        IoFreeWorkItem(m_WorkItem);
        m_WorkItem = nullptr;
    }
    m_Prepared = false;
    m_Usb = nullptr;
    m_Lower = nullptr;
    m_UsbdHandle = nullptr;
}

void XonarIsoInEngine::LogStatistics(const char* When)
{
    PAGED_CODE();
    XONARSE_INFO("capture: %s : URB %ld (err %ld, relances %ld/%ld, resets %ld), paquets err %ld (retard %ld), %ld Kio reçus",
                 When, m_StatDataUrbs, m_StatUrbErrors, m_StatResubmitted, m_StatSlotsLost, m_StatPipeResets,
                 m_StatPacketErrors, m_StatLatePackets, m_StatBytes);
}

#pragma code_seg()

// ---------------------------------------------------------------------------
// Chemin non paginé
// ---------------------------------------------------------------------------

void XonarIsoInEngine::SetAudioBuffer(PUCHAR Buffer, ULONG Size, ULONG NotificationCount)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Ring = (Buffer != nullptr && Size != 0) ? Buffer : nullptr;
    m_RingSize = m_Ring != nullptr ? Size : 0;
    m_PeriodBytes = (m_Ring != nullptr && NotificationCount != 0) ? Size / NotificationCount : 0;
    if (m_WritePos >= m_RingSize) m_WritePos = 0;
    KeReleaseSpinLock(&m_Lock, irql);
}

void XonarIsoInEngine::GetPosition(ULONG* WritePos, ULONGLONG* CapturedBytes)
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    *WritePos = m_WritePos;
    if (CapturedBytes != nullptr) *CapturedBytes = m_Captured;
    KeReleaseSpinLock(&m_Lock, irql);
}

void XonarIsoInEngine::ResetPosition()
{
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_WritePos = 0;
    m_Captured = 0;
    KeReleaseSpinLock(&m_Lock, irql);
}

NTSTATUS XonarIsoInEngine::RegisterNotificationEvent(PKEVENT Event)
{
    if (Event == nullptr) return STATUS_INVALID_PARAMETER;
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    for (const PKEVENT e : m_Events) {
        if (e == Event) {
            status = STATUS_UNSUCCESSFUL;
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

NTSTATUS XonarIsoInEngine::UnregisterNotificationEvent(PKEVENT Event)
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

NTSTATUS XonarIsoInEngine::Start()
{
    if (!m_Prepared) return STATUS_INVALID_DEVICE_STATE;
    if (m_Running) return STATUS_SUCCESS;

    m_StatDataUrbs = m_StatUrbErrors = m_StatPacketErrors = m_StatLatePackets = 0;
    m_StatSlotsLost = m_StatResubmitted = m_StatPipeResets = m_StatBytes = 0;
    // Biais rendu par Stop + une référence par emplacement amorcé.
    m_Outstanding = 1 + XONAR_ISO_IN_URBS;
    KeClearEvent(&m_Idle);

    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Stopping = false;
    m_DeviceGone = false;
    m_StarvedLogged = false;
    m_LostData = 0;
    m_WorkQueued = false;
    m_DataRounds = 0;
    m_DataAbandonLogged = false;
    for (auto& s : m_Data) s.State = SlotInFlight;
    m_DataInFlight = XONAR_ISO_IN_URBS;
    KeReleaseSpinLock(&m_Lock, irql);
    m_Running = true;

    // Amorçage à DISPATCH_LEVEL : les URB IN partent en quelques microsecondes.
    KIRQL oldIrql;
    KeRaiseIrql(DISPATCH_LEVEL, &oldIrql);
    for (auto& s : m_Data) {
        FormatInUrb(&s);
        SubmitSlot(&s);
    }
    KeLowerIrql(oldIrql);
    XONARSE_INFO("capture: moteur démarré (%lu URB IN)", XONAR_ISO_IN_URBS);
    return STATUS_SUCCESS;
}

void XonarIsoInEngine::Stop()
{
    if (!m_Running) return;

    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    m_Stopping = true;
    KeReleaseSpinLock(&m_Lock, irql);
    ReleaseOutstanding(); // rend le biais de Start

    LARGE_INTEGER timeout;
    timeout.QuadPart = -kStopTimeoutMs * 10 * 1000;
    NTSTATUS status = KeWaitForSingleObject(&m_Idle, Executive, KernelMode, FALSE, &timeout);
    if (status == STATUS_TIMEOUT) {
        XONARSE_WARN("capture: URB encore en vol après %lld ms (%ld), AbortPipe", kStopTimeoutMs, m_Outstanding);
        (void)m_Usb->AbortPipe(m_Config.DataPipe);
        timeout.QuadPart = -5000LL * 10 * 1000;
        while (KeWaitForSingleObject(&m_Idle, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
            XONARSE_ERROR("capture: %ld URB toujours en vol après AbortPipe", m_Outstanding);
        }
    }
    m_Running = false;
    LogStatistics("arrêt");
}

void XonarIsoInEngine::ReleaseOutstanding()
{
    if (InterlockedDecrement(&m_Outstanding) == 0) {
        KeSetEvent(&m_Idle, IO_NO_INCREMENT, FALSE);
    }
}

void XonarIsoInEngine::FormatInUrb(Slot* S)
{
    struct _URB_ISOCH_TRANSFER* iso = &S->Urb->UrbIsochronousTransfer;
    const SIZE_T head = FIELD_OFFSET(struct _URB_ISOCH_TRANSFER, PipeHandle);
    const SIZE_T size = sizeof(struct _URB_ISOCH_TRANSFER) + (S->Packets - 1) * sizeof(USBD_ISO_PACKET_DESCRIPTOR);
    RtlZeroMemory(reinterpret_cast<PUCHAR>(iso) + head, size - head);
    iso->Hdr.Length = static_cast<USHORT>(GET_ISO_URB_SIZE(S->Packets));
    iso->Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
    iso->Hdr.Status = USBD_STATUS_SUCCESS;
    iso->PipeHandle = m_Config.DataPipe;
    iso->TransferFlags = USBD_TRANSFER_DIRECTION_IN | USBD_START_ISO_TRANSFER_ASAP;
    iso->TransferBufferLength = S->Packets * m_Config.MaxPacketBytes;
    iso->TransferBuffer = nullptr;
    iso->TransferBufferMDL = S->Mdl;
    iso->NumberOfPackets = S->Packets;
    // Un paquet IN par microtrame ; chaque descripteur part à un offset d'un wMaxPacketSize,
    // la pile USB y écrit la longueur réellement reçue.
    for (ULONG i = 0; i < S->Packets; ++i) {
        iso->IsoPacket[i].Offset = i * m_Config.MaxPacketBytes;
        iso->IsoPacket[i].Length = 0;
    }
}

void XonarIsoInEngine::CopyToRingLocked(const UCHAR* Src, ULONG Bytes)
{
    if (m_Ring == nullptr || m_RingSize == 0 || Bytes == 0) return; // tampon absent : octets écartés
    ULONG remaining = Bytes;
    while (remaining != 0) {
        const ULONG chunk = remaining < m_RingSize - m_WritePos ? remaining : m_RingSize - m_WritePos;
        RtlCopyMemory(m_Ring + m_WritePos, Src, chunk);
        Src += chunk;
        remaining -= chunk;
        m_WritePos += chunk;
        if (m_WritePos >= m_RingSize) m_WritePos = 0;
    }
}

void XonarIsoInEngine::AdvanceCapturedLocked(ULONG Bytes)
{
    if (Bytes == 0) return;
    const ULONGLONG before = m_Captured;
    m_Captured += Bytes;

    // Notification WaveRT : un événement par période franchie par le curseur d'écriture.
    if (m_PeriodBytes != 0) {
        const ULONGLONG crossed = m_Captured / m_PeriodBytes - before / m_PeriodBytes;
        if (crossed != 0) {
            for (const PKEVENT e : m_Events) {
                if (e != nullptr) KeSetEvent(e, IO_NO_INCREMENT, FALSE);
            }
        }
    }
}

bool XonarIsoInEngine::ArmRecoveryLocked()
{
    if (m_WorkQueued || m_WorkItem == nullptr || m_Stopping) return false;
    m_WorkQueued = true;
    InterlockedIncrement(&m_Outstanding); // rendu à la fin de RunRecovery
    return true;
}

void XonarIsoInEngine::QueueRecoveryWork()
{
    IoQueueWorkItem(m_WorkItem, RecoveryWorkRoutine, DelayedWorkQueue, this);
}

void XonarIsoInEngine::SubmitSlot(Slot* S)
{
    IoReuseIrp(S->Irp, STATUS_SUCCESS);
    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(S->Irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    if (S->UsbdAllocated) {
        USBD_AssignUrbToIoStackLocation(m_UsbdHandle, next, S->Urb);
    } else {
        next->Parameters.Others.Argument1 = S->Urb;
    }
    IoSetCompletionRoutine(S->Irp, DataCompletion, S, TRUE, TRUE, TRUE);
    (void)IoCallDriver(m_Lower, S->Irp);
}

XonarIsoInEngine::Outcome XonarIsoInEngine::Classify(const Slot* S, NTSTATUS IrpStatus, USBD_STATUS UrbStatus)
{
    UNREFERENCED_PARAMETER(S);
    if (IsDeviceGoneStatus(IrpStatus) || UrbStatus == USBD_STATUS_DEVICE_GONE || m_Usb->IsSurpriseRemoved()) {
        return Outcome::Gone;
    }
    if (NT_SUCCESS(IrpStatus)) return Outcome::Ok;
    const LONG n = InterlockedIncrement(&m_StatUrbErrors);
    if (ShouldLog(n)) {
        XONARSE_WARN("capture: URB IN en erreur : IRP 0x%08X, URB 0x%08X (%ld erreurs)", IrpStatus, UrbStatus, n);
    }
    if (IsLateStatus(UrbStatus)) InterlockedIncrement(&m_StatLatePackets);
    return Outcome::Transient;
}

void XonarIsoInEngine::OnDataCompleted(Slot* S, NTSTATUS IrpStatus)
{
    InterlockedIncrement(&m_Outstanding); // garde, rendue en dernier

    const struct _URB_ISOCH_TRANSFER* iso = &S->Urb->UrbIsochronousTransfer;
    InterlockedIncrement(&m_StatDataUrbs);
    const Outcome outcome = Classify(S, IrpStatus, iso->Hdr.Status);

    bool resubmit = false;
    bool queueWork = false;
    bool starved = false;
    bool gone = false;
    KIRQL irql;
    KeAcquireSpinLock(&m_Lock, &irql);
    NT_ASSERT(m_DataInFlight != 0);
    --m_DataInFlight;
    if (outcome == Outcome::Gone) m_DeviceGone = true;
    if (m_Stopping || m_DeviceGone) {
        S->State = SlotIdle;
    } else if (outcome == Outcome::Transient) {
        S->State = SlotLost;
        m_LostData |= 1UL << S->Index;
        InterlockedIncrement(&m_StatSlotsLost);
        queueWork = ArmRecoveryLocked();
    } else {
        // Succès : copie des octets reçus de chaque paquet dans le tampon cyclique.
        ULONG total = 0;
        for (ULONG i = 0; i < iso->NumberOfPackets && i < S->Packets; ++i) {
            const USBD_ISO_PACKET_DESCRIPTOR& p = iso->IsoPacket[i];
            if (!USBD_SUCCESS(p.Status)) {
                const LONG n = InterlockedIncrement(&m_StatPacketErrors);
                if (IsLateStatus(p.Status)) InterlockedIncrement(&m_StatLatePackets);
                if (ShouldLog(n)) XONARSE_WARN("capture: paquet IN en erreur 0x%08X (%ld paquets)", p.Status, n);
                continue;
            }
            if (p.Length == 0) continue;
            if (p.Offset + p.Length > S->BufferBytes) continue; // garde contre un descripteur incohérent
            CopyToRingLocked(S->Buffer + p.Offset, p.Length);
            total += p.Length;
        }
        AdvanceCapturedLocked(total);
        if (total >= 1024) InterlockedExchangeAdd(&m_StatBytes, static_cast<LONG>(total / 1024));
        m_DataRounds = 0;
        m_StarvedLogged = false;
        S->State = SlotInFlight;
        ++m_DataInFlight;
        resubmit = true;
    }
    if (!m_Stopping && m_DataInFlight == 0 && m_LostData == 0 && !m_StarvedLogged) {
        m_StarvedLogged = true;
        starved = true;
        gone = m_DeviceGone;
    }
    KeReleaseSpinLock(&m_Lock, irql);

    if (starved) {
        XONARSE_ERROR("capture: plus aucun URB IN en vol (%s), capture suspendue",
                      gone ? "périphérique absent" : "relance en attente");
    }
    if (queueWork) QueueRecoveryWork();
    if (resubmit) {
        // Ordre : les URB IN sont identiques, resoumission directe (la référence suit l'URB).
        FormatInUrb(S);
        SubmitSlot(S);
    } else {
        ReleaseOutstanding(); // référence de l'emplacement (perdu ou arrêté)
    }
    ReleaseOutstanding(); // garde : dernier accès à l'objet
}

NTSTATUS XonarIsoInEngine::DataCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Slot* s = static_cast<Slot*>(Context);
    s->Engine->OnDataCompleted(s, Irp->IoStatus.Status);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// ---------------------------------------------------------------------------
// Relance des URB perdus (PASSIVE_LEVEL, élément de travail). Non paginé : prend m_Lock.
// ---------------------------------------------------------------------------

void XonarIsoInEngine::RecoveryWorkRoutine(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    static_cast<XonarIsoInEngine*>(Context)->RunRecovery();
}

void XonarIsoInEngine::RunRecovery()
{
    for (;;) {
        KIRQL irql;
        KeAcquireSpinLock(&m_Lock, &irql);
        if (m_Stopping || m_DeviceGone) {
            m_WorkQueued = false;
            KeReleaseSpinLock(&m_Lock, irql);
            break;
        }
        const bool dataBudget = m_DataRounds < kMaxRecoveryRounds;
        const ULONG lostData = dataBudget ? m_LostData : 0;
        ULONG abandonedData = 0;
        if (!dataBudget && m_LostData != 0 && !m_DataAbandonLogged) {
            m_DataAbandonLogged = true;
            abandonedData = BitCount(m_LostData);
        }
        const bool done = lostData == 0;
        if (done) m_WorkQueued = false;
        KeReleaseSpinLock(&m_Lock, irql);
        if (abandonedData != 0) {
            XONARSE_ERROR("capture: relance abandonnée après %lu tentatives : %lu URB perdus, capture muette",
                          kMaxRecoveryRounds, abandonedData);
        }
        if (done) break;

        KeAcquireSpinLock(&m_Lock, &irql);
        if (m_Stopping || m_DeviceGone) {
            m_WorkQueued = false;
            KeReleaseSpinLock(&m_Lock, irql);
            break;
        }
        const ULONG round = ++m_DataRounds;
        // Remise en état du pipe seulement s'il n'a plus rien en vol.
        const bool resetData = m_DataInFlight == 0;
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
            else if (!NT_SUCCESS(s)) XONARSE_WARN("capture: reset du pipe IN : 0x%08X", s);
        }
        if (m_Usb->IsSurpriseRemoved()) gone = true;

        ULONG dataToSubmit = 0;
        KeAcquireSpinLock(&m_Lock, &irql);
        if (gone) m_DeviceGone = true;
        if (!m_Stopping && !m_DeviceGone) {
            for (ULONG i = 0; i < XONAR_ISO_IN_URBS; ++i) {
                const ULONG bit = 1UL << i;
                if ((lostData & bit) == 0 || (m_LostData & bit) == 0) continue;
                m_LostData &= ~bit;
                InterlockedIncrement(&m_Outstanding);
                m_Data[i].State = SlotInFlight;
                ++m_DataInFlight;
                dataToSubmit |= bit;
            }
        }
        KeReleaseSpinLock(&m_Lock, irql);

        if (gone) {
            XONARSE_WARN("capture: périphérique absent, relance annulée");
            continue;
        }
        if (dataToSubmit != 0) {
            XONARSE_INFO("capture: relance de %lu URB IN (tentative %lu%s)", BitCount(dataToSubmit), round,
                         resetData ? ", pipe réinitialisé" : "");
            InterlockedExchangeAdd(&m_StatResubmitted, static_cast<LONG>(BitCount(dataToSubmit)));
        }
        for (ULONG i = 0; i < XONAR_ISO_IN_URBS; ++i) {
            if ((dataToSubmit & (1UL << i)) == 0) continue;
            FormatInUrb(&m_Data[i]);
            SubmitSlot(&m_Data[i]); // la référence de l'emplacement repart avec lui
        }
    }
    ReleaseOutstanding(); // référence de l'élément de travail : dernier accès à l'objet
}
