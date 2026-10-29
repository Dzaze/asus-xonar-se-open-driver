// BOUCHON de la couche USB, utilisé seulement quand usb/ n'a pas encore de .cpp ou
// quand XONAR_DRIVER_STUB_USB=ON. Permet aux modules topology/ et wave/ de compiler
// et de se lier indépendamment. Toutes les méthodes échouent proprement.
#include "../usb/usb_device.h"

struct XonarUsbDevice::Impl {};

NTSTATUS XonarUsbDevice::Create(PDEVICE_OBJECT, PDEVICE_OBJECT, PDEVICE_OBJECT, XonarUsbDevice** Device)
{
    *Device = nullptr;
    return STATUS_NOT_IMPLEMENTED;
}
XonarUsbDevice::~XonarUsbDevice() {}
LONG XonarUsbDevice::AddRef() { return InterlockedIncrement(&m_RefCount); }
LONG XonarUsbDevice::Release()
{
    const LONG r = InterlockedDecrement(&m_RefCount);
    if (r == 0) delete this;
    return r;
}
NTSTATUS XonarUsbDevice::Start() { return STATUS_NOT_IMPLEMENTED; }
void XonarUsbDevice::Stop() {}
bool XonarUsbDevice::IsStarted() const { return false; }
bool XonarUsbDevice::IsSurpriseRemoved() const { return false; }
void XonarUsbDevice::MarkSurpriseRemoved() {}
PDEVICE_OBJECT XonarUsbDevice::LowerDevice() const { return nullptr; }
USBD_HANDLE XonarUsbDevice::UsbdHandle() const { return nullptr; }
NTSTATUS XonarUsbDevice::SubmitUrbSync(PURB, ULONG) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::SelectAltSetting(UCHAR, UCHAR, XONAR_AS_PIPES*) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::AbortPipe(USBD_PIPE_HANDLE) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetCurrentFrameNumber(ULONG* Frame) { *Frame = 0; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetSampleRate(UCHAR, ULONG* Hz) { *Hz = 0; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::SetSampleRate(UCHAR, ULONG) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetSampleRateRanges(UCHAR, XONAR_RANGE32*, ULONG, ULONG* Count) { *Count = 0; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetClockValid(UCHAR, bool* Valid) { *Valid = false; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetMute(UCHAR, UCHAR, bool* Muted) { *Muted = false; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::SetMute(UCHAR, UCHAR, bool) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetVolume(UCHAR, UCHAR, SHORT* Value) { *Value = 0; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::SetVolume(UCHAR, UCHAR, SHORT) { return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetVolumeRange(UCHAR, UCHAR, XONAR_RANGE16* Range) { *Range = {}; return STATUS_NOT_IMPLEMENTED; }
NTSTATUS XonarUsbDevice::GetConnectorState(UCHAR, bool* Connected, UCHAR* NrChannels, ULONG* ChannelMask)
{
    *Connected = true;
    if (NrChannels) *NrChannels = 0;
    if (ChannelMask) *ChannelMask = 0;
    return STATUS_NOT_IMPLEMENTED;
}
NTSTATUS XonarUsbDevice::RegisterInterruptCallback(XONAR_INTERRUPT_CALLBACK, PVOID) { return STATUS_NOT_IMPLEMENTED; }
void XonarUsbDevice::UnregisterInterruptCallback(XONAR_INTERRUPT_CALLBACK, PVOID) {}
NTSTATUS XonarUsbDevice::SubmitUsbdUrbSync(PURB, ULONG) { return STATUS_NOT_IMPLEMENTED; }
void XonarUsbDevice::PrepareForRemove() {}
PDEVICE_OBJECT XonarUsbDevice::FunctionalDevice() const { return nullptr; }
NTSTATUS XonarUsbDevice::ResetPipe(USBD_PIPE_HANDLE) { return STATUS_NOT_IMPLEMENTED; }
LONG XonarUsbDevice::StreamOpened() { return 0; }
LONG XonarUsbDevice::StreamClosed() { return 0; }
LONG XonarUsbDevice::OpenStreamCount() const { return 0; }
bool XonarUsbDevice::IsInterruptReaderActive() const { return false; }
