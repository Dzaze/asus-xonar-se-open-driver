// Démarrage de l'adaptateur : appelé par PortCls à IRP_MN_START_DEVICE.
//
// 1. Lit les réglages du registre (HonorJackDetection, DefaultSampleRate).
// 2. Crée et démarre la couche USB (XonarUsbDevice) sur la pile du périphérique.
// 3. Pour chaque endpoint (haut-parleurs, casque, S/PDIF) : crée le miniport Topology
//    et le miniport WaveRT, les enregistre comme sous-périphériques et les relie.
// Un module non encore implémenté (bouchon) fait ignorer l'endpoint sans échec.
#include "xonarse.h"
#include "usb/usb_device.h"

// Exporté par ntoskrnl mais déclaré seulement dans ntifs.h, incompatible avec portcls.h.
extern "C" NTKERNELAPI PDEVICE_OBJECT IoGetLowerDeviceObject(_In_ PDEVICE_OBJECT DeviceObject);

static XONARSE_SETTINGS g_Settings = {0, xonar::hw::kDefaultRate};

const XONARSE_SETTINGS& XonarSeSettings()
{
    return g_Settings;
}

#pragma code_seg("PAGE")

// Lit une valeur DWORD sous la sous-clé "Settings" de la clé logicielle (pilote) du périphérique.
static void ReadDwordSetting(_In_ HANDLE SettingsKey, _In_ PCWSTR Name, _Inout_ ULONG* Value)
{
    PAGED_CODE();
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, Name);
    alignas(KEY_VALUE_PARTIAL_INFORMATION) UCHAR buffer[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)] = {};
    ULONG returned = 0;
    const NTSTATUS status = ZwQueryValueKey(SettingsKey, &name, KeyValuePartialInformation, buffer, sizeof(buffer), &returned);
    if (NT_SUCCESS(status)) {
        const auto* info = reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
        if (info->Type == REG_DWORD && info->DataLength == sizeof(ULONG)) {
            *Value = *reinterpret_cast<const ULONG*>(info->Data);
        }
    }
}

static void LoadSettings(_In_ PDEVICE_OBJECT Pdo)
{
    PAGED_CODE();
    HANDLE driverKey = nullptr;
    NTSTATUS status = IoOpenDeviceRegistryKey(Pdo, PLUGPLAY_REGKEY_DRIVER, KEY_READ, &driverKey);
    if (!NT_SUCCESS(status)) {
        XONARSE_WARN("Clé du pilote illisible (0x%08X) : réglages par défaut", status);
        return;
    }
    UNICODE_STRING subName;
    RtlInitUnicodeString(&subName, L"Settings");
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &subName, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, driverKey, nullptr);
    HANDLE settingsKey = nullptr;
    status = ZwOpenKey(&settingsKey, KEY_READ, &attributes);
    if (NT_SUCCESS(status)) {
        ReadDwordSetting(settingsKey, L"HonorJackDetection", &g_Settings.HonorJackDetection);
        ReadDwordSetting(settingsKey, L"DefaultSampleRate", &g_Settings.DefaultSampleRate);
        ZwClose(settingsKey);
    }
    ZwClose(driverKey);
    XONARSE_INFO("Réglages : HonorJackDetection=%lu DefaultSampleRate=%lu", g_Settings.HonorJackDetection,
                 g_Settings.DefaultSampleRate);
}

// Crée un port PortCls, l'initialise avec le miniport et l'enregistre sous Name.
static NTSTATUS InstallSubdevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PCWSTR Name,
                                 _In_ REFGUID PortClassId, _In_ PUNKNOWN Miniport, _In_ PRESOURCELIST ResourceList,
                                 _Outptr_ PUNKNOWN* PortUnknown)
{
    PAGED_CODE();
    *PortUnknown = nullptr;
    PPORT port = nullptr;
    NTSTATUS status = PcNewPort(&port, PortClassId);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("PcNewPort(%ls) : 0x%08X", Name, status);
        return status;
    }
    status = port->Init(DeviceObject, Irp, Miniport, nullptr, ResourceList);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("IPort::Init(%ls) : 0x%08X", Name, status);
        port->Release();
        return status;
    }
    status = PcRegisterSubdevice(DeviceObject, const_cast<PWSTR>(Name), port);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("PcRegisterSubdevice(%ls) : 0x%08X", Name, status);
        port->Release();
        return status;
    }
    *PortUnknown = port; // référence rendue à l'appelant
    return STATUS_SUCCESS;
}

static NTSTATUS InstallEndpoint(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PRESOURCELIST ResourceList,
                                _In_ XonarUsbDevice* Usb, _In_ XONARSE_ENDPOINT Endpoint)
{
    PAGED_CODE();
    const PCWSTR topoName = XonarSeTopologyName(Endpoint);
    const PCWSTR waveName = XonarSeWaveName(Endpoint);

    PUNKNOWN topoMiniport = nullptr;
    NTSTATUS status = CreateXonarTopologyMiniport(&topoMiniport, Usb, Endpoint);
    if (status == STATUS_NOT_IMPLEMENTED) {
        XONARSE_WARN("Endpoint %lu : miniport Topology non implémenté, ignoré", static_cast<ULONG>(Endpoint));
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("Endpoint %lu : création Topology 0x%08X", static_cast<ULONG>(Endpoint), status);
        return status;
    }

    PUNKNOWN topoPort = nullptr;
    status = InstallSubdevice(DeviceObject, Irp, topoName, CLSID_PortTopology, topoMiniport, ResourceList, &topoPort);
    topoMiniport->Release();
    if (!NT_SUCCESS(status)) return status;

    PUNKNOWN waveMiniport = nullptr;
    status = CreateXonarWaveRtMiniport(&waveMiniport, Usb, Endpoint);
    if (status == STATUS_NOT_IMPLEMENTED) {
        XONARSE_WARN("Endpoint %lu : miniport WaveRT non implémenté, topologie seule", static_cast<ULONG>(Endpoint));
        topoPort->Release();
        return STATUS_SUCCESS;
    }
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("Endpoint %lu : création WaveRT 0x%08X", static_cast<ULONG>(Endpoint), status);
        topoPort->Release();
        return status;
    }

    PUNKNOWN wavePort = nullptr;
    status = InstallSubdevice(DeviceObject, Irp, waveName, CLSID_PortWaveRT, waveMiniport, ResourceList, &wavePort);
    waveMiniport->Release();
    if (!NT_SUCCESS(status)) {
        topoPort->Release();
        return status;
    }

    if (XonarSeEndpointHw(Endpoint).IsCapture) {
        // Capture : le flux va de la topologie (connecteur micro) vers le filtre Wave (hôte).
        status = PcRegisterPhysicalConnection(DeviceObject, topoPort, XONARSE_TOPO_CAPTURE_PIN_BRIDGE, wavePort,
                                              XONARSE_WAVE_CAPTURE_PIN_BRIDGE);
    } else {
        status = PcRegisterPhysicalConnection(DeviceObject, wavePort, XONARSE_WAVE_PIN_BRIDGE, topoPort,
                                              XONARSE_TOPO_PIN_BRIDGE_IN);
    }
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("Endpoint %lu : PcRegisterPhysicalConnection 0x%08X", static_cast<ULONG>(Endpoint), status);
    } else {
        XONARSE_INFO("Endpoint %lu installé : %ls + %ls", static_cast<ULONG>(Endpoint), waveName, topoName);
    }
    wavePort->Release();
    topoPort->Release();
    return status;
}

extern "C" NTSTATUS XonarSeStartDevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp,
                                       _In_ PRESOURCELIST ResourceList)
{
    PAGED_CODE();
    XONARSE_INFO("StartDevice FDO=%p", DeviceObject);
    XONARSE_DEVICE_CONTEXT* ctx = XonarSeContext(DeviceObject);

    if (ctx->Usb != nullptr) {
        // Redémarrage après IRP_MN_STOP_DEVICE : les sous-périphériques sont déjà enregistrés.
        XONARSE_INFO("Redémarrage : reprise de la couche USB");
        return ctx->Usb->Start();
    }

    PDEVICE_OBJECT pdo = nullptr;
    NTSTATUS status = PcGetPhysicalDeviceObject(DeviceObject, &pdo);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("PcGetPhysicalDeviceObject : 0x%08X", status);
        return status;
    }
    LoadSettings(pdo);

    // Référence rendue par XonarUsbDevice à sa destruction.
    PDEVICE_OBJECT lower = IoGetLowerDeviceObject(DeviceObject);
    if (lower == nullptr) {
        XONARSE_ERROR("Aucun objet inférieur dans la pile");
        return STATUS_NO_SUCH_DEVICE;
    }

    XonarUsbDevice* usb = nullptr;
    status = XonarUsbDevice::Create(DeviceObject, pdo, lower, &usb);
    if (!NT_SUCCESS(status)) {
        ObDereferenceObject(lower);
        if (status == STATUS_NOT_IMPLEMENTED) {
            XONARSE_WARN("Couche USB non implémentée : démarrage sans endpoint (jalon B0)");
            return STATUS_SUCCESS;
        }
        XONARSE_ERROR("XonarUsbDevice::Create : 0x%08X", status);
        return status;
    }

    status = usb->Start();
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("XonarUsbDevice::Start : 0x%08X", status);
        usb->Stop();
        usb->Release();
        return status;
    }
    ctx->Usb = usb;

    for (ULONG e = 0; e < XonarEndpointCount; ++e) {
        status = InstallEndpoint(DeviceObject, Irp, ResourceList, usb, static_cast<XONARSE_ENDPOINT>(e));
        if (!NT_SUCCESS(status)) {
            // Un endpoint en échec n'empêche pas les autres de fonctionner.
            XONARSE_ERROR("Endpoint %lu non installé (0x%08X), on continue", e, status);
        }
    }
    ctx->EndpointsInstalled = true;
    return STATUS_SUCCESS;
}

void XonarSeOnStopDevice(_In_ PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();
    XONARSE_DEVICE_CONTEXT* ctx = XonarSeContext(DeviceObject);
    if (ctx->Usb != nullptr) ctx->Usb->Stop();
}

XonarUsbDevice* XonarSeOnRemoveDevice(_In_ PDEVICE_OBJECT DeviceObject)
{
    PAGED_CODE();
    XONARSE_DEVICE_CONTEXT* ctx = XonarSeContext(DeviceObject);
    XonarUsbDevice* usb = ctx->Usb;
    ctx->Usb = nullptr;
    // Arrête la couche USB et ferme la poignée USBD AVANT que l'IRP de retrait ne
    // descende la pile (exigence Microsoft pour USBD_CloseHandle).
    if (usb != nullptr) usb->PrepareForRemove();
    return usb;
}
#pragma code_seg()

void XonarSeOnSurpriseRemoval(_In_ PDEVICE_OBJECT DeviceObject)
{
    XONARSE_DEVICE_CONTEXT* ctx = XonarSeContext(DeviceObject);
    if (ctx->Usb != nullptr) ctx->Usb->MarkSurpriseRemoved();
}
