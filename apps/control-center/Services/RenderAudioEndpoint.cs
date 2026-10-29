// SPDX-License-Identifier: Apache-2.0
// Accès en lecture à un endpoint de RENDU 5.1 Xonar via les API Core Audio (WASAPI),
// SANS élévation et SANS dépendance au pilote Xonar. MIROIR de CaptureAudioEndpoint,
// mais pour la SORTIE :
//   - énumération des périphériques de rendu (MMDeviceEnumerator, eRender) ;
//   - résolution de l'endpoint 5.1 Xonar : nom convivial contenant « Xonar » et, de
//     préférence, « Haut-parleurs », ou PKEY_AudioEndpoint_PhysicalSpeakers = 0x60F ;
//   - crêtes PAR CANAL temps réel (IAudioMeterInformation.GetChannelsPeakValues),
//     mesurées en sortie du moteur audio Windows, donc APRÈS le traitement APO (post-DSP) ;
//   - format de mélange partagé (IAudioClient.GetMixFormat) pour l'affichage honnête ;
//   - brève tonalité de test par canal (rendu partagé WASAPI, -20 dBFS) pour localiser
//     chaque enceinte. Ne modifie NI le volume système NI le périphérique par défaut.
//
// Tout est protégé par HRESULT / try-catch : l'indisponibilité de Core Audio, l'absence
// de l'endpoint ou un format non pris en charge se traduit par « non détecté » / retour
// booléen (aucune exception propagée, aucune valeur inventée). Ces interfaces COM sont
// standard sous Windows et ne nécessitent aucun privilège administrateur.

using System;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Déclarations P/Invoke et COM minimales des interfaces Core Audio utilisées pour la
/// SORTIE. Jeu distinct de celui de <see cref="CoreAudioInterop"/> (capture) afin que ce
/// miroir reste autonome. Seules les méthodes réellement appelées sont déclarées ; l'ordre
/// respecte strictement la table virtuelle (vtable), les méthodes non utilisées en fin
/// d'interface pouvant être omises.
/// </summary>
internal static class RenderCoreAudioInterop
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
    internal const uint ClsctxAll = 0x17; // CLSCTX_ALL
    internal const uint StgmRead = 0x00000000;
    internal const ushort VtLpwstr = 31; // VT_LPWSTR
    internal const ushort VtUi4 = 19;    // VT_UI4

    // Masque physique de sortie 5.1 latéral attendu : 0x60F (FL FR FC LFE SL SR).
    internal const uint PhysicalSpeakers51 = 0x60F;

    // AUDCLNT_SHAREMODE_SHARED : partage du moteur audio (aucun mode exclusif).
    internal const int AudclntShareModeShared = 0;

    // PKEY_Device_FriendlyName = {a45c254e-df1c-4efd-8020-67d146a850e0}, pid 14.
    internal static readonly PropertyKey PkeyDeviceFriendlyName = new PropertyKey
    {
        FmtId = new Guid("a45c254e-df1c-4efd-8020-67d146a850e0"),
        Pid = 14,
    };

    // PKEY_AudioEndpoint_PhysicalSpeakers = {1da5d803-d492-4edd-8c23-e0c0ffee7f0e}, pid 3.
    internal static readonly PropertyKey PkeyAudioEndpointPhysicalSpeakers = new PropertyKey
    {
        FmtId = new Guid("1da5d803-d492-4edd-8c23-e0c0ffee7f0e"),
        Pid = 3,
    };

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey
    {
        public Guid FmtId;
        public uint Pid;
    }

    // PROPVARIANT : en-tête de 8 octets puis une union. « Pointer » est le premier mot de
    // l'union (valeur VT_LPWSTR, ou les 4 octets de poids faible pour VT_UI4 en x64).
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

    // WAVEFORMATEX : en-tête commun à tous les formats PCM/float. Pack = 1 (aucun bourrage).
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct WaveFormatEx
    {
        public ushort FormatTag;
        public ushort Channels;
        public uint SamplesPerSec;
        public uint AvgBytesPerSec;
        public ushort BlockAlign;
        public ushort BitsPerSample;
        public ushort Size;
    }

    internal const ushort WaveFormatPcm = 1;
    internal const ushort WaveFormatIeeeFloat = 3;
    internal const ushort WaveFormatExtensible = 0xFFFE;

    // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00aa00389b71}.
    internal static readonly Guid SubtypeIeeeFloat = new Guid("00000003-0000-0010-8000-00aa00389b71");

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

        [PreserveSig]
        int GetMeteringChannelCount(out uint pnChannelCount);

        // afPeakValues est un float* C brut (tampon de u32ChannelCount flottants, alloué par
        // l'appelant). En interop COM, un paramètre tableau est marshalé par DÉFAUT en SAFEARRAY,
        // ce qui provoque une violation d'accès fatale ici (l'API n'attend PAS de SAFEARRAY) :
        // on force donc LPArray, taille donnée par le 1er paramètre (SizeParamIndex=0). float est
        // blittable -> le tableau managé est épinglé et écrit en place (In/Out).
        [PreserveSig]
        int GetChannelsPeakValues(uint u32ChannelCount,
            [In, Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 0)] float[] afPeakValues);
    }

    [ComImport]
    [Guid("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioClient
    {
        [PreserveSig]
        int Initialize(int shareMode, uint streamFlags, long hnsBufferDuration, long hnsPeriodicity,
            IntPtr pFormat, IntPtr audioSessionGuid);

        [PreserveSig]
        int GetBufferSize(out uint pNumBufferFrames);

        [PreserveSig]
        int GetStreamLatency(out long phnsLatency);

        [PreserveSig]
        int GetCurrentPadding(out uint pNumPaddingFrames);

        [PreserveSig]
        int IsFormatSupported(int shareMode, IntPtr pFormat, out IntPtr ppClosestMatch);

        [PreserveSig]
        int GetMixFormat(out IntPtr ppDeviceFormat);

        [PreserveSig]
        int GetDevicePeriod(out long phnsDefaultDevicePeriod, out long phnsMinimumDevicePeriod);

        [PreserveSig]
        int Start();

        [PreserveSig]
        int Stop();

        [PreserveSig]
        int Reset();

        [PreserveSig]
        int SetEventHandle(IntPtr eventHandle);

        [PreserveSig]
        int GetService([MarshalAs(UnmanagedType.LPStruct)] Guid riid,
            [MarshalAs(UnmanagedType.IUnknown)] out object ppv);
    }

    [ComImport]
    [Guid("F294ACFC-3146-4483-A7BF-ADDCA7C260E2")]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    internal interface IAudioRenderClient
    {
        [PreserveSig]
        int GetBuffer(uint numFramesRequested, out IntPtr ppData);

        [PreserveSig]
        int ReleaseBuffer(uint numFramesWritten, uint dwFlags);
    }
}

/// <summary>
/// Endpoint de RENDU 5.1 Xonar résolu via Core Audio : expose les crêtes par canal
/// (post-DSP), le format de mélange partagé et une tonalité de test par canal. Toutes
/// les opérations sont tolérantes aux erreurs (retour booléen / aucune exception) afin
/// que l'interface reste honnête en cas de retrait du périphérique, de format non
/// flottant ou d'indisponibilité de Core Audio.
/// </summary>
internal sealed class RenderAudioEndpoint : IDisposable
{
    public string LastTestError { get; private set; } = "";
    private bool TestFailure(string step, int hr = 0)
    {
        LastTestError = hr < 0 ? $"{step} (0x{hr:X8})" : step;
        return false;
    }
    private RenderCoreAudioInterop.IMMDevice? _device;
    private string _deviceId = "";
    private RenderCoreAudioInterop.IAudioMeterInformation? _meter;

    // Tampon de crêtes réutilisé (évite une allocation par lecture ~50 ms).
    private float[] _peakScratch = Array.Empty<float>();
    private bool _disposed;

    private RenderAudioEndpoint()
    {
    }

    /// <summary>Nom convivial Windows de l'endpoint (ex. « Haut-parleurs (ASUS Xonar SE …) »).</summary>
    public string FriendlyName { get; private set; } = string.Empty;

    /// <summary>Nombre de canaux rapportés par le vumètre (0 si inconnu).</summary>
    public int MeteringChannelCount { get; private set; }
    public uint MixChannelMask { get; private set; }

    /// <summary>
    /// Résout le meilleur endpoint de RENDU actif dont le nom convivial contient
    /// <paramref name="nameFragment"/> (ex. « Xonar »), en privilégiant la sortie 5.1
    /// haut-parleurs (nom contenant « Haut-parleurs » ou PhysicalSpeakers = 0x60F, ou
    /// vumètre à 6 canaux). Retourne <c>null</c> si aucun ne correspond ou si Core Audio
    /// est indisponible.
    /// </summary>
    public static RenderAudioEndpoint? Resolve(string nameFragment)
    {
        // Identité matérielle et rôle haut-parleurs uniques ; aucun repli par nom.
        return ResolveById(InstalledAudioDriver.SpeakerEndpointId());
    }

    public static RenderAudioEndpoint? ResolveById(string? endpointId)
    {
        if(endpointId is null) return null;
        RenderCoreAudioInterop.IMMDeviceEnumerator? enumerator = null;
        RenderCoreAudioInterop.IMMDevice? device = null;
        try {
            enumerator = (RenderCoreAudioInterop.IMMDeviceEnumerator)new RenderCoreAudioInterop.MMDeviceEnumeratorComObject();
            if(enumerator.GetDevice(endpointId, out device) < 0 || device is null) return null;
            var result = Build(device, ReadFriendlyName(device));
            if(result is not null) device = null; // propriété transférée
            return result;
        } catch { return null; }
        finally { TryRelease(device);TryRelease(enumerator); }
    }

    public int TryGetChannelPeaks(float[] peaks)
    {
        if (_meter is null || peaks is null || peaks.Length == 0)
        {
            return 0;
        }

        int meterChannels = MeteringChannelCount;
        if (meterChannels <= 0 || _peakScratch.Length < meterChannels)
        {
            return 0;
        }

        try
        {
            // GetChannelsPeakValues exige u32ChannelCount == nombre de canaux du vumètre
            // (sinon E_INVALIDARG) ; on demande donc exactement MeteringChannelCount.
            if (_meter.GetChannelsPeakValues((uint)meterChannels, _peakScratch) < 0)
            {
                return 0;
            }

            int n = Math.Min(peaks.Length, meterChannels);
            Array.Copy(_peakScratch, peaks, n);
            return n;
        }
        catch
        {
            return 0;
        }
    }

    /// <summary>
    /// Lit le format de mélange partagé de l'endpoint (IAudioClient.GetMixFormat). Retourne
    /// faux en cas d'échec. <paramref name="isFloat"/> vaut vrai pour un flux flottant 32 bits
    /// (seul format pris en charge par la tonalité de test).
    /// </summary>
    public bool TryGetMixFormat(out int channels, out int bitsPerSample, out uint sampleRate, out bool isFloat)
    {
        channels = 0;
        bitsPerSample = 0;
        sampleRate = 0;
        isFloat = false;

        if (_device is null)
        {
            return false;
        }

        IntPtr pFormat = IntPtr.Zero;
        try
        {
            Guid clientIid = typeof(RenderCoreAudioInterop.IAudioClient).GUID;
            if (_device.Activate(clientIid, RenderCoreAudioInterop.ClsctxAll, IntPtr.Zero, out object clientObj) < 0
                || clientObj is not RenderCoreAudioInterop.IAudioClient client)
            {
                return false;
            }

            try
            {
                if (client.GetMixFormat(out pFormat) < 0 || pFormat == IntPtr.Zero)
                {
                    return false;
                }

                var format = Marshal.PtrToStructure<RenderCoreAudioInterop.WaveFormatEx>(pFormat);
                channels = format.Channels;
                bitsPerSample = format.BitsPerSample;
                sampleRate = format.SamplesPerSec;
                isFloat = IsFloatFormat(pFormat, format);
                MixChannelMask = format.FormatTag == 0xfffe && format.Size >= 22
                    ? unchecked((uint)Marshal.ReadInt32(pFormat, 20)) : channels == 2 ? 3U : channels == 1 ? 4U : 0U;
                return channels > 0 && sampleRate > 0;
            }
            finally
            {
                TryRelease(client);
            }
        }
        catch
        {
            return false;
        }
        finally
        {
            if (pFormat != IntPtr.Zero)
            {
                Marshal.FreeCoTaskMem(pFormat);
            }
        }
    }

    /// <summary>
    /// Joue une brève tonalité sinusoïdale sur chaque canal de <paramref name="channelOrder"/>
    /// successivement (rendu partagé WASAPI sur CET endpoint, sans toucher au volume système
    /// ni au périphérique par défaut). <paramref name="onChannel"/> est invoqué (sur un thread
    /// de travail) à l'entame de chaque canal, avec son index. Retourne vrai si la séquence a
    /// pu démarrer et se dérouler ; faux si le format n'est pas flottant 32 bits ou en cas
    /// d'échec WASAPI (dans ce cas, aucun son n'est émis et il faut le signaler honnêtement).
    /// </summary>
    public Task<bool> PlayChannelTestSequenceAsync(
        int[] channelOrder,
        Action<int>? onChannel,
        double secondsPerChannel,
        double frequencyHz,
        double levelDbfs,
        CancellationToken ct)
    {
        return Task.Run(() => RunChannelTestSequence(
            channelOrder, onChannel, secondsPerChannel, frequencyHz, levelDbfs, ct), ct);
    }

    // Déroulé synchrone complet sur un seul thread de travail : activation, Initialize,
    // boucle d'alimentation partagée, puis arrêt. Aucun await afin de rester sur le même
    // thread (objets COM d'apartment non marshalés entre threads).
    private bool RunChannelTestSequence(
        int[] channelOrder,
        Action<int>? onChannel,
        double secondsPerChannel,
        double frequencyHz,
        double levelDbfs,
        CancellationToken ct)
    {
        LastTestError = "";
        if (_device is null || channelOrder is null || channelOrder.Length == 0)
        {
            return TestFailure("Endpoint ou séquence de test indisponible.");
        }

        IntPtr pFormat = IntPtr.Zero;
        RenderCoreAudioInterop.IAudioClient? client = null;
        RenderCoreAudioInterop.IAudioRenderClient? render = null;
        RenderCoreAudioInterop.IMMDeviceEnumerator? enumerator = null;
        RenderCoreAudioInterop.IMMDevice? device = null;
        bool started = false;

        try
        {
            // IMMDevice résolu dans l'apartment UI ne peut pas être réutilisé sur
            // Task.Run : son proxy échoue en E_NOINTERFACE. Recréer tout ici par ID.
            enumerator = (RenderCoreAudioInterop.IMMDeviceEnumerator)new RenderCoreAudioInterop.MMDeviceEnumeratorComObject();
            int deviceHr = enumerator.GetDevice(_deviceId, out device);
            if (deviceHr < 0 || device is null) { return TestFailure("Endpoint du test déconnecté", deviceHr); }
            Guid clientIid = typeof(RenderCoreAudioInterop.IAudioClient).GUID;
            int hr = device.Activate(clientIid, RenderCoreAudioInterop.ClsctxAll, IntPtr.Zero, out object clientObj);
            if (hr < 0
                || clientObj is not RenderCoreAudioInterop.IAudioClient c)
            {
                return TestFailure("Activation WASAPI", hr);
            }
            client = c;

            hr = client.GetMixFormat(out pFormat);
            if (hr < 0 || pFormat == IntPtr.Zero)
            {
                return TestFailure("Lecture du format WASAPI", hr);
            }

            var format = Marshal.PtrToStructure<RenderCoreAudioInterop.WaveFormatEx>(pFormat);
            int channels = format.Channels;
            uint sampleRate = format.SamplesPerSec;
            // Seul le flottant 32 bits est pris en charge (format usuel du mélange partagé).
            if (channels <= 0 || sampleRate == 0 || format.BitsPerSample != 32 || !IsFloatFormat(pFormat, format))
            {
                return TestFailure($"Format non pris en charge : {channels} canaux, {format.BitsPerSample} bits, {sampleRate} Hz.");
            }

            // Tampon partagé d'environ 200 ms.
            const long hundredNsPerSecond = 10_000_000L;
            long bufferDuration = 2 * hundredNsPerSecond / 10; // 200 ms en unités de 100 ns
            hr = client.Initialize(RenderCoreAudioInterop.AudclntShareModeShared, 0, bufferDuration, 0, pFormat, IntPtr.Zero);
            if (hr < 0)
            {
                return TestFailure("Initialisation du test WASAPI", hr);
            }

            hr = client.GetBufferSize(out uint bufferFrames);
            if (hr < 0 || bufferFrames == 0)
            {
                return TestFailure("Taille du tampon WASAPI", hr);
            }

            Guid renderIid = typeof(RenderCoreAudioInterop.IAudioRenderClient).GUID;
            hr = client.GetService(renderIid, out object renderObj);
            if (hr < 0
                || renderObj is not RenderCoreAudioInterop.IAudioRenderClient r)
            {
                return TestFailure("Service de rendu WASAPI", hr);
            }
            render = r;

            double amplitude = Math.Pow(10.0, levelDbfs / 20.0);
            long totalFrames = (long)(secondsPerChannel * channelOrder.Length * sampleRate);
            long fedFrames = 0;      // trames déjà écrites dans le tampon
            int lastSegment = -1;    // dernier canal annoncé

            // Pré-remplissage puis démarrage.
            FeedFrames(render, bufferFrames, channels, channelOrder, secondsPerChannel, sampleRate,
                frequencyHz, amplitude, ref fedFrames, totalFrames, ref lastSegment, onChannel);

            hr = client.Start();
            if (hr < 0)
            {
                return TestFailure("Démarrage du test WASAPI", hr);
            }
            started = true;

            int bufferMs = (int)(1000.0 * bufferFrames / sampleRate);
            int waitMs = Math.Max(5, bufferMs / 2);

            // Boucle d'alimentation : complète le tampon jusqu'à avoir fourni toutes les
            // trames, puis attend l'épuisement du tampon avant d'arrêter.
            while (!ct.IsCancellationRequested)
            {
                if (ct.WaitHandle.WaitOne(waitMs))
                {
                    break; // annulation (page déchargée).
                }

                hr = client.GetCurrentPadding(out uint padding);
                if (hr < 0)
                {
                    return TestFailure("Lecture du tampon WASAPI", hr);
                }

                if (fedFrames < totalFrames)
                {
                    uint available = bufferFrames > padding ? bufferFrames - padding : 0;
                    if (available > 0)
                    {
                        FeedFrames(render, available, channels, channelOrder, secondsPerChannel, sampleRate,
                            frequencyHz, amplitude, ref fedFrames, totalFrames, ref lastSegment, onChannel);
                    }
                }
                else if (padding == 0)
                {
                    break; // tout a été joué.
                }
            }

            return ct.IsCancellationRequested ? TestFailure("Test annulé.") : true;
        }
        catch (Exception ex)
        {
            return TestFailure(ex.Message, ex.HResult);
        }
        finally
        {
            try
            {
                if (started)
                {
                    client?.Stop();
                    client?.Reset();
                }
            }
            catch
            {
                // Arrêt best-effort.
            }

            TryRelease(render);
            TryRelease(client);
            TryRelease(device);
            TryRelease(enumerator);
            if (pFormat != IntPtr.Zero)
            {
                Marshal.FreeCoTaskMem(pFormat);
            }
        }
    }

    // Écrit jusqu'à « framesRequested » trames flottantes entrelacées dans le tampon de
    // rendu : sinus (fréquence/amplitude données) sur le canal du segment temporel courant,
    // silence sur les autres. Une courte enveloppe de fondu (5 ms) aux bords de chaque
    // segment évite les clics lors du changement de canal. Fait avancer « fedFrames » et
    // annonce tout changement de canal via « onChannel ».
    private static void FeedFrames(
        RenderCoreAudioInterop.IAudioRenderClient render,
        uint framesRequested,
        int channels,
        int[] channelOrder,
        double secondsPerChannel,
        uint sampleRate,
        double frequencyHz,
        double amplitude,
        ref long fedFrames,
        long totalFrames,
        ref int lastSegment,
        Action<int>? onChannel)
    {
        if (framesRequested == 0)
        {
            return;
        }

        long remaining = totalFrames - fedFrames;
        if (remaining <= 0)
        {
            return;
        }

        uint frames = (uint)Math.Min(framesRequested, remaining);
        int hr = render.GetBuffer(frames, out IntPtr buffer);
        if (hr < 0 || buffer == IntPtr.Zero)
        {
            throw new COMException("Tampon de rendu WASAPI indisponible.", hr < 0 ? hr : unchecked((int)0x80004005));
        }

        var block = new float[frames * channels];
        double fadeSeconds = 0.005; // 5 ms de fondu aux extrémités d'un segment

        for (uint f = 0; f < frames; f++)
        {
            long globalFrame = fedFrames + f;
            double t = globalFrame / (double)sampleRate;
            int segment = (int)(t / secondsPerChannel);
            if (segment < 0 || segment >= channelOrder.Length)
            {
                continue; // laissé à zéro (silence)
            }

            if (segment != lastSegment)
            {
                lastSegment = segment;
                onChannel?.Invoke(channelOrder[segment]);
            }

            double tInSegment = t - segment * secondsPerChannel;
            double env = 1.0;
            if (tInSegment < fadeSeconds)
            {
                env = tInSegment / fadeSeconds;
            }
            else if (tInSegment > secondsPerChannel - fadeSeconds)
            {
                env = Math.Max(0.0, (secondsPerChannel - tInSegment) / fadeSeconds);
            }

            int target = channelOrder[segment];
            double toneHz = target == 3 && channels == 6 ? 60.0 : frequencyHz;
            double sample = amplitude * env * Math.Sin(2.0 * Math.PI * toneHz * t);
            if (target >= 0 && target < channels)
            {
                block[f * channels + target] = (float)sample;
            }
        }

        Marshal.Copy(block, 0, buffer, (int)(frames * channels));
        hr = render.ReleaseBuffer(frames, 0);
        if (hr < 0) { throw new COMException("Publication du tampon WASAPI.", hr); }
        fedFrames += frames;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        TryRelease(_meter);
        TryRelease(_device);
        _meter = null;
        _device = null;
    }

    // Active le vumètre (requis) depuis l'IMMDevice et prépare le tampon de crêtes.
    private static RenderAudioEndpoint? Build(RenderCoreAudioInterop.IMMDevice device, string name)
    {
        RenderCoreAudioInterop.IAudioMeterInformation? meter = null;
        int meterChannels = 0;

        try
        {
            Guid meterIid = typeof(RenderCoreAudioInterop.IAudioMeterInformation).GUID;
            if (device.Activate(meterIid, RenderCoreAudioInterop.ClsctxAll, IntPtr.Zero, out object meterObj) >= 0)
            {
                meter = meterObj as RenderCoreAudioInterop.IAudioMeterInformation;
            }

            if (meter is not null && meter.GetMeteringChannelCount(out uint channelCount) >= 0)
            {
                meterChannels = (int)channelCount;
            }
        }
        catch
        {
            meter = null;
        }

        // Le vumètre par canal est indispensable à l'utilité de la page.
        if (meter is null)
        {
            return null;
        }

        return new RenderAudioEndpoint
        {
            _device = device,
            _deviceId = device.GetId(out string id) >= 0 ? id : "",
            _meter = meter,
            MeteringChannelCount = meterChannels,
            FriendlyName = name,
            _peakScratch = new float[Math.Max(meterChannels, 1)],
        };
    }

    // Score de correspondance 5.1 haut-parleurs : favorise le nom « Haut-parleurs », le
    // masque PhysicalSpeakers = 0x60F et un vumètre à 6 canaux. Sert à départager plusieurs
    // endpoints Xonar (haut-parleurs vs casque vs S/PDIF).
    private int MatchScore(RenderCoreAudioInterop.IMMDevice device, string name)
    {
        int score = 0;

        if (name.IndexOf("Haut-parleurs", StringComparison.OrdinalIgnoreCase) >= 0
            || name.IndexOf("Haut-parleur", StringComparison.OrdinalIgnoreCase) >= 0
            || name.IndexOf("Speakers", StringComparison.OrdinalIgnoreCase) >= 0)
        {
            score += 3;
        }

        if (TryReadPhysicalSpeakers(device, out uint speakers) && speakers == RenderCoreAudioInterop.PhysicalSpeakers51)
        {
            score += 3;
        }

        if (MeteringChannelCount == 6)
        {
            score += 2;
        }

        return score;
    }

    // Lit PKEY_AudioEndpoint_PhysicalSpeakers (VT_UI4, masque de canaux).
    private static bool TryReadPhysicalSpeakers(RenderCoreAudioInterop.IMMDevice device, out uint speakers)
    {
        speakers = 0;
        try
        {
            if (device.OpenPropertyStore(RenderCoreAudioInterop.StgmRead, out var store) < 0 || store is null)
            {
                return false;
            }

            try
            {
                var key = RenderCoreAudioInterop.PkeyAudioEndpointPhysicalSpeakers;
                if (store.GetValue(ref key, out var pv) < 0)
                {
                    return false;
                }

                try
                {
                    if (pv.Vt == RenderCoreAudioInterop.VtUi4)
                    {
                        // VT_UI4 : valeur dans les 4 octets de poids faible du premier mot d'union.
                        speakers = (uint)(pv.Pointer.ToInt64() & 0xFFFFFFFFL);
                        return true;
                    }

                    return false;
                }
                finally
                {
                    RenderCoreAudioInterop.PropVariantClear(ref pv);
                }
            }
            finally
            {
                TryRelease(store);
            }
        }
        catch
        {
            return false;
        }
    }

    // Détermine si le format est du flottant IEEE 32 bits (directement, ou via le sous-format
    // d'un WAVEFORMATEXTENSIBLE).
    private static bool IsFloatFormat(IntPtr pFormat, RenderCoreAudioInterop.WaveFormatEx format)
    {
        if (format.FormatTag == RenderCoreAudioInterop.WaveFormatIeeeFloat)
        {
            return true;
        }

        if (format.FormatTag == RenderCoreAudioInterop.WaveFormatExtensible && format.Size >= 22)
        {
            try
            {
                // Dans WAVEFORMATEXTENSIBLE, le GUID SubFormat suit l'en-tête (18 o) + Samples (2 o)
                // + dwChannelMask (4 o) = décalage 24.
                var subFormat = Marshal.PtrToStructure<Guid>(pFormat + 24);
                return subFormat == RenderCoreAudioInterop.SubtypeIeeeFloat;
            }
            catch
            {
                return false;
            }
        }

        return false;
    }

    // Lit PKEY_Device_FriendlyName depuis le magasin de propriétés de l'endpoint.
    private static string ReadFriendlyName(RenderCoreAudioInterop.IMMDevice device)
    {
        try
        {
            if (device.OpenPropertyStore(RenderCoreAudioInterop.StgmRead, out var store) < 0 || store is null)
            {
                return string.Empty;
            }

            try
            {
                var key = RenderCoreAudioInterop.PkeyDeviceFriendlyName;
                if (store.GetValue(ref key, out var pv) < 0)
                {
                    return string.Empty;
                }

                try
                {
                    if (pv.Vt == RenderCoreAudioInterop.VtLpwstr && pv.Pointer != IntPtr.Zero)
                    {
                        return Marshal.PtrToStringUni(pv.Pointer) ?? string.Empty;
                    }

                    return string.Empty;
                }
                finally
                {
                    RenderCoreAudioInterop.PropVariantClear(ref pv);
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
