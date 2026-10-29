using System.Text.Json.Nodes;
using Xonar.Contracts.V1;
using Xonar.ControlCenter.Profiles;

namespace Xonar.ControlCenter.Services;

/// <summary>Contrôle local réel : matériel, profil persistant et acquittement audiodg.</summary>
public sealed class LiveApoBroker : IControlClient
{
    private readonly string _cfgPath;
    private readonly Func<LiveAudioDevice?> _readDevice;
    private readonly Func<DspLiveStatus?> _readStatus;
    private readonly SemaphoreSlim _applyGate = new(1, 1);
    private ProfileDocument _profile;
    private LiveAudioDevice? _device;
    private ulong _revision = 1;
    private long _lastWrittenFileTime;
    private int _lastPid;
    private long _lastPoll;
    private DspLiveStatus? _lastVerified;
    private readonly Func<long> _clock;

    public LiveApoBroker(string? cfgPath = null, Func<LiveAudioDevice?>? deviceReader = null, Func<DspLiveStatus?>? statusReader = null, Func<long>? monotonicClock = null)
    {
        _cfgPath = cfgPath ?? ApoEfxConfig.DefaultPath;
        _readDevice = deviceReader ?? LiveAudioDevice.Read;
        _clock = monotonicClock ?? (() => Environment.TickCount64);
        _readStatus = statusReader ?? (() => ReadVerifiedStatus(Path.Combine(Path.GetDirectoryName(_cfgPath)!, "apo-live-status.json")));
        _profile = ApoEfxConfig.LoadLastProfile(_cfgPath) ?? ProfileDefaults.CreateConservative();
        _device = _readDevice();
    }

    private DspLiveStatus? ReadVerifiedStatus(string path)
    {
        var status = DspLiveStatus.Read(path);
        if(status is null) return null;
        if(ReferenceEquals(status, _lastVerified)) return status;
        try { using var process = System.Diagnostics.Process.GetProcessById(status.Pid);
            if(!process.ProcessName.Equals("audiodg", StringComparison.OrdinalIgnoreCase)) return null;
            _lastVerified = status; return status;
        } catch { return null; }
    }

    public bool LastWriteOk { get; private set; } = true;
    public string LastWriteError { get; private set; } = "";
    public bool IsConnected => _device is not null;
    public LiveAudioDevice? Device => _device;
    private static bool CanProcess(LiveAudioDevice device) => device.OutputEffects && device.Format.ChannelCount == 6 &&
        device.Format.ChannelMask == 0x60f && device.Format.SampleFormat == SampleFormat.Float32;
    public DspLiveStatus? Telemetry => _readStatus();
    public event EventHandler<ConnectionChangedEventArgs>? ConnectionChanged;
    public event EventHandler<StateNotification>? StateNotification;

    // Sondage borné par l'hôte ; aucune écriture registre ou action sur les services.
    public bool RefreshHardware()
    {
        var previous = _device;
        long now = _clock();
        bool resumed = _lastPoll != 0 && now - _lastPoll > 5000;
        _lastPoll = now;
        _device = _readDevice();
        var status = _readStatus();
        bool newEngine = status is not null && _lastPid != 0 && status.Pid != _lastPid;
        if(status is not null) _lastPid = status.Pid;
        if((previous is null) != (_device is null))
            ConnectionChanged?.Invoke(this, new(_device is not null, _device is null ? "Carte Xonar indisponible." : "Carte Xonar retrouvée."));
        return _device is not null && (previous is null || previous.Id != _device.Id ||
            previous.Format.SampleRate != _device.Format.SampleRate || previous.Format.ChannelMask != _device.Format.ChannelMask ||
            previous.StreamEffects != _device.StreamEffects || previous.OutputEffects != _device.OutputEffects || resumed || newEngine);
    }

    public bool IsLastProfileApplied()
    {
        var status = _readStatus();
        return LastWriteOk && _device is not null && CanProcess(_device) && _lastWrittenFileTime != 0 && status?.ConfigFileTime == _lastWrittenFileTime &&
            status.Sources.All(s => s.ConfigFileTime == _lastWrittenFileTime);
    }

    public Task<CapabilitiesResult> GetCapabilitiesAsync(CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        var d = _device;
        return Task.FromResult(new CapabilitiesResult {
            ApiMajor = Constants.ApiMajor, ApiMinor = Constants.ApiMinor,
            Contracts = Enumerable.Range(1, 8).Select(i => new ContractVersionInfo { Id = "C" + i, Major = 1, Minor = 0 }).ToList(),
            Endpoints = d is null ? new() : new() { new() { EndpointKey = "MI_00", EndpointRole = EndpointRole.Speakers,
                Driver = d.Driver, PhysicalChannelMask = d.Format.ChannelMask,
                SupportedSampleRates = new() { d.Format.SampleRate }, Writable = CanProcess(d) } },
            Features = new() { AutoDeclaredLayout = d is not null && CanProcess(d) && d.StreamEffects,
                ChannelTest = d is not null && CanProcess(d), FormatRepair = false }
        });
    }

    internal AgentState BuildState()
    {
        var d = _device;
        var status = _readStatus();
        bool applied = IsLastProfileApplied();
        var instances = new List<ApoInstanceState>();
        if(status is not null) {
            instances.Add(new() { InstanceId = $"gfx-{status.Pid}-{status.Instance}", Stage = ApoStage.Efx,
                Epoch = (ulong)status.Pid, AppliedGeneration = (ulong)status.ConfigFileTime,
                InputFormat = new() { ChannelCount = status.Channels, ChannelMask = status.InputMask, SampleRate = status.SampleRate, SampleFormat = SampleFormat.Float32 },
                Bypass = new() { Kind = status.Enabled ? BypassKind.None : BypassKind.Observed, Reason = status.Enabled ? null : BypassReason.Other } });
            instances.AddRange(status.Sources.Select(s => new ApoInstanceState {
                InstanceId = $"lfx-{status.Pid}-{s.Instance}", Stage = ApoStage.Sfx, Epoch = (ulong)status.Pid,
                AppliedGeneration = (ulong)s.ConfigFileTime,
                InputFormat = new() { ChannelCount = s.InputChannels, ChannelMask = s.InputMask, SampleRate = s.SampleRate, SampleFormat = SampleFormat.Float32 },
                Bypass = new() { Kind = s.Enabled ? BypassKind.None : BypassKind.Observed, Reason = s.Enabled ? null : BypassReason.Other }
            }));
        }
        return new AgentState {
            EndpointKey = "MI_00", EndpointRole = EndpointRole.Speakers, Revision = _revision,
            Driver = d?.Driver ?? new() { Kind = DriverKind.Unknown, Version = "non détecté" }, NegotiatedFormat = d?.Format!,
            ProfileId = _profile.ProfileId, ListeningMode = _profile.ListeningMode, RequestedPolicy = _profile.SourcePolicy,
            ResolvedPolicy = status is null ? ResolvedPolicy.Conservative : status.Auto ? ResolvedPolicy.AutoDeclaredLayout :
                _profile.ListeningMode is ListeningMode.Stereo20 or ListeningMode.TwoPointOne ? ResolvedPolicy.Neutral : ResolvedPolicy.Preserve,
            DesiredGeneration = (ulong)_lastWrittenFileTime, CommittedGeneration = (ulong)_lastWrittenFileTime,
            PartiallyApplied = !applied, Instances = instances, Jack = new() { Reliability = JackReliability.Unknown },
            Meters = status is null ? null! : new() { AgeMs = (uint)Math.Clamp((DateTime.UtcNow - status.WrittenUtc).TotalMilliseconds, 0, uint.MaxValue),
                Stale = false, OutputPeakDbfs = status.OutputPeak.Take(6).Select(v => v > 0 ? 20 * Math.Log10(v) : -200).ToArray(),
                LimiterReductionDb = status.LimiterReductionDb }
        };
    }

    public Task<StateResult> GetStateAsync(string endpointKey, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        if(endpointKey != "MI_00") throw new ControlClientException(ErrorCode.EndpointUnavailable, "Endpoint inconnu.");
        return Task.FromResult(new StateResult { State = BuildState() });
    }

    public Task<ValidationResult> ValidateProfileAsync(string endpointKey, JsonNode rawProfile, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        if(endpointKey != "MI_00") throw new ControlClientException(ErrorCode.EndpointUnavailable, "Endpoint inconnu.");
        var result = ProfileValidator.Validate(rawProfile, _device?.Format.SampleRate);
        return Task.FromResult(new ValidationResult { Valid = result.Valid, Normalized = result.Normalized!, Diagnostics = result.Diagnostics });
    }

    public async Task<ControlResponse> ApplyProfileAsync(string endpointKey, ulong expectedRevision, ProfileDocument profile, CancellationToken ct = default)
    {
        await _applyGate.WaitAsync(ct);
        try {
            if(endpointKey != "MI_00" || _device is null || !CanProcess(_device)) return Error(ErrorCode.UnsupportedCapability, "Haut-parleurs Xonar, format 5.1 flottant ou rattachement APO indisponible. Vérifier le format Windows et réparer l’installation APO si nécessaire.");
            if(expectedRevision != _revision) return Error(ErrorCode.RevisionConflict, "La révision du profil a changé.");
            if(profile.SourcePolicy == SourcePolicy.AutoDeclaredLayout && !_device.StreamEffects) return Error(ErrorCode.UnsupportedCapability, "Auto nécessite les deux étages LFX/GFX.");
            var validation = await ValidateProfileAsync(endpointKey, ContractJson.Profile(profile), ct);
            if(!validation.Valid) return Error(ErrorCode.InvalidProfile, "Profil invalide : " + validation.Diagnostics.FirstOrDefault()?.Message);
            LastWriteOk = ApoEfxConfig.TryWrite(profile, out string error, _cfgPath, _device.StreamEffects);
            LastWriteError = error;
            if(!LastWriteOk) return Error(ErrorCode.AccessDenied, error);
            _profile = ProfileDefaults.Clone(profile);
            ++_revision;
            _lastWrittenFileTime = File.GetLastWriteTimeUtc(_cfgPath).ToFileTimeUtc();
            for(int attempt = 0; attempt < 16 && !IsLastProfileApplied(); ++attempt) await Task.Delay(50, ct);
            return new() { Status = IsLastProfileApplied() ? ResponseStatus.Applied : ResponseStatus.Committed,
                Revision = _revision, DesiredGeneration = (ulong)_lastWrittenFileTime, CommittedGeneration = (ulong)_lastWrittenFileTime };
        } finally { _applyGate.Release(); }
    }

    private ControlResponse Error(ErrorCode code, string message) => new() { Status = ResponseStatus.Error, Revision = _revision, Error = new() { Code = code, Message = message } };
    public Task<ControlResponse> RequestFormatRepairAsync(string endpointKey, ulong expectedRevision, CancellationToken ct = default) =>
        Task.FromResult(Error(ErrorCode.UnsupportedCapability, "La récupération ne modifie pas le format Windows."));

    public async Task<ControlResponse> SendAsync(ControlRequest request, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        ControlResponse Reject(ErrorCode code, string message) { var error = Error(code, message); error.RequestId = request.RequestId; return error; }
        if(request.ApiMajor != Constants.ApiMajor) return Reject(ErrorCode.UnsupportedCapability, "Version de contrat non prise en charge.");
        if(request.Command != Command.GetCapabilities && request.EndpointKey != "MI_00") return Reject(ErrorCode.EndpointUnavailable, "Endpoint inconnu.");
        ControlResponse response;
        switch(request.Command) {
            case Command.GetCapabilities: response = new() { Status = ResponseStatus.Ok, Result = await GetCapabilitiesAsync(ct) }; break;
            case Command.GetState: response = new() { Status = ResponseStatus.Ok, Result = await GetStateAsync(request.EndpointKey, ct) }; break;
            case Command.ValidateProfile when request.Payload is UncheckedProfilePayload p:
                response = new() { Status = ResponseStatus.Ok, Result = await ValidateProfileAsync(request.EndpointKey,
                    p.Profile as JsonNode ?? System.Text.Json.JsonSerializer.SerializeToNode(p.Profile)!, ct) }; break;
            case Command.ApplyProfile when request.Payload is ProfilePayload p && request.ExpectedRevision is ulong revision:
                response = await ApplyProfileAsync(request.EndpointKey, revision, p.Profile, ct); break;
            default: response = Error(ErrorCode.UnsupportedCapability, "Commande non disponible sur le contrôle local."); break;
        }
        response.RequestId = request.RequestId; response.Revision = _revision; return response;
    }

    public IDisposable SubscribeState(string endpointKey, uint maxRateHz)
    {
        ulong sequence = 0;
        return new System.Threading.Timer(_ => {
            try { StateNotification?.Invoke(this, new() { RequestId = "local", Sequence = ++sequence, State = BuildState() }); } catch { }
        }, null, TimeSpan.Zero, TimeSpan.FromMilliseconds(Math.Max(250, 1000 / Math.Clamp(maxRateHz, 1, 4))));
    }
}
