using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Services.Localization;

namespace Xonar.ControlCenter.Status;

/// <summary>
/// Modèle de la page État : abonnement borné (≤ 20 Hz), détection de péremption
/// (> 2 s), format déclaré, générations par instance, motif de bypass observé ou
/// inconnu, et bouton de réparation piloté par canRepairFormat (E2-04).
/// </summary>
public sealed class StatusViewModel : IDisposable
{
    private readonly IControlClient _client;
    private readonly string _endpointKey;
    private IDisposable? _subscription;
    private DateTimeOffset _lastNotificationUtc = DateTimeOffset.UtcNow;

    public StatusViewModel(IControlClient client, string endpointKey)
    {
        _client = client;
        _endpointKey = endpointKey;
        _client.StateNotification += OnStateNotification;
    }

    public AgentState? Latest { get; private set; }

    public bool CanRepairFormat { get; private set; }

    public bool IsStale => StalenessEvaluator.IsStale(
        Latest?.Meters,
        DateTimeOffset.UtcNow - _lastNotificationUtc);

    public bool HasFormatDrift => StalenessEvaluator.HasFormatDrift(Latest?.NegotiatedFormat);

    /// <summary>Dernière erreur lisible (français).</summary>
    public string LastError { get; private set; } = string.Empty;

    /// <summary>Levé quand l'état affiché change.</summary>
    public event Action? Changed;

    public async Task StartAsync()
    {
        try
        {
            var capabilities = await _client.GetCapabilitiesAsync();
            CanRepairFormat = capabilities.Features.FormatRepair;

            var state = await _client.GetStateAsync(_endpointKey);
            Latest = state.State;
            _lastNotificationUtc = DateTimeOffset.UtcNow;

            _subscription = _client.SubscribeState(_endpointKey, Constants.UiRefreshMaxHz);
        }
        catch (ControlClientException ex)
        {
            LastError = ex.Message;
        }
        Changed?.Invoke();
    }

    /// <summary>Appelée par la page pour actualiser l'affichage de la péremption.</summary>
    public void RefreshStaleness()
    {
        Changed?.Invoke();
    }

    /// <summary>
    /// Demande la réparation du format partagé. N'est possible que si
    /// canRepairFormat est vrai ; la réparation n'est jamais déclenchée
    /// automatiquement par une notification.
    /// </summary>
    public async Task RequestRepairAsync()
    {
        var state = Latest;
        if (state is null || !CanRepairFormat)
        {
            return;
        }

        try
        {
            var response = await _client.RequestFormatRepairAsync(_endpointKey, state.Revision);
            if (response.Status == ResponseStatus.Error && response.Error is not null)
            {
                LastError = ErrorMessages.ForErrorCode(response.Error.Code);
            }
            else
            {
                LastError = string.Empty;
            }
        }
        catch (ControlClientException ex)
        {
            LastError = ex.Message;
        }

        Changed?.Invoke();
    }

    public void Stop()
    {
        _subscription?.Dispose();
        _subscription = null;
    }

    public void Dispose()
    {
        Stop();
        _client.StateNotification -= OnStateNotification;
    }

    private void OnStateNotification(object? sender, StateNotification notification)
    {
        if (notification.State.EndpointKey != _endpointKey)
        {
            return;
        }
        Latest = notification.State;
        _lastNotificationUtc = DateTimeOffset.UtcNow;
        Changed?.Invoke();
    }
}
