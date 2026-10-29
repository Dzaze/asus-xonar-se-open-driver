// Point d'entrée du pilote xonarse.sys.
//
// Modèle : adaptateur PortCls (comme l'échantillon SYSVAD de Microsoft et comme le
// pilote C-Media de la Xonar, dont les imports montraient PcInitializeAdapterDriver,
// PcAddAdapterDevice et PcRegisterPhysicalConnection). Aucun code ASUS/C-Media ici.
#include "xonarse.h"
#include "usb/usb_device.h"

// PortCls installe sa propre routine de déchargement ; on la conserve pour l'appeler
// après notre nettoyage, comme le fait SYSVAD.
static PDRIVER_UNLOAD g_PortClsUnload = nullptr;

extern "C" DRIVER_INITIALIZE DriverEntry;
static DRIVER_UNLOAD XonarSeUnload;
static DRIVER_ADD_DEVICE XonarSeAddDevice;
static DRIVER_DISPATCH XonarSePnp;

#pragma code_seg("INIT")
extern "C" NTSTATUS DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    PAGED_CODE();
    // Active l'allocation non exécutable par défaut (POOL_NX_OPTIN).
    ExInitializeDriverRuntime(DrvRtPoolNxOptIn);
    XonarSeTraceRegister();
    XONARSE_INFO("DriverEntry (build %s %s)", __DATE__, __TIME__);

    const NTSTATUS status = PcInitializeAdapterDriver(DriverObject, RegistryPath, XonarSeAddDevice);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("PcInitializeAdapterDriver a échoué : 0x%08X", status);
        XonarSeTraceUnregister();
        return status;
    }

    g_PortClsUnload = DriverObject->DriverUnload;
    DriverObject->DriverUnload = XonarSeUnload;
    // On intercepte les IRP PnP pour arrêter la couche USB au retrait, puis on les
    // rend à PortCls (PcDispatchIrp), comme SYSVAD.
    DriverObject->MajorFunction[IRP_MJ_PNP] = XonarSePnp;
    return STATUS_SUCCESS;
}
#pragma code_seg()

#pragma code_seg("PAGE")
static VOID XonarSeUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();
    XONARSE_INFO("Unload");
    if (g_PortClsUnload != nullptr) {
        g_PortClsUnload(DriverObject);
    }
    XonarSeTraceUnregister();
}

static NTSTATUS XonarSeAddDevice(_In_ PDRIVER_OBJECT DriverObject, _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    PAGED_CODE();
    XONARSE_INFO("AddDevice PDO=%p", PhysicalDeviceObject);

    // PortCls crée le FDO, l'attache à la pile USB et nous rappellera XonarSeStartDevice.
    // L'extension réserve notre contexte après la zone propre à PortCls.
    const NTSTATUS status = PcAddAdapterDevice(DriverObject, PhysicalDeviceObject, XonarSeStartDevice,
                                               XONARSE_MAX_SUBDEVICES, XONARSE_DEVICE_EXTENSION_SIZE);
    if (!NT_SUCCESS(status)) {
        XONARSE_ERROR("PcAddAdapterDevice a échoué : 0x%08X", status);
    }
    return status;
}

static NTSTATUS XonarSePnp(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    const PIO_STACK_LOCATION stack = IoGetCurrentIrpStackLocation(Irp);

    switch (stack->MinorFunction) {
    case IRP_MN_SURPRISE_REMOVAL:
        XONARSE_INFO("PnP SURPRISE_REMOVAL");
        XonarSeOnSurpriseRemoval(DeviceObject);
        break;

    case IRP_MN_QUERY_STOP_DEVICE: {
        // On refuse l'arrêt de rééquilibrage PnP : un flux ouvert garderait des poignées de
        // pipe invalides après la nouvelle sélection de configuration (relecture du code,
        // constat 4). Refuser QUERY_STOP est permis : le gestionnaire PnP renonce simplement.
        XONARSE_INFO("PnP QUERY_STOP_DEVICE refusé");
        Irp->IoStatus.Status = STATUS_UNSUCCESSFUL;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return STATUS_UNSUCCESSFUL;
    }

    case IRP_MN_STOP_DEVICE:
        XONARSE_INFO("PnP STOP_DEVICE");
        XonarSeOnStopDevice(DeviceObject);
        break;

    case IRP_MN_REMOVE_DEVICE: {
        XONARSE_INFO("PnP REMOVE_DEVICE");
        // Le FDO et son extension sont détruits par PortCls pendant PcDispatchIrp :
        // on détache l'objet USB avant, et on le relâche après.
        XonarUsbDevice* usb = XonarSeOnRemoveDevice(DeviceObject);
        const NTSTATUS status = PcDispatchIrp(DeviceObject, Irp);
        if (usb != nullptr) {
            usb->Release();
        }
        return status;
    }

    default:
        break;
    }
    return PcDispatchIrp(DeviceObject, Irp);
}
#pragma code_seg()
