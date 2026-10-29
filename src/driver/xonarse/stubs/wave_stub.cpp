// BOUCHON du module wave/ (voir usb_stub.cpp pour le principe).
#include "../common.h"

NTSTATUS CreateXonarWaveRtMiniport(PUNKNOWN* Unknown, XonarUsbDevice*, XONARSE_ENDPOINT)
{
    *Unknown = nullptr;
    return STATUS_NOT_IMPLEMENTED;
}
