using System.Text.Json.Nodes;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Fabrique les requêtes C4 à partir des DTO générés. Centralise les règles du
/// contrat : requestId UUID canonique en minuscules, expectedRevision exigé
/// pour toute écriture, endpointKey null admis uniquement en portée agent.
/// </summary>
public static class ControlRequestBuilder
{
    private static string NewRequestId() => Guid.NewGuid().ToString("d");

    private static ControlRequest New(Command command, string? endpointKey, ulong? expectedRevision, ControlRequestPayload payload)
    {
        return new ControlRequest
        {
            ApiMajor = Constants.ApiMajor,
            ApiMinor = Constants.ApiMinor,
            RequestId = NewRequestId(),
            EndpointKey = endpointKey,
            ExpectedRevision = expectedRevision,
            Command = command,
            Payload = payload,
        };
    }

    public static ControlRequest GetCapabilities()
    {
        // Portée agent : endpointKey null, pas de révision.
        return New(Command.GetCapabilities, null, null, new EmptyPayload());
    }

    public static ControlRequest GetState(string endpointKey)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.GetState, endpointKey, null, new EmptyPayload());
    }

    public static ControlRequest ValidateProfile(string endpointKey, JsonNode rawProfile)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.ValidateProfile, endpointKey, null,
            new UncheckedProfilePayload { Profile = rawProfile });
    }

    public static ControlRequest ApplyProfile(string endpointKey, ulong expectedRevision, ProfileDocument profile)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.ApplyProfile, endpointKey, expectedRevision,
            new ProfilePayload { Profile = profile });
    }

    public static ControlRequest SubscribeState(string endpointKey, uint maxRateHz)
    {
        RequireEndpointKey(endpointKey);
        uint rate = Math.Clamp(maxRateHz, SubscribeStatePayloadLimits.MaxRateHzMin, Constants.UiRefreshMaxHz);
        return New(Command.SubscribeState, endpointKey, null, new SubscribeStatePayload { MaxRateHz = rate });
    }

    public static ControlRequest SetHardwareVolume(string endpointKey, ulong expectedRevision, uint ksChannel, double levelDb)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.SetHardwareVolume, endpointKey, expectedRevision,
            new VolumeLevelControl { KsChannel = ksChannel, LevelDb = levelDb });
    }

    public static ControlRequest SetHardwareMute(string endpointKey, ulong expectedRevision, bool mute)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.SetHardwareVolume, endpointKey, expectedRevision,
            new MuteControl { Mute = mute });
    }

    public static ControlRequest RequestFormatRepair(string endpointKey, ulong expectedRevision)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.RequestFormatRepair, endpointKey, expectedRevision, new EmptyPayload());
    }

    public static ControlRequest StartChannelTest(string endpointKey, string testId, uint channelBit, double frequencyHz, double levelDb)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.StartChannelTest, endpointKey, null,
            new StartChannelTestPayload { TestId = testId, ChannelBit = channelBit, FrequencyHz = frequencyHz, LevelDb = levelDb });
    }

    public static ControlRequest StopChannelTest(string endpointKey, string testId)
    {
        RequireEndpointKey(endpointKey);
        return New(Command.StopChannelTest, endpointKey, null, new StopChannelTestPayload { TestId = testId });
    }

    private static void RequireEndpointKey(string? endpointKey)
    {
        if (string.IsNullOrEmpty(endpointKey))
        {
            throw new ArgumentException("Une clé d'endpoint est requise pour cette commande.", nameof(endpointKey));
        }
    }
}
