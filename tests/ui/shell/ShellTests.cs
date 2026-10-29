using System.Reflection;
using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Services.Localization;
using Xonar.ControlCenter.Shell;

namespace Xonar.ControlCenter.Tests;

/// <summary>Tests E2-05 : socle, client C4, faux agent, reconnexion, robustesse.</summary>
public static class ShellTests
{
    public static void RunAll()
    {
        Test.Run("E2-05 : démarrage sans carte ni agent ne lève pas (perte de connexion)", StartsWithoutAgentDoesNotCrash);
        Test.Run("E2-05 : erreurs IPC traduites en français", IpcErrorsAreTranslated);
        Test.Run("E2-05 : reconnexion bornée (maximum 3 tentatives)", ReconnectionIsBounded);
        Test.Run("E2-05 : aucune surface noyau / test-signing / registre", NoKernelOrSigningSurface);
        Test.Run("E2-05 : refus des clés dupliquées", DuplicateKeysRejected);
        Test.Run("E2-05 : refus des documents de plus de 128 Kio", OversizedDocumentRejected);
        Test.Run("E2-05 : perte de connexion puis reconnexion du faux agent", DisconnectReconnectWorks);
    }

    private static void StartsWithoutAgentDoesNotCrash()
    {
        var broker = new FakeControlBroker { Scenario = FakeBrokerScenario.ConnectionLoss };
        var shell = new AppShellViewModel(broker);
        shell.InitializeAsync().GetAwaiter().GetResult();
        Test.Assert(!string.IsNullOrEmpty(shell.LastMessage), "Un message d'état doit être produit.");
        Test.Assert(!shell.CanWrite, "Sans agent, aucune écriture ne doit être possible.");
    }

    private static void IpcErrorsAreTranslated()
    {
        var broker = new FakeControlBroker { Scenario = FakeBrokerScenario.RevisionConflict };
        var profile = ProfileDefaults.CreateConservative();

        // ApplyProfileAsync RENVOIE la réponse : les erreurs de PROTOCOLE reviennent
        // en ResponseStatus.Error (seules les coupures de TRANSPORT lèvent, via
        // EnsureConnected). La traduction française passe par ErrorMessages.ForErrorCode
        // sur le code renvoyé — exactement ce que fait ProfileEditorViewModel.
        var response = broker.ApplyProfileAsync("MI_00", 999, profile).GetAwaiter().GetResult();

        Test.Equal(ResponseStatus.Error, response.Status, "Un conflit de révision doit revenir en statut Error.");
        Test.Assert(response.Error is not null, "La réponse d'erreur doit porter un ErrorInfo.");
        Test.Equal(ErrorCode.RevisionConflict, response.Error!.Code);
        Test.Equal(
            "Le profil a été modifié par ailleurs. Rechargez l'état puis réessayez.",
            ErrorMessages.ForErrorCode(response.Error.Code));
    }

    private static void ReconnectionIsBounded()
    {
        Test.Equal(3, ReconnectionController.MaxAttempts, "La reconnexion doit être bornée à 3 tentatives.");

        // Stub qui RESTE déconnecté : aucune reconnexion ne survient, la boucle doit
        // donc émettre exactement MaxAttempts tentatives puis abandonner proprement.
        // Délai court injecté (10 ms) pour dérouler la boucle sans attente réelle.
        // Les rapports arrivent depuis des continuations de pool de threads : on les
        // collecte donc sous verrou (accès concurrent à la liste).
        var stub = new FakeStubClient();
        var gate = new object();
        var reports = new List<string>();
        void Report(string message) { lock (gate) { reports.Add(message); } }

        using var controller = new ReconnectionController(stub, Report, TimeSpan.FromMilliseconds(10));

        stub.Disconnect();

        // Attendre la fin de la boucle bornée (émission de ReconnectFailed), avec un
        // plafond de sécurité pour ne jamais bloquer la suite des tests.
        string failed = ErrorMessages.ReconnectFailed(ReconnectionController.MaxAttempts);
        DateTime deadline = DateTime.UtcNow + TimeSpan.FromSeconds(2);
        while (DateTime.UtcNow < deadline)
        {
            lock (gate)
            {
                if (reports.Contains(failed))
                {
                    break;
                }
            }
            Thread.Sleep(5);
        }

        List<string> snapshot;
        lock (gate)
        {
            snapshot = new List<string>(reports);
        }

        // Séquence attendue, dans l'ordre : perte, puis tentatives 1/3..3/3, puis abandon.
        Test.Equal(ReconnectionController.MaxAttempts + 2, snapshot.Count,
            "Exactement une perte + MaxAttempts tentatives + un abandon doivent être signalés.");
        Test.Equal(ErrorMessages.ConnectionLost, snapshot[0],
            "La perte de connexion doit être signalée immédiatement.");
        for (int attempt = 1; attempt <= ReconnectionController.MaxAttempts; attempt++)
        {
            Test.Equal(
                ErrorMessages.ReconnectAttempt(attempt, ReconnectionController.MaxAttempts),
                snapshot[attempt],
                $"Le message {attempt} doit être la tentative {attempt}/{ReconnectionController.MaxAttempts}.");
        }
        Test.Equal(failed, snapshot[^1],
            "Après MaxAttempts tentatives sans reconnexion, l'abandon doit être signalé.");
        Test.Assert(!snapshot.Contains(ErrorMessages.Reconnected),
            "Aucune reconnexion ne doit être signalée puisque le stub reste déconnecté.");
    }

    private static void NoKernelOrSigningSurface()
    {
        var forbidden = new[] { "kernel", "sign", "registry", "testSigning", "driver" };
        foreach (var method in typeof(IControlClient).GetMethods(BindingFlags.Public | BindingFlags.Instance))
        {
            var lower = method.Name.ToLowerInvariant();
            foreach (var word in forbidden)
            {
                Test.Assert(!lower.Contains(word),
                    $"Aucune surface noyau/signature attendue, trouvée : {method.Name}");
            }
        }
    }

    private static void DuplicateKeysRejected()
    {
        const string json = "{\"schemaVersion\":1,\"schemaVersion\":1}";
        var result = JsonStrict.Parse(json);
        Test.Assert(!result.Ok, "Les clés dupliquées doivent être refusées.");
        Test.Equal("json.duplicateKey", result.DiagnosticCode);
    }

    private static void OversizedDocumentRejected()
    {
        string json = "{\"padding\":\"" + new string('x', (int)Constants.MaxDocumentBytes) + "\"}";
        var result = JsonStrict.Parse(json);
        Test.Assert(!result.Ok, "Un document > 128 Kio doit être refusé.");
        Test.Equal("json.tooLarge", result.DiagnosticCode);
    }

    private static void DisconnectReconnectWorks()
    {
        var broker = new FakeControlBroker();
        broker.Disconnect();
        Test.Throws<ControlClientException>(() =>
            broker.GetCapabilitiesAsync().GetAwaiter().GetResult());

        broker.Reconnect();
        var capabilities = broker.GetCapabilitiesAsync().GetAwaiter().GetResult();
        Test.Equal(8, capabilities.Contracts.Count, "GetCapabilities doit lister les 8 contrats.");
    }
}
