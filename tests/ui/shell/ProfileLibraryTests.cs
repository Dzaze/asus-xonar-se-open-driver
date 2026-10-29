using Xonar.ControlCenter.Services;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Tests;

public static class ProfileLibraryTests
{
    public static void RunAll()
    {
        Test.Run("Bibliothèque : sauvegarde, lecture, export/import et suppression réelle", RoundTrip);
        Test.Run("Bibliothèque : import invalide et traversée de chemin refusés", InvalidFiles);
        Test.Run("Presets : calibration conservée et paramètres distincts", PresetsPreserveCalibration);
    }

    private static void RoundTrip()
    {
        string dir = Path.Combine(Path.GetTempPath(), "xonar-library-" + Guid.NewGuid().ToString("N"));
        try {
            var library = new ProfileLibrary(dir);
            var profile = ProfileDefaults.CreateConservative("Mon 2.1");
            profile.ListeningMode = ListeningMode.TwoPointOne;
            profile.Output.ChannelDelayMs[6] = 4.25;
            library.Save(profile);
            var saved = library.List();
            Test.Equal(1, saved.Count, "Un seul profil sauvegardé.");
            Test.Equal(ListeningMode.TwoPointOne, saved[0].ListeningMode, "Le mode doit être conservé.");
            Test.Equal(4.25, saved[0].Output.ChannelDelayMs[6], "Le délai doit être conservé.");
            string exported = Path.Combine(dir, "export.txt");
            ProfileLibrary.Export(saved[0], exported);
            var imported = ProfileLibrary.Read(exported);
            Test.Equal(profile.ProfileId, imported.ProfileId, "JSON v1 interopérable.");
            library.Delete(profile.ProfileId);
            Test.Equal(0, library.List().Count, "Le profil doit être réellement supprimé.");
        } finally { if(Directory.Exists(dir)) Directory.Delete(dir, true); }
    }

    private static void InvalidFiles()
    {
        string dir = Path.Combine(Path.GetTempPath(), "xonar-library-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try {
            string path = Path.Combine(dir, "invalid.json");
            File.WriteAllText(path, "{\"schemaVersion\":1,\"schemaVersion\":1}");
            bool rejected = false;
            try { _ = ProfileLibrary.Read(path); } catch(FormatException) { rejected = true; }
            Test.Assert(rejected, "Les clés JSON dupliquées doivent être refusées.");
            rejected = false;
            try { new ProfileLibrary(dir).Delete("../../other"); } catch(FormatException) { rejected = true; }
            Test.Assert(rejected, "La suppression doit rester dans la bibliothèque.");
        } finally { Directory.Delete(dir, true); }
    }

    private static void PresetsPreserveCalibration()
    {
        var original = ProfileDefaults.CreateConservative();
        original.Output.MasterGainDb = -15;
        original.Output.ChannelGainDb[2] = -3;
        original.Output.ChannelDelayMs[6] = 5;
        var music = ListeningPresets.Create(original, 0);
        var cinema = ListeningPresets.Create(original, 1);
        var games = ListeningPresets.Create(original, 2);
        foreach(var preset in new[] { music, cinema, games }) {
            Test.Equal(-15d, preset.Output.MasterGainDb, "Le volume utilisateur doit être conservé.");
            Test.Equal(-3d, preset.Output.ChannelGainDb[2], "La calibration du centre doit être conservée.");
            Test.Equal(5d, preset.Output.ChannelDelayMs[6], "Le délai utilisateur doit être conservé.");
        }
        Test.Assert(!music.Output.BassManagement.Enabled && cinema.Output.BassManagement.Enabled, "La gestion des basses doit distinguer Musique et Cinéma.");
        Test.Equal(100d, games.Output.BassManagement.CrossoverHz, "Le crossover Jeux doit être appliqué.");
        Test.Assert(cinema.Output.Eq[2][0].Enabled, "Le réglage dialogue doit être actif.");
        Test.Assert(!original.Output.Eq[2][0].Enabled, "Construire un preset ne modifie pas le profil d'origine.");
    }
}
