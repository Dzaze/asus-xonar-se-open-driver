// Couche USB de xonarse.sys : CONTRAT PUBLIC utilisé par l'adaptateur, la topologie et
// le moteur de streaming. Implémentation : usb/*.cpp (module USB).
// Les ajouts de méthodes publiques sont permis ; la modification des signatures
// existantes doit être coordonnée.
#pragma once

#include "../common.h"

// usbdlib.h ne déclare pas ses fonctions en liaison C : sans ce bloc, l'édition de liens
// échoue sur les symboles USBD_* (relevé par le module USB). Tout module qui inclut ce
// contrat hérite donc de déclarations correctes.
extern "C" {
#include <usb.h>
#include <usbdlib.h>
#include <usbioctl.h>
}

// Requêtes de classe UAC2 (Audio Device Class 2.0, §5.2).
namespace xonar::uac2 {
constexpr UCHAR kRequestCur = 0x01;
constexpr UCHAR kRequestRange = 0x02;
// Sélecteurs de contrôle
constexpr UCHAR kCsSamFreqControl = 0x01;      // Clock Source
constexpr UCHAR kCsClockValidControl = 0x02;   // Clock Source
constexpr UCHAR kTeConnectorControl = 0x02;    // Terminal : renvoie un cluster (bNrChannels=0 si débranché)
constexpr UCHAR kFuMuteControl = 0x01;         // Feature Unit
constexpr UCHAR kFuVolumeControl = 0x02;       // Feature Unit, 1/256 dB, int16
} // namespace xonar::uac2

// Paramètres d'un sous-intervalle RANGE.
struct XONAR_RANGE32 { ULONG Min; ULONG Max; ULONG Res; };
struct XONAR_RANGE16 { SHORT Min; SHORT Max; USHORT Res; };

// Message d'interruption UAC2 (6 octets, endpoint 0x8F), déjà décodé.
struct XONAR_UAC2_INTERRUPT {
    UCHAR Info;            // bInfo (bit 0 : 0 = interface, 1 = endpoint ; bit 1 : vendor)
    UCHAR Attribute;       // 0x01 CUR, 0x02 RANGE, 0x03 MEM
    UCHAR ControlSelector; // octet haut de wValue
    UCHAR ChannelNumber;   // octet bas de wValue
    UCHAR EntityId;        // octet haut de wIndex (ID de l'entité : terminal, unité, horloge)
    UCHAR InterfaceOrEp;   // octet bas de wIndex
};

// Poignées et caractéristiques des pipes d'un altsetting AudioStreaming sélectionné.
struct XONAR_AS_PIPES {
    USBD_PIPE_HANDLE DataPipe;
    USBD_PIPE_HANDLE FeedbackPipe;   // nullptr si l'altsetting n'a pas de feedback
    USHORT DataMaxPacketBytes;
    UCHAR DataEndpoint;
    UCHAR FeedbackEndpoint;
    UCHAR DataInterval;              // bInterval (HS : 2^(n-1) microtrames)
    UCHAR FeedbackInterval;
};

// Rappel d'interruption : appelé à PASSIVE_LEVEL (élément de travail), jamais en parallèle
// avec lui-même pour une même inscription.
typedef void (*XONAR_INTERRUPT_CALLBACK)(_In_opt_ PVOID Context, _In_ const XONAR_UAC2_INTERRUPT* Message);

class XonarUsbDevice {
public:
    // Création : PASSIVE_LEVEL. Fdo = objet de PortCls ; Pdo = PcGetPhysicalDeviceObject ;
    // LowerDevice = IoGetLowerDeviceObject(Fdo) (référencé, rendu par Destroy).
    // Référence initiale = 1 (celle de l'adaptateur).
    static NTSTATUS Create(_In_ PDEVICE_OBJECT Fdo, _In_ PDEVICE_OBJECT Pdo, _In_ PDEVICE_OBJECT LowerDevice,
                           _Outptr_ XonarUsbDevice** Device);

    LONG AddRef();
    LONG Release(); // détruit l'objet à 0 (doit alors être arrêté)

    // Démarrage : lit les descripteurs, sélectionne la configuration (toutes les interfaces
    // AudioStreaming en altsetting 0), démarre la lecture de l'endpoint d'interruption.
    // Redémarrage (Start après Stop, sans PrepareForRemove) : si une configuration est déjà
    // sélectionnée et toujours valide (GET_CONFIGURATION concordant ou non concluant, et
    // descripteur de configuration identique octet pour octet), elle est CONSERVÉE : pas de
    // nouvelle SELECT_CONFIGURATION, poignées de pipe et altsettings courants inchangés (un
    // flux resté ouvert garde des poignées valides). Sinon, nouvelle sélection complète
    // (toutes les poignées de pipe antérieures deviennent invalides, trace d'avertissement).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS Start();
    // Arrêt : annule la lecture d'interruption, remet les interfaces AudioStreaming en
    // altsetting 0, SAUF si OpenStreamCount() > 0 (STOP PnP avec flux ouvert : interfaces
    // laissées en l'état, trace). PrepareForRemove referme toujours les interfaces.
    // Idempotent. Appelé sur IRP_MN_REMOVE_DEVICE / SURPRISE_REMOVAL / STOP_DEVICE.
    _IRQL_requires_(PASSIVE_LEVEL) void Stop();
    bool IsStarted() const;
    bool IsSurpriseRemoved() const;
    void MarkSurpriseRemoved();

    // --- Accès bas niveau pour le moteur de streaming (wave/) ---
    PDEVICE_OBJECT LowerDevice() const;
    USBD_HANDLE UsbdHandle() const;
    // Soumission synchrone d'un URB, PASSIVE_LEVEL, avec délai d'attente.
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SubmitUrbSync(_Inout_ PURB Urb, ULONG TimeoutMs = 5000);
    // Sélectionne un altsetting d'une interface AudioStreaming et rend ses pipes.
    // AltSetting 0 = interface fermée (Pipes peut être nullptr).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SelectAltSetting(UCHAR Interface, UCHAR AltSetting,
                                                             _Out_opt_ XONAR_AS_PIPES* Pipes);
    // Annule les transferts en cours sur un pipe (URB_FUNCTION_ABORT_PIPE). Tentée même après
    // MarkSurpriseRemoved (le moteur de streaming doit pouvoir récupérer ses URB) ; l'échec
    // dû au départ du périphérique est rendu sans trace d'erreur. Seul PrepareForRemove la
    // fait échouer d'emblée (STATUS_NO_SUCH_DEVICE).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS AbortPipe(USBD_PIPE_HANDLE Pipe);
    // Numéro de trame USB courant (URB_FUNCTION_GET_CURRENT_FRAME_NUMBER).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetCurrentFrameNumber(_Out_ ULONG* Frame);

    // --- Requêtes de classe UAC2 (PASSIVE_LEVEL, synchrones) ---
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetSampleRate(UCHAR ClockId, _Out_ ULONG* Hz);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SetSampleRate(UCHAR ClockId, ULONG Hz);
    // Remplit jusqu'à MaxRanges sous-intervalles, Count = nombre réel annoncé par la carte.
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetSampleRateRanges(UCHAR ClockId, _Out_writes_(MaxRanges) XONAR_RANGE32* Ranges,
                                                                ULONG MaxRanges, _Out_ ULONG* Count);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetClockValid(UCHAR ClockId, _Out_ bool* Valid);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetMute(UCHAR UnitId, UCHAR Channel, _Out_ bool* Muted);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SetMute(UCHAR UnitId, UCHAR Channel, bool Muted);
    // Volume UAC2 : int16 en 1/256 dB (0x8000 = -infini).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetVolume(UCHAR UnitId, UCHAR Channel, _Out_ SHORT* Value);
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SetVolume(UCHAR UnitId, UCHAR Channel, SHORT Value);
    // Premier sous-intervalle de RANGE pour le volume (min, max, pas).
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetVolumeRange(UCHAR UnitId, UCHAR Channel, _Out_ XONAR_RANGE16* Range);
    // Connecteur d'un terminal : Connected = (bNrChannels != 0) ; ChannelMask = bmChannelConfig rendu.
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS GetConnectorState(UCHAR TerminalId, _Out_ bool* Connected,
                                                              _Out_opt_ UCHAR* NrChannels, _Out_opt_ ULONG* ChannelMask);

    // --- Notifications de l'endpoint d'interruption 0x8F ---
    // Jusqu'à 8 inscriptions. Rappel à PASSIVE_LEVEL. Désinscription synchrone : au retour,
    // aucun rappel n'est en cours pour cette inscription.
    NTSTATUS RegisterInterruptCallback(_In_ XONAR_INTERRUPT_CALLBACK Callback, _In_opt_ PVOID Context);
    _IRQL_requires_(PASSIVE_LEVEL) void UnregisterInterruptCallback(_In_ XONAR_INTERRUPT_CALLBACK Callback, _In_opt_ PVOID Context);

    // --- Ajouts du module USB (méthodes supplémentaires, contrat inchangé) ---
    // Note sur SubmitUrbSync : réservé aux URB alloués par le client (pile, pool) ; l'URB est
    // placé dans Parameters.Others.Argument1. Un URB alloué par USBD_UrbAllocate,
    // USBD_IsochUrbAllocate ou USBD_Select*UrbAllocateAndBuild doit passer par
    // SubmitUsbdUrbSync (USBD_AssignUrbToIoStackLocation, exigé par Microsoft ; l'inverse
    // provoque un arrêt de la pile USB). Sans poignée USBD (mode de repli), STATUS_INVALID_DEVICE_STATE.
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS SubmitUsbdUrbSync(_Inout_ PURB Urb, ULONG TimeoutMs = 5000);
    // Préparation au retrait, à appeler sur IRP_MN_REMOVE_DEVICE AVANT de transmettre l'IRP
    // (Microsoft exige USBD_CloseHandle avant la descente de l'IRP de retrait). Fait Stop(),
    // puis ferme la poignée USBD : ensuite toute opération USB échoue avec
    // STATUS_NO_SUCH_DEVICE et UsbdHandle() rend nullptr. Les URB alloués avec UsbdHandle()
    // doivent avoir été libérés avant. Facultatif : à défaut, la poignée est fermée à la destruction.
    _IRQL_requires_(PASSIVE_LEVEL) void PrepareForRemove();

    // --- Ajouts de la relecture (contrat inchangé par ailleurs) ---
    // FDO de PortCls passé à Create (par exemple pour IoAllocateWorkItem côté wave). Tout IRQL.
    PDEVICE_OBJECT FunctionalDevice() const;
    // Réinitialise un pipe et lève un éventuel STALL (URB_FUNCTION_SYNC_RESET_PIPE_AND_CLEAR_STALL),
    // pour se remettre d'une erreur passagère. Aucun transfert ne doit être en vol sur ce
    // pipe (AbortPipe et attente des complétions d'abord). Échoue sans rien envoyer avec
    // STATUS_NO_SUCH_DEVICE après un retrait (inopiné ou PrepareForRemove),
    // STATUS_DEVICE_NOT_READY si non configuré, STATUS_INVALID_PARAMETER si Pipe est nul.
    _IRQL_requires_(PASSIVE_LEVEL) NTSTATUS ResetPipe(USBD_PIPE_HANDLE Pipe);
    // Compteur de flux ouverts tenu par le moteur de streaming : StreamOpened à l'ouverture
    // d'un flux (avant de quitter l'altsetting 0), StreamClosed à sa fermeture (après le
    // retour en altsetting 0). Opérations interlockées, tout IRQL ; chacune rend la nouvelle
    // valeur. StreamClosed ne descend jamais sous 0 (appel en trop : trace, rend 0).
    // Stop() consulte ce compteur (voir plus haut).
    LONG StreamOpened();
    LONG StreamClosed();
    LONG OpenStreamCount() const;
    // Vrai si le lecteur de l'endpoint d'interruption est réellement actif : ressources
    // allouées, pas d'arrêt demandé, pas d'abandon (budget de relances épuisé ou
    // périphérique parti). Diagnostic, tout IRQL.
    bool IsInterruptReaderActive() const;

private:
    XonarUsbDevice() = default;
    ~XonarUsbDevice();
    XonarUsbDevice(const XonarUsbDevice&) = delete;
    XonarUsbDevice& operator=(const XonarUsbDevice&) = delete;

    struct Impl;   // état privé, défini par le module USB
    Impl* m_Impl = nullptr;
    volatile LONG m_RefCount = 1;
};
