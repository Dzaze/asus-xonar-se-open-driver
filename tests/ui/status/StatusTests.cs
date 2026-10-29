using System.Reflection;
using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Status;

namespace Xonar.ControlCenter.Tests;

/// <summary>Tests E2-07 : santé audio, péremption, bypass, réparation, export expurgé.</summary>
public static class StatusTests
{
    public static void RunAll()
    {
        Test.Run("E2-07 : mesure > 2 s signalée comme périmée", StalenessEvaluated);
        Test.Run("E2-07 : notification retardée => périmée", DelayedNotificationSignalsStale);
        Test.Run("E2-07 : bouton réparation suit canRepairFormat", RepairFollowsCanRepairFormat);
        Test.Run("E2-07 : aucune LED « enceinte branchée » synthétique", NoSpeakerConnectedLed);
        Test.Run("E2-07 : export de diagnostics expurgé (sans clé d'endpoint)", ExportIsRedacted);
        Test.Run("E2-07 : file saturée n'augmente pas la charge audio (aucune écriture)", SaturatedQueueDoesNotWrite);
    }

    private static void StalenessEvaluated()
    {
        Test.Assert(StalenessEvaluator.IsStale(null, TimeSpan.Zero), "Absence de mesure => périmée.");
        Test.Assert(StalenessEvaluator.IsStale(new MeterState { AgeMs = 2001, Stale = false }, TimeSpan.Zero),
            "Age > 2 s => périmée.");
        Test.Assert(!StalenessEvaluator.IsStale(new MeterState { AgeMs = 100, Stale = false }, TimeSpan.FromMilliseconds(500)),
            "Mesure fraîche => non périmée.");
        Test.Assert(StalenessEvaluator.IsStale(new MeterState { AgeMs = 100, Stale = false }, TimeSpan.FromMilliseconds(2001)),
            "Notification retardée > 2 s => périmée.");
    }

    private static void DelayedNotificationSignalsStale()
    {
        var broker = new FakeControlBroker();
        var vm = new StatusViewModel(broker, "MI_00");
        vm.StartAsync().GetAwaiter().GetResult();
        Test.Assert(!vm.IsStale, "État initial synthétique : mesure fraîche.");

        var staleState = new AgentState
        {
            SchemaVersion = Constants.StateSchemaVersion,
            EndpointKey = "MI_00",
            EndpointRole = EndpointRole.Speakers,
            Revision = 0,
            Driver = new DriverInfo { Kind = DriverKind.Xonarse, Version = null, PackageSha256 = null },
            NegotiatedFormat = new NegotiatedFormat { SampleRate = 48000, ChannelMask = Constants.Mask51Side, ChannelCount = 6, SampleFormat = SampleFormat.Float32 },
            ProfileId = null,
            ListeningMode = null,
            RequestedPolicy = null,
            ResolvedPolicy = ResolvedPolicy.Conservative,
            DesiredGeneration = 0,
            CommittedGeneration = 0,
            PartiallyApplied = false,
            Instances = new List<ApoInstanceState>(),
            Jack = new JackState { Reliability = JackReliability.ForcedConnection, PublishedConnected = true },
            Meters = new MeterState { AgeMs = 3000, Stale = false, OutputPeakDbfs = new double[6] },
        };
        broker.SetState(staleState);
        broker.PublishStateNow("abonnement-test");
        Test.Assert(vm.IsStale, "Une notification portant une mesure âgée doit être périmée.");
    }

    private static void RepairFollowsCanRepairFormat()
    {
        var disabled = new FakeControlBroker(new FakeControlBrokerOptions { FeatureFormatRepair = false });
        var vmDisabled = new StatusViewModel(disabled, "MI_00");
        vmDisabled.StartAsync().GetAwaiter().GetResult();
        Test.Assert(!vmDisabled.CanRepairFormat, "canRepairFormat=false => bouton désactivé.");

        var enabled = new FakeControlBroker(new FakeControlBrokerOptions { FeatureFormatRepair = true });
        var vmEnabled = new StatusViewModel(enabled, "MI_00");
        vmEnabled.StartAsync().GetAwaiter().GetResult();
        Test.Assert(vmEnabled.CanRepairFormat, "canRepairFormat=true => bouton activé.");
    }

    private static void NoSpeakerConnectedLed()
    {
        var forbidden = new[] { "SpeakerConnected", "IsConnected", "SpeakerPresent" };
        foreach (var property in typeof(StatusViewModel).GetProperties(BindingFlags.Public | BindingFlags.Instance))
        {
            foreach (var word in forbidden)
            {
                Test.Assert(!property.Name.Contains(word, StringComparison.OrdinalIgnoreCase),
                    $"Aucune LED « enceinte branchée » attendue ; trouvée : {property.Name}");
            }
        }
    }

    private static void ExportIsRedacted()
    {
        var state = new AgentState
        {
            SchemaVersion = Constants.StateSchemaVersion,
            EndpointKey = "CLE-SECRETE-ENDPOINT",
            EndpointRole = EndpointRole.Speakers,
            Revision = 1,
            Driver = new DriverInfo { Kind = DriverKind.Xonarse, Version = "0.3.3.0", PackageSha256 = null },
            NegotiatedFormat = new NegotiatedFormat { SampleRate = 48000, ChannelMask = Constants.Mask51Side, ChannelCount = 6, SampleFormat = SampleFormat.Float32 },
            ProfileId = null,
            ListeningMode = null,
            RequestedPolicy = null,
            ResolvedPolicy = ResolvedPolicy.Conservative,
            DesiredGeneration = 1,
            CommittedGeneration = 1,
            PartiallyApplied = false,
            Instances = new List<ApoInstanceState>(),
            Jack = new JackState { Reliability = JackReliability.ForcedConnection, PublishedConnected = true },
            Meters = new MeterState { AgeMs = 50, Stale = false, OutputPeakDbfs = new double[] { -20, -20, -20, -20, -20, -20 } },
        };

        string export = DiagnosticsExporter.Export(state);
        Test.Assert(!export.Contains("CLE-SECRETE-ENDPOINT", StringComparison.Ordinal),
            "L'export ne doit pas contenir la clé d'endpoint complète.");
        Test.Assert(export.Contains("endpointKeyLength", StringComparison.Ordinal),
            "L'export doit conserver uniquement la longueur de la clé.");
        Test.Assert(!export.Contains("FriendlyName", StringComparison.OrdinalIgnoreCase),
            "Aucun nom convivial de périphérique ne doit être exporté.");
    }

    private static void SaturatedQueueDoesNotWrite()
    {
        // On vérifie le COMPORTEMENT réel via un stub qui COMPTE les écritures, au lieu
        // d'inspecter par réflexion les noms de méthodes du modèle. Une rafale de
        // notifications d'état (file saturée, télémétrie perdue) ne doit déclencher
        // AUCUNE écriture matérielle.
        var stub = new FakeStubClient("MI_00");
        var vm = new StatusViewModel(stub, "MI_00");
        vm.StartAsync().GetAwaiter().GetResult();
        Test.Equal(0, stub.WriteCount, "Le démarrage de la page État ne doit déclencher aucune écriture.");

        for (int i = 1; i <= 200; i++)
        {
            var state = FakeStubClient.MinimalState("MI_00");
            state.Meters.TelemetryLost = (ulong)i; // télémétrie perdue croissante : file saturée
            stub.PublishState(state);
        }

        Test.Assert(vm.Latest is not null && vm.Latest.Meters.TelemetryLost > 0,
            "La rafale de notifications doit bien avoir été reçue et traitée par le modèle.");
        Test.Equal(0, stub.WriteCount,
            "Une file saturée ne doit déclencher aucune écriture (aucune charge audio ajoutée).");
    }
}
