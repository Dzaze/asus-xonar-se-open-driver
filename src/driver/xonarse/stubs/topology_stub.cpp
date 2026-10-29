// BOUCHON du module topology/ (voir usb_stub.cpp pour le principe).
#include "../common.h"

NTSTATUS CreateXonarTopologyMiniport(PUNKNOWN* Unknown, XonarUsbDevice*, XONARSE_ENDPOINT)
{
    *Unknown = nullptr;
    return STATUS_NOT_IMPLEMENTED;
}
