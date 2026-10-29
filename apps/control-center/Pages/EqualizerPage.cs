// SPDX-License-Identifier: Apache-2.0
// Page « Égaliseur » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Dix bandes par canal, courbe de réponse superposable et comparaison avant/après.
// UI construite entièrement en code (comme OverviewPage), thème sombre turquoise.
// Les dix curseurs sont câblés au brouillon partagé (shell.Editor) : déplacer une bande
// écrit son gain dans Draft.Output.Eq[canal][bande] (application coalescée, non destructive).

using System;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Xonar.ControlCenter.Controls;
using Xonar.ControlCenter.Shell;

namespace Xonar.ControlCenter.Pages;

/// <summary>
/// Page « Égaliseur » : barre d'outils (canal, préréglage, comparaison, réinitialisation),
/// grande carte « Réponse » traçant la courbe des dix bandes du canal choisi, et carte
/// « 10 bandes » avec dix curseurs verticaux. Déplacer un curseur recalcule la courbe.
/// </summary>
public sealed class EqualizerPage : Microsoft.UI.Xaml.Controls.UserControl
{
    // ----------------------------- Données de référence -----------------------------

    // Canaux dans l'ordre physique 5.1 (identifiants techniques conservés).
    private static readonly string[] Channels = { "FL", "FR", "FC", "LFE", "SL", "SR" };

    // Fréquences réelles des dix bandes (Hz) passées au CurveChart, et étiquettes compactes.
    private static readonly double[] BandFreqs =
        { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    private static readonly string[] BandLabels =
        { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };

    // Comparaison avant/après : « avant » = courbe plate (toutes bandes à 0 dB),
    // « après » = jeu d'exemple bombé ci-dessous (dix valeurs en dB).
    private static readonly double[] ApresBombe = { 0, 2, 4, 6, 7, 7, 6, 4, 2, 0 };  // « après » bombé

    // Préréglages appliqués au canal courant (dix valeurs en dB, bornées à [-12, +12]).
    private static readonly double[] PresetPlat = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    private static readonly double[] PresetGrave = { 6, 5, 3, 1, 0, 0, 0, 0, -1, -1 };
    private static readonly double[] PresetVoix = { -2, -1, 0, 1, 3, 4, 3, 1, 0, -1 };
    private static readonly double[] PresetScene = { 4, 3, 1, -1, -2, -2, 0, 2, 4, 5 };

    // ----------------------------- État interne -------------------------------------

    private readonly AppShellViewModel _shell;

    private readonly CurveChart _chart = new() { YMin = -12, YMax = 12, MinHeight = 220 };
    private readonly TextBlock _responseSubtitle = new();
    private readonly Slider[] _bandSliders = new Slider[10];
    private readonly TextBlock[] _bandValues = new TextBlock[10];

    private ComboBox _channelCombo = null!;
    private ComboBox _presetCombo = null!;
    private ToggleSwitch _compareToggle = null!;

    private string _currentChannel = "FL";
    private bool _compare;      // état du commutateur « Comparer avant/après »
    private bool _suspend;      // garde anti-réentrance pour les mises à jour programmatiques
    private bool _selfEdit;     // vrai pendant nos propres écritures vers l'éditeur (pas d'auto-resynchro)

    // ----------------------------- Aides ressources ---------------------------------

    private Brush B(string key) => (Brush)Application.Current.Resources[key];

    private Style S(string key) => (Style)Application.Current.Resources[key];

    // ----------------------------- Construction --------------------------------------

    public EqualizerPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;
        Content = Build();

        // Premier tracé une fois toute l'UI construite (curseurs + graphe prêts).
        RefreshCurve();

        // Initialisation des curseurs depuis le brouillon et abonnement aux changements
        // une fois la page attachée à l'arbre visuel (voir OnLoaded / OnUnloaded).
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private UIElement Build()
    {
        var root = new StackPanel
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
            Spacing = 18,
        };

        // ---------- En-tête ----------
        var header = new StackPanel();
        header.Children.Add(new TextBlock { Text = "Égaliseur", Style = S("CockpitH1") });
        header.Children.Add(new TextBlock
        {
            Text = "Dix bandes par canal, courbe superposable, comparaison avant/après.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        root.Children.Add(header);

        // ---------- Barre d'outils ----------
        root.Children.Add(BuildToolbar());

        // ---------- Carte « Réponse » ----------
        root.Children.Add(BuildResponseCard());

        // ---------- Carte « 10 bandes » ----------
        root.Children.Add(BuildBandsCard());

        return new ScrollViewer
        {
            Content = root,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ----------------------------- Barre d'outils ------------------------------------

    private Border BuildToolbar()
    {
        // ComboBox de canal : affiche les identifiants FL, FR, FC, LFE, SL, SR.
        _channelCombo = new ComboBox { MinWidth = 120 };
        foreach (string code in Channels)
        {
            _channelCombo.Items.Add(code);
        }
        _channelCombo.SelectedIndex = 0;                 // sélection avant abonnement : pas de déclenchement
        _channelCombo.SelectionChanged += OnChannelChanged;

        // ComboBox de préréglage (placeholder : aucun appliqué au démarrage).
        _presetCombo = new ComboBox { MinWidth = 140, PlaceholderText = "Préréglage" };
        _presetCombo.Items.Add("Plat");
        _presetCombo.Items.Add("Grave+");
        _presetCombo.Items.Add("Voix");
        _presetCombo.Items.Add("Scène");
        _presetCombo.SelectedIndex = -1;
        _presetCombo.SelectionChanged += OnPresetChanged;

        // Commutateur « Comparer avant/après ».
        _compareToggle = new ToggleSwitch
        {
            Header = "Comparer avant/après",
            OnContent = "Activé",
            OffContent = "Désactivé",
            IsOn = false,
            VerticalAlignment = VerticalAlignment.Bottom,
        };
        _compareToggle.Toggled += OnCompareToggled;

        // Bouton « Réinitialiser » (style fantôme).
        var reset = new Button
        {
            Content = "Réinitialiser",
            Style = S("CockpitGhostButton"),
            VerticalAlignment = VerticalAlignment.Bottom,
        };
        reset.Click += OnReset;

        var controls = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 20 };
        controls.Children.Add(LabeledControl("Canal", _channelCombo));
        controls.Children.Add(LabeledControl("Préréglage", _presetCombo));
        controls.Children.Add(_compareToggle);
        controls.Children.Add(reset);

        // Ligne d'état : reflète la disponibilité réelle de l'agent (sans bloquer l'aperçu).
        var status = new TextBlock { Text = StatusText(), Style = S("CockpitCaption") };

        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(controls);
        stack.Children.Add(status);

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private StackPanel LabeledControl(string label, FrameworkElement control)
    {
        var stack = new StackPanel { Spacing = 6, VerticalAlignment = VerticalAlignment.Bottom };
        stack.Children.Add(new TextBlock { Text = label, Style = S("CockpitLabel") });
        stack.Children.Add(control);
        return stack;
    }

    private string StatusText()
    {
        try
        {
            return _shell.IsConnected
                ? "Agent connecté — les réglages pourraient être appliqués en direct."
                : "Aperçu local — valeurs d'exemple (aucun agent détecté).";
        }
        catch
        {
            return "Aperçu local — valeurs d'exemple.";
        }
    }

    // ----------------------------- Carte « Réponse » ---------------------------------

    private Border BuildResponseCard()
    {
        _responseSubtitle.Style = S("CockpitCaption");
        _responseSubtitle.Margin = new Thickness(0, -4, 0, 2);

        var stack = new StackPanel { Spacing = 10 };
        stack.Children.Add(new TextBlock { Text = "Réponse", Style = S("CockpitH2") });
        stack.Children.Add(_responseSubtitle);
        stack.Children.Add(_chart);

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // ----------------------------- Carte « 10 bandes » -------------------------------

    private Border BuildBandsCard()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };

        double[] gains = ReadDraftGains();
        for (int i = 0; i < 10; i++)
        {
            int idx = i;                 // capture pour les fermetures
            double init = gains[idx];

            var slider = new Slider
            {
                Orientation = Orientation.Vertical,   // curseur VERTICAL
                Minimum = -12,
                Maximum = 12,
                StepFrequency = 1,
                SmallChange = 1,
                Height = 170,
                HorizontalAlignment = HorizontalAlignment.Center,
                Value = init,                          // valeur posée avant l'abonnement
            };

            var valueLbl = new TextBlock
            {
                Text = FormatGain(init),
                Style = S("CockpitCaption"),
                Foreground = B("CockpitAccentBrush"),
                HorizontalAlignment = HorizontalAlignment.Center,
            };
            var freqLbl = new TextBlock
            {
                Text = BandLabels[idx],
                Style = S("CockpitLabel"),
                HorizontalAlignment = HorizontalAlignment.Center,
            };

            _bandSliders[idx] = slider;
            _bandValues[idx] = valueLbl;

            slider.ValueChanged += (_, ev) =>
            {
                if (_suspend)
                {
                    return;             // mise à jour programmatique : on ne réécrit rien
                }

                // Écrit le gain de la bande (canal courant) dans le brouillon partagé.
                WriteGainToEditor(idx, ev.NewValue);
                _bandValues[idx].Text = FormatGain(ev.NewValue);
                RefreshCurve();          // recalcule la courbe à partir des dix bandes
            };

            var col = new StackPanel { Width = 58, Spacing = 8, HorizontalAlignment = HorizontalAlignment.Center };
            col.Children.Add(valueLbl);
            col.Children.Add(slider);
            col.Children.Add(freqLbl);
            row.Children.Add(col);
        }

        var caption = new TextBlock
        {
            Text = "Dix curseurs verticaux, -12 à +12 dB. Déplacez un curseur pour redessiner la courbe de réponse.",
            Style = S("CockpitCaption"),
            Margin = new Thickness(0, -4, 0, 2),
        };

        var stack = new StackPanel { Spacing = 10 };
        stack.Children.Add(new TextBlock { Text = "10 bandes", Style = S("CockpitH2") });
        stack.Children.Add(caption);
        stack.Children.Add(row);

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // ----------------------------- Logique de courbe ---------------------------------

    /// <summary>
    /// Recalcule la courbe du CurveChart. Le CurveChart ne gère qu'une seule courbe :
    /// hors comparaison, il affiche la courbe active des dix bandes du canal courant ;
    /// en comparaison, il bascule sur le jeu d'exemple « après » (bombé), le « avant »
    /// plat servant de référence. Le sous-titre indique le mode en cours.
    /// </summary>
    private void RefreshCurve()
    {
        if (_compare)
        {
            _chart.SetCurve(BandFreqs, ApresBombe);
            _responseSubtitle.Text =
                $"Comparaison avant/après — tracé « après » (bombé). Désactivez pour revoir « avant » ({ChannelLabel(_currentChannel)}).";
        }
        else
        {
            _chart.SetCurve(BandFreqs, ReadDraftGains());
            _responseSubtitle.Text =
                $"Courbe active — {ChannelLabel(_currentChannel)} · dix bandes, -12 à +12 dB.";
        }
    }

    private void LoadChannelIntoSliders()
    {
        _suspend = true;
        try
        {
            double[] gains = ReadDraftGains();
            for (int i = 0; i < 10; i++)
            {
                _bandSliders[i].Value = gains[i];
                _bandValues[i].Text = FormatGain(gains[i]);
            }
        }
        finally
        {
            _suspend = false;
        }
    }

    // ----------------------------- Liaison à l'éditeur -------------------------------

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Initialise les dix curseurs depuis Draft.Output.Eq[canal courant] (sous _suspend),
        // trace la courbe, puis s'abonne aux changements du brouillon partagé.
        LoadChannelIntoSliders();
        RefreshCurve();

        _shell.Editor.Changed -= OnEditorChanged;   // abonnement idempotent
        _shell.Editor.Changed += OnEditorChanged;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _shell.Editor.Changed -= OnEditorChanged;
    }

    // Les notifications Changed peuvent venir d'un thread hors-UI (coalesceur
    // d'applications) ; on marshale TOUJOURS vers le thread UI avant de toucher aux
    // contrôles WinUI, sinon RPC_E_WRONG_THREAD.
    private void OnEditorChanged()
    {
        var q = DispatcherQueue;
        if (q is null || q.HasThreadAccess)
        {
            OnEditorChangedCore();
        }
        else
        {
            q.TryEnqueue(OnEditorChangedCore);
        }
    }

    private void OnEditorChangedCore()
    {
        if (_selfEdit)
        {
            return;   // changement provoqué par nos propres curseurs : l'affichage est déjà à jour
        }

        // Resynchronise l'affichage sur le brouillon (hors manipulation locale).
        LoadChannelIntoSliders();
        RefreshCurve();
    }

    // Position d'affichage (FL FR FC LFE SL SR, index 0..5 de Channels) -> valeur de l'enum
    // xonar::dsp::Channel attendue par l'éditeur et les tableaux Eq[] du contrat :
    // FL=0 FR=1 FC=2 LFE=3 SL=6 SR=7 (les index 4/5 sont BL/BR, absents du 5.1-côté 0x60F).
    private static readonly int[] DisplayToChannel = { 0, 1, 2, 3, 6, 7 };

    /// <summary>Index de canal (valeur de l'enum Channel : FL=0 … SL=6, SR=7) attendu par l'éditeur.</summary>
    private int CurrentChannelIndex()
    {
        int idx = Array.IndexOf(Channels, _currentChannel);
        if (idx < 0) { idx = 0; }
        return DisplayToChannel[idx];
    }

    /// <summary>
    /// Lit les dix gains (dB) du canal courant depuis le brouillon partagé de l'éditeur.
    /// Les gains sont câblés par INDEX de bande (0..9) : les fréquences d'affichage de la
    /// page et les FrequencyHz stockées dans le profil peuvent différer sans conséquence.
    /// </summary>
    private double[] ReadDraftGains()
    {
        var eq = _shell.Editor.Draft.Output.Eq;
        var bands = eq[CurrentChannelIndex()];
        var gains = new double[10];
        for (int b = 0; b < 10; b++)
        {
            gains[b] = bands[b].GainDb;
        }
        return gains;
    }

    /// <summary>Écrit un gain de bande dans le brouillon partagé (application coalescée par l'éditeur).</summary>
    private void WriteGainToEditor(int band, double value)
    {
        _selfEdit = true;
        try
        {
            _shell.Editor.SetEqGain(CurrentChannelIndex(), band, value);
        }
        finally
        {
            _selfEdit = false;
        }
    }

    // ----------------------------- Gestionnaires -------------------------------------

    private void OnChannelChanged(object sender, SelectionChangedEventArgs e)
    {
        int idx = _channelCombo.SelectedIndex;
        if (idx < 0 || idx >= Channels.Length)
        {
            return;
        }

        _currentChannel = Channels[idx];
        LoadChannelIntoSliders();
        RefreshCurve();
    }

    private void OnPresetChanged(object sender, SelectionChangedEventArgs e)
    {
        int idx = _presetCombo.SelectedIndex;
        if (idx < 0)
        {
            return;     // placeholder : rien à appliquer
        }

        double[] preset = idx switch
        {
            0 => PresetPlat,
            1 => PresetGrave,
            2 => PresetVoix,
            3 => PresetScene,
            _ => PresetPlat,
        };

        // Applique les dix gains du préréglage au canal courant via l'éditeur.
        int c = CurrentChannelIndex();
        _selfEdit = true;
        try
        {
            for (int b = 0; b < 10; b++)
            {
                _shell.Editor.SetEqGain(c, b, preset[b]);
            }
        }
        finally
        {
            _selfEdit = false;
        }

        LoadChannelIntoSliders();
        RefreshCurve();
    }

    private void OnCompareToggled(object sender, RoutedEventArgs e)
    {
        _compare = _compareToggle.IsOn;
        RefreshCurve();
    }

    private void OnReset(object sender, RoutedEventArgs e)
    {
        // Réinitialise les dix bandes du canal courant à 0 dB (sans toucher aux autres canaux).
        int c = CurrentChannelIndex();
        _selfEdit = true;
        try
        {
            for (int b = 0; b < 10; b++)
            {
                _shell.Editor.SetEqGain(c, b, 0);
            }
        }
        finally
        {
            _selfEdit = false;
        }

        _presetCombo.SelectedIndex = -1;     // revient au placeholder (n'applique aucun préréglage)
        LoadChannelIntoSliders();
        RefreshCurve();
    }

    // ----------------------------- Aides -------------------------------------------

    private static string ChannelLabel(string code) => code switch
    {
        "FL" => "Avant gauche (FL)",
        "FR" => "Avant droit (FR)",
        "FC" => "Centre (FC)",
        "LFE" => "Caisson (LFE)",
        "SL" => "Surround gauche (SL)",
        "SR" => "Surround droit (SR)",
        _ => code,
    };

    /// <summary>Met en forme un gain en dB : « +3 dB », « 0 dB », « -2 dB » (point invariant).</summary>
    private static string FormatGain(double v)
    {
        long r = (long)Math.Round(v);
        string num = r.ToString(CultureInfo.InvariantCulture);
        return (r > 0 ? "+" + num : num) + " dB";
    }
}
