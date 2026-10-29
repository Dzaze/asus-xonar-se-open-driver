using System.Globalization;
using System.Text;
using Xonar.Contracts.V1;

namespace Xonar.ControlCenter.Services;

/// <summary>
/// Pont entre le cockpit et l'APO RÉEL : écrit les réglages EFX dans le fichier que
/// l'APO lit, %ProgramData%\XonarSE\apo-efx.cfg (format key=value, une paire par
/// ligne, '#' = commentaire). C'est le seul endroit qui matérialise un profil vers
/// l'APO ; aucune vue ne touche le registre, KS ou l'APO directement.
///
/// Clés de BASE lues par le parseur APO actuel (src/windows-apo/src/apo_settings.cpp) :
/// enabled, masterGainDb, limiterEnabled, limiterCeilingDb, bassEnabled, crossoverHz,
/// upmix, chGainDb_&lt;canal&gt;, mute&lt;canal&gt;.
/// Clés ÉTENDUES (le « tuyau » qui fait agir les réglages sur le DSP) : mode,
/// chDelayMs_&lt;canal&gt;, eq_&lt;canal&gt;_&lt;bande&gt;, limiterReleaseMs, bassHighPass,
/// bassSumToLfe, bassSumToLfeGainDb, upmixCenterDb / upmixSurroundDb / upmixLfeDb /
/// upmixLfeHz / upmixDeriveLfe. Le moteur dsp-core applique déjà ces réglages (eq_[],
/// delays_[], limiter.releaseMs, bassManagement.*, modes) ; le parseur APO les consomme
/// via son extension dédiée, et les clés inconnues d'un parseur plus ancien sont
/// ignorées sans risque (sa boucle saute toute clé non reconnue).
/// Suffixes canaux FL FR FC LFE BL BR SL SR, dans l'ordre de l'enum Channel. Les nombres
/// sont écrits en culture INVARIANTE : l'APO attend le point décimal (std::stof), jamais
/// la virgule.
///
/// L'APO lit ce fichier à Initialize puis le surveille hors callback audio. Un profil
/// est publié par une boîte SPSC et appliqué au bloc suivant, sans redémarrer le flux.
/// </summary>
public static class ApoEfxConfig
{
    // Ordre de l'enum xonar::dsp::Channel (index 0..7).
    private static readonly string[] ChannelSuffix = { "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR" };

    /// <summary>%ProgramData%\XonarSE\apo-efx.cfg</summary>
    public static string DefaultPath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "XonarSE", "apo-efx.cfg");

    /// <summary>Rend le contenu du .cfg pour un profil, sans écrire (utile aux tests).</summary>
    public static string Render(ProfileDocument profile, bool streamEffects = false)
    {
        OutputSettings o = profile.Output ?? new OutputSettings();
        CultureInfo ci = CultureInfo.InvariantCulture;
        StringBuilder sb = new();
        sb.Append("# Genere par le centre de controle Xonar SE — ne pas editer a la main.\n");
        sb.Append("enabled=1\n");
        bool auto = profile.SourcePolicy == SourcePolicy.AutoDeclaredLayout && profile.ListeningMode == ListeningMode.Surround51;
        sb.Append("sourcePolicy=").Append(auto ? "autoDeclaredLayout" : "preserve").Append('\n');
        sb.Append("streamUpmix=").Append(streamEffects && profile.ListeningMode == ListeningMode.Upmix ? '1' : '0').Append('\n');
        // Mode d'ecoute : clé PRIORITAIRE lue par l'APO. Mapping etage EFX (sortie 5.1) :
        //   5.1   -> Mode::PassThrough          upmix -> Mode::Upmix6to6
        //   2.1   -> Mode::TwoPointOne          2.0   -> Mode::DownmixStereo6to6 (nouveau)
        // Wire.ToWire rend EXACTEMENT 5.1 / upmix / 2.1 / 2.0 (table filaire du contrat C3).
        sb.Append("mode=").Append(Wire.ToWire(profile.ListeningMode)).Append('\n');
        // Repli de compat : un parseur APO ancien ne lit que 'upmix'. = 1 UNIQUEMENT en mode
        // upmix (l'ancien mapping « upmix ssi 5.1 » etait FAUX : 5.1 = passthrough, pas upmix).
        // L'APO recent prend 'mode' s'il est present, sinon il retombe sur 'upmix'.
        bool upmix = profile.ListeningMode == ListeningMode.Upmix;
        sb.Append("upmix=").Append(upmix ? '1' : '0').Append('\n');
        sb.Append("masterGainDb=").Append(o.MasterGainDb.ToString(ci)).Append('\n');
        if (o.Limiter is { } lim)
        {
            sb.Append("limiterEnabled=").Append(lim.Enabled ? '1' : '0').Append('\n');
            sb.Append("limiterCeilingDb=").Append(lim.CeilingDb.ToString(ci)).Append('\n');
            sb.Append("limiterReleaseMs=").Append(lim.ReleaseMs.ToString(ci)).Append('\n');
        }
        if (o.BassManagement is { } bm)
        {
            sb.Append("bassEnabled=").Append(bm.Enabled ? '1' : '0').Append('\n');
            sb.Append("crossoverHz=").Append(bm.CrossoverHz.ToString(ci)).Append('\n');
            sb.Append("bassHighPass=").Append(bm.HighPassSatellites ? '1' : '0').Append('\n');
            sb.Append("bassSumToLfe=").Append(bm.SumToLfe ? '1' : '0').Append('\n');
            sb.Append("bassSumToLfeGainDb=").Append(bm.SumToLfeGainDb.ToString(ci)).Append('\n');
        }
        // Sous-reglages de l'upmix 6->6 (Routing.Upmix), lus par l'APO quand mode=upmix.
        if (profile.Routing?.Upmix is { } um)
        {
            sb.Append("upmixCenterDb=").Append(um.CenterGainDb.ToString(ci)).Append('\n');
            sb.Append("upmixSurroundDb=").Append(um.SurroundGainDb.ToString(ci)).Append('\n');
            sb.Append("upmixLfeDb=").Append(um.LfeGainDb.ToString(ci)).Append('\n');
            sb.Append("upmixLfeHz=").Append(um.LfeLowPassHz.ToString(ci)).Append('\n');
            sb.Append("upmixDeriveLfe=").Append(um.DeriveLfe ? '1' : '0').Append('\n');
        }
        double[] gains = o.ChannelGainDb ?? new double[8];
        bool[] mutes = o.ChannelMute ?? new bool[8];
        double[] delays = o.ChannelDelayMs ?? new double[8];
        for (int i = 0; i < ChannelSuffix.Length && i < gains.Length; i++)
        {
            sb.Append("chGainDb_").Append(ChannelSuffix[i]).Append('=').Append(gains[i].ToString(ci)).Append('\n');
        }
        for (int i = 0; i < ChannelSuffix.Length && i < mutes.Length; i++)
        {
            sb.Append("mute").Append(ChannelSuffix[i]).Append('=').Append(mutes[i] ? '1' : '0').Append('\n');
        }
        // Delais par canal (ms, 0..50) : memes suffixes/ordre que chGainDb_.
        for (int i = 0; i < ChannelSuffix.Length && i < delays.Length; i++)
        {
            sb.Append("chDelayMs_").Append(ChannelSuffix[i]).Append('=').Append(delays[i].ToString(ci)).Append('\n');
        }
        // Egaliseur : UNE ligne par bande ACTIVE (Enabled && GainDb != 0), bande b = 0..9,
        // format "<freqHz>,<q>,<gainDb>". La seule presence de la ligne vaut activation.
        EqBand[][]? eq = o.Eq;
        for (int i = 0; eq is not null && i < ChannelSuffix.Length && i < eq.Length; i++)
        {
            EqBand[]? bands = eq[i];
            if (bands is null) { continue; }
            for (int b = 0; b < bands.Length; b++)
            {
                EqBand? band = bands[b];
                if (band is null || !band.Enabled || band.GainDb == 0.0) { continue; }
                sb.Append("eq_").Append(ChannelSuffix[i]).Append('_').Append(b.ToString(ci)).Append('=')
                  .Append(band.FrequencyHz.ToString(ci)).Append(',')
                  .Append(band.Q.ToString(ci)).Append(',')
                  .Append(band.GainDb.ToString(ci)).Append('\n');
            }
        }
        return sb.ToString();
    }

    /// <summary>
    /// Écrit le .cfg de façon atomique (fichier temporaire puis remplacement). Ne lève
    /// JAMAIS : retourne false + message en cas d'échec (ex. droits manquants sur
    /// %ProgramData%\XonarSE — l'installeur pose l'ACL « Utilisateurs : Modifier »).
    /// </summary>
    public static bool TryWrite(ProfileDocument profile, out string error, string? path = null, bool streamEffects = false)
    {
        error = "";
        try
        {
            path ??= DefaultPath;
            string dir = Path.GetDirectoryName(path)!;
            Directory.CreateDirectory(dir);
            string content = Render(profile, streamEffects);
            // Pas de nouveau jeton de fichier ni de relecture DSP si rien n'a changé.
            if (!File.Exists(path) || File.ReadAllText(path) != content) {
                string tmp = path + ".tmp";
                File.WriteAllText(tmp, content, new UTF8Encoding(false));
                File.Move(tmp, path, overwrite: true);
            }
            string saved = Path.ChangeExtension(path, ".profile.json");
            string json = System.Text.Json.JsonSerializer.Serialize(profile);
            if (!File.Exists(saved) || File.ReadAllText(saved) != json) {
                File.WriteAllText(saved + ".tmp", json, new UTF8Encoding(false));
                File.Move(saved + ".tmp", saved, overwrite: true);
            }
            return true;
        }
        catch (Exception ex)
        {
            error = ex.Message;
            return false;
        }
    }

    public static ProfileDocument? LoadLastProfile(string? path = null)
    {
        try {
            path ??= DefaultPath;
            string saved = Path.ChangeExtension(path, ".profile.json");
            if (File.Exists(saved)) {
                var stored = System.Text.Json.JsonSerializer.Deserialize<ProfileDocument>(File.ReadAllText(saved));
                if(stored is not null) {
                    var validation = ProfileValidator.Validate(ContractJson.Profile(stored));
                    if(validation.Valid) return validation.Normalized;
                }
            }
            if (!File.Exists(path)) return null;
            // Migration du premier prototype, qui ne conservait que le fichier plat.
            var profile = ProfileDefaults.CreateConservative("Derniers réglages");
            var o = profile.Output;
            foreach (string line in File.ReadLines(path)) {
                string[] pair = line.Split('=', 2);
                if (pair.Length != 2 || pair[0].StartsWith('#')) continue;
                string key = pair[0].Trim(), val = pair[1].Trim();
                if (key == "mode") {
                    profile.ListeningMode = val switch { "upmix" => ListeningMode.Upmix, "2.0" => ListeningMode.Stereo20, "2.1" => ListeningMode.TwoPointOne, _ => ListeningMode.Surround51 };
                    continue;
                }
                if(key == "sourcePolicy") {
                    profile.SourcePolicy = val == "autoDeclaredLayout" ? SourcePolicy.AutoDeclaredLayout : SourcePolicy.Preserve;
                    continue;
                }
                if(key.StartsWith("eq_", StringComparison.Ordinal)) {
                    var name = key.Split('_');var data = val.Split(',');
                    int channel = name.Length == 3 ? Array.IndexOf(ChannelSuffix, name[1]) : -1;
                    if(channel >= 0 && int.TryParse(name[2], out int band) && band >= 0 && band < 10 && data.Length == 3) {
                        var target = o.Eq[channel][band];
                        target.FrequencyHz = double.Parse(data[0], CultureInfo.InvariantCulture);
                        target.Q = double.Parse(data[1], CultureInfo.InvariantCulture);
                        target.GainDb = double.Parse(data[2], CultureInfo.InvariantCulture);target.Enabled = true;
                    }
                    continue;
                }
                if (!double.TryParse(val, NumberStyles.Float, CultureInfo.InvariantCulture, out double value) || !double.IsFinite(value)) continue;
                switch(key) {
                    case "masterGainDb": o.MasterGainDb = value; break;
                    case "limiterEnabled": o.Limiter.Enabled = value != 0; break;
                    case "limiterCeilingDb": o.Limiter.CeilingDb = value; break;
                    case "limiterReleaseMs": o.Limiter.ReleaseMs = value; break;
                    case "bassEnabled": o.BassManagement.Enabled = value != 0; break;
                    case "crossoverHz": o.BassManagement.CrossoverHz = value; break;
                    case "bassHighPass": o.BassManagement.HighPassSatellites = value != 0; break;
                    case "bassSumToLfe": o.BassManagement.SumToLfe = value != 0; break;
                    case "bassSumToLfeGainDb": o.BassManagement.SumToLfeGainDb = value; break;
                    case "upmixCenterDb": profile.Routing.Upmix.CenterGainDb = value; break;
                    case "upmixSurroundDb": profile.Routing.Upmix.SurroundGainDb = value; break;
                    case "upmixLfeDb": profile.Routing.Upmix.LfeGainDb = value; break;
                    case "upmixLfeHz": profile.Routing.Upmix.LfeLowPassHz = value; break;
                    case "upmixDeriveLfe": profile.Routing.Upmix.DeriveLfe = value != 0; break;
                }
                for (int channel = 0; channel < ChannelSuffix.Length; channel++) {
                    string suffix = ChannelSuffix[channel];
                    if(key == "chGainDb_" + suffix) o.ChannelGainDb[channel] = value;
                    if(key == "chDelayMs_" + suffix) o.ChannelDelayMs[channel] = value;
                    if(key == "mute" + suffix) o.ChannelMute[channel] = value != 0;
                }
            }
            return profile;
        } catch { return null; }
    }
}
