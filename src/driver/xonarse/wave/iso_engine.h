// Moteur de streaming isochrone OUT asynchrone (USB 2.0 High Speed, UAC2) alimenté par
// le tampon cyclique WaveRT. Module wave/ uniquement.
//
// Principe :
// - XONAR_ISO_DATA_URBS URB de données en vol sur le pipe OUT, chacun de
//   XONAR_ISO_PACKETS_PER_URB paquets (1 paquet par microtrame de 125 µs, bInterval 1).
// - Pompe ordonnée : chaque complétion range son emplacement dans une file (m_Lock) ;
//   un seul propriétaire de la pompe à la fois (drapeau interlocked) retire, remplit sous
//   verrou, relâche le verrou puis soumet. L'ordre de remplissage est donc celui de la
//   soumission, même avec des complétions simultanées sur plusieurs processeurs.
// - La position de prélèvement avance par blocs d'une milliseconde, environ
//   XONAR_ISO_DATA_URBS ms avant la sortie réelle ; la position « jouée » avance à la
//   complétion de chaque URB.
// - La taille de chaque paquet vient d'un accumulateur 16.16 alimenté par l'endpoint de
//   feedback (valeur High Speed 16.16 en trames par microtrame), bornée à la nominale
//   +/- 1 trame et à wMaxPacketSize. Sans feedback valide : cadence nominale.
// - Erreurs : seuls l'arrêt et la disparition du périphérique sont définitifs. Un URB en
//   erreur passagère est marqué « à relancer » ; un IO_WORKITEM (PASSIVE_LEVEL) remet le
//   pipe en état s'il ne reste plus rien en vol dessus, puis resoumet, avec un budget de
//   tentatives et un délai croissant.
// - Arrêt : drapeau d'arrêt (plus de resoumission), compteur m_Outstanding (URB en vol,
//   élément de travail en file, complétions en cours) + KEVENT. AbortPipe en secours.
#pragma once

// usb_device.h en premier : il inclut usb.h / usbdlib.h / usbioctl.h sous extern "C".
#include "../usb/usb_device.h"

constexpr ULONG XONAR_ISO_DATA_URBS = 8;          // URB de données en vol
constexpr ULONG XONAR_ISO_PACKETS_PER_URB = 8;    // 8 microtrames = 1 ms (multiple de 8 exigé en HS, bInterval 1)
constexpr ULONG XONAR_FB_URBS = 3;                // URB de feedback en vol
constexpr ULONG XONAR_FB_PACKETS_PER_URB = 4;     // bInterval 4 = 1 paquet par ms : 4 ms par URB
constexpr ULONG XONAR_FB_PACKET_BYTES = 4;        // wMaxPacketSize de 0x85 / 0x86 / 0x87 (capture du 27/10/2026)
constexpr ULONG XONAR_MAX_NOTIFICATION_EVENTS = 4;
// Géométrie figée ci-dessus : seuls ces bInterval (descripteurs HS) sont acceptés.
constexpr UCHAR XONAR_ISO_DATA_INTERVAL = 1;      // 2^(1-1) = 1 microtrame
constexpr UCHAR XONAR_FB_INTERVAL = 4;            // 2^(4-1) = 8 microtrames = 1 ms

struct XonarIsoEngineConfig {
    ULONG SampleRate;          // Hz
    USHORT BlockAlign;         // octets par trame
    USHORT MaxPacketBytes;     // wMaxPacketSize effectif du pipe de données
    USBD_PIPE_HANDLE DataPipe;
    USBD_PIPE_HANDLE FeedbackPipe; // nullptr : pas de feedback, cadence nominale
    UCHAR DataInterval;        // bInterval du descripteur de l'endpoint de données
    UCHAR FeedbackInterval;    // bInterval du descripteur de l'endpoint de feedback
};

class XonarIsoOutEngine {
public:
    XonarIsoOutEngine();
    ~XonarIsoOutEngine();
    XonarIsoOutEngine(const XonarIsoOutEngine&) = delete;
    XonarIsoOutEngine& operator=(const XonarIsoOutEngine&) = delete;

    // Majorant des octets prélevés dans le tampon cyclique et pas encore joués (tous les
    // URB de données pleins à la cadence maximale autorisée). Multiple de BlockAlign.
    static ULONG MaxInFlightBytes(ULONG SampleRate, USHORT BlockAlign);

    // Alloue IRP, URB, tampons de transfert, MDL et élément de travail. Usb doit rester
    // valide jusqu'à Unprepare. STATUS_DEVICE_CONFIGURATION_ERROR si la géométrie des
    // endpoints (bInterval) ne correspond pas à celle du moteur.
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Prepare(_In_ XonarUsbDevice* Usb, _In_ const XonarIsoEngineConfig& Config);
    // Libère tout (le moteur doit être arrêté). Idempotent. Appelé au plus tard à
    // ACQUIRE -> STOP ou à la destruction du flux : les URB alloués avec UsbdHandle()
    // doivent être rendus avant XonarUsbDevice::PrepareForRemove (fermeture de la poignée).
    _IRQL_requires_(PASSIVE_LEVEL) void Unprepare();
    bool IsPrepared() const { return m_Prepared; }
    bool IsRunning() const { return m_Running; }

    // Tampon cyclique WaveRT (adresse noyau non paginée). NotificationCount = 0 : aucune
    // notification. Peut être appelé à tout moment : l'échange se fait sous verrou et
    // un tampon absent est remplacé par du silence.
    _IRQL_requires_max_(DISPATCH_LEVEL) void SetAudioBuffer(_In_opt_ PUCHAR Buffer, _In_ ULONG Size, _In_ ULONG NotificationCount);

    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Start();
    // Arrêt synchrone : au retour, plus aucun URB ni élément de travail n'est en vol.
    _IRQL_requires_(PASSIVE_LEVEL) void Stop();

    // PlayOffset : position jouée (URB terminés) dans le tampon cyclique.
    // ReadOffset : position de prélèvement (prochain octet copié vers un URB).
    // PlayedBytes : total linéaire joué depuis le dernier ResetPosition.
    _IRQL_requires_max_(DISPATCH_LEVEL) void GetPosition(_Out_ ULONG* PlayOffset, _Out_opt_ ULONG* ReadOffset,
                                                         _Out_opt_ ULONGLONG* PlayedBytes);
    _IRQL_requires_max_(DISPATCH_LEVEL) void ResetPosition();
    // Octets prélevés dans le tampon mais pas encore joués (URB en vol), valeur nominale.
    ULONG InFlightBytes() const { return m_InFlightBytes; }

    _IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS RegisterNotificationEvent(_In_ PKEVENT Event);
    _IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS UnregisterNotificationEvent(_In_ PKEVENT Event);

private:
    enum SlotState : UCHAR {
        SlotIdle = 0,   // ni en vol ni en file (arrêt, périphérique absent, abandon)
        SlotQueued,     // dans la file de la pompe (données seulement)
        SlotInFlight,   // possédé par la pile USB
        SlotLost,       // erreur passagère : à relancer par l'élément de travail
    };
    enum class Outcome { Ok, Transient, Gone };

    struct Slot {
        XonarIsoOutEngine* Engine;
        PIRP Irp;
        PURB Urb;
        PUCHAR Buffer;
        PMDL Mdl;
        ULONG BufferBytes;
        ULONG Packets;
        ULONG Index;        // rang dans m_Data / m_Fb (masques de relance)
        ULONG RingBytes;    // octets prélevés dans le tampon cyclique (0 = silence)
        SlotState State;    // protégé par m_Lock
        bool UsbdAllocated; // URB alloué par USBD_IsochUrbAllocate (sinon ExAllocatePool2)
        bool Feedback;
    };

    static NTSTATUS DataCompletion(_In_opt_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_reads_opt_(_Inexpressible_("varies")) PVOID Context);
    static NTSTATUS FeedbackCompletion(_In_opt_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_reads_opt_(_Inexpressible_("varies")) PVOID Context);
    static IO_WORKITEM_ROUTINE RecoveryWorkRoutine;

    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AllocateSlot(_Inout_ Slot* S, ULONG Index, ULONG Packets, ULONG BufferBytes, bool Feedback);
    _IRQL_requires_(PASSIVE_LEVEL) void FreeSlot(_Inout_ Slot* S);

    _IRQL_requires_max_(DISPATCH_LEVEL) void FormatIsoUrb(_Inout_ Slot* S, USBD_PIPE_HANDLE Pipe, ULONG Flags, ULONG TotalBytes);
    _IRQL_requires_max_(DISPATCH_LEVEL) void FormatFeedbackUrb(_Inout_ Slot* S);
    // Verrou tenu.
    _IRQL_requires_(DISPATCH_LEVEL) void FillDataUrbLocked(_Inout_ Slot* S);
    _IRQL_requires_(DISPATCH_LEVEL) ULONG NextPacketFramesLocked();
    _IRQL_requires_(DISPATCH_LEVEL) ULONG CopyFromRingLocked(_Out_writes_bytes_(Bytes) PUCHAR Dest, ULONG Bytes);
    _IRQL_requires_(DISPATCH_LEVEL) void ProcessFeedbackLocked(ULONG Raw);
    _IRQL_requires_(DISPATCH_LEVEL) void RevertToNominalLocked();
    _IRQL_requires_(DISPATCH_LEVEL) void AdvancePlayedLocked(ULONG Bytes);
    _IRQL_requires_(DISPATCH_LEVEL) void EnqueueLocked(_Inout_ Slot* S);
    _IRQL_requires_(DISPATCH_LEVEL) bool ArmRecoveryLocked();

    _IRQL_requires_max_(DISPATCH_LEVEL) void SubmitSlot(_Inout_ Slot* S);
    // Vide la file de la pompe (un seul propriétaire à la fois). L'appelant tient une
    // référence m_Outstanding pendant l'appel.
    _IRQL_requires_max_(DISPATCH_LEVEL) void Pump();
    _IRQL_requires_max_(DISPATCH_LEVEL) void QueueRecoveryWork();
    _IRQL_requires_max_(DISPATCH_LEVEL) void OnDataCompleted(_Inout_ Slot* S, NTSTATUS IrpStatus);
    _IRQL_requires_max_(DISPATCH_LEVEL) void OnFeedbackCompleted(_Inout_ Slot* S, NTSTATUS IrpStatus);
    _IRQL_requires_max_(DISPATCH_LEVEL) Outcome Classify(_In_ const Slot* S, NTSTATUS IrpStatus, USBD_STATUS UrbStatus);
    _IRQL_requires_(PASSIVE_LEVEL) void RunRecovery();
    // Dernier accès possible à l'objet quand le compteur tombe à zéro.
    _IRQL_requires_max_(DISPATCH_LEVEL) void ReleaseOutstanding();
    _IRQL_requires_(PASSIVE_LEVEL) void LogStatistics(_In_z_ const char* When);

    // --- Configuration (figée entre Prepare et Unprepare) ---
    XonarUsbDevice* m_Usb = nullptr;
    PDEVICE_OBJECT m_Lower = nullptr;
    USBD_HANDLE m_UsbdHandle = nullptr;
    PIO_WORKITEM m_WorkItem = nullptr; // relance des URB perdus (nullptr : pas de FDO, pas de relance)
    XonarIsoEngineConfig m_Config = {};
    ULONG m_NominalQ16 = 0;   // trames par microtrame, 16.16
    ULONG m_MinFrames = 0;    // bornes par paquet
    ULONG m_MaxFrames = 0;
    ULONG m_InFlightBytes = 0;
    bool m_Prepared = false;
    bool m_Running = false;   // PASSIVE seulement (Start/Stop sérialisés par SetState)

    Slot m_Data[XONAR_ISO_DATA_URBS] = {};
    Slot m_Fb[XONAR_FB_URBS] = {};
    ULONG m_FbSlotCount = 0;

    // --- État partagé avec les complétions (protégé par m_Lock) ---
    KSPIN_LOCK m_Lock;
    bool m_Stopping = true;
    bool m_DeviceGone = false;
    bool m_StarvedLogged = false;  // trace « plus aucun URB de données en vol » déjà émise
    PUCHAR m_Ring = nullptr;
    ULONG m_RingSize = 0;
    ULONG m_PeriodBytes = 0;  // 0 = pas de notification
    ULONG m_ReadPos = 0;      // octets, multiple de BlockAlign
    ULONG m_PlayPos = 0;      // octets joués modulo m_RingSize
    ULONGLONG m_Linear = 0;   // octets prélevés depuis le dernier ResetPosition
    ULONGLONG m_Played = 0;   // octets joués (URB terminés) depuis le dernier ResetPosition
    ULONG m_AccQ16 = 0;       // reste fractionnaire (mode feedback)
    ULONG m_NominalAcc = 0;   // reste exact en 1/8000 (mode nominal, sans feedback)
    ULONG m_FeedbackQ16 = 0;  // 0 = pas de feedback valide : cadence nominale
    ULONG m_FbEmptyUrbs = 0;  // URB de feedback consécutifs sans valeur exploitable
    LONG m_FbShift = 0;
    bool m_FbShiftKnown = false;
    PKEVENT m_Events[XONAR_MAX_NOTIFICATION_EVENTS] = {};

    // Pompe : file circulaire d'emplacements de données à remplir et soumettre.
    Slot* m_Queue[XONAR_ISO_DATA_URBS] = {};
    ULONG m_QueueHead = 0;
    ULONG m_QueueCount = 0;
    ULONG m_DataInFlight = 0;
    ULONG m_FbInFlight = 0;
    volatile LONG m_PumpOwner = 0;  // 1 = un appelant vide la file

    // Relance : masques des emplacements perdus, budget de tentatives consécutives.
    ULONG m_LostData = 0;
    ULONG m_LostFb = 0;
    bool m_WorkQueued = false;
    ULONG m_DataRounds = 0;   // tentatives depuis le dernier URB de données réussi
    ULONG m_FbRounds = 0;     // tentatives depuis le dernier URB de feedback réussi
    bool m_DataAbandonLogged = false;
    bool m_FbAbandonLogged = false;

    // --- Arrêt ---
    volatile LONG m_Outstanding = 0; // URB en vol/en file + travail + 1 (biais tenu par Start, rendu par Stop)
    KEVENT m_Idle;

    // --- Statistiques (InterlockedIncrement, journalisées avec parcimonie) ---
    volatile LONG m_StatDataUrbs = 0;
    volatile LONG m_StatUrbErrors = 0;
    volatile LONG m_StatPacketErrors = 0;
    volatile LONG m_StatLatePackets = 0;    // rattrapages : paquets arrivés trop tard pour leur microtrame
    volatile LONG m_StatClamped = 0;        // paquets bornés (feedback hors limites)
    volatile LONG m_StatFbValues = 0;
    volatile LONG m_StatFbRejected = 0;
    volatile LONG m_StatSlotsLost = 0;      // URB marqués « à relancer »
    volatile LONG m_StatResubmitted = 0;    // URB relancés par l'élément de travail
    volatile LONG m_StatPipeResets = 0;
    volatile LONG m_StatMultiPeriod = 0;    // URB franchissant plus d'une frontière de période
};
