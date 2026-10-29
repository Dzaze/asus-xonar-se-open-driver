using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>Mêmes presets depuis l'accueil et la page Profils ; calibration conservée.</summary>
public static class ListeningPresets
{
    public static ProfileDocument Create(ProfileDocument current, int preset)
    {
        var profile = ProfileDefaults.Clone(current);
        profile.ListeningMode = ListeningMode.Surround51;
        profile.SourcePolicy = SourcePolicy.AutoDeclaredLayout;
        foreach(var channel in profile.Output.Eq) foreach(var band in channel) { band.Enabled = false; band.GainDb = 0; }
        var bass = profile.Output.BassManagement;
        bass.Enabled = preset != 0;
        bass.HighPassSatellites = true;
        bass.SumToLfe = true;
        bass.CrossoverHz = preset == 2 ? 100 : 80;
        void Eq(int channel, int band, double frequency, double q, double gain) {
            var target = profile.Output.Eq[channel][band];
            target.FrequencyHz = frequency;target.Q = q;target.GainDb = gain;target.Enabled = true;
        }
        if (preset == 1) Eq(2, 0, 900, .9, 2);
        if (preset == 2) {
            Eq(0, 0, 60, .8, 2.5);Eq(1, 0, 60, .8, 2.5);
            Eq(0, 1, 9000, .8, 1.5);Eq(1, 1, 9000, .8, 1.5);
        }
        return profile;
    }
}
