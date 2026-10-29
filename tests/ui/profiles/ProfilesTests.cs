using Xonar.Contracts.V1;
using Xonar.ControlCenter.Profiles;
using Xonar.ControlCenter.Services;

namespace Xonar.ControlCenter.Tests;

/// <summary>Tests E2-06 : édition des profils, calibration, validation, annulation.</summary>
public static class ProfilesTests
{
    public static void RunAll()
    {
        Test.Run("E2-06 : les quatre modes d'écoute valident", FourModesValidate);
        Test.Run("E2-06 : 80 bandes d'EQ indexées (8×10) et valeurs conservées", EightyBandsIndexed);
        Test.Run("E2-06 : bornes, Nyquist et délais refusés", BoundsNyquistDelayRejected);
        Test.Run("E2-06 : « appliqué » seulement après acquittement", AppliedOnlyAfterAck);
        Test.Run("E2-06 : annulation restaure le dernier document valide", CancelRestoresLastValid);
        Test.Run("E2-06 : coalesceur ≤ 10 commandes/s (même clé coalescée)", CoalescerCoalesces);
    }

    private static void FourModesValidate()
    {
        foreach (var mode in Wire.AllListeningMode)
        {
            var profile = ProfileDefaults.CreateConservative();
            profile.ListeningMode = mode;
            var node = ContractJson.Profile(profile);
            var outcome = ProfileValidator.Validate(node, 48000);
            Test.Assert(outcome.Valid, $"Le mode {Wire.ToWire(mode)} doit valider : {outcome.Diagnostics.FirstOrDefault()?.Message}");
        }
    }

    private static void EightyBandsIndexed()
    {
        var profile = ProfileDefaults.CreateConservative();
        for (int ch = 0; ch < Constants.MaxChannels; ch++)
        {
            for (int band = 0; band < Constants.EqBandsPerChannel; band++)
            {
                profile.Output.Eq[ch][band] = new EqBand
                {
                    Enabled = band % 2 == 0,
                    FrequencyHz = 100 + ch * 1000 + band * 10,
                    Q = 0.5 + band * 0.25,
                    GainDb = -12 + ch + band,
                };
            }
        }

        var node = ContractJson.Profile(profile);
        var outcome = ProfileValidator.Validate(node, 48000);
        Test.Assert(outcome.Valid, "Le profil 80 bandes doit valider.");
        Test.Assert(outcome.Normalized is not null, "Un document normalisé doit être produit.");

        for (int ch = 0; ch < Constants.MaxChannels; ch++)
        {
            for (int band = 0; band < Constants.EqBandsPerChannel; band++)
            {
                var src = profile.Output.Eq[ch][band];
                var dst = outcome.Normalized!.Output.Eq[ch][band];
                Test.Assert(src.Enabled == dst.Enabled, $"État enabled eq[{ch}][{band}] non conservé.");
                Test.Assert(Math.Abs(src.FrequencyHz - dst.FrequencyHz) < 1e-9, $"Fréquence eq[{ch}][{band}] non conservée.");
                Test.Assert(Math.Abs(src.Q - dst.Q) < 1e-9, $"Q eq[{ch}][{band}] non conservé.");
                Test.Assert(Math.Abs(src.GainDb - dst.GainDb) < 1e-9, $"Gain eq[{ch}][{band}] non conservé.");
            }
        }
    }

    private static void BoundsNyquistDelayRejected()
    {
        var outOfRange = ProfileDefaults.CreateConservative();
        outOfRange.Output.MasterGainDb = 100;
        Test.Assert(!ProfileValidator.Validate(ContractJson.Profile(outOfRange), 48000).Valid,
            "masterGainDb hors bornes doit être refusé.");

        // Délai : channelDelayMs=60 serait pris par la BORNE de tableau (50) avant la
        // règle sémantique. On reste SOUS la borne (40 <= 50) mais au-dessus d'un
        // maxDelayMs réduit (30) pour exercer RÉELLEMENT semantic.delayExceedsMax.
        var delay = ProfileDefaults.CreateConservative();
        delay.Output.MaxDelayMs = 30;
        delay.Output.ChannelDelayMs[0] = 40;
        var delayOutcome = ProfileValidator.Validate(ContractJson.Profile(delay), 48000);
        Test.Assert(!delayOutcome.Valid, "Un délai supérieur à maxDelayMs doit être refusé.");
        Test.Assert(delayOutcome.Diagnostics.Any(d => d.Code == "semantic.delayExceedsMax"),
            "Le refus doit venir de la règle sémantique delayExceedsMax, pas d'une borne de tableau.");

        // Nyquist : à 48 kHz, aucune fréquence admissible par la borne EQ (<= 20000) ne
        // dépasse Nyquist (24000). On force donc une fréquence d'échantillonnage basse
        // (22050 -> Nyquist 11025) avec une fréquence SOUS la borne (15000 <= 20000) mais
        // au-dessus de Nyquist, pour exercer RÉELLEMENT la règle semantic.nyquist.
        var nyquist = ProfileDefaults.CreateConservative();
        nyquist.Output.Eq[0][0].FrequencyHz = 15000;
        var nyquistOutcome = ProfileValidator.Validate(ContractJson.Profile(nyquist), 22050);
        Test.Assert(!nyquistOutcome.Valid, "Une fréquence d'EQ au-dessus de Nyquist doit être refusée.");
        Test.Assert(nyquistOutcome.Diagnostics.Any(d => d.Code == "semantic.nyquist"),
            "Le refus doit venir de la règle Nyquist (semantic.nyquist), pas d'une borne.");
    }

    private static void AppliedOnlyAfterAck()
    {
        var normalBroker = new FakeControlBroker { Scenario = FakeBrokerScenario.Normal };
        using var normalEditor = new ProfileEditorViewModel(normalBroker, "MI_00");
        normalEditor.ApplyNowAsync().GetAwaiter().GetResult();
        Test.Equal(ProfileEditorState.Applied, normalEditor.State,
            "En scénario normal, l'état doit être Applied (acquittement reçu).");

        var partialBroker = new FakeControlBroker { Scenario = FakeBrokerScenario.PartialApply };
        using var partialEditor = new ProfileEditorViewModel(partialBroker, "MI_00");
        partialEditor.ApplyNowAsync().GetAwaiter().GetResult();
        Test.Equal(ProfileEditorState.PartiallyApplied, partialEditor.State,
            "En application partielle, l'état ne doit PAS être Applied.");
    }

    private static void CancelRestoresLastValid()
    {
        var broker = new FakeControlBroker { Scenario = FakeBrokerScenario.Normal };
        using var editor = new ProfileEditorViewModel(broker, "MI_00");
        editor.ApplyNowAsync().GetAwaiter().GetResult();
        Test.Equal(ProfileEditorState.Applied, editor.State);

        editor.SetMasterGainDb(5);
        editor.Cancel();

        Test.Equal(0.0, editor.Draft.Output.MasterGainDb, "L'annulation doit restaurer le dernier document valide.");
    }

    private static void CoalescerCoalesces()
    {
        using var coalescer = new CommandCoalescer(10);
        int executed = 0;
        for (int i = 0; i < 50; i++)
        {
            coalescer.Enqueue("key", () => { executed++; return Task.CompletedTask; });
        }
        Thread.Sleep(250);
        Test.Equal(1, executed, "50 modifications de la même clé doivent produire une seule commande.");
        Test.Equal(100, coalescer.PeriodMs, "À 10 commandes/s, la période doit être de 100 ms.");
    }
}
