// SPDX-License-Identifier: Apache-2.0

using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Services.Localization;

namespace Xonar.ControlCenter.Tests;

/// <summary>
/// Implémentation FACTICE et MINIMALE de <see cref="IControlClient"/> pour les
/// tests d'interface, volontairement INDÉPENDANTE de <c>FakeControlBroker</c>.
/// Elle sert à vérifier des COMPORTEMENTS observables plutôt que des détails
/// d'implémentation :
/// <list type="bullet">
///   <item>état de connexion pilotable (<see cref="Disconnect"/> / <see cref="Reconnect"/>,
///         qui lèvent <see cref="ConnectionChanged"/>) ; reste déconnecté tant qu'on
///         ne le reconnecte pas ;</item>
///   <item>comptage des appels d'ÉCRITURE via <see cref="WriteCount"/>
///         (ApplyProfile, RequestFormatRepair, et SendAsync d'une commande d'écriture) ;</item>
///   <item>lectures renvoyant des valeurs par défaut valides (capacités, état).</item>
/// </list>
/// Aucune logique matérielle n'est simulée : les écritures ne sont jamais
/// appliquées, seulement COMPTÉES.
/// </summary>
public sealed class FakeStubClient : IControlClient
{
    private readonly object _gate = new();
    private readonly string _endpointKey;
    private bool _connected;
    private int _writeCount;

    public FakeStubClient(string endpointKey = "MI_00", bool connected = true)
    {
        _endpointKey = endpointKey;
        _connected = connected;
    }

    /// <summary>
    /// Nombre d'appels d'ÉCRITURE reçus depuis la construction. Reste à 0 tant
    /// qu'aucune écriture matérielle n'est demandée.
    /// </summary>
    public int WriteCount => Volatile.Read(ref _writeCount);

    public bool IsConnected
    {
        get { lock (_gate) { return _connected; } }
    }

    public event EventHandler<ConnectionChangedEventArgs>? ConnectionChanged;
    public event EventHandler<StateNotification>? StateNotification;

    /// <summary>Coupe la connexion et lève <see cref="ConnectionChanged"/> (si elle était active).</summary>
    public void Disconnect()
    {
        lock (_gate)
        {
            if (!_connected)
            {
                return;
            }
            _connected = false;
        }
        ConnectionChanged?.Invoke(this, new ConnectionChangedEventArgs(false, ErrorMessages.ConnectionLost));
    }

    /// <summary>Rétablit la connexion et lève <see cref="ConnectionChanged"/> (si elle était coupée).</summary>
    public void Reconnect()
    {
        lock (_gate)
        {
            if (_connected)
            {
                return;
            }
            _connected = true;
        }
        ConnectionChanged?.Invoke(this, new ConnectionChangedEventArgs(true, ErrorMessages.Reconnected));
    }

    /// <summary>
    /// Pousse immédiatement une notification d'état (simule la file de
    /// rafraîchissement bornée de l'abonnement). N'écrit jamais.
    /// </summary>
    public void PublishState(AgentState state)
    {
        StateNotification?.Invoke(this, new StateNotification
        {
            RequestId = "abonnement-stub",
            Sequence = unchecked((ulong)Environment.TickCount64),
            State = state,
        });
    }

    public Task<ControlResponse> SendAsync(ControlRequest request, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        if (EnumAttributes.AccessOf(request.Command) == CommandAccess.Write)
        {
            Interlocked.Increment(ref _writeCount);
        }
        return Task.FromResult(OkResponse(request.RequestId));
    }

    public Task<CapabilitiesResult> GetCapabilitiesAsync(CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        return Task.FromResult(new CapabilitiesResult
        {
            ApiMajor = Constants.ApiMajor,
            ApiMinor = Constants.ApiMinor,
            Contracts = new List<ContractVersionInfo>
            {
                new() { Id = "C1", Major = 1, Minor = 0 },
                new() { Id = "C2", Major = 1, Minor = 0 },
                new() { Id = "C3", Major = 1, Minor = 0 },
                new() { Id = "C4", Major = 1, Minor = 0 },
                new() { Id = "C5", Major = 1, Minor = 0 },
                new() { Id = "C6", Major = 1, Minor = 0 },
                new() { Id = "C7", Major = 1, Minor = 0 },
                new() { Id = "C8", Major = 1, Minor = 0 },
            },
            Endpoints = new List<EndpointCapability>
            {
                new()
                {
                    EndpointKey = _endpointKey,
                    EndpointRole = EndpointRole.Speakers,
                    Driver = new DriverInfo { Kind = DriverKind.Xonarse, Version = "0.3.3.0", PackageSha256 = null },
                    SupportedSampleRates = new List<uint> { 44100, 48000, 96000, 192000 },
                    PhysicalChannelMask = Constants.PhysicalOutputMask,
                    Writable = true,
                },
            },
            Features = new FeatureFlags
            {
                AutoDeclaredLayout = true,
                ChannelTest = true,
                FormatRepair = false,
            },
        });
    }

    public Task<StateResult> GetStateAsync(string endpointKey, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        return Task.FromResult(new StateResult { State = MinimalState(endpointKey) });
    }

    public Task<ValidationResult> ValidateProfileAsync(string endpointKey, System.Text.Json.Nodes.JsonNode rawProfile, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        return Task.FromResult(new ValidationResult
        {
            Valid = true,
            Normalized = null,
            Diagnostics = new List<Diagnostic>(),
        });
    }

    public Task<ControlResponse> ApplyProfileAsync(string endpointKey, ulong expectedRevision, ProfileDocument profile, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        Interlocked.Increment(ref _writeCount);
        return Task.FromResult(OkResponse(string.Empty));
    }

    public Task<ControlResponse> RequestFormatRepairAsync(string endpointKey, ulong expectedRevision, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        Interlocked.Increment(ref _writeCount);
        return Task.FromResult(OkResponse(string.Empty));
    }

    public IDisposable SubscribeState(string endpointKey, uint maxRateHz) => new Subscription();

    /// <summary>Construit un <see cref="AgentState"/> minimal mais valide pour les tests.</summary>
    public static AgentState MinimalState(string endpointKey) => new()
    {
        SchemaVersion = Constants.StateSchemaVersion,
        EndpointKey = endpointKey,
        EndpointRole = EndpointRole.Speakers,
        Revision = 0,
        Driver = new DriverInfo { Kind = DriverKind.Xonarse, Version = null, PackageSha256 = null },
        NegotiatedFormat = new NegotiatedFormat
        {
            SampleRate = 48000,
            ChannelMask = Constants.Mask51Side,
            ChannelCount = Constants.PhysicalOutputChannels,
            SampleFormat = SampleFormat.Float32,
        },
        ProfileId = null,
        ListeningMode = null,
        RequestedPolicy = null,
        ResolvedPolicy = ResolvedPolicy.Conservative,
        DesiredGeneration = 0,
        CommittedGeneration = 0,
        PartiallyApplied = false,
        Instances = new List<ApoInstanceState>(),
        Jack = new JackState { Reliability = JackReliability.ForcedConnection, PublishedConnected = true },
        Meters = new MeterState
        {
            AgeMs = 100,
            Stale = false,
            OutputPeakDbfs = new double[] { -60, -60, -60, -60, -60, -60 },
            LimiterReductionDb = 0,
            NonFiniteFrames = 0,
            TelemetryLost = 0,
        },
    };

    private static ControlResponse OkResponse(string requestId) => new()
    {
        RequestId = requestId,
        Status = ResponseStatus.Ok,
        Revision = 0,
        DesiredGeneration = 0,
        CommittedGeneration = 0,
        AppliedInstances = new List<InstanceGeneration>(),
        Error = null,
        Result = null,
    };

    private sealed class Subscription : IDisposable
    {
        public void Dispose()
        {
            // Abonnement factice : rien à libérer.
        }
    }
}
