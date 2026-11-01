using Xonar.Contracts.V1;
using Xonar.ControlCenter.Profiles;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Services.Localization;

namespace Xonar.ControlCenter.Shell;

/// <summary>
/// Modèle du shell : état de connexion, dernier message utilisateur (français)
/// et accès au client C4. Ne contient aucune logique audio ni écriture système.
/// </summary>
public sealed class AppShellViewModel
{
    private readonly ReconnectionController _reconnection;
    private string _lastMessage = string.Empty;

    public AppShellViewModel(IControlClient client)
    {
        Client = client;
        _reconnection = new ReconnectionController(client, message => LastMessage = message);
        // Éditeur créé dès la construction (toujours disponible pour les pages).
        // Endpoint par défaut « MI_00 » = celui du faux agent / de la Xonar ; aligné
        // sur SelectedEndpointKey une fois les capacités obtenues (même valeur en dev).
        Editor = new ProfileEditorViewModel(client, "MI_00",
            client is LiveApoBroker ? ApoEfxConfig.LoadLastProfile() : null);
        LastMessage = ErrorMessages.NoCardNoAgent;
    }

    public IControlClient Client { get; }

    /// <summary>Éditeur de profil partagé entre toutes les pages (toujours disponible).</summary>
    public ProfileEditorViewModel Editor { get; }

    /// <summary>Levé quand <see cref="LastMessage"/> change (mise à jour de la bannière).</summary>
    public event Action<string>? MessageChanged;

    /// <summary>Dernier message d'état affiché dans la bannière du shell.</summary>
    public string LastMessage
    {
        get => _lastMessage;
        private set
        {
            if (_lastMessage == value)
            {
                return;
            }
            _lastMessage = value;
            MessageChanged?.Invoke(value);
        }
    }

    public bool IsConnected => Client.IsConnected;

    /// <summary>Vrai si une écriture est actuellement possible : agent connecté ET
    /// endpoint sélectionné déclaré inscriptible (Writable) par GetCapabilities.
    /// Sans capacités connues (pas d'agent), aucune écriture n'est possible.</summary>
    public bool CanWrite => IsConnected
        && Capabilities?.Endpoints.FirstOrDefault(e => e.EndpointKey == SelectedEndpointKey)?.Writable == true;

    /// <summary>Capacités connues (null tant que GetCapabilities n'a pas réussi).</summary>
    public CapabilitiesResult? Capabilities { get; private set; }

    public string? SelectedEndpointKey { get; private set; }

    public event Action? CapabilitiesChanged;

    public bool CanSelectSourcePolicy(SourcePolicy policy) => Client is not LiveApoBroker ||
        (CanWrite && policy != SourcePolicy.ForceStereoFrontPair &&
         (policy != SourcePolicy.AutoDeclaredLayout || Capabilities?.Features.AutoDeclaredLayout == true));

    public async Task InitializeAsync()
    {
        try
        {
            Capabilities = await Client.GetCapabilitiesAsync();
            SelectedEndpointKey = Capabilities.Endpoints.FirstOrDefault()?.EndpointKey;
            LastMessage = SelectedEndpointKey is null
                ? ErrorMessages.NoCardNoAgent
                : Client is LiveApoBroker && !CanWrite ? "Traitement APO indisponible : voir Diagnostic pour vérifier le format ou réparer l’installation."
                : "Haut-parleurs Xonar SE détectés.";
        }
        catch (ControlClientException ex)
        {
            LastMessage = ex.Message;
        }
        catch (Exception)
        {
            LastMessage = ErrorMessages.NoCardNoAgent;
        }
        finally
        {
            CapabilitiesChanged?.Invoke();
        }
    }
}
