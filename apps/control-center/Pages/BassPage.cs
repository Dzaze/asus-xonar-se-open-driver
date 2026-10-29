// SPDX-License-Identifier: Apache-2.0
// Page « Basses & crossover » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Construite entièrement en code (pas de XAML). Elle regroupe :
//   - le crossover : passe-haut des satellites, graphe de réponse et choix de la
//     fréquence de coupure ;
//   - le bass management : activation, passe-haut des satellites, sommation vers le LFE ;
//   - le dosage du canal LFE (.1) adressé au caisson ;
//   - la protection anti-saturation : limiteur et plafond de sortie.
// Les contrôles sont reliés à l'éditeur de profil partagé (shell.Editor, toujours
// disponible) : ils sont initialisés depuis le brouillon (Draft.Output.BassManagement
// et Draft.Output.Limiter) et chaque action écrit dans ce brouillon, dont l'éditeur
// coalesce l'application vers l'agent. La page n'écrit aucun volume matériel ni aucune
// API système Windows.

using System;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Xonar.ControlCenter.Controls;
using Xonar.ControlCenter.Shell;

namespace Xonar.ControlCenter.Pages;

public sealed class BassPage : Microsoft.UI.Xaml.Controls.UserControl
{
    // Fréquences de coupure proposées pour le crossover (Hz).
    private static readonly double[] CrossoverFreqs = { 60, 80, 100, 120, 150 };

    // Grille de fréquences du graphe : échelle logarithmique 20 Hz – 20 kHz,
    // identique à celle employée par la vue d'ensemble.
    private static readonly double[] CurveFreqs = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };

    // Plancher d'affichage de la courbe, aligné sur le YMin du graphe de crossover.
    private const double CurveFloorDb = -24.0;

    private readonly AppShellViewModel _shell;

    // Garde anti-réentrance : vrai pendant l'initialisation des contrôles depuis le
    // brouillon (Draft). Chaque gestionnaire d'événement commence par « if (_suspend)
    // return; » afin qu'une écriture programmatique (init ou notification Changed) ne
    // soit jamais réinjectée vers l'éditeur (pas de boucle de rétroaction).
    private bool _suspend;

    // Contrôles conservés pour le recalcul et l'affichage dynamique.
    private readonly CurveChart _xover = new() { YMin = -24, YMax = 0, MinHeight = 200 };
    private readonly ComboBox _freqCombo = new() { MinWidth = 132 };

    private readonly ToggleSwitch _bassManagement = new();
    private readonly ToggleSwitch _satHighPass = new();
    private readonly ToggleSwitch _sumToLfe = new();
    private readonly ToggleSwitch _limiter = new();

    private readonly Slider _lfeGain = new() { Minimum = -12, Maximum = 6, Value = 0, StepFrequency = 0.5 };
    private readonly TextBlock _lfeGainValue = new();
    private readonly Slider _ceiling = new() { Minimum = -3, Maximum = 0, Value = -1, StepFrequency = 0.1 };
    private readonly TextBlock _ceilingValue = new();

    private Brush B(string key) => (Brush)Application.Current.Resources[key];

    private Style S(string key) => (Style)Application.Current.Resources[key];

    public BassPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;
        Content = Build();

        // L'init depuis le brouillon et l'abonnement à l'éditeur se font au chargement
        // (DispatcherQueue disponible) ; l'abonnement est retiré au déchargement.
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    // ---------------------------------------------------------------------------
    private UIElement Build()
    {
        var root = new StackPanel
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
            Spacing = 18,
        };

        root.Children.Add(BuildHeader());
        root.Children.Add(BuildCrossoverCard());

        // Rangée de trois cartes : bass management, dosage du LFE, protection.
        var row = new Grid { ColumnSpacing = 18 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        Border bass = BuildBassManagementCard();
        Border lfe = BuildLfeCard();
        Border protection = BuildProtectionCard();
        Grid.SetColumn(lfe, 1);
        Grid.SetColumn(protection, 2);
        row.Children.Add(bass);
        row.Children.Add(lfe);
        row.Children.Add(protection);
        root.Children.Add(row);

        // Premier tracé de la courbe de crossover selon la fréquence sélectionnée.
        UpdateCrossoverCurve();

        return new ScrollViewer
        {
            Content = root,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ---------------------------------------------------------------------------
    private UIElement BuildHeader()
    {
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var titleStack = new StackPanel();
        titleStack.Children.Add(new TextBlock { Text = "Basses & crossover", Style = S("CockpitH1") });
        titleStack.Children.Add(new TextBlock
        {
            Text = "Crossover, bass management, dosage du LFE et protection contre la saturation.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        head.Children.Add(titleStack);

        Border chip = StatusChip();
        Grid.SetColumn(chip, 1);
        head.Children.Add(chip);
        return head;
    }

    // Puce d'état discrète : reflète l'état de connexion de l'agent (sans écriture).
    private Border StatusChip()
    {
        bool connected;
        try
        {
            connected = _shell.IsConnected;
        }
        catch
        {
            connected = false; // En dev, l'absence d'agent n'empêche pas l'affichage.
        }

        var inner = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9, VerticalAlignment = VerticalAlignment.Center };
        inner.Children.Add(new Ellipse
        {
            Width = 8,
            Height = 8,
            Fill = connected ? B("CockpitAccentBrush") : B("CockpitTextTertiaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        });
        inner.Children.Add(new TextBlock
        {
            Text = connected ? "Agent connecté" : "Mode démonstration",
            Foreground = B("CockpitTextSecondaryBrush"),
            FontSize = 13,
            VerticalAlignment = VerticalAlignment.Center,
        });

        return new Border
        {
            Background = B("CockpitSurface2Brush"),
            BorderBrush = B("CockpitBorderBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(9),
            Padding = new Thickness(14, 8, 14, 8),
            VerticalAlignment = VerticalAlignment.Center,
            Child = inner,
        };
    }

    // ---------------------------------------------------------------------------
    private Border BuildCrossoverCard()
    {
        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(new TextBlock { Text = "Crossover", Style = S("CockpitH2") });
        stack.Children.Add(new TextBlock
        {
            Text = "Passe-haut appliqué aux satellites, pente d'environ 12 dB/octave (2e ordre).",
            Style = S("CockpitCaption"),
            Margin = new Thickness(0, -4, 0, 2),
        });

        foreach (double f in CrossoverFreqs)
        {
            _freqCombo.Items.Add(new ComboBoxItem { Content = FormatHz(f) });
        }

        // Sélection par défaut posée AVANT l'abonnement : aucun déclenchement du
        // gestionnaire (donc aucune écriture vers l'éditeur) pendant la construction.
        // LoadFromDraft repositionnera ensuite la fréquence sur celle du brouillon.
        _freqCombo.SelectedIndex = 1; // 80 Hz par défaut (réglage THX usuel).
        _freqCombo.SelectionChanged += (_, _) =>
        {
            if (_suspend)
            {
                return;
            }

            int idx = _freqCombo.SelectedIndex;
            if (idx < 0)
            {
                return;
            }

            double frequency = idx < CrossoverFreqs.Length ? CrossoverFreqs[idx]
                : (double)((ComboBoxItem)_freqCombo.SelectedItem).Tag;
            _shell.Editor.SetCrossoverHz(frequency);
            UpdateCrossoverCurve(); // recalcul local de la courbe conservé
        };

        stack.Children.Add(FieldRow("Fréquence de coupure", _freqCombo));
        stack.Children.Add(_xover);
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private Border BuildBassManagementCard()
    {
        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(new TextBlock { Text = "Bass management", Style = S("CockpitH2") });

        // ToggleRow pose l'état initial AVANT l'abonnement Toggled ci-dessous : la valeur
        // de construction ne déclenche donc aucune écriture. LoadFromDraft réglera ensuite
        // chaque bascule depuis le brouillon (sous _suspend).
        stack.Children.Add(ToggleRow("Activer le bass management", _bassManagement, true));
        _bassManagement.Toggled += (_, _) =>
        {
            if (_suspend)
            {
                return;
            }
            _shell.Editor.SetBassManagementEnabled(_bassManagement.IsOn);
        };

        stack.Children.Add(ToggleRow("Passe-haut des satellites", _satHighPass, true));
        _satHighPass.Toggled += (_, _) =>
        {
            if (_suspend)
            {
                return;
            }
            _shell.Editor.SetHighPassSatellites(_satHighPass.IsOn);
        };

        stack.Children.Add(ToggleRow("Sommer les basses vers le LFE", _sumToLfe, true));
        _sumToLfe.Toggled += (_, _) =>
        {
            if (_suspend)
            {
                return;
            }
            _shell.Editor.SetSumToLfe(_sumToLfe.IsOn);
        };

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private Border BuildLfeCard()
    {
        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(new TextBlock { Text = "Dosage du LFE", Style = S("CockpitH2") });
        stack.Children.Add(new TextBlock
        {
            Text = "Niveau du canal LFE (.1) adressé au caisson de basses.",
            Style = S("CockpitCaption"),
            Margin = new Thickness(0, -4, 0, 2),
        });

        StyleValue(_lfeGainValue);
        // Le curseur couvre -12..+6 dB (visuel conservé), sous-plage de la borne éditeur
        // -20..+6 dB ; l'éditeur reclampe de toute façon, donc toute valeur du curseur est
        // valide. Le libellé est mis à jour ici et, à l'init, dans LoadFromDraft (sous
        // _suspend), jamais via ce gestionnaire.
        _lfeGain.ValueChanged += (_, e) =>
        {
            if (_suspend)
            {
                return;
            }
            _lfeGainValue.Text = FormatSigned(e.NewValue, "dB");
            _shell.Editor.SetLfeGainDb(e.NewValue);
        };
        _lfeGainValue.Text = FormatSigned(_lfeGain.Value, "dB");

        stack.Children.Add(FieldRow("Gain LFE", _lfeGainValue));
        stack.Children.Add(_lfeGain);
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private Border BuildProtectionCard()
    {
        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(new TextBlock { Text = "Protection", Style = S("CockpitH2") });

        // État initial posé avant l'abonnement (aucune écriture à la construction).
        stack.Children.Add(ToggleRow("Limiteur", _limiter, true));
        _limiter.Toggled += (_, _) =>
        {
            if (_suspend)
            {
                return;
            }
            _shell.Editor.SetLimiterEnabled(_limiter.IsOn);
        };

        StyleValue(_ceilingValue);
        // Plafond borné -12..0 dB côté éditeur ; le curseur (-3..0) reste une sous-plage.
        _ceiling.ValueChanged += (_, e) =>
        {
            if (_suspend)
            {
                return;
            }
            _ceilingValue.Text = FormatSigned(e.NewValue, "dBFS");
            _shell.Editor.SetLimiterCeilingDb(e.NewValue);
        };
        _ceilingValue.Text = FormatSigned(_ceiling.Value, "dBFS");

        stack.Children.Add(FieldRow("Plafond", _ceilingValue));
        stack.Children.Add(_ceiling);
        stack.Children.Add(new TextBlock
        {
            Text = "Évite la saturation du caisson.",
            Style = S("CockpitCaption"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // ---------------------------------------------------------------------------
    // Recalcule la réponse du passe-haut des satellites pour la fréquence de coupure
    // sélectionnée, puis la pousse dans le graphe via SetCurve.
    private void UpdateCrossoverCurve()
    {
        var bass = _shell.Editor.Draft.Output.BassManagement;
        double fc = bass.CrossoverHz;

        var values = new double[CurveFreqs.Length];
        for (int i = 0; i < CurveFreqs.Length; i++)
        {
            if(!bass.Enabled || !bass.HighPassSatellites) { values[i] = 0;continue; }
            // LR4 : -6 dB à la coupure, 24 dB/octave, comme le DSP réel.
            // La valeur est bornée au plancher de l'axe (YMin) pour rester lisible.
            double ratio = CurveFreqs[i] / fc;
            double r4 = ratio * ratio * ratio * ratio;
            double db = 20.0 * Math.Log10(r4 / (1.0 + r4));
            values[i] = db < CurveFloorDb ? CurveFloorDb : db;
        }

        _xover.SetCurve(CurveFreqs, values);
    }

    // ===========================================================================
    // Liaison avec l'éditeur de profil partagé (shell.Editor)
    // ===========================================================================

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Réabonnement sûr même si Loaded se répète (retrait puis ajout : une seule
        // souscription). L'éditeur partagé survit à la page, d'où le retrait au
        // déchargement pour éviter toute fuite.
        _shell.Editor.Changed -= OnEditorChanged;
        _shell.Editor.Changed += OnEditorChanged;
        LoadFromDraft();
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _shell.Editor.Changed -= OnEditorChanged;
    }

    // L'éditeur partagé lève Changed depuis le coalesceur d'applications (thread
    // hors-UI). On MARSHALE toujours vers le thread UI avant de toucher aux contrôles
    // WinUI, sinon RPC_E_WRONG_THREAD (même idiome que CalibrationPage / StatusPage).
    private void OnEditorChanged()
    {
        var q = DispatcherQueue;
        if (q is null || q.HasThreadAccess)
        {
            LoadFromDraft();
        }
        else
        {
            q.TryEnqueue(LoadFromDraft);
        }
    }

    // Initialise tous les contrôles depuis le brouillon (Draft.Output.BassManagement /
    // Limiter), sous la garde _suspend pour qu'aucune écriture ne soit réinjectée vers
    // l'éditeur. La courbe est recalculée après l'init par un appel DIRECT (hors
    // gestionnaire, que _suspend neutraliserait).
    private void LoadFromDraft()
    {
        _suspend = true;
        try
        {
            var bass = _shell.Editor.Draft.Output.BassManagement;
            var limiter = _shell.Editor.Draft.Output.Limiter;

            while(_freqCombo.Items.Count > CrossoverFreqs.Length) _freqCombo.Items.RemoveAt(CrossoverFreqs.Length);
            int selected = NearestCrossoverIndex(bass.CrossoverHz);
            if(Math.Abs(CrossoverFreqs[selected] - bass.CrossoverHz) > .001) {
                _freqCombo.Items.Add(new ComboBoxItem { Content = FormatHz(bass.CrossoverHz), Tag = bass.CrossoverHz });
                selected = CrossoverFreqs.Length;
            }
            _freqCombo.SelectedIndex = selected;
            _bassManagement.IsOn = bass.Enabled;
            _satHighPass.IsOn = bass.HighPassSatellites;
            _sumToLfe.IsOn = bass.SumToLfe;

            // Le curseur reclampe à sa propre plage ; on relit sa valeur pour le libellé
            // afin qu'il reflète ce qui est réellement affiché.
            _lfeGain.Value = bass.SumToLfeGainDb;
            _lfeGainValue.Text = FormatSigned(_lfeGain.Value, "dB");

            _limiter.IsOn = limiter.Enabled;
            _ceiling.Value = limiter.CeilingDb;
            _ceilingValue.Text = FormatSigned(_ceiling.Value, "dBFS");
        }
        finally
        {
            _suspend = false;
        }

        UpdateCrossoverCurve();
    }

    // Indice du choix de crossover le plus proche (en Hz) de la valeur du brouillon.
    private static int NearestCrossoverIndex(double hz)
    {
        int best = 0;
        double bestDiff = double.MaxValue;
        for (int i = 0; i < CrossoverFreqs.Length; i++)
        {
            double diff = Math.Abs(CrossoverFreqs[i] - hz);
            if (diff < bestDiff)
            {
                bestDiff = diff;
                best = i;
            }
        }
        return best;
    }

    // ---------------------------------------------------------------------------
    // Grille libellé / contrôle : libellé à gauche (colonne extensible), contrôle à droite.
    private Grid FieldRow(string label, FrameworkElement control)
    {
        var grid = new Grid { ColumnSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var lbl = new TextBlock
        {
            Text = label,
            Style = S("CockpitLabel"),
            VerticalAlignment = VerticalAlignment.Center,
            TextWrapping = TextWrapping.Wrap,
        };
        grid.Children.Add(lbl);

        control.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(control, 1);
        grid.Children.Add(control);
        return grid;
    }

    private Grid ToggleRow(string label, ToggleSwitch toggle, bool isOn)
    {
        toggle.OnContent = null;
        toggle.OffContent = null;
        toggle.IsOn = isOn;
        toggle.MinWidth = 0;
        return FieldRow(label, toggle);
    }

    private void StyleValue(TextBlock value)
    {
        value.Style = S("CockpitBody");
        value.FontWeight = Microsoft.UI.Text.FontWeights.SemiBold;
        value.Foreground = B("CockpitAccentBrush");
    }

    private static string FormatHz(double hz) =>
        ((long)Math.Round(hz)).ToString(CultureInfo.InvariantCulture) + " Hz";

    // Met en forme une valeur signée en dB : « +6 », « 0 », « -1.5 » (point invariant,
    // cohérent avec les étiquettes du graphe CurveChart).
    private static string FormatSigned(double value, string unit)
    {
        double r = Math.Round(value, 1);
        string num = r == Math.Round(r)
            ? ((long)Math.Round(r)).ToString(CultureInfo.InvariantCulture)
            : r.ToString("0.0", CultureInfo.InvariantCulture);
        return (r > 0 ? "+" + num : num) + " " + unit;
    }
}
