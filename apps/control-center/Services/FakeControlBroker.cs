using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>Scénarios simulables par le faux agent (hors matériel, hors pilote).</summary>
public enum FakeBrokerScenario
{
    /// <summary>Validation et application nominales.</summary>
    Normal,

    /// <summary>ValidateProfile répond invalide (refus de schéma).</summary>
    RejectSchema,

    /// <summary>ApplyProfile répond RevisionConflict sans rien écrire.</summary>
    RevisionConflict,

    /// <summary>ApplyProfile écrit, puis reste « partiellement appliqué ».</summary>
    PartialApply,

    /// <summary>Le transport est coupé : toute commande échoue.</summary>
    ConnectionLoss,
}

/// <summary>Options de construction du faux agent.</summary>
public sealed class FakeControlBrokerOptions
{
    public string EndpointKey { get; init; } = "MI_00";
    public bool Writable { get; init; } = true;
    public bool FeatureAutoDeclaredLayout { get; init; } = true;
    public bool FeatureChannelTest { get; init; } = true;
    public bool FeatureFormatRepair { get; init; } = false;
    public IReadOnlyList<uint> SampleRates { get; init; } = new uint[] { 44100, 48000, 96000, 192000 };
}

/// <summary>
/// Implémentation FACTICE et injectable du client C4. Elle simule, sans carte ni
/// pilote : validation OK, refus de schéma, conflit de révision, application
/// partielle, perte de connexion et reconnexion. Toute l'interface et tous les
/// tests tournent sur cette classe ; aucune vue n'écrit dans KS/CAPX/registre.
/// </summary>
public sealed class FakeControlBroker : IControlClient
{
    private readonly object _gate = new();
    private readonly FakeControlBrokerOptions _options;
    private bool _connected = true;
    private ulong _revision;
    private ulong _committedGeneration;
    private ulong _desiredGeneration;
    private AgentState _state;
    private readonly List<StateSubscription> _subscriptions = new();

    public FakeControlBroker(FakeControlBrokerOptions? options = null)
    {
        _options = options ?? new FakeControlBrokerOptions();
        _state = BuildInitialState(_options);
    }

    public bool IsConnected
    {
        // Cohérent avec EnsureConnected() : le scénario ConnectionLoss simule un
        // transport coupé, donc l'agent n'est PAS connecté même si _connected n'a
        // pas été basculé par Disconnect(). Sans cela, CanWrite resterait vrai alors
        // que toute commande échoue.
        get { lock (_gate) { return _connected && Scenario != FakeBrokerScenario.ConnectionLoss; } }
    }

    public FakeBrokerScenario Scenario { get; set; } = FakeBrokerScenario.Normal;

    public event EventHandler<ConnectionChangedEventArgs>? ConnectionChanged;
    public event EventHandler<StateNotification>? StateNotification;

    /// <summary>Coupe la connexion (simule la perte du named pipe).</summary>
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
        ConnectionChanged?.Invoke(this, new ConnectionChangedEventArgs(false, Localization.ErrorMessages.ConnectionLost));
    }

    /// <summary>Rétablit la connexion.</summary>
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
        ConnectionChanged?.Invoke(this, new ConnectionChangedEventArgs(true, Localization.ErrorMessages.Reconnected));
    }

    /// <summary>Remplace l'état synthétique exposé par GetState et les notifications.</summary>
    public void SetState(AgentState state)
    {
        lock (_gate)
        {
            _state = state;
            _revision = state.Revision;
            _committedGeneration = state.CommittedGeneration;
            _desiredGeneration = state.DesiredGeneration;
        }
    }

    /// <summary>Pousse immédiatement une notification d'état (utile aux tests retardés).</summary>
    public void PublishStateNow(string subscriptionRequestId)
    {
        AgentState snapshot;
        lock (_gate)
        {
            snapshot = CloneState(_state);
        }
        StateNotification?.Invoke(this, new StateNotification
        {
            RequestId = subscriptionRequestId,
            Sequence = NextSequence(subscriptionRequestId),
            State = snapshot,
        });
    }

    private ulong NextSequence(string requestId)
    {
        // Séquence déterministe par abonnement, bornée par abonnement actif.
        return (ulong)Environment.TickCount64 & 0xFFFF_FFFFUL;
    }

    public Task<ControlResponse> SendAsync(ControlRequest request, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        EnsureConnected();

        lock (_gate)
        {
            switch (request.Command)
            {
                case Command.GetCapabilities:
                    return Task.FromResult(BuildCapabilitiesResponse(request));
                case Command.GetState:
                    return Task.FromResult(BuildStateResponse(request));
                case Command.ValidateProfile:
                    return Task.FromResult(BuildValidationResponse(request));
                case Command.ApplyProfile:
                    return Task.FromResult(BuildApplyResponse(request));
                case Command.SubscribeState:
                    return Task.FromResult(BuildSubscribeResponse(request));
                case Command.RequestFormatRepair:
                    return Task.FromResult(BuildFormatRepairResponse(request));
                case Command.SetHardwareVolume:
                    return Task.FromResult(BuildHardwareVolumeResponse(request));
                case Command.StartChannelTest:
                case Command.StopChannelTest:
                    return Task.FromResult(BuildChannelTestResponse(request));
                default:
                    throw new ControlClientException(ErrorCode.UnsupportedCapability,
                        Localization.ErrorMessages.ForErrorCode(ErrorCode.UnsupportedCapability));
            }
        }
    }

    public Task<CapabilitiesResult> GetCapabilitiesAsync(CancellationToken ct = default)
    {
        var response = SendAsync(ControlRequestBuilder.GetCapabilities(), ct).GetAwaiter().GetResult();
        ThrowOnError(response);
        if (response.Result is CapabilitiesResult capabilities)
        {
            return Task.FromResult(capabilities);
        }
        throw new ControlClientException(ErrorCode.UnsupportedCapability,
            Localization.ErrorMessages.ForErrorCode(ErrorCode.UnsupportedCapability));
    }

    public Task<StateResult> GetStateAsync(string endpointKey, CancellationToken ct = default)
    {
        var response = SendAsync(ControlRequestBuilder.GetState(endpointKey), ct).GetAwaiter().GetResult();
        ThrowOnError(response);
        if (response.Result is StateResult state)
        {
            return Task.FromResult(state);
        }
        throw new ControlClientException(ErrorCode.UnsupportedCapability,
            Localization.ErrorMessages.ForErrorCode(ErrorCode.UnsupportedCapability));
    }

    public Task<ValidationResult> ValidateProfileAsync(string endpointKey, System.Text.Json.Nodes.JsonNode rawProfile, CancellationToken ct = default)
    {
        var response = SendAsync(ControlRequestBuilder.ValidateProfile(endpointKey, rawProfile), ct).GetAwaiter().GetResult();
        ThrowOnError(response);
        if (response.Result is ValidationResult validation)
        {
            return Task.FromResult(validation);
        }
        throw new ControlClientException(ErrorCode.UnsupportedCapability,
            Localization.ErrorMessages.ForErrorCode(ErrorCode.UnsupportedCapability));
    }

    public Task<ControlResponse> ApplyProfileAsync(string endpointKey, ulong expectedRevision, ProfileDocument profile, CancellationToken ct = default)
    {
        return SendAsync(ControlRequestBuilder.ApplyProfile(endpointKey, expectedRevision, profile), ct);
    }

    public Task<ControlResponse> RequestFormatRepairAsync(string endpointKey, ulong expectedRevision, CancellationToken ct = default)
    {
        return SendAsync(ControlRequestBuilder.RequestFormatRepair(endpointKey, expectedRevision), ct);
    }

    public IDisposable SubscribeState(string endpointKey, uint maxRateHz)
    {
        var subscription = new StateSubscription(this, endpointKey, maxRateHz);
        lock (_gate)
        {
            _subscriptions.Add(subscription);
        }
        return subscription;
    }

    // ---------------------------------------------------------------------------

    private void EnsureConnected()
    {
        if (Scenario == FakeBrokerScenario.ConnectionLoss || !IsConnected)
        {
            throw new ControlClientException(Localization.ErrorMessages.ConnectionLost);
        }
    }

    private static void ThrowOnError(ControlResponse response)
    {
        if (response.Status == ResponseStatus.Error && response.Error is not null)
        {
            throw new ControlClientException(response.Error.Code, Localization.ErrorMessages.ForErrorCode(response.Error.Code));
        }
    }

    private ControlResponse BuildCapabilitiesResponse(ControlRequest request)
    {
        var result = new CapabilitiesResult
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
                    EndpointKey = _options.EndpointKey,
                    EndpointRole = EndpointRole.Speakers,
                    Driver = new DriverInfo { Kind = DriverKind.Xonarse, Version = "0.3.3.0", PackageSha256 = null },
                    SupportedSampleRates = new List<uint>(_options.SampleRates),
                    PhysicalChannelMask = Constants.PhysicalOutputMask,
                    Writable = _options.Writable,
                },
            },
            Features = new FeatureFlags
            {
                AutoDeclaredLayout = _options.FeatureAutoDeclaredLayout,
                ChannelTest = _options.FeatureChannelTest,
                FormatRepair = _options.FeatureFormatRepair,
            },
        };

        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Ok,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = new List<InstanceGeneration>(),
            Error = null,
            Result = result,
        };
    }

    private ControlResponse BuildStateResponse(ControlRequest request)
    {
        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Ok,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = BuildAppliedInstances(),
            Error = null,
            Result = new StateResult { State = CloneState(_state) },
        };
    }

    private ControlResponse BuildValidationResponse(ControlRequest request)
    {
        if (Scenario == FakeBrokerScenario.RejectSchema)
        {
            var invalidResult = new ValidationResult
            {
                Valid = false,
                Normalized = null,
                Diagnostics = new List<Diagnostic>
                {
                    new() { Code = "schema.maximum", Path = "$/output/masterGainDb", Message = "Valeur hors bornes simulée par le faux agent." },
                },
            };
            return new ControlResponse
            {
                RequestId = request.RequestId,
                Status = ResponseStatus.Ok,
                Revision = _revision,
                DesiredGeneration = _desiredGeneration,
                CommittedGeneration = _committedGeneration,
                AppliedInstances = new List<InstanceGeneration>(),
                Error = null,
                Result = invalidResult,
            };
        }

        var outcome = ValidatePayload(request);
        var result = new ValidationResult
        {
            Valid = outcome.Valid,
            Normalized = outcome.Normalized,
            Diagnostics = outcome.Diagnostics,
        };
        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Ok,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = new List<InstanceGeneration>(),
            Error = null,
            Result = result,
        };
    }

    private ControlResponse BuildApplyResponse(ControlRequest request)
    {
        if (request.ExpectedRevision is not ulong expected || expected != _revision)
        {
            return ErrorResponse(request, ErrorCode.RevisionConflict,
                "La révision attendue ne correspond pas à la révision actuelle de l'endpoint.");
        }

        if (Scenario == FakeBrokerScenario.RevisionConflict)
        {
            return ErrorResponse(request, ErrorCode.RevisionConflict,
                "Conflit de révision simulé par le faux agent.");
        }

        var outcome = ValidatePayload(request);
        if (!outcome.Valid || outcome.Normalized is null)
        {
            return ErrorResponse(request, ErrorCode.InvalidProfile,
                "Le profil fourni est invalide : " + (outcome.Diagnostics.FirstOrDefault()?.Message ?? "voir le détail."));
        }

        _revision += 1;
        _committedGeneration += 1;
        _desiredGeneration = _committedGeneration;
        _state.Revision = _revision;
        _state.CommittedGeneration = _committedGeneration;
        _state.DesiredGeneration = _desiredGeneration;
        _state.ProfileId = outcome.Normalized.ProfileId;
        _state.ListeningMode = outcome.Normalized.ListeningMode;
        _state.RequestedPolicy = outcome.Normalized.SourcePolicy;
        _state.ResolvedPolicy = ResolvePolicy(outcome.Normalized.ListeningMode, outcome.Normalized.SourcePolicy);

        bool partially = Scenario == FakeBrokerScenario.PartialApply;
        _state.PartiallyApplied = partially;
        UpdateInstanceGenerations(partially);

        var applied = BuildAppliedInstances();
        var status = partially ? ResponseStatus.Committed : ResponseStatus.Applied;

        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = status,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = applied,
            Error = null,
            Result = null,
        };
    }

    private ControlResponse BuildSubscribeResponse(ControlRequest request)
    {
        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Ok,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = BuildAppliedInstances(),
            Error = null,
            Result = null,
        };
    }

    private ControlResponse BuildFormatRepairResponse(ControlRequest request)
    {
        if (!_options.FeatureFormatRepair)
        {
            return ErrorResponse(request, ErrorCode.UnsupportedCapability,
                "La réparation du format partagé n'est pas disponible sur cet endpoint.");
        }
        if (request.ExpectedRevision is not ulong expected || expected != _revision)
        {
            return ErrorResponse(request, ErrorCode.RevisionConflict,
                "La révision attendue ne correspond pas à la révision actuelle.");
        }
        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Ok,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = BuildAppliedInstances(),
            Error = null,
            Result = null,
        };
    }

    private ControlResponse BuildHardwareVolumeResponse(ControlRequest request)
    {
        if (request.ExpectedRevision is not ulong expected || expected != _revision)
        {
            return ErrorResponse(request, ErrorCode.RevisionConflict,
                "La révision attendue ne correspond pas à la révision actuelle.");
        }
        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Ok,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = new List<InstanceGeneration>(),
            Error = null,
            Result = new HardwareVolumeResult
            {
                KsChannel = null,
                LevelDb = null,
                Mute = null,
            },
        };
    }

    private ControlResponse BuildChannelTestResponse(ControlRequest request)
    {
        if (!_options.FeatureChannelTest)
        {
            return ErrorResponse(request, ErrorCode.UnsupportedCapability,
                "Le test de canal n'est pas disponible sur cet endpoint.");
        }
        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Ok,
            Revision = _revision,
            DesiredGeneration = _desiredGeneration,
            CommittedGeneration = _committedGeneration,
            AppliedInstances = new List<InstanceGeneration>(),
            Error = null,
            Result = new ChannelTestResult
            {
                TestId = Guid.NewGuid().ToString("d"),
                ExpiresInMs = Constants.ChannelTestMaxExpiryMs,
            },
        };
    }

    private ProfileValidationOutcome ValidatePayload(ControlRequest request)
    {
        // ApplyProfile porte un ProfilePayload typé ; ValidateProfile porte un
        // objet JSON brut (UncheckedProfilePayload). Les deux passent par le même
        // validateur de schéma.
        if (request.Payload is ProfilePayload typed)
        {
            return ProfileValidator.Validate(ContractJson.Profile(typed.Profile), 48000);
        }

        if (request.Payload is UncheckedProfilePayload uncheckedPayload && uncheckedPayload.Profile is System.Text.Json.Nodes.JsonNode raw)
        {
            return ProfileValidator.Validate(raw, 48000);
        }

        return new ProfileValidationOutcome();
    }

    private List<InstanceGeneration> BuildAppliedInstances()
    {
        if (_state.PartiallyApplied)
        {
            // La génération validée n'est pas encore acquittée par toutes les instances.
            return _state.Instances
                .Where(i => i.AppliedGeneration == _committedGeneration)
                .Select(i => new InstanceGeneration
                {
                    InstanceId = i.InstanceId,
                    Stage = i.Stage,
                    AppliedGeneration = i.AppliedGeneration,
                })
                .ToList();
        }

        return _state.Instances.Select(i => new InstanceGeneration
        {
            InstanceId = i.InstanceId,
            Stage = i.Stage,
            AppliedGeneration = _committedGeneration,
        }).ToList();
    }

    private void UpdateInstanceGenerations(bool partially)
    {
        foreach (var instance in _state.Instances)
        {
            if (partially && instance.Stage == ApoStage.Efx)
            {
                // L'instance EFX reste sur l'ancienne génération : état partiel.
                continue;
            }
            instance.AppliedGeneration = _committedGeneration;
        }
    }

    private static ResolvedPolicy ResolvePolicy(ListeningMode mode, SourcePolicy requested)
    {
        // C2 : en modes de sortie 2.0/2.1, la politique résolue est neutre avant
        // mixage. Sinon, la politique demandée est résolue telle quelle.
        if (mode == ListeningMode.TwoPointOne || mode == ListeningMode.Stereo20)
        {
            return ResolvedPolicy.Neutral;
        }
        return requested switch
        {
            SourcePolicy.Preserve => ResolvedPolicy.Preserve,
            SourcePolicy.AutoDeclaredLayout => ResolvedPolicy.AutoDeclaredLayout,
            SourcePolicy.ForceStereoFrontPair => ResolvedPolicy.ForceStereoFrontPair,
            _ => ResolvedPolicy.Conservative,
        };
    }

    private static ControlResponse ErrorResponse(ControlRequest request, ErrorCode code, string message)
    {
        return new ControlResponse
        {
            RequestId = request.RequestId,
            Status = ResponseStatus.Error,
            Revision = 0,
            DesiredGeneration = 0,
            CommittedGeneration = 0,
            AppliedInstances = new List<InstanceGeneration>(),
            Error = new ErrorInfo { Code = code, Message = message },
            Result = null,
        };
    }

    private static AgentState BuildInitialState(FakeControlBrokerOptions options)
    {
        return new AgentState
        {
            SchemaVersion = Constants.StateSchemaVersion,
            EndpointKey = options.EndpointKey,
            EndpointRole = EndpointRole.Speakers,
            Revision = 0,
            Driver = new DriverInfo { Kind = DriverKind.Xonarse, Version = "0.3.3.0", PackageSha256 = null },
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
            Instances = new List<ApoInstanceState>
            {
                new()
                {
                    InstanceId = "sfx-1",
                    Stage = ApoStage.Sfx,
                    Epoch = 1,
                    AppliedGeneration = 0,
                    InputFormat = new NegotiatedFormat { SampleRate = 48000, ChannelMask = Constants.Mask51Side, ChannelCount = 6, SampleFormat = SampleFormat.Float32 },
                    Bypass = new BypassState { Kind = BypassKind.None, Reason = null },
                },
                new()
                {
                    InstanceId = "efx-1",
                    Stage = ApoStage.Efx,
                    Epoch = 1,
                    AppliedGeneration = 0,
                    InputFormat = new NegotiatedFormat { SampleRate = 48000, ChannelMask = Constants.Mask51Side, ChannelCount = 6, SampleFormat = SampleFormat.Float32 },
                    Bypass = new BypassState { Kind = BypassKind.Unknown, Reason = null },
                },
            },
            Jack = new JackState { Reliability = JackReliability.ForcedConnection, PublishedConnected = true },
            Meters = new MeterState
            {
                // État de DÉMONSTRATION : crêtes post-DSP variées et vivantes (ordre
                // physique FL FR FC LFE SL SR), alignées sur les valeurs d'exemple de la
                // page « Vue d'ensemble » pour que l'UI reste parlante dès le démarrage
                // (sinon les vumètres retombent à -60 dBFS une fois l'état chargé).
                // Mesure fraîche : AgeMs < seuil de péremption C7 (2000 ms) et non périmée.
                AgeMs = 300,
                Stale = false,
                OutputPeakDbfs = new double[] { -12.4, -8.1, -10.5, -18.6, -14.2, -11.9 },
                LimiterReductionDb = 0,
                NonFiniteFrames = 0,
                TelemetryLost = 0,
            },
        };
    }

    private static AgentState CloneState(AgentState source)
    {
        return new AgentState
        {
            SchemaVersion = source.SchemaVersion,
            EndpointKey = source.EndpointKey,
            EndpointRole = source.EndpointRole,
            Revision = source.Revision,
            Driver = source.Driver is null ? new DriverInfo() : new DriverInfo { Kind = source.Driver.Kind, Version = source.Driver.Version, PackageSha256 = source.Driver.PackageSha256 },
            NegotiatedFormat = source.NegotiatedFormat,
            ProfileId = source.ProfileId,
            ListeningMode = source.ListeningMode,
            RequestedPolicy = source.RequestedPolicy,
            ResolvedPolicy = source.ResolvedPolicy,
            DesiredGeneration = source.DesiredGeneration,
            CommittedGeneration = source.CommittedGeneration,
            PartiallyApplied = source.PartiallyApplied,
            Instances = source.Instances?.Select(i => new ApoInstanceState
            {
                InstanceId = i.InstanceId,
                Stage = i.Stage,
                Epoch = i.Epoch,
                AppliedGeneration = i.AppliedGeneration,
                InputFormat = i.InputFormat,
                Bypass = new BypassState { Kind = i.Bypass?.Kind ?? BypassKind.Unknown, Reason = i.Bypass?.Reason },
            }).ToList() ?? new List<ApoInstanceState>(),
            Jack = new JackState { Reliability = source.Jack?.Reliability ?? JackReliability.Unknown, PublishedConnected = source.Jack?.PublishedConnected },
            Meters = source.Meters,
        };
    }

    private sealed class StateSubscription : IDisposable
    {
        private readonly FakeControlBroker _broker;
        private readonly string _requestId;
        private int _disposed;

        public StateSubscription(FakeControlBroker broker, string endpointKey, uint maxRateHz)
        {
            _broker = broker;
            _requestId = Guid.NewGuid().ToString("d");
            // Abonnement réel borné : pas de timer ici, le faux agent pousse via
            // PublishStateNow() ou un timer minimal piloté par les tests.
        }

        public void Dispose()
        {
            // Idempotent : ne se retire qu'une fois, et se retire RÉELLEMENT de la
            // liste du broker (sinon les abonnements s'accumulent a chaque navigation
            // vers la page État — fuite). _disposed empeche toute double suppression.
            if (Interlocked.Exchange(ref _disposed, 1) != 0)
            {
                return;
            }
            lock (_broker._gate)
            {
                _broker._subscriptions.Remove(this);
            }
        }
    }
}
