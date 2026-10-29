using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;

namespace Xonar.ControlCenter.Tests;

/// <summary>
/// Tests du pont cockpit -> APO : ApoEfxConfig (rendu/écriture de apo-efx.cfg) et
/// LiveApoBroker (écriture à ApplyProfile). Le format doit correspondre EXACTEMENT à
/// ce que lit src/windows-apo/src/apo_settings.cpp.
/// </summary>
public static class ApoEfxConfigTests
{
    private static LiveApoBroker CreateBroker(string path) => new(path, () => new("test-endpoint",
        new() { ChannelCount = 6, ChannelMask = 0x60f, SampleRate = 48000, SampleFormat = SampleFormat.Float32 },
        new() { Kind = DriverKind.Xonarse, Version = "test" }, true), () => null);
    public static void RunAll()
    {
        Test.Run("Pont APO : nombres en culture invariante (point décimal)", InvariantNumbers);
        Test.Run("Pont APO : mapping des gains par canal (FC index 2, SL index 6)", ChannelGainMapping);
        Test.Run("Pont APO : mapping des mutes par canal", MuteMapping);
        Test.Run("Pont APO : clés limiteur et bass management", LimiterAndBass);
        Test.Run("Pont APO : TryWrite écrit un fichier identique à Render", TryWriteMatchesRender);
        Test.Run("Pont APO : LiveApoBroker écrit le cfg à ApplyProfile", LiveBrokerWritesCfg);
        Test.Run("Pont APO : refus de révision sans modifier le DSP", RejectedRevisionDoesNotWrite);
        Test.Run("Pont APO : écriture impossible signalée sans faux succès", WriteFailureIsReported);
        Test.Run("Pont APO : écriture seule ne vaut pas acquittement audio", NoFakeAcknowledgement);
        Test.Run("Pont APO : modes et réglages conservés au redémarrage", SavedProfileRoundTrip);
        Test.Run("Auto : routage par flux et calibration de sortie unique", AutoRoutingConfig);
        Test.Run("Vumètres : cache, actualisation et trame corrompue", TelemetryCache);
    }

    private static ProfileDocument SampleProfile()
    {
        var profile = ProfileDefaults.CreateConservative();
        profile.Output.MasterGainDb = -6.5;
        profile.Output.ChannelGainDb = new double[] { 0, 0, 3, 0, 0, 0, -2, 0 };
        profile.Output.ChannelMute = new bool[] { false, false, false, false, false, false, true, false };
        profile.Output.BassManagement.Enabled = true;
        profile.Output.BassManagement.CrossoverHz = 90;
        return profile;
    }

    private static void InvariantNumbers()
    {
        string cfg = ApoEfxConfig.Render(SampleProfile());
        Test.Assert(cfg.Contains("masterGainDb=-6.5"), "master en point décimal attendu.");
        Test.Assert(!cfg.Contains("-6,5"), "aucune virgule décimale ne doit apparaître.");
        Test.Assert(cfg.Contains("enabled=1"), "la clé enabled doit être présente.");
    }

    private static void ChannelGainMapping()
    {
        string cfg = ApoEfxConfig.Render(SampleProfile());
        Test.Assert(cfg.Contains("chGainDb_FC=3"), "ChannelGainDb[2] -> chGainDb_FC.");
        Test.Assert(cfg.Contains("chGainDb_SL=-2"), "ChannelGainDb[6] -> chGainDb_SL.");
        Test.Assert(cfg.Contains("chGainDb_FL=0"), "ChannelGainDb[0] -> chGainDb_FL.");
    }

    private static void MuteMapping()
    {
        string cfg = ApoEfxConfig.Render(SampleProfile());
        Test.Assert(cfg.Contains("muteSL=1"), "ChannelMute[6] -> muteSL=1.");
        Test.Assert(cfg.Contains("muteFL=0"), "ChannelMute[0] -> muteFL=0.");
    }

    private static void LimiterAndBass()
    {
        string cfg = ApoEfxConfig.Render(SampleProfile());
        Test.Assert(cfg.Contains("limiterEnabled=1"), "limiterEnabled=1.");
        Test.Assert(cfg.Contains("limiterCeilingDb=-0.3"), "limiterCeilingDb=-0.3.");
        Test.Assert(cfg.Contains("bassEnabled=1"), "bassEnabled=1.");
        Test.Assert(cfg.Contains("crossoverHz=90"), "crossoverHz=90.");
    }

    private static void TryWriteMatchesRender()
    {
        string path = Path.Combine(Path.GetTempPath(), "xonar-test-" + Guid.NewGuid().ToString("N") + ".cfg");
        try
        {
            ProfileDocument p = SampleProfile();
            bool ok = ApoEfxConfig.TryWrite(p, out string err, path);
            Test.Assert(ok, "TryWrite doit réussir dans le dossier temporaire : " + err);
            Test.Assert(File.Exists(path), "le fichier doit exister après écriture.");
            Test.Equal(ApoEfxConfig.Render(p), File.ReadAllText(path), "contenu écrit == Render.");
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    private static void LiveBrokerWritesCfg()
    {
        string path = Path.Combine(Path.GetTempPath(), "xonar-test-" + Guid.NewGuid().ToString("N") + ".cfg");
        try
        {
            var broker = CreateBroker(path);
            // Le matériel est injecté ; l’écriture de configuration est réelle.
            ulong revision = broker.GetStateAsync("MI_00").GetAwaiter().GetResult().State.Revision;
            _ = broker.ApplyProfileAsync("MI_00", revision, SampleProfile()).GetAwaiter().GetResult();
            Test.Assert(broker.LastWriteOk, "LiveApoBroker doit avoir écrit le cfg : " + broker.LastWriteError);
            Test.Assert(File.Exists(path), "ApplyProfile doit produire le fichier cfg.");
            Test.Assert(File.ReadAllText(path).Contains("masterGainDb=-6.5"), "le cfg doit refléter le profil appliqué.");
        }
        finally
        {
            if (File.Exists(path)) File.Delete(path);
        }
    }

    private static void RejectedRevisionDoesNotWrite()
    {
        string dir = Path.Combine(Path.GetTempPath(), "xonar-control-" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(dir, "apo.cfg");
        try {
            var broker = CreateBroker(path);
            var response = broker.ApplyProfileAsync("MI_00", 0, SampleProfile()).GetAwaiter().GetResult();
            Test.Equal(ResponseStatus.Error, response.Status, "La révision doit être refusée.");
            Test.Assert(!File.Exists(path), "Un profil refusé ne doit jamais atteindre le DSP.");
        } finally { if(Directory.Exists(dir)) Directory.Delete(dir, true); }
    }

    private static void WriteFailureIsReported()
    {
        string blockingFile = Path.GetTempFileName();
        try {
            var broker = CreateBroker(Path.Combine(blockingFile, "apo.cfg"));
            ulong revision = broker.GetStateAsync("MI_00").GetAwaiter().GetResult().State.Revision;
            var response = broker.ApplyProfileAsync("MI_00", revision, SampleProfile()).GetAwaiter().GetResult();
            Test.Equal(ResponseStatus.Error, response.Status, "L'échec disque doit atteindre l'interface.");
            Test.Assert(!broker.LastWriteOk && broker.LastWriteError.Length > 0, "L'erreur doit être observable.");
        } finally { File.Delete(blockingFile); }
    }

    private static void NoFakeAcknowledgement()
    {
        string dir = Path.Combine(Path.GetTempPath(), "xonar-control-" + Guid.NewGuid().ToString("N"));
        try {
            var broker = CreateBroker(Path.Combine(dir, "apo.cfg"));
            ulong revision = broker.GetStateAsync("MI_00").GetAwaiter().GetResult().State.Revision;
            var response = broker.ApplyProfileAsync("MI_00", revision, SampleProfile()).GetAwaiter().GetResult();
            Test.Equal(ResponseStatus.Committed, response.Status, "Sans audiodg, le faux agent ne peut pas certifier Applied.");
        } finally { if(Directory.Exists(dir)) Directory.Delete(dir, true); }
    }

    private static void SavedProfileRoundTrip()
    {
        string dir = Path.Combine(Path.GetTempPath(), "xonar-control-" + Guid.NewGuid().ToString("N"));
        string path = Path.Combine(dir, "apo.cfg");
        try {
            foreach(var mode in new[] { ListeningMode.Surround51, ListeningMode.Upmix, ListeningMode.Stereo20, ListeningMode.TwoPointOne }) {
                var profile = SampleProfile();profile.ListeningMode = mode;
                profile.Output.ChannelDelayMs[6] = 7.5;
                Test.Assert(ApoEfxConfig.TryWrite(profile, out string error, path), error);
                var loaded = ApoEfxConfig.LoadLastProfile(path)!;
                Test.Equal(mode, loaded.ListeningMode, "Le mode doit survivre à un redémarrage.");
                Test.Equal(7.5, loaded.Output.ChannelDelayMs[6], "Le délai doit être conservé.");
                Test.Assert(File.ReadAllText(path).Contains("mode=" + Wire.ToWire(mode)), "La valeur filaire doit correspondre au mode affiché.");
                DateTime before = File.GetLastWriteTimeUtc(path);
                Thread.Sleep(20);
                Test.Assert(ApoEfxConfig.TryWrite(profile, out error, path), error);
                Test.Equal(before, File.GetLastWriteTimeUtc(path), "Un profil identique ne doit pas déclencher de relecture audio.");
            }
        } finally { if(Directory.Exists(dir)) Directory.Delete(dir, true); }
    }

    private static void AutoRoutingConfig()
    {
        var profile = ProfileDefaults.CreateConservative();
        profile.SourcePolicy = SourcePolicy.AutoDeclaredLayout;
        profile.Output.MasterGainDb = -6;
        string cfg = ApoEfxConfig.Render(profile, streamEffects: true);
        Test.Assert(cfg.Contains("sourcePolicy=autoDeclaredLayout\n") && cfg.Contains("mode=5.1\n"), "Auto doit conserver l'EFX en identité de routage.");
        Test.Assert(cfg.Contains("streamUpmix=0\n") && cfg.Contains("masterGainDb=-6\n"), "L'EFX applique la calibration après le routage automatique.");
        profile.SourcePolicy = SourcePolicy.Preserve;profile.ListeningMode = ListeningMode.Upmix;
        Test.Assert(ApoEfxConfig.Render(profile, true).Contains("streamUpmix=1\n"), "L'upmix manuel doit être délégué au LFX lorsqu'il est installé.");
        Test.Assert(ApoEfxConfig.Render(profile, false).Contains("streamUpmix=0\n"), "Le déploiement GFX seul doit conserver son ancien routage.");
    }

    private static void TelemetryCache()
    {
        string path = Path.Combine(Path.GetTempPath(), "xonar-status-" + Guid.NewGuid().ToString("N") + ".json");
        try {
            DspLiveStatus.ClearCache();
            File.WriteAllText(path, "{\"active\":true,\"configFileTime\":1,\"pid\":42,\"outputRms\":[0.01]}");
            for(int read = 0; read < 100; read++) Test.Equal(1L, DspLiveStatus.Read(path)!.ConfigFileTime, "La trame courante doit être disponible.");
            Test.Equal(1L, DspLiveStatus.DeserializationCount, "100 lectures de la même trame ne doivent faire qu'une désérialisation.");
            File.WriteAllText(path, "{\"active\":true,\"configFileTime\":2,\"pid\":42,\"outputRms\":[0.02]}");
            Thread.Sleep(120);
            Test.Equal(2L, DspLiveStatus.Read(path)!.ConfigFileTime, "La nouvelle trame doit remplacer le cache.");
            File.WriteAllText(path, "{");Thread.Sleep(120);
            Test.Equal(2L, DspLiveStatus.Read(path)!.ConfigFileTime, "Une écriture partielle ne doit pas devenir un nouvel acquittement.");
            File.SetLastWriteTimeUtc(path, DateTime.UtcNow.AddSeconds(-3));Thread.Sleep(120);
            Test.Assert(DspLiveStatus.Read(path) is null, "Les données périmées ne doivent pas animer les vumètres.");
        } finally { File.Delete(path);DspLiveStatus.ClearCache(); }
    }
}
