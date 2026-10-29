// SPDX-License-Identifier: Apache-2.0
// Accès en lecture/écriture à un endpoint de CAPTURE Windows via les API Core Audio
// (WASAPI), SANS élévation et SANS dépendance au pilote Xonar :
//   - énumération des périphériques d'enregistrement (MMDeviceEnumerator) ;
//   - résolution de l'endpoint dont le nom convivial contient « Xonar » ;
//   - crête de niveau temps réel (IAudioMeterInformation.GetPeakValue) ;
//   - volume et sourdine du micro (IAudioEndpointVolume, échelle 0..1).
//
// Tout est protégé par HRESULT / try-catch : l'indisponibilité de Core Audio ou
// l'absence du périphérique se traduit par « non détecté » (aucune exception
// propagée, aucune valeur inventée). Ces interfaces COM sont standard sous Windows
// et ne nécessitent aucun privilège administrateur.

using System;
using System.Runtime.InteropServices;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Déclarations P/Invoke et COM minimales des interfaces Core Audio utilisées.
/// Seules les méthodes réellement appelées sont déclarées ; l'ordre des méthodes
/// respecte strictement la table virtuelle (vtable) de chaque interface, les
/// méthodes non utilisées en fin d'interface pouvant être omises.
/// </summary>
internal static class CoreAudioInterop
{
    internal enum EDataFlow
    {
        ERender = 0,
        ECapture = 1,
        EAll = 2,
    }

    internal enum ERole
    {
        EConsole = 0,
        EMultimedia = 1,
        ECommunications = 2,
    }

    internal const uint DeviceStateActive = 0x00000001;
    internal const uint ClsctxAll = 0x17; // CLSCTX_ALL (INPROC_SERVER|INPROC_HANDLER|LOCAL_SERVER|REMOTE_SERVER)
    internal const uint StgmRead = 0x00000000;
    internal const ushort VtLpwstr = 31; // VT_LPWSTR

    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, pid 14.
    internal static readonly PropertyKey PkeyDeviceFriendlyName = new PropertyKey
    {
        FmtId = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"),
        Pid = 14,
    };

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey
    {
        public Guid FmtId;
        public uint Pid;
    }

    // PROPVARIANT : en-tête de 8 octets puis une union. Deux champs pointeur après
    // l'en-tête donnent la taille réelle (16 octets en x86, 24 en x64) ; « Pointer »
    // est le premier mot de l'union (valeur VT_LPWSTR).
    [StructLayout(LayoutKind.Sequential)]
    internal struct PropVariant
    {
        public ushort Vt;
        public ushort Reserved1;
        public ushort Reserved2;
        public ushort Reserved3;
        public IntPtr Pointer;
        public IntPtr Pointer2;
    }

    [DllImport("ole32.dll")]
    internal static extern int PropVariantClear(ref PropVariant pvar);

    [ComImport]
    [Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    internal class MMDeviceEnumeratorComObject
    {
    }

    [ComImport]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(EDataFlow dataFlow, uint dwStateMask, out IMMDeviceCollection ppDevices);

        [PreserveSig]
        int GetDefaultAudioEndpoint(EDataFlow dataFlow, ERole role, out IMMDevice ppEndpoint);

        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string pwstrId, out IMMDevice ppDevice);
    }

    [ComImport]
    [Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDeviceCollection
    {
        [PreserveSig]
        int GetCount(out uint pcDevices);

        [PreserveSig]
        int Item(uint nDevice, out IMMDevice ppDevice);
    }

    [ComImport]
    [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IMMDevice
    {
        [PreserveSig]
        int Activate([MarshalAs(UnmanagedType.LPStruct)] Guid iid, uint dwClsCtx, IntPtr pActivationParams,
            [MarshalAs(UnmanagedType.IUnknown)] out object ppInterface);

        [PreserveSig]
        int OpenPropertyStore(uint stgmAccess, out IPropertyStore ppProperties);

        [PreserveSig]
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string ppstrId);

        [PreserveSig]
        int GetState(out uint pdwState);
    }

    [ComImport]
    [Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IPropertyStore
    {
        [PreserveSig]
        int GetCount(out uint cProps);

        [PreserveSig]
        int GetAt(uint iProp, out PropertyKey pkey);

        [PreserveSig]
        int GetValue(ref PropertyKey key, out PropVariant pv);
    }

    [ComImport]
    [Guid("C02216F6-8C67-4B5B-9D00-D008E73E0064")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioMeterInformation
    {
        [PreserveSig]
        int GetPeakValue(out float pfPeak);
    }

    [ComImport]
    [Guid("5CDF2C82-841E-4546-9722-0CF74078229A")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioEndpointVolume
    {
        [PreserveSig]
        int RegisterControlChangeNotify(IntPtr pNotify);

        [PreserveSig]
        int UnregisterControlChangeNotify(IntPtr pNotify);

        [PreserveSig]
        int GetChannelCount(out uint pnChannelCount);

        [PreserveSig]
        int SetMasterVolumeLevel(float fLevelDb, ref Guid pguidEventContext);

        [PreserveSig]
        int SetMasterVolumeLevelScalar(float fLevel, ref Guid pguidEventContext);

        [PreserveSig]
        int GetMasterVolumeLevel(out float pfLevelDb);

        [PreserveSig]
        int GetMasterVolumeLevelScalar(out float pfLevel);

        [PreserveSig]
        int SetChannelVolumeLevel(uint nChannel, float fLevelDb, ref Guid pguidEventContext);

        [PreserveSig]
        int SetChannelVolumeLevelScalar(uint nChannel, float fLevel, ref Guid pguidEventContext);

        [PreserveSig]
        int GetChannelVolumeLevel(uint nChannel, out float pfLevelDb);

        [PreserveSig]
        int GetChannelVolumeLevelScalar(uint nChannel, out float pfLevel);

        [PreserveSig]
        int SetMute([MarshalAs(UnmanagedType.Bool)] bool bMute, ref Guid pguidEventContext);

        [PreserveSig]
        int GetMute([MarshalAs(UnmanagedType.Bool)] out bool pbMute);

        [PreserveSig]
        int GetVolumeStepInfo(out uint pnStep, out uint pnStepCount);

        [PreserveSig]
        int VolumeStepUp(ref Guid pguidEventContext);

        [PreserveSig]
        int VolumeStepDown(ref Guid pguidEventContext);

        [PreserveSig]
        int QueryHardwareSupport(out uint pdwHardwareSupportMask);

        [PreserveSig]
        int GetVolumeRange(out float pflVolumeMinDb, out float pflVolumeMaxDb, out float pflVolumeIncrementDb);
    }
}

/// <summary>
/// Endpoint de capture Xonar résolu via Core Audio : expose la crête de niveau, le
/// volume (0..1) et la sourdine. Toutes les opérations sont tolérantes aux erreurs
/// (retour booléen / aucune exception propagée) afin que l'interface reste honnête
/// en cas de retrait du périphérique ou d'indisponibilité de Core Audio.
/// </summary>
internal sealed class CaptureAudioEndpoint : IDisposable
{
    private CoreAudioInterop.IMMDevice? _device;
    private CoreAudioInterop.IAudioMeterInformation? _meter;
    private CoreAudioInterop.IAudioEndpointVolume? _volume;

    // Contexte d'évènement propre à l'application (passé aux Set* ; non nul, stable).
    private Guid _eventContext = Guid.NewGuid();
    private bool _disposed;

    private CaptureAudioEndpoint()
    {
    }

    /// <summary>Nom convivial Windows de l'endpoint (ex. « Microphone (ASUS Xonar SE …) »).</summary>
    public string FriendlyName { get; private set; } = string.Empty;

    /// <summary>Vrai si le périphérique expose un contrôle de volume/sourdine utilisable.</summary>
    public bool HasVolumeControl => _volume is not null;

    /// <summary>
    /// Résout le premier endpoint de CAPTURE actif dont le nom convivial contient
    /// <paramref name="nameFragment"/> (comparaison insensible à la casse). Retourne
    /// <c>null</c> si aucun ne correspond ou si Core Audio est indisponible.
    /// </summary>
    public static CaptureAudioEndpoint? Resolve(string nameFragment)
    {
        try
        {
            var enumerator = new CoreAudioInterop.MMDeviceEnumeratorComObject() as CoreAudioInterop.IMMDeviceEnumerator;
            if (enumerator is null)
            {
                return null;
            }

            try
            {
                if (enumerator.EnumAudioEndpoints(CoreAudioInterop.EDataFlow.ECapture,
                        CoreAudioInterop.DeviceStateActive, out var collection) < 0 || collection is null)
                {
                    return null;
                }

                try
                {
                    if (collection.GetCount(out uint count) < 0)
                    {
                        return null;
                    }

                    for (uint i = 0; i < count; i++)
                    {
                        if (collection.Item(i, out var device) < 0 || device is null)
                        {
                            continue;
                        }

                        string name = ReadFriendlyName(device);
                        if (!string.IsNullOrEmpty(name)
                            && name.IndexOf(nameFragment, StringComparison.OrdinalIgnoreCase) >= 0)
                        {
                            var endpoint = Build(device, name);
                            if (endpoint is not null)
                            {
                                return endpoint; // « device » est transféré à l'instance.
                            }
                        }

                        TryRelease(device);
                    }
                }
                finally
                {
                    TryRelease(collection);
                }
            }
            finally
            {
                TryRelease(enumerator);
            }
        }
        catch
        {
            // Core Audio indisponible / interop COM refusée : traité comme « non détecté ».
        }

        return null;
    }

    /// <summary>Lit la crête instantanée (amplitude 0..1). Retourne faux en cas d'échec.</summary>
    public bool TryGetPeak(out float peak)
    {
        peak = 0f;
        if (_meter is null)
        {
            return false;
        }

        try
        {
            return _meter.GetPeakValue(out peak) >= 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Lit le volume d'entrée (échelle 0..1). Retourne faux en cas d'échec.</summary>
    public bool TryGetVolumeScalar(out float scalar)
    {
        scalar = 0f;
        if (_volume is null)
        {
            return false;
        }

        try
        {
            return _volume.GetMasterVolumeLevelScalar(out scalar) >= 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Règle le volume d'entrée (échelle 0..1, bornée). Sans effet si non exposé.</summary>
    public void SetVolumeScalar(float scalar)
    {
        if (_volume is null)
        {
            return;
        }

        scalar = Math.Clamp(scalar, 0f, 1f);
        try
        {
            _volume.SetMasterVolumeLevelScalar(scalar, ref _eventContext);
        }
        catch
        {
            // Échec silencieux : l'interface se resynchronisera à la prochaine lecture.
        }
    }

    /// <summary>Lit l'état de sourdine. Retourne faux en cas d'échec.</summary>
    public bool TryGetMute(out bool muted)
    {
        muted = false;
        if (_volume is null)
        {
            return false;
        }

        try
        {
            return _volume.GetMute(out muted) >= 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Active/désactive la sourdine. Sans effet si le volume n'est pas exposé.</summary>
    public void SetMute(bool muted)
    {
        if (_volume is null)
        {
            return;
        }

        try
        {
            _volume.SetMute(muted, ref _eventContext);
        }
        catch
        {
            // Échec silencieux : l'interface se resynchronisera à la prochaine lecture.
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        TryRelease(_meter);
        TryRelease(_volume);
        TryRelease(_device);
        _meter = null;
        _volume = null;
        _device = null;
    }

    // Active le vumètre (requis) et le contrôle de volume (optionnel) depuis l'IMMDevice.
    private static CaptureAudioEndpoint? Build(CoreAudioInterop.IMMDevice device, string name)
    {
        CoreAudioInterop.IAudioMeterInformation? meter = null;
        CoreAudioInterop.IAudioEndpointVolume? volume = null;

        try
        {
            Guid meterIid = typeof(CoreAudioInterop.IAudioMeterInformation).GUID;
            if (device.Activate(meterIid, CoreAudioInterop.ClsctxAll, IntPtr.Zero, out object meterObj) >= 0)
            {
                meter = meterObj as CoreAudioInterop.IAudioMeterInformation;
            }

            Guid volumeIid = typeof(CoreAudioInterop.IAudioEndpointVolume).GUID;
            if (device.Activate(volumeIid, CoreAudioInterop.ClsctxAll, IntPtr.Zero, out object volumeObj) >= 0)
            {
                volume = volumeObj as CoreAudioInterop.IAudioEndpointVolume;
            }
        }
        catch
        {
            meter = null;
            volume = null;
        }

        // Le vumètre est indispensable à l'utilité de la page ; le volume reste facultatif.
        if (meter is null)
        {
            TryRelease(volume);
            return null;
        }

        return new CaptureAudioEndpoint
        {
            _device = device,
            _meter = meter,
            _volume = volume,
            FriendlyName = name,
        };
    }

    // Lit PKEY_Device_FriendlyName depuis le magasin de propriétés de l'endpoint.
    private static string ReadFriendlyName(CoreAudioInterop.IMMDevice device)
    {
        try
        {
            if (device.OpenPropertyStore(CoreAudioInterop.StgmRead, out var store) < 0 || store is null)
            {
                return string.Empty;
            }

            try
            {
                var key = CoreAudioInterop.PkeyDeviceFriendlyName;
                if (store.GetValue(ref key, out var pv) < 0)
                {
                    return string.Empty;
                }

                try
                {
                    if (pv.Vt == CoreAudioInterop.VtLpwstr && pv.Pointer != IntPtr.Zero)
                    {
                        return Marshal.PtrToStringUni(pv.Pointer) ?? string.Empty;
                    }

                    return string.Empty;
                }
                finally
                {
                    CoreAudioInterop.PropVariantClear(ref pv);
                }
            }
            finally
            {
                TryRelease(store);
            }
        }
        catch
        {
            return string.Empty;
        }
    }

    private static void TryRelease(object? comObject)
    {
        if (comObject is null)
        {
            return;
        }

        try
        {
            if (Marshal.IsComObject(comObject))
            {
                Marshal.FinalReleaseComObject(comObject);
            }
        }
        catch
        {
            // Libération best-effort : un échec n'empêche pas la récupération GC ultérieure.
        }
    }
}
