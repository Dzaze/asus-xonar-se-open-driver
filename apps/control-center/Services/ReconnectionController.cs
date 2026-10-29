using Xonar.ControlCenter.Services.Localization;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Reconnexion BORNÉE du client C4. À chaque perte de connexion, tente de se
/// reconnecter un nombre limité de fois avec un délai borné, puis abandonne
/// proprement : l'application reste utilisable en consultation. Aucune écriture
/// matérielle n'est tentée pendant la déconnexion.
/// </summary>
public sealed class ReconnectionController : IDisposable
{
    public const int MaxAttempts = 3;
    public static readonly TimeSpan DelayBetweenAttempts = TimeSpan.FromSeconds(1);

    private readonly IControlClient _client;
    private readonly Action<string> _report;
    private readonly TimeSpan _delayBetweenAttempts;
    private CancellationTokenSource? _cts;

    public ReconnectionController(IControlClient client, Action<string> report, TimeSpan? delayBetweenAttempts = null)
    {
        _client = client;
        _report = report;
        // Délai INJECTABLE entre tentatives (défaut = DelayBetweenAttempts). Permet
        // aux tests de dérouler la boucle bornée en quelques millisecondes, sans
        // attendre une horloge réelle d'une seconde par tentative.
        _delayBetweenAttempts = delayBetweenAttempts ?? DelayBetweenAttempts;
        _client.ConnectionChanged += OnConnectionChanged;
    }

    private async void OnConnectionChanged(object? sender, ConnectionChangedEventArgs e)
    {
        if (e.IsConnected)
        {
            _cts?.Cancel();
            _report(ErrorMessages.Reconnected);
            return;
        }

        _report(ErrorMessages.ConnectionLost);
        _cts?.Cancel();
        _cts = new CancellationTokenSource();
        await ReconnectLoopAsync(_cts.Token);
    }

    private async Task ReconnectLoopAsync(CancellationToken ct)
    {
        for (int attempt = 1; attempt <= MaxAttempts; attempt++)
        {
            if (ct.IsCancellationRequested)
            {
                return;
            }

            _report(ErrorMessages.ReconnectAttempt(attempt, MaxAttempts));

            try
            {
                await Task.Delay(_delayBetweenAttempts, ct);
            }
            catch (TaskCanceledException)
            {
                return;
            }

            if (_client.IsConnected)
            {
                // La reconnexion est assurée par le transport réel du client ; quand
                // elle réussit, le client émet ConnectionChanged -> OnConnectionChanged
                // rapporte déjà « reconnecté ». On ne le réémet donc PAS ici, sinon
                // double rapport (conf#3). Le code de production n'a AUCUNE connaissance
                // du faux agent : la boucle se contente d'attendre puis de vérifier
                // _client.IsConnected à chaque tentative (conf#7).
                return;
            }
        }

        _report(ErrorMessages.ReconnectFailed(MaxAttempts));
    }

    public void Dispose()
    {
        _client.ConnectionChanged -= OnConnectionChanged;
        _cts?.Cancel();
        _cts?.Dispose();
    }
}
