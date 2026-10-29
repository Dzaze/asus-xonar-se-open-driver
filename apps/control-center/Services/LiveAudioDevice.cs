using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>Instantané en lecture seule, identité matérielle unique et format réellement relu.</summary>
public sealed record LiveAudioDevice(string Id, NegotiatedFormat Format, DriverInfo Driver, bool StreamEffects, bool OutputEffects = true)
{
    public static LiveAudioDevice? Read()
    {
        string? id = InstalledAudioDriver.SpeakerEndpointId();
        using var endpoint = RenderAudioEndpoint.ResolveById(id);
        if(id is null || endpoint is null || !endpoint.TryGetMixFormat(out int channels, out int bits, out uint rate, out bool floating)) return null;
        return new(id, new NegotiatedFormat {
            ChannelCount = (uint)channels, ChannelMask = endpoint.MixChannelMask,
            SampleRate = rate, SampleFormat = floating ? SampleFormat.Float32 : bits == 16 ? SampleFormat.Pcm16 : bits == 24 ? SampleFormat.Pcm24 : SampleFormat.Pcm32
        }, InstalledAudioDriver.Read(), InstalledAudioDriver.HasStreamApo(), InstalledAudioDriver.HasOutputApo());
    }
}
