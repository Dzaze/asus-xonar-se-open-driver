using System.Text.Json.Nodes;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Client du contrat C4 (named pipe « XonarSE.Control.v1.<sessionId> »).
/// Cette interface est la seule porte d'entrée vers l'agent audio-control :
/// aucune vue n'écrit directement dans KS, CAPX, le registre ou le format
/// d'endpoint. L'implémentation réelle (E2-02) est remplacée, pour le
/// développement et les tests, par <see cref="FakeControlBroker"/>.
/// </summary>
public interface IControlClient
{
    /// <summary>Vrai si le transport est actuellement connecté à l'agent.</summary>
    bool IsConnected { get; }

    /// <summary>Levé lors d'une perte ou d'un rétablissement de connexion.</summary>
    event EventHandler<ConnectionChangedEventArgs>? ConnectionChanged;

    /// <summary>Notification d'état poussée après un abonnement réussi.</summary>
    event EventHandler<StateNotification>? StateNotification;

    /// <summary>Envoie une requête brute et retourne la réponse acquittée par l'agent.</summary>
    Task<ControlResponse> SendAsync(ControlRequest request, CancellationToken ct = default);

    /// <summary>Lit les capacités de l'agent (portée agent, sans endpointKey).</summary>
    Task<CapabilitiesResult> GetCapabilitiesAsync(CancellationToken ct = default);

    /// <summary>Lit l'état versionné d'un endpoint.</summary>
    Task<StateResult> GetStateAsync(string endpointKey, CancellationToken ct = default);

    /// <summary>
    /// Fait valider un document JSON brut par l'agent. Le profil est fourni
    /// comme objet JSON quelconque : c'est l'agent qui applique le schéma v1
    /// et renvoie l'ensemble des diagnostics.
    /// </summary>
    Task<ValidationResult> ValidateProfileAsync(string endpointKey, JsonNode rawProfile, CancellationToken ct = default);

    /// <summary>
    /// Applique un profil v1 valide. L'écriture exige la dernière révision connue ;
    /// le statut « Applied » n'est vrai qu'après acquittement de toutes les instances.
    /// </summary>
    Task<ControlResponse> ApplyProfileAsync(string endpointKey, ulong expectedRevision, ProfileDocument profile, CancellationToken ct = default);

    /// <summary>Demande la réparation du format partagé (capacité canRepairFormat d'E2-04).</summary>
    Task<ControlResponse> RequestFormatRepairAsync(string endpointKey, ulong expectedRevision, CancellationToken ct = default);

    /// <summary>
    /// S'abonne aux notifications d'état (rafraîchissement borné à UiRefreshMaxHz).
    /// Retourne un jeton jetable qui arrête l'abonnement.
    /// </summary>
    IDisposable SubscribeState(string endpointKey, uint maxRateHz);
}

/// <summary>Changement d'état de connexion du client C4.</summary>
public sealed class ConnectionChangedEventArgs : EventArgs
{
    public ConnectionChangedEventArgs(bool isConnected, string reason)
    {
        IsConnected = isConnected;
        Reason = reason;
    }

    /// <summary>Nouvel état de la connexion.</summary>
    public bool IsConnected { get; }

    /// <summary>Motif lisible (français, sans donnée sensible).</summary>
    public string Reason { get; }
}
