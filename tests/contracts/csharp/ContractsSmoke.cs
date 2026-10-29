// Test de fumée des DTO C# générés (E0-02).
// Volontairement en C# 5 : compilé par le csc du .NET Framework présent sous
// Windows, sans SDK .NET. Vérifie les tables filaires, les GUID, les attributs
// et, par réflexion, que les défauts C# égalent la normalisation Python d'une
// fixture versionnée (noms JSON camelCase ↔ propriétés PascalCase compris).
namespace Xonar.Contracts.V1.Tests
{
    using System;
    using System.Collections;
    using System.Collections.Generic;
    using System.IO;
    using System.Reflection;
    using System.Text;
    using System.Web.Script.Serialization;
    using Xonar.Contracts.V1;

    internal static class Program
    {
        private static int checks;
        private static int failures;

        private static void Check(bool ok, string label)
        {
            checks++;
            if (!ok)
            {
                failures++;
                Console.WriteLine("ECHEC : " + label);
            }
        }

        private static void RoundTrip<T>(IEnumerable<T> all, string label)
        {
            MethodInfo toWire = typeof(Wire).GetMethod("ToWire", new Type[] { typeof(T) });
            MethodInfo tryFrom = typeof(Wire).GetMethod("TryFromWire", new Type[] { typeof(string), typeof(T).MakeByRefType() });
            foreach (T v in all)
            {
                string wire = (string)toWire.Invoke(null, new object[] { v });
                object[] args = new object[] { wire, null };
                bool ok = (bool)tryFrom.Invoke(null, args);
                Check(ok && v.Equals((T)args[1]), label + " " + wire);
            }
            object[] bad = new object[] { "valeur-inconnue", null };
            Check(!(bool)tryFrom.Invoke(null, bad), label + " inconnu refusé");
        }

        private static string Pascal(string name)
        {
            return char.ToUpperInvariant(name[0]) + name.Substring(1);
        }

        private static void Compare(object json, object dto, string path)
        {
            IDictionary<string, object> obj = json as IDictionary<string, object>;
            if (obj != null)
            {
                foreach (KeyValuePair<string, object> kv in obj)
                {
                    PropertyInfo prop = dto.GetType().GetProperty(Pascal(kv.Key));
                    Check(prop != null, path + "/" + kv.Key + " : propriété C# absente");
                    if (prop != null)
                    {
                        Compare(kv.Value, prop.GetValue(dto, null), path + "/" + kv.Key);
                    }
                }
                return;
            }
            IList list = json as IList;
            if (list != null)
            {
                Array arr = dto as Array;
                Check(arr != null && arr.Length == list.Count, path + " : longueur");
                if (arr != null)
                {
                    for (int i = 0; i < Math.Min(arr.Length, list.Count); ++i)
                    {
                        Compare(list[i], arr.GetValue(i), path + "/" + i);
                    }
                }
                return;
            }
            if (json is bool)
            {
                Check(dto is bool && (bool)json == (bool)dto, path);
                return;
            }
            string text = json as string;
            if (text != null)
            {
                if (dto != null && dto.GetType().IsEnum)
                {
                    MethodInfo toWire = typeof(Wire).GetMethod("ToWire", new Type[] { dto.GetType() });
                    Check(text == (string)toWire.Invoke(null, new object[] { dto }), path);
                }
                else
                {
                    Check(text == dto as string, path);
                }
                return;
            }
            Check(dto != null && Convert.ToDouble(json) == Convert.ToDouble(dto), path + " : " + json + " ≠ " + dto);
        }

        private static int Main(string[] args)
        {
            RoundTrip(Wire.AllChannel, "Channel");
            RoundTrip(Wire.AllListeningMode, "ListeningMode");
            RoundTrip(Wire.AllSourcePolicy, "SourcePolicy");
            RoundTrip(Wire.AllResolvedPolicy, "ResolvedPolicy");
            RoundTrip(Wire.AllCommand, "Command");
            RoundTrip(Wire.AllErrorCode, "ErrorCode");
            RoundTrip(Wire.AllResponseStatus, "ResponseStatus");

            Check(Wire.ToWire(ListeningMode.Surround51) == "5.1", "ListeningMode 5.1");
            Check(Guids.ClsidSfxApo != Guids.ClsidEfxApo, "CLSID distincts");
            Check(Guids.ClsidSfxApo == new Guid(Guids.ClsidSfxApoString), "CLSID SFX textuel");
            Check(Guids.PkeyEfxProfileDocument.FormatId == Guids.CapxPropertySet, "FMTID");
            Check(EnumAttributes.EfxDspModeOf(ListeningMode.Stereo20) == DspMode.Downmix6to2, "2.0 → Downmix6to2");
            Check(EnumAttributes.NeutralRoutingOf(ListeningMode.TwoPointOne), "2.1 neutre");
            Check(EnumAttributes.AccessOf(Command.ApplyProfile) == CommandAccess.Write, "ApplyProfile écriture");
            Check(EnumAttributes.MaskBitOf(Channel.SideRight) == 0x400U, "masque SR");
            Check(Constants.PhysicalOutputOrder.Count == 6 && Constants.PhysicalOutputMask == 0x60FU, "sortie physique");
            Check(Constants.PipeNamePrefix == @"\\.\pipe\XonarSE.Control.v1.", "préfixe du pipe");
            Check(OutputSettingsLimits.ChannelDelayMsMax == 50.0, "borne délai");

            ProfileDocument profile = new ProfileDocument();
            Check(profile.SchemaVersion == 1U && profile.Output.Eq.Length == 8 && profile.Output.Eq[7].Length == 10,
                  "dimensions eq[8][10]");
            Check(profile.Name == null, "nom optionnel absent");
            ControlRequest request = new ControlRequest();
            Check(request.Payload is EmptyPayload && request.ExpectedRevision == null, "ControlRequest par défaut");
            VolumeLevelControl volume = new VolumeLevelControl();
            Check(volume.Control == HardwareControl.Volume && volume is ControlRequestPayload, "union HardwareVolumePayload");
            Check(new StateResult().Kind == ResultKind.State, "discriminant StateResult");

            if (args.Length == 1)
            {
                string text = File.ReadAllText(args[0], new UTF8Encoding(false));
                IDictionary<string, object> json = (IDictionary<string, object>)new JavaScriptSerializer().DeserializeObject(text);
                Compare(json["routing"], profile.Routing, "/routing");
                Compare(json["output"], profile.Output, "/output");
            }
            else
            {
                Check(false, "chemin de la fixture normalisée attendu en argument");
            }

            Console.WriteLine(checks + " verifications, " + failures + " echec(s)");
            return failures == 0 ? 0 : 1;
        }
    }
}
