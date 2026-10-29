// Point d'entrée historique (jalon B0) : tout est désormais dans common.h.
#pragma once

#include "common.h"

// Nombre maximal de sous-périphériques PortCls : Topology + Wave pour haut-parleurs,
// casque et S/PDIF, plus capture à venir.
constexpr ULONG XONARSE_MAX_SUBDEVICES = 10;

// Contexte de l'adaptateur, stocké dans l'extension de périphérique au-delà de la
// zone réservée à PortCls (PcAddAdapterDevice avec une taille > PORT_CLASS_DEVICE_EXTENSION_SIZE).
struct XONARSE_DEVICE_CONTEXT {
    XonarUsbDevice* Usb;
    bool EndpointsInstalled;
};

inline XONARSE_DEVICE_CONTEXT* XonarSeContext(PDEVICE_OBJECT Fdo)
{
    return reinterpret_cast<XONARSE_DEVICE_CONTEXT*>(static_cast<PUCHAR>(Fdo->DeviceExtension) +
                                                     PORT_CLASS_DEVICE_EXTENSION_SIZE);
}

constexpr ULONG XONARSE_DEVICE_EXTENSION_SIZE = PORT_CLASS_DEVICE_EXTENSION_SIZE + sizeof(XONARSE_DEVICE_CONTEXT);

// Démarrage (adapter.cpp), appelé par PortCls à IRP_MN_START_DEVICE.
extern "C" NTSTATUS XonarSeStartDevice(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp,
                                       _In_ PRESOURCELIST ResourceList);

// Gestion du retrait (adapter.cpp), appelée par le gestionnaire PnP de driver.cpp
// AVANT de transmettre l'IRP à PortCls.
void XonarSeOnSurpriseRemoval(_In_ PDEVICE_OBJECT DeviceObject);
// Retourne l'objet USB détaché du contexte (à relâcher APRÈS PcDispatchIrp).
XonarUsbDevice* XonarSeOnRemoveDevice(_In_ PDEVICE_OBJECT DeviceObject);
void XonarSeOnStopDevice(_In_ PDEVICE_OBJECT DeviceObject);
