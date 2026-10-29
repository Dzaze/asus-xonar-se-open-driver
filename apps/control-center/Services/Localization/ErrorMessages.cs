using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services.Localization;

/// <summary>
/// Traductions françaises des erreurs stables du contrat C4 et des erreurs
/// locales du client. Aucun message n'expose de donnée sensible (SID, chemin
/// système, contenu de tampon audio).
/// </summary>
public static class ErrorMessages
{
    public static string ForErrorCode(ErrorCode code)
    {
        return code switch
        {
            ErrorCode.InvalidProfile => "Le profil fourni est invalide : consultez le détail des champs en erreur.",
            ErrorCode.RevisionConflict => "Le profil a été modifié par ailleurs. Rechargez l'état puis réessayez.",
            ErrorCode.UnsupportedCapability => "Cette fonction n'est pas disponible sur l'endpoint sélectionné.",
            ErrorCode.EndpointUnavailable => "La carte Xonar SE n'est pas accessible pour le moment.",
            ErrorCode.Busy => "L'agent est occupé. Réessayez dans un instant.",
            ErrorCode.AccessDenied => "L'agent a refusé l'accès pour cette session.",
            ErrorCode.RestartRequired => "Un redémarrage du service audio est nécessaire avant d'appliquer ce changement.",
            ErrorCode.TimedOut => "L'agent n'a pas répondu dans le délai imparti.",
            _ => "Erreur inconnue de l'agent audio.",
        };
    }

    public static string ConnectionLost => "Connexion à l'agent audio perdue.";

    public static string ReconnectAttempt(int attempt, int maxAttempts) =>
        $"Tentative de reconnexion {attempt}/{maxAttempts}…";

    public static string ReconnectFailed(int maxAttempts) =>
        $"Impossible de joindre l'agent après {maxAttempts} tentatives. L'application reste utilisable en consultation.";

    public static string Reconnected => "Connexion à l'agent audio rétablie.";

    public static string NoCardNoAgent => "Aucune carte ni agent détecté : l'interface reste en consultation, sans aucune écriture matérielle.";

    public static string PayloadTooLarge => "Le document dépasse la taille maximale de 128 Kio.";
}
