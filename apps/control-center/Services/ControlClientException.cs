using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Erreur de transport ou de protocole du client C4. Le message est toujours
/// localisé en français ; le code d'erreur stable du contrat est conservé
/// séparément pour les décisions de l'interface.
/// </summary>
public sealed class ControlClientException : Exception
{
    public ControlClientException(ErrorCode code, string message, Exception? inner = null)
        : base(message, inner)
    {
        Code = code;
    }

    public ControlClientException(string message, Exception? inner = null)
        : base(message, inner)
    {
        Code = null;
    }

    /// <summary>Code d'erreur stable C4, ou null pour une erreur de transport locale.</summary>
    public ErrorCode? Code { get; }
}
