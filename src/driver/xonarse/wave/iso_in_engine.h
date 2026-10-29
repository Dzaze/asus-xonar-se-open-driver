// Moteur de streaming isochrone IN asynchrone (USB 2.0 High Speed, UAC2) pour la CAPTURE.
// Remplit le tampon cyclique WaveRT à partir des paquets reçus du périphérique. Module wave/.
//
// Différences avec XonarIsoOutEngine (iso_engine.*) :
// - Sens IN : le PÉRIPHÉRIQUE décide de la taille de chaque paquet ; on soumet des URB
//   de paquets au maximum (wMaxPacketSize) et chaque descripteur de paquet rend la
//   longueur réellement reçue, copiée dans le tampon cyclique.
// - AUCUN endpoint de feedback (la capture n'en a pas) : pas d'accumulateur de cadence,
//   pas de pompe ordonnée (les URB IN sont identiques, resoumis directement à la complétion).
// - La position « jouée » WaveRT = le curseur d'ÉCRITURE de capture (octets déjà reçus) ;
//   les notifications partent quand ce curseur franchit une période.
//
// On conserve à l'identique la mécanique éprouvée du moteur OUT : comptage m_Outstanding
// (1 garde par complétion + 1 par emplacement + biais de Start), arrêt synchrone par KEVENT
// avec repli AbortPipe, et relance des URB perdus par IO_WORKITEM (reset de pipe).
#pragma once

// usb_device.h en premier : il inclut usb.h / usbdlib.h / usbioctl.h sous extern "C".
#include "../usb/usb_device.h"

constexpr ULONG XONAR_ISO_IN_URBS = 8;             // URB IN en vol
constexpr ULONG XONAR_ISO_IN_PACKETS_PER_URB = 8;  // 8 microtrames = 1 ms (multiple de 8 exigé en HS, bInterval 1)
constexpr ULONG XONAR_ISO_IN_MAX_EVENTS = 4;
constexpr UCHAR XONAR_ISO_IN_DATA_INTERVAL = 1;    // 2^(1-1) = 1 microtrame

struct XonarIsoInConfig {
    ULONG SampleRate;          // Hz
    USHORT BlockAlign;         // octets par trame
    USHORT MaxPacketBytes;     // wMaxPacketSize effectif du pipe de données IN
    USBD_PIPE_HANDLE DataPipe;
    UCHAR DataInterval;        // bInterval du descripteur de l'endpoint de données
};

class XonarIsoInEngine {
public:
    XonarIsoInEngine();
    ~XonarIsoInEngine();
    XonarIsoInEngine(const XonarIsoInEngine&) = delete;
    XonarIsoInEngine& operator=(const XonarIsoInEngine&) = delete;

    // Majorant des octets reçus par cycle (tous les URB pleins à la cadence maximale).
    static ULONG MaxInFlightBytes(ULONG SampleRate, USHORT BlockAlign);

    // Alloue IRP, URB, tampons de transfert, MDL et élément de travail. Usb doit rester
    // valide jusqu'à Unprepare. STATUS_DEVICE_CONFIGURATION_ERROR si le bInterval ne
    // correspond pas (attendu 1).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Prepare(_In_ XonarUsbDevice* Usb, _In_ const XonarIsoInConfig& Config);
    _IRQL_requires_(PASSIVE_LEVEL) void Unprepare();
    bool IsPrepared() const { return m_Prepared; }
    bool IsRunning() const { return m_Running; }

    // Tampon cyclique WaveRT (adresse noyau non paginée). NotificationCount = 0 : aucune
    // notification. Échange sous verrou ; un tampon absent fait écarter les octets reçus.
    _IRQL_requires_max_(DISPATCH_LEVEL) void SetAudioBuffer(_In_opt_ PUCHAR Buffer, _In_ ULONG Size,
                                                            _In_ ULONG NotificationCount);

    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Start();
    // Arrêt synchrone : au retour, plus aucun URB ni élément de travail n'est en vol.
    _IRQL_requires_(PASSIVE_LEVEL) void Stop();

    // WritePos : curseur d'écriture de capture (octets déjà reçus modulo la taille du
    // tampon). CapturedBytes : total linéaire reçu depuis le dernier ResetPosition.
    _IRQL_requires_max_(DISPATCH_LEVEL) void GetPosition(_Out_ ULONG* WritePos, _Out_opt_ ULONGLONG* CapturedBytes);
    _IRQL_requires_max_(DISPATCH_LEVEL) void ResetPosition();
    ULONG InFlightBytes() const { return m_InFlightBytes; }

    _IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS RegisterNotificationEvent(_In_ PKEVENT Event);
    _IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS UnregisterNotificationEvent(_In_ PKEVENT Event);

private:
    enum SlotState : UCHAR {
        SlotIdle = 0,   // ni en vol ni perdu (arrêt, périphérique absent, abandon)
        SlotInFlight,   // possédé par la pile USB
        SlotLost,       // erreur passagère : à relancer par l'élément de travail
    };
    enum class Outcome { Ok, Transient, Gone };

    struct Slot {
        XonarIsoInEngine* Engine;
        PIRP Irp;
        PURB Urb;
        PUCHAR Buffer;
        PMDL Mdl;
        ULONG BufferBytes;
        ULONG Packets;
        ULONG Index;        // rang dans m_Data (masque de relance)
        SlotState State;    // protégé par m_Lock
        bool UsbdAllocated; // URB alloué par USBD_IsochUrbAllocate (sinon ExAllocatePool2)
    };

    static NTSTATUS DataCompletion(_In_opt_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp,
                                   _In_reads_opt_(_Inexpressible_("varies")) PVOID Context);
    static IO_WORKITEM_ROUTINE RecoveryWorkRoutine;

    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AllocateSlot(_Inout_ Slot* S, ULONG Index, ULONG Packets, ULONG BufferBytes);
    _IRQL_requires_(PASSIVE_LEVEL) void FreeSlot(_Inout_ Slot* S);

    _IRQL_requires_max_(DISPATCH_LEVEL) void FormatInUrb(_Inout_ Slot* S);
    // Verrou tenu. Copie Bytes octets reçus dans le tampon cyclique (rebouclage).
    _IRQL_requires_(DISPATCH_LEVEL) void CopyToRingLocked(_In_reads_bytes_(Bytes) const UCHAR* Src, ULONG Bytes);
    _IRQL_requires_(DISPATCH_LEVEL) void AdvanceCapturedLocked(ULONG Bytes);
    _IRQL_requires_(DISPATCH_LEVEL) bool ArmRecoveryLocked();

    _IRQL_requires_max_(DISPATCH_LEVEL) void SubmitSlot(_Inout_ Slot* S);
    _IRQL_requires_max_(DISPATCH_LEVEL) void QueueRecoveryWork();
    _IRQL_requires_max_(DISPATCH_LEVEL) void OnDataCompleted(_Inout_ Slot* S, NTSTATUS IrpStatus);
    _IRQL_requires_max_(DISPATCH_LEVEL) Outcome Classify(_In_ const Slot* S, NTSTATUS IrpStatus, USBD_STATUS UrbStatus);
    _IRQL_requires_(PASSIVE_LEVEL) void RunRecovery();
    _IRQL_requires_max_(DISPATCH_LEVEL) void ReleaseOutstanding();
    _IRQL_requires_(PASSIVE_LEVEL) void LogStatistics(_In_z_ const char* When);

    // --- Configuration (figée entre Prepare et Unprepare) ---
    XonarUsbDevice* m_Usb = nullptr;
    PDEVICE_OBJECT m_Lower = nullptr;
    USBD_HANDLE m_UsbdHandle = nullptr;
    PIO_WORKITEM m_WorkItem = nullptr;
    XonarIsoInConfig m_Config = {};
    ULONG m_InFlightBytes = 0;
    bool m_Prepared = false;
    bool m_Running = false;

    Slot m_Data[XONAR_ISO_IN_URBS] = {};

    // --- État partagé avec les complétions (protégé par m_Lock) ---
    KSPIN_LOCK m_Lock;
    bool m_Stopping = true;
    bool m_DeviceGone = false;
    bool m_StarvedLogged = false;
    PUCHAR m_Ring = nullptr;
    ULONG m_RingSize = 0;
    ULONG m_PeriodBytes = 0;  // 0 = pas de notification
    ULONG m_WritePos = 0;     // octets, curseur d'écriture modulo m_RingSize
    ULONGLONG m_Captured = 0; // octets reçus depuis le dernier ResetPosition
    PKEVENT m_Events[XONAR_ISO_IN_MAX_EVENTS] = {};

    ULONG m_DataInFlight = 0;

    // Relance : masque des emplacements perdus, budget de tentatives consécutives.
    ULONG m_LostData = 0;
    bool m_WorkQueued = false;
    ULONG m_DataRounds = 0;
    bool m_DataAbandonLogged = false;

    // --- Arrêt ---
    volatile LONG m_Outstanding = 0; // URB en vol + travail + 1 (biais tenu par Start, rendu par Stop)
    KEVENT m_Idle;

    // --- Statistiques ---
    volatile LONG m_StatDataUrbs = 0;
    volatile LONG m_StatUrbErrors = 0;
    volatile LONG m_StatPacketErrors = 0;
    volatile LONG m_StatLatePackets = 0;
    volatile LONG m_StatSlotsLost = 0;
    volatile LONG m_StatResubmitted = 0;
    volatile LONG m_StatPipeResets = 0;
    volatile LONG m_StatBytes = 0;      // octets reçus / 1024 (parcimonie)
};
