// Couche USB de xonarse.sys : implémentation de XonarUsbDevice (contrat : usb_device.h).
//
// Vue d'ensemble
// - Soumission des URB : IRP non associé à un thread (IoAllocateIrp) envoyé à LowerDevice
//   avec IOCTL_INTERNAL_USB_SUBMIT_URB ; la routine de complétion signale un KEVENT et rend
//   STATUS_MORE_PROCESSING_REQUIRED, on libère l'IRP nous-mêmes. Ce schéma ne dépend pas
//   des APC du thread appelant (contrairement à IoBuildDeviceIoControlRequest) : il reste
//   correct sous le KGUARDED_MUTEX des requêtes de contrôle, qui désactive les APC.
//   En cas d'expiration du délai : IoCancelIrp puis attente de la complétion réelle.
// - Poignée USBD (USBD_CreateHandle, contrat 602) quand elle est disponible ; sinon mode
//   de repli : URB alloués en pool et API USBD historiques (USBD_CreateConfigurationRequestEx).
// - Requêtes de contrôle (descripteurs, sélection d'interface, requêtes de classe UAC2) :
//   sérialisées par ControlLock (KGUARDED_MUTEX), tampon de transfert non paginé commun.
// - Lecteur continu de l'endpoint d'interruption 0x8F : 2 IRP/URB en vol, complétion à
//   DISPATCH_LEVEL qui décode le message de 6 octets, le range dans un anneau protégé par
//   un verrou tournant et réveille un IO_WORKITEM unique ; celui-ci appelle les rappels
//   inscrits à PASSIVE_LEVEL (jamais deux exécutions simultanées). Les IRP sont resoumis
//   par IoReuseIrp. Un compteur d'opérations en cours (biais 1 + IRP + élément de travail)
//   et un KEVENT permettent à Stop d'attendre que tout soit retombé.
// - Automate par emplacement du lecteur (Interlocked, sans verrou) :
//     Libre -> Soumission -> EnVol -> (complétion) Libre
//     EnVol -> Annulation (StopReader dans IoCancelIrp) -> EnVol, ou -> Remis si l'IRP est
//     revenu pendant IoCancelIrp ; StopReader solde alors l'opération.
//   Le drapeau « annulation demandée » (ReaderStopping) est relu après le passage en EnVol,
//   juste avant IoCallDriver : aucun IRP n'est envoyé après une demande d'arrêt observée,
//   IoReuseIrp n'a lieu qu'en Soumission (jamais visé par IoCancelIrp), et IoCancelIrp ne
//   vise qu'un IRP en EnVol, qui ne peut pas être réutilisé tant que StopReader le tient.
// - Erreurs de l'endpoint d'interruption : relance rapide (10 ms, réinitialisation du pipe)
//   pendant kReaderFastRetries erreurs consécutives, puis relance différée de 1 s pendant
//   kReaderSlowRetries erreurs, puis abandon tracé (IsInterruptReaderActive devient faux).
//   Un transfert réussi remet le budget à zéro.
//
// Aucun code ASUS/C-Media. Les schémas (IRP alloué + MORE_PROCESSING_REQUIRED, lecteur
// d'interruption à IRP réutilisés) sont les schémas WDM classiques documentés par Microsoft ;
// aucun passage n'est copié des échantillons SYSVAD/usbsamp.
#include "../common.h"

// usbdlib.h ne déclare pas ses fonctions en liaison C : incluses depuis du C++, elles
// seraient décorées et introuvables dans usbd.lib / usbdex.lib. On les inclut donc ici sous
// extern "C" AVANT usb_device.h, dont les inclusions deviennent sans effet (gardes).
extern "C" {
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
}

#include "usb_device.h"

namespace {

constexpr ULONG kMaxInterfaces = 8;        // la fonction audio en compte 6 (0 à 5)
constexpr ULONG kMaxCallbacks = 8;
constexpr ULONG kReaderUrbs = 2;
constexpr ULONG kRingSize = 16;
constexpr ULONG kInterruptBufferBytes = 64;
constexpr ULONG kControlBufferBytes = 256;  // RANGE horloge : 2 + 21 x 12 octets au plus
constexpr ULONG kControlTimeoutMs = 2000;
constexpr ULONG kConfigTimeoutMs = 5000;
constexpr LONG kReaderFastRetries = 16;     // erreurs consécutives relancées après 10 ms
constexpr LONG kReaderSlowRetries = 32;     // puis relancées après 1 s, avant abandon
constexpr ULONG kReaderFastPauseMs = 10;
constexpr ULONG kReaderSlowPauseMs = 1000;
constexpr ULONG kStopPollMs = 250;

// États d'un emplacement du lecteur d'interruption (voir l'en-tête du fichier).
constexpr LONG kSlotIdle = 0;        // Libre : l'IRP est entre nos mains, jamais visé par IoCancelIrp
constexpr LONG kSlotSubmitting = 1;  // Soumission : IoReuseIrp et préparation de l'URB
constexpr LONG kSlotInFlight = 2;    // EnVol : confié (ou sur le point de l'être) à la pile USB
constexpr LONG kSlotCancelling = 3;  // Annulation : StopReader est dans IoCancelIrp
constexpr LONG kSlotHandOff = 4;     // Remis : revenu pendant IoCancelIrp, StopReader solde
constexpr ULONG kInterruptMessageBytes = 6;

constexpr UCHAR kAudioClass = 0x01;
constexpr UCHAR kAudioControlSubclass = 0x01;

inline USHORT ReadLe16(const UCHAR* p) { return static_cast<USHORT>(p[0] | (p[1] << 8)); }
inline ULONG ReadLe32(const UCHAR* p)
{
    return static_cast<ULONG>(p[0]) | (static_cast<ULONG>(p[1]) << 8) | (static_cast<ULONG>(p[2]) << 16) |
           (static_cast<ULONG>(p[3]) << 24);
}
inline void WriteLe16(UCHAR* p, USHORT v)
{
    p[0] = static_cast<UCHAR>(v & 0xFF);
    p[1] = static_cast<UCHAR>(v >> 8);
}
inline void WriteLe32(UCHAR* p, ULONG v)
{
    p[0] = static_cast<UCHAR>(v & 0xFF);
    p[1] = static_cast<UCHAR>((v >> 8) & 0xFF);
    p[2] = static_cast<UCHAR>((v >> 16) & 0xFF);
    p[3] = static_cast<UCHAR>(v >> 24);
}

inline bool IsDeviceGoneStatus(NTSTATUS s)
{
    return s == STATUS_NO_SUCH_DEVICE || s == STATUS_DEVICE_NOT_CONNECTED || s == STATUS_DEVICE_REMOVED ||
           s == STATUS_DELETE_PENDING;
}

inline LARGE_INTEGER RelativeTimeoutMs(ULONG ms)
{
    LARGE_INTEGER t;
    t.QuadPart = -10000LL * static_cast<LONGLONG>(ms);
    return t;
}

// Parcours borné des descripteurs : rend le descripteur commençant à *Offset s'il tient
// entièrement dans le tampon et avance *Offset, sinon nullptr (descripteur malformé ou fin).
const USB_COMMON_DESCRIPTOR* WalkDescriptor(_In_reads_bytes_(Length) const UCHAR* Base, ULONG Length,
                                            _Inout_ ULONG* Offset)
{
    if (*Offset + 2 > Length) return nullptr;
    const auto* d = reinterpret_cast<const USB_COMMON_DESCRIPTOR*>(Base + *Offset);
    if (d->bLength < 2 || *Offset + d->bLength > Length) return nullptr;
    *Offset += d->bLength;
    return d;
}

// Routine de complétion des soumissions synchrones : l'IRP nous appartient (IoAllocateIrp),
// on arrête la remontée et on réveille l'attente. Non paginée (DISPATCH_LEVEL possible).
NTSTATUS SyncSubmitCompletion(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(static_cast<PKEVENT>(Context), IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// Affichage d'un volume UAC2 (1/256 dB) sous forme signe + entier + centièmes.
struct DbView {
    char Sign;
    ULONG Whole;
    ULONG Hundredths;
};
inline DbView ToDb(SHORT v)
{
    const LONG value = v;
    const ULONG a = static_cast<ULONG>(value < 0 ? -value : value);
    return DbView{value < 0 ? '-' : '+', a / 256, (a % 256) * 100 / 256};
}

} // namespace

// ---------------------------------------------------------------------------------------
// État privé
// ---------------------------------------------------------------------------------------
struct XonarUsbDevice::Impl {
    struct InterfaceState {
        UCHAR Number;
        UCHAR CurrentAlt;
        UCHAR Class;
        UCHAR SubClass;
        PUSBD_INTERFACE_INFORMATION Info; // copie en pool non paginé
    };

    struct ReaderSlot {
        Impl* Owner;
        PIRP Irp;
        PURB Urb;
        bool UrbFromUsbd;
        volatile LONG State;        // kSlot*, transitions interlockées uniquement
        volatile LONG NeedsRestart; // erreur transitoire : resoumission par l'élément de travail
        NTSTATUS LastStatus;        // dernière erreur (traces de l'élément de travail)
        USBD_STATUS LastUsbdStatus;
        UCHAR Buffer[kInterruptBufferBytes];
    };

    struct CallbackSlot {
        XONAR_INTERRUPT_CALLBACK Callback;
        PVOID Context;
        bool InUse;
        bool Waiting;  // une désinscription attend la fin du rappel en cours
        LONG Busy;     // rappel en cours d'exécution (0 ou 1, un seul élément de travail)
        PKTHREAD Thread; // thread qui exécute le rappel en cours (valide si Busy > 0)
        KEVENT Idle;
    };

    // Identité et poignées
    PDEVICE_OBJECT Fdo;
    PDEVICE_OBJECT Pdo;
    PDEVICE_OBJECT Lower;   // référencé, rendu à la destruction
    USBD_HANDLE Usbd;       // nullptr : mode de repli
    bool UsbdClosed;        // PrepareForRemove effectué
    volatile LONG SurpriseRemoved;
    volatile LONG Started;
    volatile LONG Configured;

    // Requêtes de contrôle
    KGUARDED_MUTEX ControlLock;
    PUCHAR ControlBuffer;   // kControlBufferBytes, non paginé, protégé par ControlLock

    // Configuration (protégée par ControlLock)
    PUSB_CONFIGURATION_DESCRIPTOR Config;
    ULONG ConfigLength;
    USBD_CONFIGURATION_HANDLE ConfigHandle;
    UCHAR AcInterface;
    InterfaceState Interfaces[kMaxInterfaces];
    ULONG InterfaceCount;

    // Lecteur d'interruption
    USBD_PIPE_HANDLE InterruptPipe;
    ULONG InterruptLength;
    ReaderSlot Reader[kReaderUrbs];
    bool ReaderAllocated;           // ressources du lecteur allouées (entre StartReader et StopReader)
    volatile LONG ReaderStopping;   // drapeau « annulation demandée »
    volatile LONG ReaderFailed;     // abandon (budget épuisé ou périphérique parti)
    volatile LONG ReaderOutstanding;
    volatile LONG ReaderErrors;     // erreurs consécutives, remis à 0 par un succès
    KEVENT ReaderIdle;
    KEVENT ReaderStopSignal;        // réveille une relance différée lors d'un arrêt

    // Flux ouverts déclarés par le moteur de streaming (StreamOpened / StreamClosed)
    volatile LONG OpenStreams;
    PIO_WORKITEM WorkItem;
    KSPIN_LOCK RingLock;    // protège Ring*, WorkActive
    XONAR_UAC2_INTERRUPT Ring[kRingSize];
    ULONG RingHead;
    ULONG RingCount;
    ULONG RingDropped;
    bool WorkActive;        // élément de travail en file ou en cours

    // Rappels
    KSPIN_LOCK CallbackLock;
    CallbackSlot Callbacks[kMaxCallbacks];

    // --- Soumission ---
    NTSTATUS Submit(_Inout_ PURB Urb, bool UsbdAllocated, ULONG TimeoutMs, bool Force);
    NTSTATUS PipeRequest(USHORT Function, USBD_PIPE_HANDLE Pipe, bool Force);

    // --- Contrôle (ControlLock tenu) ---
    NTSTATUS ReadDescriptorLocked(UCHAR Type, UCHAR Index, _Out_writes_bytes_(Length) PVOID Buffer, ULONG Length,
                                  _Out_ ULONG* Transferred);
    NTSTATUS ClassRequestLocked(bool In, UCHAR Request, UCHAR Selector, UCHAR Channel, UCHAR Entity,
                                _Inout_updates_bytes_opt_(Length) PVOID Data, ULONG Length,
                                _Out_opt_ ULONG* Transferred);
    NTSTATUS ReadRangeLocked(UCHAR Selector, UCHAR Channel, UCHAR Entity, ULONG SubrangeBytes,
                             _Out_ ULONG* Reported, _Out_ ULONG* Bytes);
    NTSTATUS ClassGet(UCHAR Selector, UCHAR Channel, UCHAR Entity, _Out_writes_bytes_(Length) PVOID Data,
                      ULONG Length, ULONG MinLength);
    NTSTATUS ClassSet(UCHAR Selector, UCHAR Channel, UCHAR Entity, _In_reads_bytes_(Length) const VOID* Data,
                      ULONG Length);
    NTSTATUS CheckReady() const;

    // --- Configuration (ControlLock tenu) ---
    void LogDeviceDescriptorLocked();
    NTSTATUS ReadConfigurationLocked(_Outptr_ PUSB_CONFIGURATION_DESCRIPTOR* Out, _Out_ ULONG* Length);
    NTSTATUS LoadConfigurationLocked();
    bool ConfigurationStillValidLocked();
    NTSTATUS SelectConfigurationLocked();
    void StopDevice(bool ForRemove);
    void FreeInterfaceInfos();
    InterfaceState* FindInterfaceState(UCHAR Number);
    PUSB_INTERFACE_DESCRIPTOR FindInterfaceDescriptor(UCHAR Number, UCHAR Alt, _Out_ ULONG* After) const;
    NTSTATUS SelectAltLocked(UCHAR Interface, UCHAR Alt, _Out_opt_ XONAR_AS_PIPES* Pipes);
    void FillPipes(_In_ const USBD_INTERFACE_INFORMATION* Info, ULONG DescOffset, _Out_ XONAR_AS_PIPES* Pipes) const;
    void FindInterruptPipe();

    // --- Lecteur d'interruption ---
    NTSTATUS StartReader();
    void StopReader();
    void CancelReaderIo();
    void FreeReaderResources();
    void SubmitSlot(_Inout_ ReaderSlot* Slot);
    static bool ReclaimSlot(_Inout_ ReaderSlot* Slot);
    void RestartSlots(ULONG Mask);
    void PushMessage(_In_reads_bytes_(kInterruptMessageBytes) const UCHAR* Bytes);
    void QueueWork();
    void ResetRing();
    void ReleaseOutstanding();
    void RunWorker();
    void Dispatch(_In_ const XONAR_UAC2_INTERRUPT& Message);
    static NTSTATUS ReaderCompletion(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_opt_ PVOID Context);
    static VOID WorkerRoutine(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context);
};

// =======================================================================================
// Code non paginé : tout ce qui peut tourner à DISPATCH_LEVEL ou tient un verrou tournant.
// =======================================================================================

LONG XonarUsbDevice::AddRef()
{
    return InterlockedIncrement(&m_RefCount);
}

LONG XonarUsbDevice::Release()
{
    const LONG r = InterlockedDecrement(&m_RefCount);
    if (r == 0) {
        // La destruction (USBD_CloseHandle, attente du lecteur) exige PASSIVE_LEVEL.
        NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
        delete this;
    }
    return r;
}

bool XonarUsbDevice::IsStarted() const
{
    return m_Impl != nullptr && m_Impl->Started != 0;
}

bool XonarUsbDevice::IsSurpriseRemoved() const
{
    return m_Impl != nullptr && m_Impl->SurpriseRemoved != 0;
}

void XonarUsbDevice::MarkSurpriseRemoved()
{
    if (m_Impl == nullptr) return;
    if (InterlockedExchange(&m_Impl->SurpriseRemoved, 1) == 0) {
        XONARSE_WARN("USB : retrait inopiné signalé, toute requête échouera désormais");
    }
}

PDEVICE_OBJECT XonarUsbDevice::LowerDevice() const
{
    return m_Impl != nullptr ? m_Impl->Lower : nullptr;
}

USBD_HANDLE XonarUsbDevice::UsbdHandle() const
{
    return m_Impl != nullptr ? m_Impl->Usbd : nullptr;
}

PDEVICE_OBJECT XonarUsbDevice::FunctionalDevice() const
{
    return m_Impl != nullptr ? m_Impl->Fdo : nullptr;
}

LONG XonarUsbDevice::StreamOpened()
{
    if (m_Impl == nullptr) return 0;
    const LONG n = InterlockedIncrement(&m_Impl->OpenStreams);
    XONARSE_INFO("USB : flux ouvert, %ld flux ouvert(s)", n);
    return n;
}

LONG XonarUsbDevice::StreamClosed()
{
    if (m_Impl == nullptr) return 0;
    // Décrément borné à 0 : un appel en trop ne doit pas fausser les arrêts suivants.
    for (;;) {
        const LONG current = m_Impl->OpenStreams;
        if (current <= 0) {
            XONARSE_WARN("USB : StreamClosed sans flux ouvert, ignoré");
            return 0;
        }
        if (InterlockedCompareExchange(&m_Impl->OpenStreams, current - 1, current) == current) {
            XONARSE_INFO("USB : flux fermé, %ld flux ouvert(s)", current - 1);
            return current - 1;
        }
    }
}

LONG XonarUsbDevice::OpenStreamCount() const
{
    return m_Impl != nullptr ? m_Impl->OpenStreams : 0;
}

bool XonarUsbDevice::IsInterruptReaderActive() const
{
    const Impl* d = m_Impl;
    return d != nullptr && d->ReaderAllocated && d->ReaderStopping == 0 && d->ReaderFailed == 0;
}

NTSTATUS XonarUsbDevice::RegisterInterruptCallback(XONAR_INTERRUPT_CALLBACK Callback, PVOID Context)
{
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    if (Callback == nullptr) return STATUS_INVALID_PARAMETER;
    Impl* d = m_Impl;
    NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;
    KIRQL irql;
    KeAcquireSpinLock(&d->CallbackLock, &irql);
    ULONG free = kMaxCallbacks;
    for (ULONG i = 0; i < kMaxCallbacks; ++i) {
        const Impl::CallbackSlot& s = d->Callbacks[i];
        if (s.InUse && s.Callback == Callback && s.Context == Context) {
            free = kMaxCallbacks;
            status = STATUS_DUPLICATE_OBJECTID;
            break;
        }
        // Un emplacement dont la désinscription attend encore un rappel n'est pas réutilisé.
        if (free == kMaxCallbacks && !s.InUse && !s.Waiting && s.Busy == 0) free = i;
    }
    if (status != STATUS_DUPLICATE_OBJECTID && free < kMaxCallbacks) {
        Impl::CallbackSlot& s = d->Callbacks[free];
        s.Callback = Callback;
        s.Context = Context;
        s.InUse = true;
        status = STATUS_SUCCESS;
    }
    KeReleaseSpinLock(&d->CallbackLock, irql);
    if (!NT_SUCCESS(status)) XONARSE_WARN("USB : inscription de rappel refusée (0x%08X)", status);
    return status;
}

void XonarUsbDevice::UnregisterInterruptCallback(XONAR_INTERRUPT_CALLBACK Callback, PVOID Context)
{
    if (m_Impl == nullptr) return;
    Impl* d = m_Impl;
    KIRQL irql;
    KeAcquireSpinLock(&d->CallbackLock, &irql);
    for (ULONG i = 0; i < kMaxCallbacks; ++i) {
        Impl::CallbackSlot& s = d->Callbacks[i];
        if (!s.InUse || s.Callback != Callback || s.Context != Context) continue;
        s.InUse = false; // plus aucun nouvel appel
        // Rappel en cours sur un autre thread : on attend sa fin. Depuis le rappel lui-même,
        // attendre serait un interblocage ; le retour du rappel en cours suffit alors à
        // garantir la sémantique. Le thread est mémorisé PAR EMPLACEMENT (avec Busy, sous
        // CallbackLock) : deux exécutions chevauchantes de l'élément de travail ne peuvent
        // pas fausser la comparaison.
        if (s.Busy > 0 && s.Thread != KeGetCurrentThread()) {
            s.Waiting = true;
            KeClearEvent(&s.Idle);
            KeReleaseSpinLock(&d->CallbackLock, irql);
            KeWaitForSingleObject(&s.Idle, Executive, KernelMode, FALSE, nullptr);
            KeAcquireSpinLock(&d->CallbackLock, &irql);
            s.Waiting = false;
        }
        s.Callback = nullptr;
        s.Context = nullptr;
        KeReleaseSpinLock(&d->CallbackLock, irql);
        return;
    }
    KeReleaseSpinLock(&d->CallbackLock, irql);
    XONARSE_WARN("USB : désinscription d'un rappel inconnu ignorée");
}

// --- Lecteur d'interruption : parties appelées à DISPATCH_LEVEL ---

void XonarUsbDevice::Impl::ReleaseOutstanding()
{
    if (InterlockedDecrement(&ReaderOutstanding) == 0) {
        KeSetEvent(&ReaderIdle, IO_NO_INCREMENT, FALSE);
    }
}

// Non paginée : tient RingLock (appelée par StartReader, lui paginable).
void XonarUsbDevice::Impl::ResetRing()
{
    KIRQL irql;
    KeAcquireSpinLock(&RingLock, &irql);
    RingHead = 0;
    RingCount = 0;
    RingDropped = 0;
    WorkActive = false;
    KeReleaseSpinLock(&RingLock, irql);
}

void XonarUsbDevice::Impl::QueueWork()
{
    bool queue = false;
    KIRQL irql;
    KeAcquireSpinLock(&RingLock, &irql);
    if (!WorkActive) {
        WorkActive = true;
        queue = true;
    }
    KeReleaseSpinLock(&RingLock, irql);
    if (queue) {
        // L'appelant détient déjà une opération en cours : le compteur ne peut pas être à 0 ici.
        InterlockedIncrement(&ReaderOutstanding);
        IoQueueWorkItem(WorkItem, WorkerRoutine, DelayedWorkQueue, this);
    }
}

void XonarUsbDevice::Impl::PushMessage(const UCHAR* Bytes)
{
    // Message d'interruption UAC2 (§6.1) : bInfo, bAttribute, wValue (petit-boutiste :
    // octet bas = canal, octet haut = sélecteur), wIndex (octet bas = interface ou
    // endpoint, octet haut = ID d'entité).
    XONAR_UAC2_INTERRUPT msg;
    msg.Info = Bytes[0];
    msg.Attribute = Bytes[1];
    msg.ChannelNumber = Bytes[2];
    msg.ControlSelector = Bytes[3];
    msg.InterfaceOrEp = Bytes[4];
    msg.EntityId = Bytes[5];

    KIRQL irql;
    KeAcquireSpinLock(&RingLock, &irql);
    if (RingCount < kRingSize) {
        Ring[(RingHead + RingCount) % kRingSize] = msg;
        ++RingCount;
    } else {
        ++RingDropped;
    }
    KeReleaseSpinLock(&RingLock, irql);
    QueueWork();
}

// Reprise d'un IRP qui n'est plus (ou pas encore) dans la pile USB : EnVol -> Libre, et
// l'appelant en redevient propriétaire (rend true). Si StopReader est dans IoCancelIrp
// (Annulation), l'IRP lui est remis (Remis) et c'est lui qui solde l'opération en cours
// (rend false) : l'appelant ne doit plus toucher à l'IRP. Aucune attente active.
bool XonarUsbDevice::Impl::ReclaimSlot(ReaderSlot* Slot)
{
    for (;;) {
        const LONG seen = InterlockedCompareExchange(&Slot->State, kSlotIdle, kSlotInFlight);
        if (seen == kSlotInFlight) return true;
        if (seen == kSlotCancelling) {
            if (InterlockedCompareExchange(&Slot->State, kSlotHandOff, kSlotCancelling) == kSlotCancelling) return false;
            continue; // StopReader vient de sortir d'IoCancelIrp (retour en EnVol) : on recommence
        }
        // État incohérent (défaut de programmation) : on reprend l'IRP plutôt que de boucler.
        NT_ASSERT(FALSE);
        InterlockedExchange(&Slot->State, kSlotIdle);
        return true;
    }
}

void XonarUsbDevice::Impl::SubmitSlot(ReaderSlot* Slot)
{
    // Libre -> Soumission : seul le propriétaire courant de l'IRP (complétion, élément de
    // travail ou StartReader) arrive ici ; StopReader ignore cet état.
    const LONG previous = InterlockedExchange(&Slot->State, kSlotSubmitting);
    NT_ASSERT(previous == kSlotIdle);
    UNREFERENCED_PARAMETER(previous);
    if (ReaderStopping || ReaderFailed) {
        InterlockedExchange(&Slot->State, kSlotIdle);
        ReleaseOutstanding();
        return;
    }
    PIRP irp = Slot->Irp;
    IoReuseIrp(irp, STATUS_NOT_SUPPORTED); // uniquement en Soumission : aucune annulation perdue
    UsbBuildInterruptOrBulkTransferRequest(Slot->Urb, sizeof(struct _URB_BULK_OR_INTERRUPT_TRANSFER), InterruptPipe,
                                           Slot->Buffer, nullptr, InterruptLength,
                                           USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK, nullptr);
    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    if (Slot->UrbFromUsbd) {
        USBD_AssignUrbToIoStackLocation(Usbd, next, Slot->Urb);
    } else {
        next->Parameters.Others.Argument1 = Slot->Urb;
    }
    IoSetCompletionRoutine(irp, ReaderCompletion, Slot, TRUE, TRUE, TRUE);

    // Soumission -> EnVol AVANT IoCallDriver (la complétion peut survenir avant son retour),
    // puis relecture de la demande d'arrêt après cette barrière complète. StopReader écrit
    // ReaderStopping puis lit State : l'un des deux voit forcément l'écriture de l'autre.
    InterlockedExchange(&Slot->State, kSlotInFlight);
    if (ReaderStopping) {
        // Arrêt demandé pendant la préparation : l'IRP n'est pas envoyé. Si StopReader le
        // tient déjà (IoCancelIrp sur un IRP non envoyé, sans effet), il solde lui-même.
        if (ReclaimSlot(Slot)) ReleaseOutstanding();
        return;
    }
    // Le statut est traité dans la routine de complétion, appelée dans tous les cas.
    (void)IoCallDriver(Lower, irp);
}

NTSTATUS XonarUsbDevice::Impl::ReaderCompletion(PDEVICE_OBJECT DeviceObject, PIRP Irp, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    auto* slot = static_cast<ReaderSlot*>(Context);
    Impl* self = slot->Owner;
    // EnVol -> Libre ; si StopReader est dans IoCancelIrp, il reprend la main et solde.
    if (!ReclaimSlot(slot)) return STATUS_MORE_PROCESSING_REQUIRED;
    const NTSTATUS status = Irp->IoStatus.Status;

    if (self->ReaderStopping || self->ReaderFailed) {
        self->ReleaseOutstanding();
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    if (NT_SUCCESS(status)) {
        if (InterlockedExchange(&self->ReaderErrors, 0) > kReaderFastRetries) {
            XONARSE_INFO("USB : lecteur d'interruption rétabli");
        }
        const ULONG got = slot->Urb->UrbBulkOrInterruptTransfer.TransferBufferLength;
        if (got >= kInterruptMessageBytes) {
            self->PushMessage(slot->Buffer);
        } else if (got != 0) {
            XONARSE_WARN("USB : message d'interruption tronqué (%lu octets) ignoré", got);
        }
        self->SubmitSlot(slot); // l'IRP garde son opération en cours
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    const USBD_STATUS usbdStatus = slot->Urb->UrbHeader.Status;
    if (IsDeviceGoneStatus(status) || usbdStatus == USBD_STATUS_DEVICE_GONE || self->SurpriseRemoved) {
        if (InterlockedExchange(&self->ReaderFailed, 1) == 0) {
            XONARSE_WARN("USB : lecteur d'interruption arrêté, périphérique parti (0x%08X, USBD 0x%08X)", status,
                         usbdStatus);
        }
        self->ReleaseOutstanding();
        return STATUS_MORE_PROCESSING_REQUIRED;
    }

    // Erreur (y compris une annulation que nous n'avons pas demandée) : relance par
    // l'élément de travail, qui applique le budget ; la réinitialisation du pipe exige
    // PASSIVE_LEVEL. L'IRP garde son opération en cours jusque-là.
    slot->LastStatus = status;
    slot->LastUsbdStatus = usbdStatus;
    InterlockedIncrement(&self->ReaderErrors);
    InterlockedExchange(&slot->NeedsRestart, 1);
    self->QueueWork();
    return STATUS_MORE_PROCESSING_REQUIRED;
}

// --- Élément de travail (PASSIVE_LEVEL, mais tient des verrous tournants : non paginé) ---

VOID XonarUsbDevice::Impl::WorkerRoutine(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    static_cast<Impl*>(Context)->RunWorker();
}

void XonarUsbDevice::Impl::Dispatch(const XONAR_UAC2_INTERRUPT& Message)
{
    XONARSE_INFO("USB : interruption UAC2 bInfo=0x%02X attribut=0x%02X sélecteur=0x%02X canal=%u entité=%u "
                 "interface/ep=%u",
                 Message.Info, Message.Attribute, Message.ControlSelector, Message.ChannelNumber, Message.EntityId,
                 Message.InterfaceOrEp);
    for (ULONG i = 0; i < kMaxCallbacks; ++i) {
        XONAR_INTERRUPT_CALLBACK cb = nullptr;
        PVOID ctx = nullptr;
        KIRQL irql;
        KeAcquireSpinLock(&CallbackLock, &irql);
        CallbackSlot& s = Callbacks[i];
        if (s.InUse) {
            cb = s.Callback;
            ctx = s.Context;
            ++s.Busy;
            s.Thread = KeGetCurrentThread(); // relu par UnregisterInterruptCallback sous ce verrou
        }
        KeReleaseSpinLock(&CallbackLock, irql);
        if (cb == nullptr) continue;

        cb(ctx, &Message); // PASSIVE_LEVEL, hors verrou

        KeAcquireSpinLock(&CallbackLock, &irql);
        if (--s.Busy == 0) {
            s.Thread = nullptr;
            if (s.Waiting) KeSetEvent(&s.Idle, IO_NO_INCREMENT, FALSE);
        }
        KeReleaseSpinLock(&CallbackLock, irql);
    }
}

// Remise en route des emplacements en erreur (Mask), à PASSIVE_LEVEL depuis l'élément de
// travail. Chaque emplacement du masque est Libre et détient une opération en cours, que
// cette fonction rend (abandon) ou transmet à SubmitSlot.
void XonarUsbDevice::Impl::RestartSlots(ULONG Mask)
{
    const LONG errors = ReaderErrors;
    bool abandon = ReaderStopping || ReaderFailed || SurpriseRemoved;
    if (!abandon && errors > kReaderFastRetries + kReaderSlowRetries) {
        if (InterlockedExchange(&ReaderFailed, 1) == 0) {
            XONARSE_ERROR("USB : %ld erreurs consécutives sur l'endpoint d'interruption, budget de relances épuisé, "
                          "lecteur abandonné jusqu'au prochain Start",
                          errors);
        }
        abandon = true;
    }
    if (abandon) {
        for (ULONG i = 0; i < kReaderUrbs; ++i) {
            if (Mask & (1u << i)) ReleaseOutstanding();
        }
        return;
    }

    // Traces : première erreur d'une série, puis chaque relance différée.
    const bool slow = errors > kReaderFastRetries;
    for (ULONG i = 0; i < kReaderUrbs; ++i) {
        if (!(Mask & (1u << i))) continue;
        if (errors <= static_cast<LONG>(kReaderUrbs)) {
            XONARSE_WARN("USB : erreur sur l'endpoint d'interruption (0x%08X, USBD 0x%08X), relance", Reader[i].LastStatus,
                         Reader[i].LastUsbdStatus);
        } else if (slow) {
            XONARSE_WARN("USB : endpoint d'interruption en erreur (0x%08X, USBD 0x%08X), relance différée %ld/%ld",
                         Reader[i].LastStatus, Reader[i].LastUsbdStatus, errors - kReaderFastRetries, kReaderSlowRetries);
        }
        break;
    }

    // Pause avant relance : courte, puis 1 s au-delà de kReaderFastRetries erreurs. L'attente
    // porte sur ReaderStopSignal pour qu'un arrêt n'attende pas la fin du délai.
    LARGE_INTEGER pause = RelativeTimeoutMs(slow ? kReaderSlowPauseMs : kReaderFastPauseMs);
    (void)KeWaitForSingleObject(&ReaderStopSignal, Executive, KernelMode, FALSE, &pause);

    if (!ReaderStopping && !SurpriseRemoved) {
        // On ne réinitialise le pipe que si aucun autre transfert n'y est en vol (la pile
        // refuse ou annule sinon) ; dans le cas contraire, l'autre URB échouera aussi si le
        // pipe est bloqué, et la réinitialisation suivra.
        bool otherInFlight = false;
        for (ULONG i = 0; i < kReaderUrbs; ++i) {
            if (!(Mask & (1u << i)) && Reader[i].State != kSlotIdle) otherInFlight = true;
        }
        if (!otherInFlight) {
            const NTSTATUS s = PipeRequest(URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL, InterruptPipe, false);
            if (!NT_SUCCESS(s)) XONARSE_WARN("USB : réinitialisation du pipe d'interruption 0x%08X", s);
        }
    }
    // SubmitSlot relit lui-même la demande d'arrêt et rend l'opération le cas échéant.
    for (ULONG i = 0; i < kReaderUrbs; ++i) {
        if (Mask & (1u << i)) SubmitSlot(&Reader[i]);
    }
}

void XonarUsbDevice::Impl::RunWorker()
{
    for (;;) {
        // 1. Remise en route des URB en erreur transitoire.
        ULONG restartMask = 0;
        for (ULONG i = 0; i < kReaderUrbs; ++i) {
            if (InterlockedExchange(&Reader[i].NeedsRestart, 0) != 0) restartMask |= 1u << i;
        }
        if (restartMask != 0) RestartSlots(restartMask);

        // 2. Un message de l'anneau, ou fin du travail si rien n'est en attente. La décision
        // de fin est prise sous RingLock, comme la mise en file par QueueWork : aucun réveil
        // ne peut être perdu.
        XONAR_UAC2_INTERRUPT msg = {};
        bool have = false;
        bool done = false;
        ULONG dropped = 0;
        KIRQL irql;
        KeAcquireSpinLock(&RingLock, &irql);
        if (RingCount != 0) {
            msg = Ring[RingHead];
            RingHead = (RingHead + 1) % kRingSize;
            --RingCount;
            have = true;
        } else {
            bool restartPending = false;
            for (ULONG i = 0; i < kReaderUrbs; ++i) {
                if (Reader[i].NeedsRestart) restartPending = true;
            }
            if (!restartPending) {
                WorkActive = false;
                done = true;
            }
        }
        dropped = RingDropped;
        RingDropped = 0;
        KeReleaseSpinLock(&RingLock, irql);

        if (dropped != 0) XONARSE_WARN("USB : %lu message(s) d'interruption perdus (anneau plein)", dropped);
        if (have && !ReaderStopping) Dispatch(msg);
        if (done) break;
    }
    ReleaseOutstanding(); // dernière action : l'objet peut être libéré ensuite
}

// =======================================================================================
// Code paginable : PASSIVE_LEVEL uniquement, sans verrou tournant.
// =======================================================================================
#pragma code_seg("PAGE")

NTSTATUS XonarUsbDevice::Create(PDEVICE_OBJECT Fdo, PDEVICE_OBJECT Pdo, PDEVICE_OBJECT LowerDevice,
                                XonarUsbDevice** Device)
{
    PAGED_CODE();
    if (Device == nullptr) return STATUS_INVALID_PARAMETER;
    *Device = nullptr;
    if (Fdo == nullptr || LowerDevice == nullptr) return STATUS_INVALID_PARAMETER;

    auto* dev = new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) XonarUsbDevice();
    if (dev == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    auto* d = new (POOL_FLAG_NON_PAGED, XONARSE_POOL_TAG) Impl(); // mémoire mise à zéro
    if (d == nullptr) {
        delete dev;
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    dev->m_Impl = d;

    d->ControlBuffer = static_cast<PUCHAR>(ExAllocatePool2(POOL_FLAG_NON_PAGED, kControlBufferBytes, XONARSE_POOL_TAG));
    if (d->ControlBuffer == nullptr) {
        delete dev; // Lower n'est pas encore pris : l'appelant le rend en cas d'échec
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    d->Fdo = Fdo;
    d->Pdo = Pdo;
    KeInitializeGuardedMutex(&d->ControlLock);
    KeInitializeEvent(&d->ReaderIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&d->ReaderStopSignal, NotificationEvent, FALSE);
    KeInitializeSpinLock(&d->RingLock);
    KeInitializeSpinLock(&d->CallbackLock);
    for (ULONG i = 0; i < kMaxCallbacks; ++i) KeInitializeEvent(&d->Callbacks[i].Idle, NotificationEvent, FALSE);
    for (ULONG i = 0; i < kReaderUrbs; ++i) d->Reader[i].Owner = d;

    USBD_HANDLE handle = nullptr;
    const NTSTATUS status = USBD_CreateHandle(Fdo, LowerDevice, USBD_CLIENT_CONTRACT_VERSION_602, XONARSE_POOL_TAG, &handle);
    if (NT_SUCCESS(status)) {
        d->Usbd = handle;
        XONARSE_INFO("USB : poignée USBD créée (contrat 0x602), interface 0x603 %s",
                     USBD_IsInterfaceVersionSupported(handle, USBD_INTERFACE_VERSION_603) ? "disponible" : "absente");
    } else {
        d->Usbd = nullptr;
        XONARSE_WARN("USB : USBD_CreateHandle 0x%08X, mode de repli (URB en pool, API USBD historiques)", status);
    }

    d->Lower = LowerDevice; // à partir d'ici, la référence est à nous
    *Device = dev;
    return STATUS_SUCCESS;
}

XonarUsbDevice::~XonarUsbDevice()
{
    PAGED_CODE();
    Impl* d = m_Impl;
    if (d == nullptr) return;
    if (d->ReaderAllocated) {
        XONARSE_WARN("USB : destruction sans Stop préalable, arrêt du lecteur");
        d->StopReader();
    }
    for (ULONG i = 0; i < kMaxCallbacks; ++i) {
        if (d->Callbacks[i].InUse) XONARSE_WARN("USB : rappel d'interruption encore inscrit à la destruction");
    }
    d->FreeInterfaceInfos();
    if (d->Config != nullptr) ExFreePool(d->Config);
    if (d->ControlBuffer != nullptr) ExFreePool(d->ControlBuffer);
    if (d->Usbd != nullptr) USBD_CloseHandle(d->Usbd);
    if (d->Lower != nullptr) ObDereferenceObject(d->Lower);
    delete d;
    m_Impl = nullptr;
}

// --- Soumission synchrone ---

NTSTATUS XonarUsbDevice::Impl::Submit(PURB Urb, bool UsbdAllocated, ULONG TimeoutMs, bool Force)
{
    PAGED_CODE();
    if (Urb == nullptr) return STATUS_INVALID_PARAMETER;
    if (UsbdClosed) return STATUS_NO_SUCH_DEVICE;
    if (!Force && SurpriseRemoved) return STATUS_NO_SUCH_DEVICE;
    if (UsbdAllocated && Usbd == nullptr) return STATUS_INVALID_DEVICE_STATE;

    PIRP irp = IoAllocateIrp(Lower->StackSize, FALSE);
    if (irp == nullptr) return STATUS_INSUFFICIENT_RESOURCES;

    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(irp);
    next->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    next->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_USB_SUBMIT_URB;
    if (UsbdAllocated) {
        USBD_AssignUrbToIoStackLocation(Usbd, next, Urb);
    } else {
        next->Parameters.Others.Argument1 = Urb;
    }

    KEVENT done; // pile non paginable : l'attente se fait en KernelMode
    KeInitializeEvent(&done, NotificationEvent, FALSE);
    IoSetCompletionRoutine(irp, SyncSubmitCompletion, &done, TRUE, TRUE, TRUE);
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;

    bool timedOut = false;
    NTSTATUS status = IoCallDriver(Lower, irp);
    if (status == STATUS_PENDING) {
        LARGE_INTEGER timeout = RelativeTimeoutMs(TimeoutMs != 0 ? TimeoutMs : 5000);
        if (KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, &timeout) == STATUS_TIMEOUT) {
            timedOut = true;
            IoCancelIrp(irp);
            // L'IRP reste à nous tant que la complétion n'a pas eu lieu : attente sans délai.
            KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, nullptr);
        }
    } else {
        // Complétion synchrone : la routine a déjà signalé l'événement.
        KeWaitForSingleObject(&done, Executive, KernelMode, FALSE, nullptr);
    }
    status = irp->IoStatus.Status;
    IoFreeIrp(irp);

    if (timedOut && (status == STATUS_CANCELLED || NT_SUCCESS(status))) {
        status = NT_SUCCESS(status) ? status : STATUS_IO_TIMEOUT;
    }
    // Après un retrait inopiné (requêtes forcées seulement), l'échec est attendu : pas de trace.
    if (!NT_SUCCESS(status) && !IsDeviceGoneStatus(status) && !SurpriseRemoved) {
        XONARSE_WARN("USB : URB fonction 0x%04X : 0x%08X (USBD 0x%08X)%s", Urb->UrbHeader.Function, status,
                     Urb->UrbHeader.Status, timedOut ? " après expiration du délai" : "");
    }
    return status;
}

NTSTATUS XonarUsbDevice::Impl::PipeRequest(USHORT Function, USBD_PIPE_HANDLE Pipe, bool Force)
{
    PAGED_CODE();
    if (Pipe == nullptr) return STATUS_INVALID_PARAMETER;
    struct _URB_PIPE_REQUEST req = {};
    req.Hdr.Length = sizeof(req);
    req.Hdr.Function = Function;
    req.PipeHandle = Pipe;
    return Submit(reinterpret_cast<PURB>(&req), false, kConfigTimeoutMs, Force);
}

NTSTATUS XonarUsbDevice::SubmitUrbSync(PURB Urb, ULONG TimeoutMs)
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    return m_Impl->Submit(Urb, false, TimeoutMs, false);
}

NTSTATUS XonarUsbDevice::SubmitUsbdUrbSync(PURB Urb, ULONG TimeoutMs)
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    return m_Impl->Submit(Urb, true, TimeoutMs, false);
}

NTSTATUS XonarUsbDevice::AbortPipe(USBD_PIPE_HANDLE Pipe)
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    Impl* d = m_Impl;
    // Forcé : après un retrait inopiné, le moteur de streaming doit encore pouvoir faire
    // terminer ses URB (comme CancelReaderIo). Si le périphérique est vraiment parti, la pile
    // termine de toute façon les transferts : l'échec est rendu, tracé en simple information.
    const NTSTATUS status = d->PipeRequest(URB_FUNCTION_ABORT_PIPE, Pipe, true);
    if (!NT_SUCCESS(status) && (d->SurpriseRemoved || IsDeviceGoneStatus(status))) {
        XONARSE_INFO("USB : ABORT_PIPE après retrait : 0x%08X, ignoré", status);
    }
    return status;
}

NTSTATUS XonarUsbDevice::ResetPipe(USBD_PIPE_HANDLE Pipe)
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    Impl* d = m_Impl;
    NTSTATUS status = d->CheckReady(); // retiré : STATUS_NO_SUCH_DEVICE sans rien envoyer
    if (!NT_SUCCESS(status)) return status;
    if (Pipe == nullptr) return STATUS_INVALID_PARAMETER;
    // Échec tracé par Submit (sauf départ du périphérique) ; pas de trace en cas de succès,
    // le moteur de streaming peut l'appeler à chaque erreur passagère.
    return d->PipeRequest(URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL, Pipe, false);
}

NTSTATUS XonarUsbDevice::GetCurrentFrameNumber(ULONG* Frame)
{
    PAGED_CODE();
    *Frame = 0;
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    struct _URB_GET_CURRENT_FRAME_NUMBER req = {};
    req.Hdr.Length = sizeof(req);
    req.Hdr.Function = URB_FUNCTION_GET_CURRENT_FRAME_NUMBER;
    const NTSTATUS status = m_Impl->Submit(reinterpret_cast<PURB>(&req), false, kControlTimeoutMs, false);
    if (NT_SUCCESS(status)) *Frame = req.FrameNumber;
    return status;
}

// --- Requêtes de contrôle ---

NTSTATUS XonarUsbDevice::Impl::CheckReady() const
{
    PAGED_CODE();
    if (SurpriseRemoved || UsbdClosed) return STATUS_NO_SUCH_DEVICE;
    if (!Configured) return STATUS_DEVICE_NOT_READY;
    return STATUS_SUCCESS;
}

// Buffer doit être en mémoire non paginée (pool ou ControlBuffer).
NTSTATUS XonarUsbDevice::Impl::ReadDescriptorLocked(UCHAR Type, UCHAR Index, PVOID Buffer, ULONG Length,
                                                    ULONG* Transferred)
{
    PAGED_CODE();
    *Transferred = 0;
    if (Buffer == nullptr || Length == 0 || Length > MAXUSHORT) return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(Buffer, Length);
    struct _URB_CONTROL_DESCRIPTOR_REQUEST req = {};
    PURB urb = reinterpret_cast<PURB>(&req);
    UsbBuildGetDescriptorRequest(urb, sizeof(req), Type, Index, 0, Buffer, nullptr, Length, nullptr);
    const NTSTATUS status = Submit(urb, false, kConfigTimeoutMs, false);
    if (NT_SUCCESS(status)) *Transferred = req.TransferBufferLength;
    return status;
}

NTSTATUS XonarUsbDevice::Impl::ClassRequestLocked(bool In, UCHAR Request, UCHAR Selector, UCHAR Channel, UCHAR Entity,
                                                  PVOID Data, ULONG Length, ULONG* Transferred)
{
    PAGED_CODE();
    if (Transferred != nullptr) *Transferred = 0;
    if (Length == 0 || Length > kControlBufferBytes) return STATUS_INVALID_BUFFER_SIZE;
    if (In) {
        RtlZeroMemory(ControlBuffer, Length);
    } else {
        if (Data == nullptr) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(ControlBuffer, Data, Length);
    }

    // UAC2 §5.2.2 : wValue = (CS << 8) | CN ; wIndex = (ID d'entité << 8) | interface AudioControl.
    // URB_FUNCTION_CLASS_INTERFACE : la pile compose bmRequestType (classe, interface, sens).
    const USHORT value = static_cast<USHORT>((Selector << 8) | Channel);
    const USHORT index = static_cast<USHORT>((Entity << 8) | AcInterface);
    struct _URB_CONTROL_VENDOR_OR_CLASS_REQUEST req = {};
    PURB urb = reinterpret_cast<PURB>(&req);
    UsbBuildVendorRequest(urb, URB_FUNCTION_CLASS_INTERFACE, sizeof(req),
                          In ? (USBD_TRANSFER_DIRECTION_IN | USBD_SHORT_TRANSFER_OK) : USBD_TRANSFER_DIRECTION_OUT, 0,
                          Request, value, index, ControlBuffer, nullptr, Length, nullptr);
    const NTSTATUS status = Submit(urb, false, kControlTimeoutMs, false);
    if (!NT_SUCCESS(status)) return status;

    const ULONG done = req.TransferBufferLength <= Length ? req.TransferBufferLength : Length;
    if (In && Data != nullptr) RtlCopyMemory(Data, ControlBuffer, done);
    if (Transferred != nullptr) *Transferred = done;
    return STATUS_SUCCESS;
}

NTSTATUS XonarUsbDevice::Impl::ReadRangeLocked(UCHAR Selector, UCHAR Channel, UCHAR Entity, ULONG SubrangeBytes,
                                               ULONG* Reported, ULONG* Bytes)
{
    PAGED_CODE();
    *Reported = 0;
    *Bytes = 0;
    // Plus grande longueur multiple d'un sous-intervalle qui tient dans le tampon commun.
    const ULONG cap = 2 + ((kControlBufferBytes - 2) / SubrangeBytes) * SubrangeBytes;
    ULONG done = 0;
    // UAC2 §5.2.1 : on lit d'abord wNumSubRanges (2 octets), puis la totalité.
    NTSTATUS status = ClassRequestLocked(true, xonar::uac2::kRequestRange, Selector, Channel, Entity, nullptr, 2, &done);
    ULONG want = cap;
    if (NT_SUCCESS(status) && done >= 2) {
        const ULONG n = ReadLe16(ControlBuffer);
        const ULONG full = 2 + n * SubrangeBytes;
        want = full < cap ? full : cap;
        if (n == 0) {
            *Bytes = 2;
            return STATUS_SUCCESS;
        }
    } else if (!NT_SUCCESS(status) && (status == STATUS_IO_TIMEOUT || IsDeviceGoneStatus(status))) {
        return status;
    }
    // Repli si la lecture courte a échoué : certaines puces refusent wLength=2, on demande
    // directement la taille maximale (la carte répond par un paquet court).
    status = ClassRequestLocked(true, xonar::uac2::kRequestRange, Selector, Channel, Entity, nullptr, want, &done);
    if (!NT_SUCCESS(status)) return status;
    if (done < 2) return STATUS_DEVICE_PROTOCOL_ERROR;
    *Reported = ReadLe16(ControlBuffer);
    *Bytes = done;
    return STATUS_SUCCESS;
}

NTSTATUS XonarUsbDevice::Impl::ClassGet(UCHAR Selector, UCHAR Channel, UCHAR Entity, PVOID Data, ULONG Length,
                                        ULONG MinLength)
{
    PAGED_CODE();
    NTSTATUS status = CheckReady();
    if (!NT_SUCCESS(status)) return status;
    ULONG done = 0;
    KeAcquireGuardedMutex(&ControlLock);
    status = ClassRequestLocked(true, xonar::uac2::kRequestCur, Selector, Channel, Entity, Data, Length, &done);
    KeReleaseGuardedMutex(&ControlLock);
    if (NT_SUCCESS(status) && done < MinLength) status = STATUS_DEVICE_PROTOCOL_ERROR;
    return status;
}

NTSTATUS XonarUsbDevice::Impl::ClassSet(UCHAR Selector, UCHAR Channel, UCHAR Entity, const VOID* Data, ULONG Length)
{
    PAGED_CODE();
    NTSTATUS status = CheckReady();
    if (!NT_SUCCESS(status)) return status;
    KeAcquireGuardedMutex(&ControlLock);
    status = ClassRequestLocked(false, xonar::uac2::kRequestCur, Selector, Channel, Entity, const_cast<PVOID>(Data),
                                Length, nullptr);
    KeReleaseGuardedMutex(&ControlLock);
    return status;
}

NTSTATUS XonarUsbDevice::GetSampleRate(UCHAR ClockId, ULONG* Hz)
{
    PAGED_CODE();
    *Hz = 0;
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    UCHAR raw[4] = {};
    const NTSTATUS status = m_Impl->ClassGet(xonar::uac2::kCsSamFreqControl, 0, ClockId, raw, sizeof(raw), sizeof(raw));
    if (NT_SUCCESS(status)) *Hz = ReadLe32(raw);
    return status;
}

NTSTATUS XonarUsbDevice::SetSampleRate(UCHAR ClockId, ULONG Hz)
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    UCHAR raw[4];
    WriteLe32(raw, Hz);
    const NTSTATUS status = m_Impl->ClassSet(xonar::uac2::kCsSamFreqControl, 0, ClockId, raw, sizeof(raw));
    if (NT_SUCCESS(status)) XONARSE_INFO("USB : horloge %u programmée à %lu Hz", ClockId, Hz);
    return status;
}

NTSTATUS XonarUsbDevice::GetSampleRateRanges(UCHAR ClockId, XONAR_RANGE32* Ranges, ULONG MaxRanges, ULONG* Count)
{
    PAGED_CODE();
    *Count = 0;
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    if (MaxRanges != 0 && Ranges == nullptr) return STATUS_INVALID_PARAMETER;
    Impl* d = m_Impl;
    NTSTATUS status = d->CheckReady();
    if (!NT_SUCCESS(status)) return status;

    constexpr ULONG kSub = 12; // dMIN, dMAX, dRES sur 4 octets chacun
    ULONG reported = 0;
    ULONG bytes = 0;
    KeAcquireGuardedMutex(&d->ControlLock);
    status = d->ReadRangeLocked(xonar::uac2::kCsSamFreqControl, 0, ClockId, kSub, &reported, &bytes);
    if (NT_SUCCESS(status)) {
        const ULONG available = bytes >= 2 ? (bytes - 2) / kSub : 0;
        const ULONG fill = reported < MaxRanges ? reported : MaxRanges;
        for (ULONG i = 0; i < fill; ++i) {
            if (i < available) {
                const UCHAR* p = d->ControlBuffer + 2 + i * kSub;
                Ranges[i].Min = ReadLe32(p);
                Ranges[i].Max = ReadLe32(p + 4);
                Ranges[i].Res = ReadLe32(p + 8);
            } else {
                Ranges[i] = XONAR_RANGE32{0, 0, 0}; // annoncé mais non transmis
            }
        }
        if (available < reported) {
            XONARSE_WARN("USB : RANGE horloge %u : %lu sous-intervalles annoncés, %lu reçus", ClockId, reported, available);
        }
        *Count = reported;
    }
    KeReleaseGuardedMutex(&d->ControlLock);
    return status;
}

NTSTATUS XonarUsbDevice::GetClockValid(UCHAR ClockId, bool* Valid)
{
    PAGED_CODE();
    *Valid = false;
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    UCHAR raw = 0;
    const NTSTATUS status = m_Impl->ClassGet(xonar::uac2::kCsClockValidControl, 0, ClockId, &raw, 1, 1);
    if (NT_SUCCESS(status)) *Valid = raw != 0;
    return status;
}

NTSTATUS XonarUsbDevice::GetMute(UCHAR UnitId, UCHAR Channel, bool* Muted)
{
    PAGED_CODE();
    *Muted = false;
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    UCHAR raw = 0;
    const NTSTATUS status = m_Impl->ClassGet(xonar::uac2::kFuMuteControl, Channel, UnitId, &raw, 1, 1);
    if (NT_SUCCESS(status)) *Muted = raw != 0;
    return status;
}

NTSTATUS XonarUsbDevice::SetMute(UCHAR UnitId, UCHAR Channel, bool Muted)
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    const UCHAR raw = Muted ? 1 : 0;
    return m_Impl->ClassSet(xonar::uac2::kFuMuteControl, Channel, UnitId, &raw, 1);
}

NTSTATUS XonarUsbDevice::GetVolume(UCHAR UnitId, UCHAR Channel, SHORT* Value)
{
    PAGED_CODE();
    *Value = 0;
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    UCHAR raw[2] = {};
    const NTSTATUS status = m_Impl->ClassGet(xonar::uac2::kFuVolumeControl, Channel, UnitId, raw, sizeof(raw), sizeof(raw));
    if (NT_SUCCESS(status)) *Value = static_cast<SHORT>(ReadLe16(raw));
    return status;
}

NTSTATUS XonarUsbDevice::SetVolume(UCHAR UnitId, UCHAR Channel, SHORT Value)
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    UCHAR raw[2];
    WriteLe16(raw, static_cast<USHORT>(Value));
    return m_Impl->ClassSet(xonar::uac2::kFuVolumeControl, Channel, UnitId, raw, sizeof(raw));
}

NTSTATUS XonarUsbDevice::GetVolumeRange(UCHAR UnitId, UCHAR Channel, XONAR_RANGE16* Range)
{
    PAGED_CODE();
    *Range = XONAR_RANGE16{0, 0, 0};
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    Impl* d = m_Impl;
    NTSTATUS status = d->CheckReady();
    if (!NT_SUCCESS(status)) return status;

    constexpr ULONG kSub = 6; // wMIN, wMAX, wRES sur 2 octets chacun
    ULONG reported = 0;
    ULONG bytes = 0;
    KeAcquireGuardedMutex(&d->ControlLock);
    status = d->ReadRangeLocked(xonar::uac2::kFuVolumeControl, Channel, UnitId, kSub, &reported, &bytes);
    if (NT_SUCCESS(status)) {
        if (reported == 0 || bytes < 2 + kSub) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            const UCHAR* p = d->ControlBuffer + 2;
            Range->Min = static_cast<SHORT>(ReadLe16(p));
            Range->Max = static_cast<SHORT>(ReadLe16(p + 2));
            Range->Res = ReadLe16(p + 4);
        }
    }
    KeReleaseGuardedMutex(&d->ControlLock);
    return status;
}

NTSTATUS XonarUsbDevice::GetConnectorState(UCHAR TerminalId, bool* Connected, UCHAR* NrChannels, ULONG* ChannelMask)
{
    PAGED_CODE();
    // Valeur sûre en cas d'échec : « connecté », pour ne jamais masquer un endpoint.
    *Connected = true;
    if (NrChannels != nullptr) *NrChannels = 0;
    if (ChannelMask != nullptr) *ChannelMask = 0;
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    Impl* d = m_Impl;
    NTSTATUS status = d->CheckReady();
    if (!NT_SUCCESS(status)) return status;

    // Cluster UAC2 : bNrChannels (1), bmChannelConfig (4), iChannelNames (1).
    UCHAR raw[6] = {};
    ULONG done = 0;
    KeAcquireGuardedMutex(&d->ControlLock);
    status = d->ClassRequestLocked(true, xonar::uac2::kRequestCur, xonar::uac2::kTeConnectorControl, 0, TerminalId, raw,
                                   sizeof(raw), &done);
    KeReleaseGuardedMutex(&d->ControlLock);
    if (!NT_SUCCESS(status)) return status;
    if (done < 1) return STATUS_DEVICE_PROTOCOL_ERROR;
    *Connected = raw[0] != 0;
    if (NrChannels != nullptr) *NrChannels = raw[0];
    if (ChannelMask != nullptr && done >= 5) *ChannelMask = ReadLe32(raw + 1);
    return STATUS_SUCCESS;
}

// --- Configuration ---

void XonarUsbDevice::Impl::FreeInterfaceInfos()
{
    PAGED_CODE();
    for (ULONG i = 0; i < kMaxInterfaces; ++i) {
        if (Interfaces[i].Info != nullptr) ExFreePool(Interfaces[i].Info);
        Interfaces[i] = InterfaceState{};
    }
    InterfaceCount = 0;
    InterruptPipe = nullptr;
    InterruptLength = 0;
}

XonarUsbDevice::Impl::InterfaceState* XonarUsbDevice::Impl::FindInterfaceState(UCHAR Number)
{
    PAGED_CODE();
    for (ULONG i = 0; i < InterfaceCount; ++i) {
        if (Interfaces[i].Number == Number) return &Interfaces[i];
    }
    return nullptr;
}

PUSB_INTERFACE_DESCRIPTOR XonarUsbDevice::Impl::FindInterfaceDescriptor(UCHAR Number, UCHAR Alt, ULONG* After) const
{
    PAGED_CODE();
    *After = 0;
    if (Config == nullptr) return nullptr;
    const auto* base = reinterpret_cast<const UCHAR*>(Config);
    ULONG offset = 0;
    while (const USB_COMMON_DESCRIPTOR* d = WalkDescriptor(base, ConfigLength, &offset)) {
        if (d->bDescriptorType != USB_INTERFACE_DESCRIPTOR_TYPE || d->bLength < sizeof(USB_INTERFACE_DESCRIPTOR)) continue;
        const auto* id = reinterpret_cast<const USB_INTERFACE_DESCRIPTOR*>(d);
        if (id->bInterfaceNumber == Number && id->bAlternateSetting == Alt) {
            *After = offset;
            return const_cast<PUSB_INTERFACE_DESCRIPTOR>(id);
        }
    }
    return nullptr;
}

// Copie d'une USBD_INTERFACE_INFORMATION rendue par la pile (elle vit dans l'URB, libéré ensuite).
static PUSBD_INTERFACE_INFORMATION CopyInterfaceInfo(_In_ const USBD_INTERFACE_INFORMATION* Source)
{
    PAGED_CODE();
    const ULONG header = FIELD_OFFSET(USBD_INTERFACE_INFORMATION, Pipes);
    const ULONG need = header + Source->NumberOfPipes * sizeof(USBD_PIPE_INFORMATION);
    if (Source->Length < header || Source->Length < need) return nullptr;
    auto* copy = static_cast<PUSBD_INTERFACE_INFORMATION>(
        ExAllocatePool2(POOL_FLAG_NON_PAGED, Source->Length, XONARSE_POOL_TAG));
    if (copy != nullptr) RtlCopyMemory(copy, Source, Source->Length);
    return copy;
}

void XonarUsbDevice::Impl::LogDeviceDescriptorLocked()
{
    PAGED_CODE();
    ULONG done = 0;
    const NTSTATUS status =
        ReadDescriptorLocked(USB_DEVICE_DESCRIPTOR_TYPE, 0, ControlBuffer, sizeof(USB_DEVICE_DESCRIPTOR), &done);
    if (!NT_SUCCESS(status) || done < sizeof(USB_DEVICE_DESCRIPTOR)) {
        XONARSE_WARN("USB : descripteur de périphérique illisible (0x%08X, %lu octets)", status, done);
        return;
    }
    const auto* dd = reinterpret_cast<const USB_DEVICE_DESCRIPTOR*>(ControlBuffer);
    XONARSE_INFO("USB : périphérique VID %04X PID %04X bcdDevice %04X bcdUSB %04X", dd->idVendor, dd->idProduct,
                 dd->bcdDevice, dd->bcdUSB);
    if (dd->idVendor != xonar::hw::kVendorId || dd->idProduct != xonar::hw::kProductId) {
        XONARSE_WARN("USB : VID/PID inattendus, la carte n'est peut-être pas une Xonar SE");
    }
}

// Lit le descripteur de configuration complet dans un tampon non paginé alloué (à libérer
// par ExFreePool) ; *Length = octets réellement exploitables.
NTSTATUS XonarUsbDevice::Impl::ReadConfigurationLocked(PUSB_CONFIGURATION_DESCRIPTOR* Out, ULONG* Length)
{
    PAGED_CODE();
    *Out = nullptr;
    *Length = 0;
    // 1. Les 9 premiers octets donnent wTotalLength. Via usbccgp, la pile rend le
    //    descripteur de la fonction seule (interfaces 0 à 5), plus court que celui de la carte.
    ULONG done = 0;
    NTSTATUS status = ReadDescriptorLocked(USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, ControlBuffer,
                                           sizeof(USB_CONFIGURATION_DESCRIPTOR), &done);
    if (!NT_SUCCESS(status)) return status;
    const auto* head = reinterpret_cast<const USB_CONFIGURATION_DESCRIPTOR*>(ControlBuffer);
    if (done < sizeof(USB_CONFIGURATION_DESCRIPTOR) || head->bDescriptorType != USB_CONFIGURATION_DESCRIPTOR_TYPE ||
        head->wTotalLength < sizeof(USB_CONFIGURATION_DESCRIPTOR)) {
        XONARSE_ERROR("USB : en-tête de configuration invalide (%lu octets)", done);
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    const ULONG total = head->wTotalLength;

    // 2. Descripteur complet.
    auto* config =
        static_cast<PUSB_CONFIGURATION_DESCRIPTOR>(ExAllocatePool2(POOL_FLAG_NON_PAGED, total, XONARSE_POOL_TAG));
    if (config == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    status = ReadDescriptorLocked(USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, config, total, &done);
    if (!NT_SUCCESS(status) || done < sizeof(USB_CONFIGURATION_DESCRIPTOR)) {
        ExFreePool(config);
        return NT_SUCCESS(status) ? STATUS_DEVICE_CONFIGURATION_ERROR : status;
    }
    if (done < total) XONARSE_WARN("USB : configuration tronquée (%lu octets sur %lu)", done, total);
    *Out = config;
    *Length = done < total ? done : total;
    return STATUS_SUCCESS;
}

NTSTATUS XonarUsbDevice::Impl::LoadConfigurationLocked()
{
    PAGED_CODE();
    PUSB_CONFIGURATION_DESCRIPTOR config = nullptr;
    ULONG length = 0;
    const NTSTATUS status = ReadConfigurationLocked(&config, &length);
    if (!NT_SUCCESS(status)) return status;
    if (Config != nullptr) ExFreePool(Config);
    Config = config;
    ConfigLength = length;
    XONARSE_INFO("USB : configuration %u lue : %lu octets, %u interface(s)", config->bConfigurationValue, ConfigLength,
                 config->bNumInterfaces);
    return STATUS_SUCCESS;
}

// Redémarrage après un STOP PnP : la configuration déjà sélectionnée reste-t-elle utilisable ?
// Oui si la carte n'a pas été déconfigurée (GET_CONFIGURATION concordant ; une réponse en
// échec, par exemple refusée par usbccgp, n'est pas concluante) et si le descripteur de
// configuration est identique octet pour octet à celui qui a servi à la sélection.
bool XonarUsbDevice::Impl::ConfigurationStillValidLocked()
{
    PAGED_CODE();
    if (!Configured || Config == nullptr || ConfigHandle == nullptr || InterfaceCount == 0) return false;

    struct _URB_CONTROL_GET_CONFIGURATION_REQUEST req = {};
    req.Hdr.Length = sizeof(req);
    req.Hdr.Function = URB_FUNCTION_GET_CONFIGURATION; // sens IN implicite (Reserved0 à 0)
    req.TransferBuffer = ControlBuffer;
    req.TransferBufferLength = 1;
    ControlBuffer[0] = 0;
    NTSTATUS status = Submit(reinterpret_cast<PURB>(&req), false, kControlTimeoutMs, false);
    if (NT_SUCCESS(status) && req.TransferBufferLength >= 1) {
        if (ControlBuffer[0] != Config->bConfigurationValue) {
            XONARSE_WARN("USB : configuration courante %u, %u attendue", ControlBuffer[0], Config->bConfigurationValue);
            return false;
        }
    } else if (IsDeviceGoneStatus(status) || status == STATUS_IO_TIMEOUT) {
        return false;
    } else {
        XONARSE_INFO("USB : GET_CONFIGURATION non concluant (0x%08X), contrôle par le descripteur seul", status);
    }

    PUSB_CONFIGURATION_DESCRIPTOR fresh = nullptr;
    ULONG length = 0;
    status = ReadConfigurationLocked(&fresh, &length);
    if (!NT_SUCCESS(status)) return false;
    const bool same = length == ConfigLength && RtlCompareMemory(fresh, Config, length) == length;
    ExFreePool(fresh);
    if (!same) XONARSE_WARN("USB : descripteur de configuration modifié depuis la sélection");
    return same;
}

void XonarUsbDevice::Impl::FindInterruptPipe()
{
    PAGED_CODE();
    InterruptPipe = nullptr;
    InterruptLength = 0;
    const InterfaceState* ac = FindInterfaceState(AcInterface);
    if (ac == nullptr || ac->Info == nullptr) return;
    const USBD_PIPE_INFORMATION* found = nullptr;
    for (ULONG p = 0; p < ac->Info->NumberOfPipes; ++p) {
        const USBD_PIPE_INFORMATION& pipe = ac->Info->Pipes[p];
        if (pipe.EndpointAddress == xonar::hw::kEpInterrupt) {
            found = &pipe;
            break;
        }
        if (found == nullptr && pipe.PipeType == UsbdPipeTypeInterrupt && USB_ENDPOINT_DIRECTION_IN(pipe.EndpointAddress)) {
            found = &pipe; // repli : premier pipe d'interruption IN de l'interface AudioControl
        }
    }
    if (found == nullptr) return;
    InterruptPipe = found->PipeHandle;
    ULONG len = found->MaximumPacketSize;
    if (len < kInterruptMessageBytes) len = kInterruptMessageBytes;
    if (len > kInterruptBufferBytes) len = kInterruptBufferBytes;
    InterruptLength = len;
    XONARSE_INFO("USB : pipe d'interruption 0x%02X, %lu octets, intervalle %u", found->EndpointAddress, len,
                 found->Interval);
}

NTSTATUS XonarUsbDevice::Impl::SelectConfigurationLocked()
{
    PAGED_CODE();
    // Toutes les interfaces de la fonction, en altsetting 0, dans l'ordre du descripteur.
    USBD_INTERFACE_LIST_ENTRY list[kMaxInterfaces + 1] = {};
    ULONG count = 0;
    const auto* base = reinterpret_cast<const UCHAR*>(Config);
    ULONG offset = 0;
    while (const USB_COMMON_DESCRIPTOR* d = WalkDescriptor(base, ConfigLength, &offset)) {
        if (d->bDescriptorType != USB_INTERFACE_DESCRIPTOR_TYPE || d->bLength < sizeof(USB_INTERFACE_DESCRIPTOR)) continue;
        auto* id = const_cast<PUSB_INTERFACE_DESCRIPTOR>(reinterpret_cast<const USB_INTERFACE_DESCRIPTOR*>(d));
        if (id->bAlternateSetting != 0) continue;
        if (count == kMaxInterfaces) {
            XONARSE_WARN("USB : plus de %lu interfaces, interface %u ignorée", kMaxInterfaces, id->bInterfaceNumber);
            continue;
        }
        list[count++].InterfaceDescriptor = id;
    }
    if (count == 0) {
        XONARSE_ERROR("USB : aucune interface dans la configuration");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    PURB urb = nullptr;
    const bool usbd = Usbd != nullptr;
    NTSTATUS status;
    if (usbd) {
        status = USBD_SelectConfigUrbAllocateAndBuild(Usbd, Config, list, &urb);
        if (!NT_SUCCESS(status)) return status;
    } else {
        urb = USBD_CreateConfigurationRequestEx(Config, list); // pool, libéré par ExFreePool
        if (urb == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    }

    status = Submit(urb, usbd, kConfigTimeoutMs, false);
    if (NT_SUCCESS(status)) {
        FreeInterfaceInfos();
        ConfigHandle = urb->UrbSelectConfiguration.ConfigurationHandle;
        AcInterface = xonar::hw::kIfControl;
        bool acFound = false;
        for (ULONG i = 0; i < count; ++i) {
            const PUSB_INTERFACE_DESCRIPTOR id = list[i].InterfaceDescriptor;
            InterfaceState& st = Interfaces[i];
            st.Number = id->bInterfaceNumber;
            st.CurrentAlt = 0;
            st.Class = id->bInterfaceClass;
            st.SubClass = id->bInterfaceSubClass;
            st.Info = list[i].Interface != nullptr ? CopyInterfaceInfo(list[i].Interface) : nullptr;
            if (st.Info == nullptr) {
                status = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                XONARSE_INFO("USB : interface %u (classe %02X/%02X) altsetting 0, %lu pipe(s)", st.Number, st.Class,
                             st.SubClass, st.Info->NumberOfPipes);
            }
            if (!acFound && st.Class == kAudioClass && st.SubClass == kAudioControlSubclass) {
                AcInterface = st.Number;
                acFound = true;
            }
        }
        InterfaceCount = count;
        if (!acFound) XONARSE_WARN("USB : interface AudioControl absente, interface %u supposée", AcInterface);
        if (NT_SUCCESS(status)) FindInterruptPipe();
    } else {
        XONARSE_ERROR("USB : SELECT_CONFIGURATION 0x%08X (USBD 0x%08X)", status, urb->UrbHeader.Status);
    }

    if (usbd) {
        USBD_UrbFree(Usbd, urb);
    } else {
        ExFreePool(urb);
    }
    return status;
}

void XonarUsbDevice::Impl::FillPipes(const USBD_INTERFACE_INFORMATION* Info, ULONG DescOffset, XONAR_AS_PIPES* Pipes) const
{
    PAGED_CODE();
    RtlZeroMemory(Pipes, sizeof(*Pipes));
    // Parcours des descripteurs d'endpoint de l'altsetting : l'usage (bits 4-5 de
    // bmAttributes : 0 données, 1 feedback, 2 données à feedback implicite) n'apparaît
    // pas dans USBD_PIPE_INFORMATION. bInterval est pris tel quel dans le descripteur.
    const auto* base = reinterpret_cast<const UCHAR*>(Config);
    ULONG offset = DescOffset;
    while (const USB_COMMON_DESCRIPTOR* d = WalkDescriptor(base, ConfigLength, &offset)) {
        if (d->bDescriptorType == USB_INTERFACE_DESCRIPTOR_TYPE) break;
        if (d->bDescriptorType != USB_ENDPOINT_DESCRIPTOR_TYPE || d->bLength < sizeof(USB_ENDPOINT_DESCRIPTOR)) continue;
        const auto* ed = reinterpret_cast<const USB_ENDPOINT_DESCRIPTOR*>(d);
        const USBD_PIPE_INFORMATION* pipe = nullptr;
        for (ULONG p = 0; p < Info->NumberOfPipes; ++p) {
            if (Info->Pipes[p].EndpointAddress == ed->bEndpointAddress) pipe = &Info->Pipes[p];
        }
        if (pipe == nullptr) continue;
        const UCHAR usage = static_cast<UCHAR>((ed->bmAttributes >> 4) & 0x03);
        const bool isIn = USB_ENDPOINT_DIRECTION_IN(ed->bEndpointAddress) != 0;
        if (usage == 1 && isIn) {
            if (Pipes->FeedbackPipe == nullptr) {
                Pipes->FeedbackPipe = pipe->PipeHandle;
                Pipes->FeedbackEndpoint = ed->bEndpointAddress;
                Pipes->FeedbackInterval = ed->bInterval;
            }
        } else if (Pipes->DataPipe == nullptr) {
            Pipes->DataPipe = pipe->PipeHandle;
            Pipes->DataEndpoint = ed->bEndpointAddress;
            Pipes->DataInterval = ed->bInterval;
            Pipes->DataMaxPacketBytes = static_cast<USHORT>(pipe->MaximumPacketSize);
        }
    }

    // Repli sans descripteurs exploitables : premier pipe = données ; si les données vont
    // vers la carte, un pipe IN de l'altsetting est le feedback.
    if (Pipes->DataPipe == nullptr && Info->NumberOfPipes != 0) {
        const USBD_PIPE_INFORMATION& p0 = Info->Pipes[0];
        Pipes->DataPipe = p0.PipeHandle;
        Pipes->DataEndpoint = p0.EndpointAddress;
        Pipes->DataInterval = p0.Interval;
        Pipes->DataMaxPacketBytes = static_cast<USHORT>(p0.MaximumPacketSize);
    }
    if (Pipes->FeedbackPipe == nullptr && Pipes->DataPipe != nullptr && !USB_ENDPOINT_DIRECTION_IN(Pipes->DataEndpoint)) {
        for (ULONG p = 0; p < Info->NumberOfPipes; ++p) {
            const USBD_PIPE_INFORMATION& pi = Info->Pipes[p];
            if (USB_ENDPOINT_DIRECTION_IN(pi.EndpointAddress)) {
                Pipes->FeedbackPipe = pi.PipeHandle;
                Pipes->FeedbackEndpoint = pi.EndpointAddress;
                Pipes->FeedbackInterval = pi.Interval;
                break;
            }
        }
    }
}

NTSTATUS XonarUsbDevice::Impl::SelectAltLocked(UCHAR Interface, UCHAR Alt, XONAR_AS_PIPES* Pipes)
{
    PAGED_CODE();
    if (Pipes != nullptr) RtlZeroMemory(Pipes, sizeof(*Pipes));
    InterfaceState* st = FindInterfaceState(Interface);
    if (st == nullptr) return STATUS_INVALID_PARAMETER;
    ULONG after = 0;
    PUSB_INTERFACE_DESCRIPTOR desc = FindInterfaceDescriptor(Interface, Alt, &after);
    if (desc == nullptr) {
        XONARSE_WARN("USB : altsetting %u absent de l'interface %u", Alt, Interface);
        return STATUS_INVALID_PARAMETER;
    }

    PURB urb = nullptr;
    PUSBD_INTERFACE_INFORMATION info = nullptr;
    const bool usbd = Usbd != nullptr;
    if (usbd) {
        USBD_INTERFACE_LIST_ENTRY entry = {};
        entry.InterfaceDescriptor = desc;
        entry.Interface = nullptr;
        const NTSTATUS s = USBD_SelectInterfaceUrbAllocateAndBuild(Usbd, ConfigHandle, &entry, &urb);
        if (!NT_SUCCESS(s)) return s;
        info = entry.Interface;
    } else {
        // Repli : URB en pool, taille calculée pour bNumEndpoints pipes (au moins un).
        const ULONG numEp = desc->bNumEndpoints;
        const ULONG size = static_cast<ULONG>(GET_SELECT_INTERFACE_REQUEST_SIZE(numEp != 0 ? numEp : 1));
        urb = static_cast<PURB>(ExAllocatePool2(POOL_FLAG_NON_PAGED, size, XONARSE_POOL_TAG));
        if (urb == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
        UsbBuildSelectInterfaceRequest(urb, static_cast<USHORT>(size), ConfigHandle, Interface, Alt);
        info = &urb->UrbSelectInterface.Interface;
        for (ULONG p = 0; p < numEp; ++p) info->Pipes[p].MaximumTransferSize = USBD_DEFAULT_MAXIMUM_TRANSFER_SIZE;
    }

    NTSTATUS status = info != nullptr ? Submit(urb, usbd, kConfigTimeoutMs, false) : STATUS_INVALID_PARAMETER;
    if (NT_SUCCESS(status)) {
        PUSBD_INTERFACE_INFORMATION copy = CopyInterfaceInfo(info);
        // La carte est dans le nouvel altsetting même si la copie échoue.
        st->CurrentAlt = Alt;
        if (copy == nullptr) {
            status = STATUS_INSUFFICIENT_RESOURCES;
        } else {
            if (st->Info != nullptr) ExFreePool(st->Info);
            st->Info = copy;
            if (Pipes != nullptr && Alt != 0) FillPipes(copy, after, Pipes);
            XONARSE_INFO("USB : interface %u -> altsetting %u (%lu pipe(s))", Interface, Alt, copy->NumberOfPipes);
        }
    } else {
        XONARSE_ERROR("USB : SELECT_INTERFACE %u/%u : 0x%08X", Interface, Alt, status);
    }

    if (usbd) {
        USBD_UrbFree(Usbd, urb);
    } else {
        ExFreePool(urb);
    }
    return status;
}

NTSTATUS XonarUsbDevice::SelectAltSetting(UCHAR Interface, UCHAR AltSetting, XONAR_AS_PIPES* Pipes)
{
    PAGED_CODE();
    if (Pipes != nullptr) RtlZeroMemory(Pipes, sizeof(*Pipes));
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    Impl* d = m_Impl;
    NTSTATUS status = d->CheckReady();
    if (!NT_SUCCESS(status)) return status;
    KeAcquireGuardedMutex(&d->ControlLock);
    status = d->SelectAltLocked(Interface, AltSetting, Pipes);
    KeReleaseGuardedMutex(&d->ControlLock);
    return status;
}

// --- Lecteur d'interruption : démarrage et arrêt ---

void XonarUsbDevice::Impl::FreeReaderResources()
{
    PAGED_CODE();
    for (ULONG i = 0; i < kReaderUrbs; ++i) {
        ReaderSlot& s = Reader[i];
        if (s.Irp != nullptr) IoFreeIrp(s.Irp);
        if (s.Urb != nullptr) {
            if (s.UrbFromUsbd) {
                USBD_UrbFree(Usbd, s.Urb);
            } else {
                ExFreePool(s.Urb);
            }
        }
        s.Irp = nullptr;
        s.Urb = nullptr;
        s.UrbFromUsbd = false;
        s.State = kSlotIdle;
        s.NeedsRestart = 0;
    }
    if (WorkItem != nullptr) {
        IoFreeWorkItem(WorkItem);
        WorkItem = nullptr;
    }
}

NTSTATUS XonarUsbDevice::Impl::StartReader()
{
    PAGED_CODE();
    if (ReaderAllocated) return STATUS_SUCCESS;
    if (InterruptPipe == nullptr) {
        XONARSE_WARN("USB : pas de pipe d'interruption, notifications de jack indisponibles");
        return STATUS_SUCCESS;
    }

    WorkItem = IoAllocateWorkItem(Fdo);
    if (WorkItem == nullptr) return STATUS_INSUFFICIENT_RESOURCES;
    for (ULONG i = 0; i < kReaderUrbs; ++i) {
        ReaderSlot& s = Reader[i];
        s.Owner = this;
        s.State = kSlotIdle;
        s.NeedsRestart = 0;
        s.Irp = IoAllocateIrp(Lower->StackSize, FALSE);
        if (Usbd != nullptr) {
            PURB urb = nullptr;
            if (NT_SUCCESS(USBD_UrbAllocate(Usbd, &urb))) {
                s.Urb = urb;
                s.UrbFromUsbd = true;
            }
        } else {
            s.Urb = static_cast<PURB>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(URB), XONARSE_POOL_TAG));
            s.UrbFromUsbd = false;
        }
        if (s.Irp == nullptr || s.Urb == nullptr) {
            FreeReaderResources();
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    ResetRing();
    InterlockedExchange(&ReaderStopping, 0);
    InterlockedExchange(&ReaderFailed, 0);
    InterlockedExchange(&ReaderErrors, 0);
    InterlockedExchange(&ReaderOutstanding, 1); // biais, retiré par StopReader
    KeClearEvent(&ReaderIdle);
    KeClearEvent(&ReaderStopSignal);
    ReaderAllocated = true;
    for (ULONG i = 0; i < kReaderUrbs; ++i) {
        InterlockedIncrement(&ReaderOutstanding);
        SubmitSlot(&Reader[i]);
    }
    XONARSE_INFO("USB : lecteur d'interruption démarré (%lu URB en vol)", kReaderUrbs);
    return STATUS_SUCCESS;
}

void XonarUsbDevice::Impl::CancelReaderIo()
{
    PAGED_CODE();
    // ABORT_PIPE termine tous les transferts du pipe ; IoCancelIrp couvre un IRP soumis juste
    // après (sa soumission avait lu ReaderStopping avant sa mise à 1). ReaderStopping est
    // déjà à 1 : un emplacement Libre ou en Soumission ne sera plus envoyé (SubmitSlot relit
    // le drapeau après être passé en EnVol), il n'y a donc rien à annuler.
    (void)PipeRequest(URB_FUNCTION_ABORT_PIPE, InterruptPipe, true);
    for (ULONG i = 0; i < kReaderUrbs; ++i) {
        ReaderSlot& s = Reader[i];
        if (s.Irp == nullptr) continue;
        // EnVol -> Annulation : tant que cet état dure, la complétion ne peut ni réutiliser
        // ni resoumettre l'IRP (elle passe en Remis et nous laisse solder).
        if (InterlockedCompareExchange(&s.State, kSlotCancelling, kSlotInFlight) != kSlotInFlight) continue;
        IoCancelIrp(s.Irp);
        if (InterlockedCompareExchange(&s.State, kSlotInFlight, kSlotCancelling) != kSlotCancelling) {
            // Remis : l'IRP est revenu pendant IoCancelIrp (ou n'a pas été envoyé) ; arrêt en
            // cours, il n'y a qu'à le libérer et rendre son opération en cours.
            InterlockedExchange(&s.State, kSlotIdle);
            ReleaseOutstanding();
        }
    }
}

void XonarUsbDevice::Impl::StopReader()
{
    PAGED_CODE();
    if (!ReaderAllocated) return;
    InterlockedExchange(&ReaderStopping, 1); // barrière complète avant la lecture des états
    KeSetEvent(&ReaderStopSignal, IO_NO_INCREMENT, FALSE); // écourte une relance différée
    CancelReaderIo();
    ReleaseOutstanding(); // retire le biais

    // Attente de la retombée de tous les IRP et de l'élément de travail. Par sécurité, on
    // relance l'annulation périodiquement tant que le compteur n'est pas à 0.
    ULONG rounds = 0;
    LARGE_INTEGER poll = RelativeTimeoutMs(kStopPollMs);
    while (KeWaitForSingleObject(&ReaderIdle, Executive, KernelMode, FALSE, &poll) == STATUS_TIMEOUT) {
        if (++rounds % 20 == 0) XONARSE_WARN("USB : arrêt du lecteur d'interruption en attente (%lu)", rounds);
        CancelReaderIo();
    }
    FreeReaderResources();
    ReaderAllocated = false;
    XONARSE_INFO("USB : lecteur d'interruption arrêté");
}

// --- Journal de diagnostic du démarrage (réponses aux questions ouvertes [T]) ---

static bool DiagnosticMustStop(NTSTATUS Status)
{
    PAGED_CODE();
    return Status == STATUS_IO_TIMEOUT || IsDeviceGoneStatus(Status);
}

static void LogStartupDiagnostics(_In_ XonarUsbDevice* Dev)
{
    PAGED_CODE();
    using namespace xonar::hw;
    NTSTATUS s;

    static const UCHAR clocks[] = {kClockSpeakers, kClockHeadphones, kClockSpdif};
    for (UCHAR clock : clocks) {
        XONAR_RANGE32 ranges[8] = {};
        ULONG count = 0;
        s = Dev->GetSampleRateRanges(clock, ranges, ARRAYSIZE(ranges), &count);
        if (NT_SUCCESS(s)) {
            XONARSE_INFO("Diag : horloge %u, RANGE : %lu sous-intervalle(s)", clock, count);
            for (ULONG i = 0; i < count && i < ARRAYSIZE(ranges); ++i) {
                XONARSE_INFO("Diag :   [%lu] min=%lu max=%lu pas=%lu", i, ranges[i].Min, ranges[i].Max, ranges[i].Res);
            }
        } else {
            XONARSE_WARN("Diag : horloge %u, RANGE en échec 0x%08X", clock, s);
            if (DiagnosticMustStop(s)) return;
        }
        ULONG hz = 0;
        s = Dev->GetSampleRate(clock, &hz);
        if (NT_SUCCESS(s)) {
            XONARSE_INFO("Diag : horloge %u, fréquence courante %lu Hz", clock, hz);
        } else {
            XONARSE_WARN("Diag : horloge %u, CUR fréquence en échec 0x%08X", clock, s);
            if (DiagnosticMustStop(s)) return;
        }
        bool valid = false;
        s = Dev->GetClockValid(clock, &valid);
        if (NT_SUCCESS(s)) {
            XONARSE_INFO("Diag : horloge %u, %s", clock, valid ? "valide" : "NON valide");
        } else {
            XONARSE_WARN("Diag : horloge %u, validité en échec 0x%08X", clock, s);
            if (DiagnosticMustStop(s)) return;
        }
    }

    static const UCHAR terminals[] = {kOtSpeaker, kOtHeadphones, kOtSpdif, kItMic1, kItLine, kItMic2};
    for (UCHAR t : terminals) {
        bool connected = true;
        UCHAR channels = 0;
        ULONG mask = 0;
        s = Dev->GetConnectorState(t, &connected, &channels, &mask);
        if (NT_SUCCESS(s)) {
            XONARSE_INFO("Diag : terminal %u, Connector : bNrChannels=%u masque=0x%08lX (%s)", t, channels, mask,
                         connected ? "branché" : "débranché");
        } else {
            XONARSE_WARN("Diag : terminal %u, Connector en échec 0x%08X", t, s);
            if (DiagnosticMustStop(s)) return;
        }
    }

    static const UCHAR volumeUnits[] = {kFuSpeakers, kFuHeadphones};
    for (UCHAR fu : volumeUnits) {
        XONAR_RANGE16 r = {};
        s = Dev->GetVolumeRange(fu, 1, &r);
        if (NT_SUCCESS(s)) {
            const DbView mn = ToDb(r.Min);
            const DbView mx = ToDb(r.Max);
            XONARSE_INFO("Diag : FU%u canal 1, RANGE volume min=%d (%c%lu.%02lu dB) max=%d (%c%lu.%02lu dB) pas=%u/256 dB",
                         fu, r.Min, mn.Sign, mn.Whole, mn.Hundredths, r.Max, mx.Sign, mx.Whole, mx.Hundredths, r.Res);
        } else {
            XONARSE_WARN("Diag : FU%u canal 1, RANGE volume en échec 0x%08X", fu, s);
            if (DiagnosticMustStop(s)) return;
        }
        SHORT v = 0;
        s = Dev->GetVolume(fu, 1, &v);
        if (NT_SUCCESS(s)) {
            const DbView cur = ToDb(v);
            XONARSE_INFO("Diag : FU%u canal 1, volume courant %d (%c%lu.%02lu dB)", fu, v, cur.Sign, cur.Whole,
                         cur.Hundredths);
        } else {
            XONARSE_WARN("Diag : FU%u canal 1, CUR volume en échec 0x%08X", fu, s);
            if (DiagnosticMustStop(s)) return;
        }
    }

    static const UCHAR muteUnits[] = {kFuSpeakers, kFuHeadphones, kFuSpdif};
    for (UCHAR fu : muteUnits) {
        bool muted = false;
        s = Dev->GetMute(fu, kMasterChannel, &muted);
        if (NT_SUCCESS(s)) {
            XONARSE_INFO("Diag : FU%u maître, mute=%s", fu, muted ? "oui" : "non");
        } else {
            XONARSE_WARN("Diag : FU%u maître, mute en échec 0x%08X", fu, s);
            if (DiagnosticMustStop(s)) return;
        }
    }
}

// --- Cycle de vie ---

NTSTATUS XonarUsbDevice::Start()
{
    PAGED_CODE();
    if (m_Impl == nullptr) return STATUS_INVALID_DEVICE_STATE;
    Impl* d = m_Impl;
    if (d->Started) return STATUS_SUCCESS;
    if (d->SurpriseRemoved || d->UsbdClosed) return STATUS_NO_SUCH_DEVICE;

    // Redémarrage après un STOP PnP : une configuration déjà sélectionnée et toujours valide
    // est conservée (pas de nouvelle SELECT_CONFIGURATION) ; un flux resté ouvert garde ainsi
    // des poignées de pipe valides. Sinon, sélection complète depuis l'altsetting 0.
    KeAcquireGuardedMutex(&d->ControlLock);
    NTSTATUS status = STATUS_SUCCESS;
    bool reused = false;
    if (d->Configured) {
        reused = d->ConfigurationStillValidLocked();
        if (reused) {
            XONARSE_INFO("USB : redémarrage, configuration conservée (poignées de pipe et altsettings inchangés)");
        } else {
            XONARSE_WARN("USB : redémarrage, configuration à refaire (%ld flux ouvert(s), poignées de pipe invalidées)",
                         d->OpenStreams);
            InterlockedExchange(&d->Configured, 0);
        }
    }
    if (!reused) {
        d->LogDeviceDescriptorLocked();
        status = d->LoadConfigurationLocked();
        if (NT_SUCCESS(status)) status = d->SelectConfigurationLocked();
        if (NT_SUCCESS(status)) InterlockedExchange(&d->Configured, 1);
    }
    KeReleaseGuardedMutex(&d->ControlLock);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("USB : démarrage impossible, configuration 0x%08X", status);
        return status;
    }
    if (reused && d->InterruptPipe != nullptr) {
        // Le pipe d'interruption a pu rester bloqué (STALL) pendant l'arrêt ; aucun transfert
        // n'y est en vol (lecteur arrêté).
        const NTSTATUS s = d->PipeRequest(URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL, d->InterruptPipe, false);
        if (!NT_SUCCESS(s)) XONARSE_WARN("USB : réinitialisation du pipe d'interruption au redémarrage 0x%08X", s);
    }

    status = d->StartReader();
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("USB : lecteur d'interruption non démarré 0x%08X", status);
        return status;
    }
    InterlockedExchange(&d->Started, 1);
    XONARSE_INFO("USB : démarré (%s)", d->Usbd != nullptr ? "poignée USBD" : "mode de repli");

    // Diagnostic seulement : aucune de ces lectures ne fait échouer le démarrage.
    LogStartupDiagnostics(this);
    return STATUS_SUCCESS;
}

// Arrêt commun à Stop (ForRemove = false) et PrepareForRemove (ForRemove = true).
void XonarUsbDevice::Impl::StopDevice(bool ForRemove)
{
    PAGED_CODE();
    StopReader();
    const bool wasStarted = InterlockedExchange(&Started, 0) != 0;
    // Idempotent ; pour un retrait, on referme quand même des interfaces laissées ouvertes
    // par un STOP PnP antérieur (boucle sans effet si toutes sont en altsetting 0).
    if (!wasStarted && !ForRemove) return;

    if (!SurpriseRemoved && Configured && !UsbdClosed) {
        KeAcquireGuardedMutex(&ControlLock);
        const LONG streams = OpenStreams;
        if (streams > 0 && !ForRemove) {
            // STOP PnP avec flux ouvert : le moteur de streaming garde ses pipes, on ne lui
            // retire pas l'interface (Start conservera la configuration si elle reste valide).
            XONARSE_WARN("USB : arrêt avec %ld flux ouvert(s), interfaces AudioStreaming laissées en l'état", streams);
        } else {
            if (streams > 0) XONARSE_WARN("USB : retrait avec %ld flux ouvert(s), interfaces refermées", streams);
            // Interfaces AudioStreaming laissées ouvertes : retour en altsetting 0 (bande passante rendue).
            for (ULONG i = 0; i < InterfaceCount; ++i) {
                const InterfaceState& st = Interfaces[i];
                if (st.Number == AcInterface || st.CurrentAlt == 0) continue;
                const NTSTATUS s = SelectAltLocked(st.Number, 0, nullptr);
                if (!NT_SUCCESS(s)) XONARSE_WARN("USB : interface %u non refermée (0x%08X)", st.Number, s);
            }
        }
        KeReleaseGuardedMutex(&ControlLock);
    }
    if (wasStarted) XONARSE_INFO("USB : arrêté");
}

void XonarUsbDevice::Stop()
{
    PAGED_CODE();
    if (m_Impl == nullptr) return;
    m_Impl->StopDevice(false);
}

void XonarUsbDevice::PrepareForRemove()
{
    PAGED_CODE();
    if (m_Impl == nullptr) return;
    Impl* d = m_Impl;
    d->StopDevice(true);
    KeAcquireGuardedMutex(&d->ControlLock);
    InterlockedExchange(&d->Configured, 0);
    d->UsbdClosed = true;
    d->FreeInterfaceInfos();
    if (d->Usbd != nullptr) {
        USBD_CloseHandle(d->Usbd);
        d->Usbd = nullptr;
    }
    KeReleaseGuardedMutex(&d->ControlLock);
    XONARSE_INFO("USB : prêt pour le retrait, poignée USBD fermée");
}

#pragma code_seg()
