using System.Globalization;
using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using Xonar.Contracts.V1;
using Xonar.ControlCenter.Services;
using Microsoft.UI.Dispatching;
using Xonar.ControlCenter.Profiles;

namespace Xonar.ControlCenter.Shell;

/// <summary>
/// Pipe de contrôle pour les TESTS AUTOMATISÉS (développement uniquement). Serveur named
/// pipe « xonar-cc-test » : reçoit des commandes texte (une par ligne) et appelle les
/// mutateurs de l'éditeur de profil SUR LE THREAD UI — exactement comme le ferait un
/// clic utilisateur. Permet de piloter le cockpit depuis un script et de vérifier la
/// chaîne complète UI -> apo-efx.cfg -> APO sans intervention manuelle.
///
/// Désactivé par défaut : n'est démarré que si la variable d'environnement
/// XONAR_TEST_PIPE=1 est présente au lancement (aucune surface en production).
///
/// Commandes : ping | get | set masterGainDb &lt;dB&gt; | set chgain &lt;canal&gt; &lt;dB&gt; |
/// set mute &lt;canal&gt; &lt;0|1&gt; | set limiter &lt;0|1&gt; | set bass &lt;0|1&gt; |
/// set crossover &lt;Hz&gt;. &lt;canal&gt; = FL FR FC LFE BL BR SL SR ou 0..7.
/// </summary>
public sealed class TestControlPipe : IDisposable
{
    public const string PipeName = "xonar-cc-test";
    private static readonly string[] Ch = { "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR" };

    private readonly ProfileEditorViewModel _editor;
    private readonly DispatcherQueue _ui;
    private readonly CancellationTokenSource _cts = new();
    private readonly Func<Task<string>>? _speakerTest;
    private readonly Action<string>? _navigate;
    private readonly Func<Task<string>>? _diagnostic;

    public TestControlPipe(ProfileEditorViewModel editor, DispatcherQueue ui, Func<Task<string>>? speakerTest = null, Action<string>? navigate = null, Func<Task<string>>? diagnostic = null)
    {
        _editor = editor;
        _ui = ui;
        _speakerTest = speakerTest;
        _navigate = navigate;
        _diagnostic = diagnostic;
        _ = RunAsync(_cts.Token);
    }

    private async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                using var server = new NamedPipeServerStream(PipeName, PipeDirection.InOut, 1,
                    PipeTransmissionMode.Byte, PipeOptions.Asynchronous);
                await server.WaitForConnectionAsync(ct);
                using var reader = new StreamReader(server, Encoding.UTF8, false, 1024, leaveOpen: true);
                using var writer = new StreamWriter(server, new UTF8Encoding(false)) { AutoFlush = true };
                string? line;
                while ((line = await reader.ReadLineAsync(ct)) is not null)
                {
                    await writer.WriteLineAsync(await HandleAsync(line.Trim()));
                }
            }
            catch (OperationCanceledException) { break; }
            catch { try { await Task.Delay(150, ct); } catch { break; } }
        }
    }

    private static int ChannelIndex(string s)
    {
        if (int.TryParse(s, out int i) && i >= 0 && i < 8) return i;
        for (int k = 0; k < Ch.Length; k++)
        {
            if (string.Equals(Ch[k], s, StringComparison.OrdinalIgnoreCase)) return k;
        }
        return -1;
    }

    private static bool AsBool(string s) => s is "1" or "true" or "on" or "yes";
    private static double AsNum(string s) => double.Parse(s, CultureInfo.InvariantCulture);

    private async Task<string> HandleAsync(string cmd)
    {
        var p = cmd.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        if (p.Length == 0) return "err:empty";
        if (p[0] == "ping") return "pong";
        var completion = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);
        if (!_ui.TryEnqueue(async () => {
            try { completion.TrySetResult(await HandleOnUiAsync(p)); }
            catch (Exception ex) { completion.TrySetResult("err:" + ex.Message); }
        })) return "err:UI queue unavailable";
        try { return await completion.Task.WaitAsync(TimeSpan.FromSeconds(15), _cts.Token); }
        catch (Exception ex) { return "err:" + ex.Message; }
    }

    private async Task<string> HandleOnUiAsync(string[] p)
    {
        try
        {
            switch (p[0].ToLowerInvariant())
            {
                case "ping":
                    return "pong";
                case "get":
                    string mode = _editor.Draft.SourcePolicy == SourcePolicy.AutoDeclaredLayout && _editor.Draft.ListeningMode == ListeningMode.Surround51 ? "auto" : Wire.ToWire(_editor.Draft.ListeningMode);
                    return $"ok endpoint={_editor.EndpointKey} mode={mode} master={_editor.Draft.Output?.MasterGainDb.ToString(CultureInfo.InvariantCulture)} state={_editor.State} telemetryReads={DspLiveStatus.DeserializationCount} message={_editor.LastMessage}";
                case "snapshot":
                    return Convert.ToBase64String(JsonSerializer.SerializeToUtf8Bytes(_editor.Draft));
                case "diagnostic":
                    return _diagnostic is null ? "err:unavailable" : Convert.ToBase64String(Encoding.UTF8.GetBytes(await _diagnostic()));
                case "restore" when p.Length == 2:
                    _editor.LoadProfile(JsonSerializer.Deserialize<ProfileDocument>(Convert.FromBase64String(p[1]))!);
                    await _editor.ApplyNowAsync();
                    return "ok";
                case "apply":
                    await _editor.ApplyNowAsync();
                    return "ok " + _editor.State;
                case "speakers":
                    return _speakerTest is null ? "err:unavailable" : await _speakerTest();
                case "view" when p.Length == 2:
                    _navigate?.Invoke(p[1]);
                    await Task.Delay(250);
                    return "ok view " + p[1];
                case "set" when p.Length == 7 && p[1] == "eq":
                    int channel = ChannelIndex(p[2]);
                    int band = int.Parse(p[3], CultureInfo.InvariantCulture);
                    if (channel < 0 || band < 0 || band >= 10) throw new FormatException("Canal ou bande invalide.");
                    _editor.SetEqFrequency(channel, band, AsNum(p[4]));
                    _editor.SetEqQ(channel, band, AsNum(p[5]));
                    _editor.SetEqGain(channel, band, AsNum(p[6]));
                    _editor.SetEqEnabled(channel, band, true);
                    return "ok";
                case "set" when p.Length >= 3:
                    ApplySet(p[1].ToLowerInvariant(), p[2], p.Length >= 4 ? p[3] : "");
                    return "ok";
                default:
                    return "err:usage";
            }
        }
        catch (Exception ex)
        {
            return "err:" + ex.Message;
        }
    }

    private void ApplySet(string key, string a, string b)
    {
        switch (key)
        {
            case "mode": _editor.SetSourcePolicy(a == "auto" ? SourcePolicy.AutoDeclaredLayout : SourcePolicy.Preserve); _editor.SetListeningMode(a switch {
                "auto" => ListeningMode.Surround51,
                "5.1" => ListeningMode.Surround51, "upmix" => ListeningMode.Upmix,
                "2.0" => ListeningMode.Stereo20, "2.1" => ListeningMode.TwoPointOne,
                _ => throw new FormatException("Mode invalide.") }); break;
            case "mastergaindb": _editor.SetMasterGainDb(AsNum(a)); break;
            case "limiter": _editor.SetLimiterEnabled(AsBool(a)); break;
            case "bass": _editor.SetBassManagementEnabled(AsBool(a)); break;
            case "crossover": _editor.SetCrossoverHz(AsNum(a)); break;
            case "delay": { int c = ChannelIndex(a); if(c < 0) throw new FormatException("Canal invalide"); _editor.SetChannelDelayMs(c, AsNum(b)); break; }
            case "ceiling": _editor.SetLimiterCeilingDb(AsNum(a)); break;
            case "highpass": _editor.SetHighPassSatellites(AsBool(a)); break;
            case "sumtolfe": _editor.SetSumToLfe(AsBool(a)); break;
            case "chgain": { int c = ChannelIndex(a); if (c >= 0) _editor.SetChannelGainDb(c, AsNum(b)); break; }
            case "mute": { int c = ChannelIndex(a); if (c >= 0) _editor.SetChannelMute(c, AsBool(b)); break; }
            default: throw new FormatException("Réglage inconnu.");
        }
    }

    public void Dispose() => _cts.Cancel();
}
