using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Services.Localization;

namespace Xonar.ControlCenter.Profiles;

/// <summary>État d'application d'un brouillon de profil.</summary>
public enum ProfileEditorState
{
    Draft,
    Validating,
    Applying,
    Applied,
    PartiallyApplied,
    Error,
}

/// <summary>
/// Modèle d'édition de profil : brouillon local, validation par l'agent,
/// application coalescée (≤ 10 commandes/s) et annulation vers le dernier
/// document validé. Ne touche jamais au volume matériel ni à aucune API système.
/// </summary>
public sealed class ProfileEditorViewModel : IDisposable
{
    private readonly IControlClient _client;
    private readonly CommandCoalescer _coalescer = new(10);
    private readonly SemaphoreSlim _applyLock = new(1, 1);
    private volatile bool _applyDirty;
    private ulong _knownRevision;

    public ProfileEditorViewModel(IControlClient client, string endpointKey, ProfileDocument? initial = null)
    {
        _client = client;
        EndpointKey = endpointKey;
        Draft = initial is null ? ProfileDefaults.CreateConservative() : ProfileDefaults.Clone(initial);
        LastValidDocument = ProfileDefaults.Clone(Draft);
        _ = LoadRevisionAsync();
    }

    public string EndpointKey { get; }

    public ProfileDocument Draft { get; private set; }

    /// <summary>Dernier document validé ou appliqué ; restauré par l'annulation.</summary>
    public ProfileDocument LastValidDocument { get; private set; }

    public ProfileEditorState State { get; private set; } = ProfileEditorState.Draft;

    public string LastMessage { get; private set; } = "Brouillon local, rien d'appliqué.";

    /// <summary>Levé quand le brouillon, l'état ou le message change.</summary>
    public event Action? Changed;

    public IReadOnlyList<string> ValidationDiagnostics { get; private set; } = Array.Empty<string>();

    public void Dispose()
    {
        _coalescer.Dispose();
        _applyLock.Dispose();
    }

    // ---------------------------------------------------------------------------
    // Mutateurs (chaque modification relance une application coalescée)
    // ---------------------------------------------------------------------------

    public void SetListeningMode(ListeningMode mode)
    {
        Draft.ListeningMode = mode;
        OnDraftChanged();
    }

    public void LoadProfile(ProfileDocument profile)
    {
        Draft = ProfileDefaults.Clone(profile);
        OnDraftChanged();
    }

    public void RefreshLiveState()
    {
        if(State == ProfileEditorState.Applied && _client is LiveApoBroker live && !live.IsLastProfileApplied()) {
            State = ProfileEditorState.PartiallyApplied;
            LastMessage = "Profil enregistré ; en attente d’un flux DSP actif.";
            Changed?.Invoke();
        }
        if(State == ProfileEditorState.PartiallyApplied && _client is LiveApoBroker broker && broker.IsLastProfileApplied()) {
            State = ProfileEditorState.Applied;
            LastMessage = "Profil appliqué par le DSP audio.";
            Changed?.Invoke();
        }
    }

    public void SetSourcePolicy(SourcePolicy policy)
    {
        Draft.SourcePolicy = policy;
        OnDraftChanged();
    }

    public void SetMasterGainDb(double value)
    {
        Draft.Output.MasterGainDb = Clamp(value, OutputSettingsLimits.MasterGainDbMin, OutputSettingsLimits.MasterGainDbMax);
        OnDraftChanged();
    }

    public void SetChannelGainDb(int channel, double value)
    {
        EnsureChannel(channel);
        Draft.Output.ChannelGainDb[channel] = Clamp(value, OutputSettingsLimits.ChannelGainDbMin, OutputSettingsLimits.ChannelGainDbMax);
        OnDraftChanged();
    }

    public void SetChannelDelayMs(int channel, double value)
    {
        EnsureChannel(channel);
        Draft.Output.ChannelDelayMs[channel] = Clamp(value, OutputSettingsLimits.ChannelDelayMsMin, OutputSettingsLimits.ChannelDelayMsMax);
        OnDraftChanged();
    }

    public void SetChannelMute(int channel, bool value)
    {
        EnsureChannel(channel);
        Draft.Output.ChannelMute[channel] = value;
        OnDraftChanged();
    }

    public void SetEqEnabled(int channel, int band, bool value)
    {
        EnsureBand(channel, band);
        Draft.Output.Eq[channel][band].Enabled = value;
        OnDraftChanged();
    }

    public void SetEqFrequency(int channel, int band, double value)
    {
        EnsureBand(channel, band);
        Draft.Output.Eq[channel][band].FrequencyHz = Clamp(value, EqBandLimits.FrequencyHzMin, EqBandLimits.FrequencyHzMax);
        OnDraftChanged();
    }

    public void SetEqQ(int channel, int band, double value)
    {
        EnsureBand(channel, band);
        Draft.Output.Eq[channel][band].Q = Clamp(value, 0.0001, EqBandLimits.QMax);
        OnDraftChanged();
    }

    public void SetEqGain(int channel, int band, double value)
    {
        EnsureBand(channel, band);
        Draft.Output.Eq[channel][band].GainDb = Clamp(value, EqBandLimits.GainDbMin, EqBandLimits.GainDbMax);
        OnDraftChanged();
    }

    public void SetLimiterEnabled(bool value)
    {
        Draft.Output.Limiter.Enabled = value;
        OnDraftChanged();
    }

    public void SetLimiterCeilingDb(double value)
    {
        Draft.Output.Limiter.CeilingDb = Clamp(value, LimiterSettingsLimits.CeilingDbMin, LimiterSettingsLimits.CeilingDbMax);
        OnDraftChanged();
    }

    public void SetCrossoverHz(double value)
    {
        Draft.Output.BassManagement.CrossoverHz = Clamp(value, BassManagementSettingsLimits.CrossoverHzMin, BassManagementSettingsLimits.CrossoverHzMax);
        OnDraftChanged();
    }

    public void SetBassManagementEnabled(bool value)
    {
        Draft.Output.BassManagement.Enabled = value;
        OnDraftChanged();
    }

    public void SetHighPassSatellites(bool value)
    {
        Draft.Output.BassManagement.HighPassSatellites = value;
        OnDraftChanged();
    }

    public void SetSumToLfe(bool value)
    {
        Draft.Output.BassManagement.SumToLfe = value;
        OnDraftChanged();
    }

    /// <summary>Dosage du grave sommé vers le LFE (bass management).</summary>
    public void SetLfeGainDb(double value)
    {
        Draft.Output.BassManagement.SumToLfeGainDb = Clamp(value, BassManagementSettingsLimits.SumToLfeGainDbMin, BassManagementSettingsLimits.SumToLfeGainDbMax);
        OnDraftChanged();
    }

    public void RestoreConservativePreset()
    {
        Draft = ProfileDefaults.CreateConservative(Draft.Name ?? "Preset conservateur");
        OnDraftChanged();
    }

    // ---------------------------------------------------------------------------

    /// <summary>Validation explicite par l'agent (ne modifie rien).</summary>
    public async Task ValidateAsync()
    {
        State = ProfileEditorState.Validating;
        LastMessage = "Validation en cours…";
        Changed?.Invoke();

        try
        {
            var node = ContractJson.Profile(Draft);
            var result = await _client.ValidateProfileAsync(EndpointKey, node);
            ValidationDiagnostics = result.Diagnostics
                .Select(d => $"{d.Path} : {d.Message}")
                .ToList();
            if (result.Valid)
            {
                LastMessage = "Profil valide pour le DSP et l'endpoint sélectionné.";
            }
            else
            {
                LastMessage = "Le profil contient des erreurs (voir le détail).";
            }
        }
        catch (ControlClientException ex)
        {
            LastMessage = ex.Message;
        }

        State = ProfileEditorState.Draft;
        Changed?.Invoke();
    }

    /// <summary>Application immédiate (bouton « Appliquer »).</summary>
    public async Task ApplyNowAsync()
    {
        await ApplyCoreAsync();
    }

    /// <summary>Annulation : restaure le dernier document validé, jamais les volumes système.</summary>
    public void Cancel()
    {
        Draft = ProfileDefaults.Clone(LastValidDocument);
        State = ProfileEditorState.Draft;
        LastMessage = "Modifications annulées ; dernier document valide restauré. Volumes système non modifiés.";
        Changed?.Invoke();
    }

    // ---------------------------------------------------------------------------

    private void OnDraftChanged()
    {
        State = ProfileEditorState.Draft;
        LastMessage = "Brouillon modifié (application coalescée en cours)…";
        Changed?.Invoke();
        // Func<Task> (groupe de méthodes), jamais async void : le coalesceur peut ainsi
        // l'attendre et observer ses exceptions au lieu de terminer le processus (conf#0).
        _coalescer.Enqueue("apply", ApplyCoreAsync);
    }

    private async Task ApplyCoreAsync()
    {
        // Sérialisation dédiée, indépendante de State (qui n'est qu'un champ d'affichage
        // et était remis à Draft par OnDraftChanged, neutralisant l'ancien garde) : une
        // seule application en vol à la fois. Si le brouillon change pendant une
        // application, on repasse une seule fois avec le dernier brouillon (drapeau
        // dirty), au lieu de lancer deux applications concurrentes avec la même révision
        // -> RevisionConflict parasite et perte de la dernière modification (conf#4).
        _applyDirty = true;
        if (!await _applyLock.WaitAsync(0))
        {
            return; // une application est déjà en vol ; elle rejouera via le drapeau dirty.
        }

        try
        {
            while (_applyDirty)
            {
                _applyDirty = false;

                var snapshot = ProfileDefaults.Clone(Draft);
                State = ProfileEditorState.Applying;
                LastMessage = "Application en cours…";
                Changed?.Invoke();

                try
                {
                    var response = await _client.ApplyProfileAsync(EndpointKey, _knownRevision, snapshot);

                    switch (response.Status)
                    {
                        case ResponseStatus.Applied:
                            _knownRevision = response.Revision;
                            LastValidDocument = snapshot;
                            State = ProfileEditorState.Applied;
                            LastMessage = "Profil appliqué par le DSP audio.";
                            break;

                        case ResponseStatus.Committed:
                            _knownRevision = response.Revision;
                            LastValidDocument = snapshot;
                            State = ProfileEditorState.PartiallyApplied;
                            LastMessage = "Profil enregistré ; en attente de confirmation du DSP sur un flux audio actif.";
                            break;

                        case ResponseStatus.Error when response.Error is not null:
                            State = ProfileEditorState.Error;
                            LastMessage = ErrorMessages.ForErrorCode(response.Error.Code);
                            await RefreshRevisionAsync();
                            break;

                        default:
                            State = ProfileEditorState.Error;
                            LastMessage = "Réponse inattendue de l'agent ; aucun changement validé.";
                            break;
                    }
                }
                catch (ControlClientException ex)
                {
                    State = ProfileEditorState.Error;
                    LastMessage = ex.Message;
                }

                Changed?.Invoke();
            }
        }
        finally
        {
            _applyLock.Release();
        }
    }

    private async Task LoadRevisionAsync()
    {
        try
        {
            var state = await _client.GetStateAsync(EndpointKey);
            _knownRevision = state.State.Revision;
        }
        catch (ControlClientException)
        {
            _knownRevision = 0;
        }
    }

    private async Task RefreshRevisionAsync()
    {
        try
        {
            var state = await _client.GetStateAsync(EndpointKey);
            _knownRevision = state.State.Revision;
        }
        catch (ControlClientException)
        {
            // Conserver la révision connue : une erreur ne publie jamais d'état partiel.
        }
    }

    private static double Clamp(double value, double min, double max) =>
        value < min ? min : (value > max ? max : value);

    private static void EnsureChannel(int channel)
    {
        if (channel < 0 || channel >= Constants.MaxChannels)
        {
            throw new ArgumentOutOfRangeException(nameof(channel));
        }
    }

    private static void EnsureBand(int channel, int band)
    {
        EnsureChannel(channel);
        if (band < 0 || band >= Constants.EqBandsPerChannel)
        {
            throw new ArgumentOutOfRangeException(nameof(band));
        }
    }
}
