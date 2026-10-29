// SPDX-License-Identifier: Apache-2.0
// Diagnostic matériel et DSP réel, sans données de démonstration.
// Les mesures absentes restent inconnues ; récupération et export expurgé.

using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Xonar.Contracts.V1;
using Xonar.ControlCenter.Controls;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Services.Localization;
using Xonar.ControlCenter.Status;

namespace Xonar.ControlCenter.Pages;

public sealed class DiagnosticPage : Microsoft.UI.Xaml.Controls.UserControl
{
    // Ordre physique des six positions de sortie (FL FR FC LFE SL SR), identique
    // au contrat C7 (MeterState.OutputPeakDbfs) et à la page Vue d'ensemble.
    private static readonly string[] Channels = { "FL", "FR", "FC", "LFE", "SL", "SR" };

    // Plancher visuel quand aucune mesure n’est disponible.
    private static readonly double[] UnavailablePeaks = { -60, -60, -60, -60, -60, -60 };

    private readonly Xonar.ControlCenter.Shell.AppShellViewModel _shell;

    // Contrôles reconstruits/mis à jour après le chargement réel.
    private readonly VuMeterBars _vu = new();
    private readonly TextBlock _fmtValue = new();
    private readonly TextBlock _driverValue = new();
    private readonly TextBlock _revValue = new();
    private readonly TextBlock _driftValue = new();
    private readonly TextBlock _jackValue = new();
    private readonly TextBlock _vuNote = new();
    private readonly StackPanel _instancesPanel = new() { Spacing = 10 };
    private readonly Button _repairButton = new();
    private readonly Button _exportButton = new();
    private readonly TextBlock _repairStatus = new();
    private readonly TextBox _exportBox = new();
    private readonly DispatcherTimer _refreshTimer = new() { Interval = TimeSpan.FromMilliseconds(500) };
    private bool _loading;

    // État réel chargé (null tant qu'aucun agent n'a répondu).
    private AgentState? _state;
    private string? _key;

    private Brush B(string key) => (Brush)Application.Current.Resources[key];
    private Style S(string key) => (Style)Application.Current.Resources[key];

    public DiagnosticPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;
        Content = Build();
        _refreshTimer.Tick += async (_, _) => await LoadStateAsync();
        Loaded += async (_, _) => { _refreshTimer.Start(); await LoadStateAsync(); };
        Unloaded += (_, _) => _refreshTimer.Stop();
    }

    // ---------------------------------------------------------------------------
    // Construction de l'arbre visuel
    // ---------------------------------------------------------------------------
    private UIElement Build()
    {
        // Conteneur : grille mono-cellule bornée à 1480 px, alignée à gauche, dans
        // laquelle une pile verticale empile les cartes sur toute la largeur.
        var root = new Grid
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
        };

        var col = new StackPanel { Spacing = 18 };
        root.Children.Add(col);

        // ---------- En-tête ----------
        var head = new StackPanel();
        head.Children.Add(new TextBlock { Text = "Diagnostic", Style = S("CockpitH1") });
        head.Children.Add(new TextBlock
        {
            Text = "Format Windows réel, traitement actif, état du pilote et récupération.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        col.Children.Add(head);

        // ---------- Carte « Format & pilote » ----------
        col.Children.Add(FormatCard());

        // ---------- Carte « État du jack » ----------
        col.Children.Add(JackCard());

        // ---------- Carte « Instances APO » ----------
        RenderInstances(Array.Empty<ApoInstanceState>());
        col.Children.Add(Card("Instances APO",
            "LFX avant mixage, GFX après mixage. Seules les instances observées dans audiodg sont affichées.",
            _instancesPanel));

        // ---------- Carte « Vumètres post-DSP » ----------
        col.Children.Add(VuCard());

        // ---------- Carte « Récupération » ----------
        col.Children.Add(RecoveryCard());

        return new ScrollViewer
        {
            Content = root,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    private Border FormatCard()
    {
        // État inconnu avant la première lecture réelle.
        _fmtValue.Style = S("CockpitBody");
        _fmtValue.Text = "En attente de lecture du matériel";
        _driverValue.Style = S("CockpitBody");
        _driverValue.Text = "Non déterminé";
        _revValue.Style = S("CockpitBody");
        _revValue.Text = "Révision 0 · souhaitée 0 · écrite 0";
        _driftValue.Style = S("CockpitBody");
        _driftValue.Text = "Format non déterminé.";
        _driftValue.Foreground = B("CockpitTextSecondaryBrush");

        var grid = DefGrid();
        AddDef(grid, 0, "Format partagé Windows", _fmtValue);
        AddDef(grid, 1, "Pilote", _driverValue);
        AddDef(grid, 2, "Révision / générations", _revValue);
        AddDef(grid, 3, "Conformité 5.1", _driftValue);

        return Card("Format & pilote",
            "Format partagé lu dans Windows et pilote observé. Le format de chaque flux est détaillé dans les instances ci-dessous.",
            grid);
    }

    private Border JackCard()
    {
        _jackValue.Style = S("CockpitBody");
        _jackValue.Text = "État du jack non mesuré.";

        var caveat = new TextBlock
        {
            // Honnêteté explicite : aucune LED « branché » synthétique n'est affichée.
            Text = "L’état de connexion publié par un pilote ne prouve pas la présence physique d’enceintes. La détection de prise reste inconnue ici.",
            Style = S("CockpitCaption"),
            Foreground = B("CockpitWarningBrush"),
            Margin = new Thickness(0, 10, 0, 0),
        };

        var stack = new StackPanel();
        stack.Children.Add(_jackValue);
        stack.Children.Add(caveat);

        return Card("État du jack", null, stack);
    }

    private Border VuCard()
    {
        _vu.MinHeight = 210;
        _vu.Margin = new Thickness(0, 2, 0, 0);
        _vu.SetLevels(Channels, UnavailablePeaks);

        _vuNote.Style = S("CockpitCaption");
        _vuNote.Text = "En attente de mesures réelles.";

        var stack = new StackPanel { Spacing = 10 };
        stack.Children.Add(_vu);
        stack.Children.Add(_vuNote);

        return Card("Vumètres post-DSP",
            "Crêtes de sortie de l'APO (≠ niveaux analogiques). Une mesure datée de plus de 2 s est marquée « périmée », jamais confondue avec le silence.",
            stack);
    }

    private Border RecoveryCard()
    {
        _repairButton.Style = S("CockpitAccentButton");
        _repairButton.Content = "Retrouver la carte et réappliquer";
        _repairButton.IsEnabled = false; // activé après lecture des capacités/état.
        AutomationProperties.SetName(_repairButton, "Retrouver la carte et réappliquer le profil");
        _repairButton.Click += async (_, _) => await RequestRepairAsync();

        _exportButton.Style = S("CockpitGhostButton");
        _exportButton.Content = "Exporter le diagnostic (expurgé)";
        AutomationProperties.SetName(_exportButton, "Exporter le diagnostic expurgé");
        _exportButton.Click += (_, _) => _exportBox.Text = DiagnosticsExporter.Export(_state, (_shell.Client as LiveApoBroker)?.Telemetry);

        var actions = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        actions.Children.Add(_repairButton);
        actions.Children.Add(_exportButton);
        var install = new Button { Content = "Réparer l’installation (administrateur)", Style = S("CockpitGhostButton") };
        install.Click += async (_, _) => {
            install.IsEnabled = false;
            _repairStatus.Text = "Installation en cours ; une élévation Windows peut être demandée.";
            try {
                var result = await InstallerLauncher.RunAsync();
                _repairStatus.Text = result.Message;
                if(result.Success) await RequestRepairAsync();
            } finally { install.IsEnabled = true; }
        };

        _repairStatus.Style = S("CockpitCaption");
        _repairStatus.Text = "La carte est recherchée automatiquement. Si une réinstallation du pilote a supprimé les effets, réparer leur installation nécessite l’élévation Windows.";

        _exportBox.IsReadOnly = true;
        _exportBox.AcceptsReturn = true;
        _exportBox.IsSpellCheckEnabled = false;
        _exportBox.TextWrapping = TextWrapping.Wrap;
        _exportBox.Height = 220;
        _exportBox.FontFamily = new FontFamily("Consolas");
        _exportBox.FontSize = 12;
        _exportBox.Background = B("CockpitSurfaceInsetBrush");
        _exportBox.Foreground = B("CockpitTextPrimaryBrush");
        _exportBox.BorderBrush = B("CockpitBorderSubtleBrush");
        _exportBox.PlaceholderText = "Le diagnostic expurgé s'affiche ici après export.";
        ScrollViewer.SetVerticalScrollBarVisibility(_exportBox, ScrollBarVisibility.Auto);
        AutomationProperties.SetName(_exportBox, "Contenu du diagnostic exporté");

        var redaction = new TextBlock
        {
            Text = "Export expurgé : aucune clé d'endpoint complète (seule sa longueur), aucun PCM utilisateur, "
                 + "aucun nom de machine ni numéro de série. Uniquement la télémétrie C7 utile au diagnostic.",
            Style = S("CockpitCaption"),
            Margin = new Thickness(0, 2, 0, 0),
        };

        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(actions);
        stack.Children.Add(install);
        stack.Children.Add(_repairStatus);
        stack.Children.Add(redaction);
        stack.Children.Add(_exportBox);

        return Card("Récupération", null, stack);
    }

    // ---------------------------------------------------------------------------
    // Rendu des instances APO
    // ---------------------------------------------------------------------------
    private void RenderInstances(IReadOnlyList<ApoInstanceState>? instances)
    {
        _instancesPanel.Children.Clear();
        if (instances is null || instances.Count == 0)
        {
            _instancesPanel.Children.Add(new TextBlock { Text = "Aucune instance APO rapportée.", Style = S("CockpitBody") });
            return;
        }

        foreach (var inst in instances)
        {
            _instancesPanel.Children.Add(InstanceRow(inst));
        }
    }

    private Border InstanceRow(ApoInstanceState inst)
    {
        var stack = new StackPanel { Spacing = 3 };
        stack.Children.Add(new TextBlock
        {
            Text = $"{(inst.Stage == ApoStage.Sfx ? "LFX · avant mixage" : "GFX · après mixage")} · {inst.InstanceId}",
            FontSize = 14,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = B("CockpitTextPrimaryBrush"),
        });
        stack.Children.Add(new TextBlock
        {
            Text = $"Acquittement {inst.AppliedGeneration} · processus {inst.Epoch}" +
                (inst.InputFormat is { } format ? $" · entrée {format.ChannelCount} canaux / {format.SampleRate} Hz / 0x{format.ChannelMask:X}" : ""),
            Style = S("CockpitCaption"),
        });

        string bypass;
        Brush bypassBrush;
        switch (inst.Bypass?.Kind)
        {
            case BypassKind.None:
                bypass = "Bypass : aucun";
                bypassBrush = B("CockpitTextSecondaryBrush");
                break;
            case BypassKind.Observed:
                bypass = inst.Bypass?.Reason is BypassReason r
                    ? $"Bypass observé : {Wire.ToWire(r)}"
                    : "Bypass observé (motif non précisé)";
                bypassBrush = B("CockpitWarningBrush");
                break;
            default: // Unknown ou absent : on ne suppose rien.
                bypass = "Bypass : inconnu";
                bypassBrush = B("CockpitTextTertiaryBrush");
                break;
        }
        stack.Children.Add(new TextBlock { Text = bypass, FontSize = 12.5, Foreground = bypassBrush });

        return new Border
        {
            Background = B("CockpitSurfaceInsetBrush"),
            BorderBrush = B("CockpitBorderSubtleBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(14, 12, 14, 12),
            Child = stack,
        };
    }

    // ---------------------------------------------------------------------------
    // Chargement réel de l'état (gardé) et application aux cartes
    // ---------------------------------------------------------------------------
    private async Task LoadStateAsync()
    {
        if(_loading) return;
        _loading = true;
        try
        {
            string key = _shell.SelectedEndpointKey ?? "MI_00";

            var result = await _shell.Client.GetStateAsync(key);
            var st = result.State;
            if (st is null)
            {
                return;
            }

            _key = key;
            _state = st;
            ApplyState(st);
        }
        catch
        {
            _vu.SetLevels(Channels, UnavailablePeaks);
            _vuNote.Text = "Diagnostic indisponible.";
        }
        finally { _loading = false; }
    }

    private void ApplyState(AgentState st)
    {
        // ---- Format & pilote ----
        NegotiatedFormat? f = st.NegotiatedFormat;
        _fmtValue.Text = f is null
            ? "inconnu"
            : $"{f.SampleRate} Hz · masque 0x{f.ChannelMask:X} · {f.ChannelCount} canaux · {Wire.ToWire(f.SampleFormat)}";

        _driverValue.Text = $"{Wire.ToWire(st.Driver.Kind)}"
            + (string.IsNullOrEmpty(st.Driver.Version) ? "" : $" · version {st.Driver.Version}");

        _revValue.Text = $"Révision {st.Revision} · souhaitée {st.DesiredGeneration} · écrite {st.CommittedGeneration}"
            + (st.PartiallyApplied ? " · application partielle" : "");

        bool drift = StalenessEvaluator.HasFormatDrift(f);
        _driftValue.Text = f is null
            ? "Format inconnu."
            : (drift ? "Dérive détectée : hors du 5.1 attendu (0x60F, 6 canaux)." : "Conforme au 5.1 attendu (0x60F, 6 canaux).");
        _driftValue.Foreground = drift ? B("CockpitWarningBrush") : B("CockpitTextSecondaryBrush");

        // ---- État du jack (honnête) ----
        string conn = st.Jack?.PublishedConnected is bool c
            ? (c ? "publié connecté" : "publié déconnecté")
            : "non publié";
        _jackValue.Text = $"État publié à Windows : {Wire.ToWire(st.Jack?.Reliability ?? JackReliability.Unknown)} ({conn}).";

        // ---- Instances APO ----
        RenderInstances(st.Instances);

        // ---- Vumètres post-DSP + péremption (> 2 s) ----
        MeterState? meters = st.Meters;
        if (meters?.OutputPeakDbfs is double[] peaks && peaks.Length >= 6)
        {
            var vals = new double[6];
            for (int i = 0; i < 6; i++)
            {
                double v = peaks[i];
                // Silence numérique (plancher JSON) ramené au bas d'échelle du vumètre.
                if (double.IsNaN(v) || double.IsInfinity(v) || v <= Constants.MeterFloorDbfs)
                {
                    v = -60;
                }
                vals[i] = v;
            }
            _vu.SetLevels(Channels, vals);
        }

        bool stale = StalenessEvaluator.IsStale(meters, TimeSpan.Zero);
        if (meters is null)
        {
            _vu.SetLevels(Channels, UnavailablePeaks);
            _vuNote.Text = "Mesures indisponibles.";
            _vuNote.Foreground = B("CockpitTextTertiaryBrush");
        }
        else if (stale)
        {
            _vuNote.Text = $"Mesures périmées (datées de {meters.AgeMs} ms, seuil 2 s) — ce n'est pas du silence en direct.";
            _vuNote.Foreground = B("CockpitWarningBrush");
        }
        else
        {
            _vuNote.Text = $"Publication DSP datée de {meters.AgeMs} ms. Crêtes sur fenêtres de 50 ms, ordre FL FR FC LFE SL SR. " +
                HeadroomAdvice.From((_shell.Client as LiveApoBroker)?.Telemetry).Message;
            _vuNote.Foreground = B("CockpitTextTertiaryBrush");
        }

        // ---- Récupération : bouton actif selon la capacité déclarée ----
        _repairButton.IsEnabled = CanRepair();
    }

    // ---------------------------------------------------------------------------
    // Récupération : réparation du format 5.1 (déclenchée par l'utilisateur)
    // ---------------------------------------------------------------------------
    private bool CanRepair() => _shell.Client is LiveApoBroker || (_shell.Capabilities?.Features?.FormatRepair == true && _state is not null);

    private async Task RequestRepairAsync()
    {
        if(_shell.Client is LiveApoBroker live) {
            _repairButton.IsEnabled = false;
            try {
                live.RefreshHardware();
                await _shell.InitializeAsync();
                if(!_shell.CanWrite) {
                    _repairStatus.Text = live.Device is null ? "Carte Xonar indisponible." : "Traitements APO absents ou format incompatible. Vérifier le 5.1 puis réparer l’installation si nécessaire.";
                } else {
                    await _shell.Editor.ApplyNowAsync();
                    _repairStatus.Text = _shell.Editor.LastMessage;
                }
                await LoadStateAsync();
            } finally { _repairButton.IsEnabled = CanRepair(); }
            return;
        }
        // Jamais de réparation automatique : uniquement sur action utilisateur,
        // bornée par la capacité (canRepairFormat) et l'état réellement chargé.
        if (_key is not string key || _state is not AgentState st || !CanRepair())
        {
            return;
        }

        _repairButton.IsEnabled = false;
        try
        {
            var resp = await _shell.Client.RequestFormatRepairAsync(key, st.Revision);
            _repairStatus.Text = resp.Status switch
            {
                ResponseStatus.Applied => "Format 5.1 réparé : toutes les instances rapportées ont appliqué la nouvelle génération.",
                ResponseStatus.Committed => "Réparation écrite ; confirmation de toutes les instances encore en attente.",
                ResponseStatus.Ok => "Demande de réparation acceptée par l'agent.",
                ResponseStatus.Error => resp.Error is not null
                    ? ErrorMessages.ForErrorCode(resp.Error.Code)
                    : "L'agent a renvoyé une erreur sans code exploitable.",
                _ => "Réponse inattendue de l'agent.",
            };

            // Relecture de l'état réel après la tentative (révision/générations à jour).
            await LoadStateAsync();
        }
        catch
        {
            _repairStatus.Text = "La demande de réparation n'a pas pu être transmise à l'agent.";
        }
        finally
        {
            _repairButton.IsEnabled = CanRepair();
        }
    }

    // ---------------------------------------------------------------------------
    // Aides de mise en page
    // ---------------------------------------------------------------------------
    private Border Card(string title, string? subtitle, UIElement body)
    {
        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(new TextBlock { Text = title, Style = S("CockpitH2") });
        if (subtitle is not null)
        {
            stack.Children.Add(new TextBlock { Text = subtitle, Style = S("CockpitCaption"), Margin = new Thickness(0, -4, 0, 2) });
        }
        stack.Children.Add(body);
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private static Grid DefGrid()
    {
        var g = new Grid { RowSpacing = 9, ColumnSpacing = 18 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(220) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        return g;
    }

    private void AddDef(Grid g, int row, string label, TextBlock value)
    {
        g.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var lbl = new TextBlock { Text = label, Style = S("CockpitLabel"), VerticalAlignment = VerticalAlignment.Top };
        Grid.SetRow(lbl, row);
        Grid.SetColumn(lbl, 0);
        g.Children.Add(lbl);

        Grid.SetRow(value, row);
        Grid.SetColumn(value, 1);
        g.Children.Add(value);
    }
}
