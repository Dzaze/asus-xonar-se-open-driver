// SPDX-License-Identifier: Apache-2.0
// Page « Vue d'ensemble » : vitrine du cockpit. Scène sonore 5.1 interactive, bouton de
// volume, vumètres post-DSP et courbes EQ/crossover.
//
// Les AFFICHEURS sont désormais RÉELS :
//   - Niveaux de sortie : crêtes PAR CANAL mesurées en temps réel sur l'endpoint de
//     RENDU 5.1 Xonar (WASAPI, post-DSP) via RenderAudioEndpoint, rafraîchies ~50 ms.
//   - Courbe Égaliseur : somme des réponses des bandes peaking actives du canal
//     sélectionné (Draft.Output.Eq), recalculée quand le brouillon ou le canal change.
//   - Courbe Crossover : passe-haut des satellites calculé depuis BassManagement.CrossoverHz.
//   - Format Windows : vrai format de mélange partagé de l'endpoint de rendu.
//   - « Tester les enceintes » : brève tonalité -20 dBFS (440 Hz) par canal via WASAPI.
//   - Presets Musique / Cinéma / Jeux : appliquent un réglage cohérent à l'éditeur.
// En l'absence d'endpoint de rendu, les afficheurs retombent proprement sur des valeurs
// d'exemple (ou l'état rapporté par l'agent), sans planter et sans élévation.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Threading;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Xonar.Contracts.V1;
using Xonar.ControlCenter.Controls;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Shell;
using Xonar.ControlCenter.Profiles;

namespace Xonar.ControlCenter.Pages;

public sealed class OverviewPage : UserControl
{
    private static readonly string[] Channels = { "FL", "FR", "FC", "LFE", "SL", "SR" };

    // Position d'affichage (index 0..5 dans Channels, ordre physique 0x60F : FL FR FC LFE SL SR)
    // -> valeur de l'enum xonar::dsp::Channel, qui indexe les tableaux OutputSettings (8 larges)
    // et que l'éditeur/APO attendent : FL=0 FR=1 FC=2 LFE=3 SL=6 SR=7. Les index 4/5 sont BL/BR,
    // absents du 5.1-côté : sans cette table, les réglages de surround partaient dans BL/BR et ne
    // sortaient jamais sur les enceintes SL/SR.
    private static readonly int[] DisplayToChannel = { 0, 1, 2, 3, 6, 7 };

    // Fragment de nom convivial recherché pour l'endpoint de rendu (voir RenderAudioEndpoint).
    private const string XonarNameFragment = "Xonar";

    // Grille de fréquences (Hz) des courbes : échelle log 20 Hz – 20 kHz, alignée sur les
    // graduations du CurveChart (10 nœuds, cohérent avec les pages Égaliseur et Basses).
    private static readonly double[] CurveFreqs = { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };

    // Séparateur décimal français pour les fréquences affichées (ex. « 48 kHz »).
    private static readonly NumberFormatInfo FrenchNumbers = new NumberFormatInfo { NumberDecimalSeparator = "," };

    // Plancher/plafond de l'échelle des vumètres (dBFS) et lissage du relâchement.
    private const double VuFloorDbfs = -60.0;
    private const double VuCeilDbfs = 0.0;
    private const double VuReleaseFactor = 0.30;
    private const int MeterIntervalMs = 50;

    // Ordre de test des enceintes (physique FL FR FC LFE SL SR), tonalité et durée.
    private static readonly int[] TestChannelOrder = { 0, 1, 2, 3, 4, 5 };
    private const double TestFrequencyHz = 440.0;
    private const double TestLevelDbfs = -20.0;
    private const double TestSecondsPerChannel = 0.4;

    private readonly AppShellViewModel _shell;
    private readonly VuMeterBars _vu = new();
    private readonly SpeakerScene _scene = new();
    private readonly VolumeKnob _knob = new() { Value = 62 };
    private readonly TextBlock _selLabel = new();
    private readonly NumberBox _selGain = new();
    private readonly NumberBox _selDelay = new();
    private readonly ToggleSwitch _selMute = new();

    // Courbes et sous-titres réels (promus en champs pour mise à jour dynamique).
    private readonly CurveChart _eq = new() { YMin = -12, YMax = 12, MinHeight = 150 };
    private readonly CurveChart _xover = new() { YMin = -24, YMax = 0, MinHeight = 150 };
    private readonly TextBlock _eqSubtitle = new();
    private readonly TextBlock _xoverSubtitle = new();

    // Ligne « Format Windows » (valeur mise à jour depuis le format de rendu réel).
    private readonly TextBlock _formatValue = new();
    private readonly TextBlock _modeDescription = new();
    private readonly TextBlock _protectionValue = new();
    private readonly TextBlock _headroomValue = new() { TextWrapping = TextWrapping.Wrap };
    private readonly Button _headroomApply = new() { Content = "Appliquer l’atténuation", IsEnabled = false };

    // Bouton « Tester les enceintes » + ligne d'état du test (annonce du canal).
    private readonly Button _testBtn;
    private readonly TextBlock _testStatus = new();

    // Garde anti-réentrance : vrai pendant qu'on recopie le brouillon dans les contrôles,
    // afin que les gestionnaires de contrôle n'appellent pas de mutateur en retour
    // (sinon boucles de synchronisation et écrasement des valeurs utilisateur).
    private bool _suspend;

    // Index du canal réglé par le panneau enceinte (ordre physique FL0 FR1 FC2 LFE3 SL4 SR5).
    private int _selectedChannel;

    // Sélecteur visuel des onglets de mode, capté à la construction du contrôle segmenté
    // (permet de pré-sélectionner l'onglet depuis le brouillon sans déclencher de mutateur).
    private Action<int>? _applyModeTab;
    private Border? _autoModeTab;

    // Endpoint de rendu 5.1 Xonar (WASAPI) : source réelle des niveaux et du format.
    private RenderAudioEndpoint? _render;
    private DispatcherQueueTimer? _meterTimer;
    private readonly float[] _peaks = new float[6];
    private readonly double[] _vuDb = new double[6];
    private double _mixSampleRate; // Hz réels du format de rendu (0 si inconnu -> 48 000 par défaut)

    // Test des enceintes : exécution asynchrone annulable, une seule à la fois.
    private CancellationTokenSource? _testCts;
    private bool _testRunning;

    private Brush B(string key) => (Brush)Application.Current.Resources[key];
    private Style S(string key) => (Style)Application.Current.Resources[key];

    public OverviewPage(AppShellViewModel shell)
    {
        _shell = shell;
        _testBtn = IconButton("", "Tester les enceintes");
        _testBtn.Click += (_, _) => OnTestSpeakers();

        // Niveaux affichés initialisés sur les valeurs d'exemple (repli sans endpoint).
        Array.Fill(_vuDb, VuFloorDbfs);

        Content = Build();
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    private UIElement Build()
    {
        var root = new Grid
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            MinWidth = 1040,
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });   // en-tête
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });   // onglets
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1.3, GridUnitType.Star) }); // scène + volume
        root.RowDefinitions.Add(new RowDefinition { Height = new GridLength(1, GridUnitType.Star) });   // bas

        // ---------- En-tête ----------
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var titleStack = new StackPanel();
        titleStack.Children.Add(new TextBlock { Text = "Votre espace sonore", Style = S("CockpitH1") });
        _modeDescription.Style = S("CockpitLabel");_modeDescription.Margin = new Thickness(0, 2, 0, 0);
        titleStack.Children.Add(_modeDescription);
        head.Children.Add(titleStack);
        var device = DeviceChip("Xonar SE");
        Grid.SetColumn(device, 1);
        head.Children.Add(device);
        root.Children.Add(head);

        // ---------- Onglets de mode + presets ----------
        var tabs = new Grid { Margin = new Thickness(0, 20, 0, 0) };
        tabs.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        tabs.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        tabs.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        tabs.Children.Add(Segmented(
            new[] { "5.1", "Upmix", "2.1", "2.0", "Auto" },
            ModeToIndex(_shell.Editor.Draft.ListeningMode),
            OnModeSelected,
            apply => _applyModeTab = apply));
        var presets = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, HorizontalAlignment = HorizontalAlignment.Right };
        presets.Children.Add(PresetButton("Musique", () => ApplyPreset(Preset.Musique)));
        presets.Children.Add(PresetButton("Cinéma", () => ApplyPreset(Preset.Cinema)));
        presets.Children.Add(PresetButton("Jeux", () => ApplyPreset(Preset.Jeux)));
        Grid.SetColumn(presets, 2);
        tabs.Children.Add(presets);
        Grid.SetRow(tabs, 1);
        root.Children.Add(tabs);

        // ---------- Scène + réglage enceinte + volume ----------
        var mid = new Grid { Margin = new Thickness(0, 18, 0, 0) };
        mid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        mid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(300) });

        // Scène sonore (carte)
        var sceneHead = new Grid();
        sceneHead.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        sceneHead.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var sceneTitle = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        sceneTitle.Children.Add(new TextBlock { Text = "Scène sonore", Style = S("CockpitH2") });
        _testStatus.Style = S("CockpitCaption");
        _testStatus.Foreground = B("CockpitAccentBrush");
        _testStatus.Visibility = Visibility.Collapsed;
        _testStatus.Margin = new Thickness(0, 2, 0, 0);
        sceneTitle.Children.Add(_testStatus);
        sceneHead.Children.Add(sceneTitle);
        _testBtn.VerticalAlignment = VerticalAlignment.Top;
        Grid.SetColumn(_testBtn, 1);
        sceneHead.Children.Add(_testBtn);

        var sceneInner = new Grid();
        sceneInner.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        sceneInner.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        _scene.MinHeight = 300;
        _scene.SelectedChannel = "FL";
        _scene.SpeakerSelected += (_, code) => UpdateSelectedSpeaker(code);
        sceneInner.Children.Add(_scene);
        var selPanel = SelectedSpeakerPanel();
        Grid.SetColumn(selPanel, 1);
        sceneInner.Children.Add(selPanel);

        var sceneContent = new StackPanel { Spacing = 14 };
        sceneContent.Children.Add(sceneHead);
        sceneContent.Children.Add(sceneInner);
        var sceneCard = new Border { Style = S("CockpitCard"), Child = sceneContent, Margin = new Thickness(0, 0, 18, 0) };
        mid.Children.Add(sceneCard);

        // Volume (carte)
        var volCard = VolumeCard();
        Grid.SetColumn(volCard, 1);
        mid.Children.Add(volCard);

        Grid.SetRow(mid, 2);
        root.Children.Add(mid);

        // ---------- Bas : vumètres, EQ, crossover ----------
        var bottom = new Grid { Margin = new Thickness(0, 18, 0, 0) };
        for (int i = 0; i < 3; i++)
        {
            bottom.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(i == 0 ? 1.1 : 1.0, GridUnitType.Star) });
        }

        // Niveaux de sortie : valeurs d'exemple au départ, remplacées en temps réel au Loaded.
        _vu.SetLevels(Channels, _vuDb);
        var vuCard = Card("Niveaux de sortie (dBFS)", _vu, "Niveaux RMS mesurés après DSP · FL FR FC LFE SL SR.");
        vuCard.Margin = new Thickness(0, 0, 9, 0);
        bottom.Children.Add(vuCard);

        var eqCard = CurveCard("Égaliseur", _eq, _eqSubtitle, "Plat (aucune bande active).");
        eqCard.Margin = new Thickness(9, 0, 9, 0);
        Grid.SetColumn(eqCard, 1);
        bottom.Children.Add(eqCard);

        var xoverCard = CurveCard("Crossover", _xover, _xoverSubtitle, "Satellites pleine bande (passe-haut inactif).");
        xoverCard.Margin = new Thickness(9, 0, 0, 0);
        Grid.SetColumn(xoverCard, 2);
        bottom.Children.Add(xoverCard);

        Grid.SetRow(bottom, 3);
        root.Children.Add(bottom);

        // Format Windows initial (exemple) ; affiné au Loaded depuis le format de rendu réel.
        _formatValue.Text = "6 canaux · 24 bits · 48 kHz";
        _formatValue.Style = S("CockpitBody");

        // Premier tracé des courbes (depuis le brouillon ; valeurs réelles, non d'exemple).
        RefreshEqCurve();
        RefreshCrossoverCurve();

        return new ScrollViewer { Content = root, VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto, HorizontalContentAlignment = HorizontalAlignment.Stretch };
    }

    // ===========================================================================
    // Cycle de vie : métrologie temps réel et abonnement au brouillon partagé.
    // ===========================================================================
    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        // Pré-remplit les contrôles depuis le brouillon partagé (sous _suspend).
        InitializeFromDraft();

        // Recalcule les courbes à chaque modification du brouillon (abonnement idempotent).
        _shell.Editor.Changed -= OnEditorChanged;
        _shell.Editor.Changed += OnEditorChanged;
        RefreshEqCurve();
        RefreshCrossoverCurve();

        // Endpoint de rendu réel : niveaux par canal + format. Résolution paresseuse.
        _render ??= RenderAudioEndpoint.Resolve(XonarNameFragment);
        bool formatFromRender = ApplyRenderFormat();
        bool realMeter = _render is not null && _render.MeteringChannelCount > 0;
        StartMeterIfPossible();

        // Repli agent : niveaux (si pas de vumètre de rendu) et/ou format (si indisponible).
        if (!realMeter || !formatFromRender)
        {
            await LoadStateAsync(applyMeters: !realMeter, applyFormat: !formatFromRender);
        }
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _shell.Editor.Changed -= OnEditorChanged;
        StopMeter();
        CancelTest();
    }

    // L'éditeur partagé lève Changed depuis le coalesceur d'applications (thread hors-UI) ;
    // on marshale toujours vers le thread UI avant de toucher aux contrôles WinUI.
    private void OnEditorChanged()
    {
        void Refresh() { InitializeFromDraft();RefreshCurves(); }
        var q = DispatcherQueue;
        if (q is null || q.HasThreadAccess)
        {
            Refresh();
        }
        else
        {
            q.TryEnqueue(Refresh);
        }
    }

    private void RefreshCurves()
    {
        RefreshEqCurve();
        RefreshCrossoverCurve();
    }

    // ---------------------------------------------------------------------------
    private Border Card(string title, UIElement content, string? subtitle = null)
    {
        var stack = new StackPanel { Spacing = 10 };
        stack.Children.Add(new TextBlock { Text = title, Style = S("CockpitH2") });
        if (subtitle is not null)
        {
            stack.Children.Add(new TextBlock { Text = subtitle, Style = S("CockpitCaption"), Margin = new Thickness(0, -6, 0, 2) });
        }
        stack.Children.Add(content);
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // Carte de courbe avec sous-titre dynamique (TextBlock conservé pour mise à jour).
    private Border CurveCard(string title, UIElement chart, TextBlock subtitle, string initialSubtitle)
    {
        subtitle.Text = initialSubtitle;
        subtitle.Style = S("CockpitCaption");
        subtitle.Margin = new Thickness(0, -6, 0, 2);

        var stack = new StackPanel { Spacing = 10 };
        stack.Children.Add(new TextBlock { Text = title, Style = S("CockpitH2") });
        stack.Children.Add(subtitle);
        stack.Children.Add(chart);
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private Border DeviceChip(string name)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        row.Children.Add(new FontIcon { Glyph = "", FontSize = 15, Foreground = B("CockpitAccentBrush") });
        row.Children.Add(new TextBlock { Text = name, Foreground = B("CockpitTextPrimaryBrush"), FontSize = 14, VerticalAlignment = VerticalAlignment.Center });
        row.Children.Add(new FontIcon { Glyph = "", FontSize = 11, Foreground = B("CockpitTextTertiaryBrush") });
        return new Border
        {
            Background = B("CockpitSurface2Brush"),
            BorderBrush = B("CockpitBorderBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(9),
            Padding = new Thickness(14, 8, 14, 8),
            VerticalAlignment = VerticalAlignment.Center,
            Child = row,
        };
    }

    private Border Segmented(string[] options, int selectedIndex, Action<int>? onSelect = null, Action<Action<int>>? captureSelector = null)
    {
        var inner = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };
        var buttons = new List<Border>();

        // Applique la sélection (visuel seulement) : n'invoque aucun mutateur, donc
        // réutilisable pour la pré-sélection depuis le brouillon sans boucle de rétroaction.
        void ApplySelection(int idx)
        {
            for (int k = 0; k < buttons.Count; k++)
            {
                bool on = k == idx;
                buttons[k].Background = on ? B("CockpitAccentBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent);
                ((TextBlock)buttons[k].Child).Foreground = on ? new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(255, 7, 23, 26)) : B("CockpitTextSecondaryBrush");
            }
        }

        for (int i = 0; i < options.Length; i++)
        {
            int idx = i;
            var label = new TextBlock { Text = options[i], FontSize = 14, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center, HorizontalAlignment = HorizontalAlignment.Center };
            var seg = new Border { CornerRadius = new CornerRadius(8), Padding = new Thickness(26, 9, 26, 9), Child = label };
            if(options[i] == "Auto") _autoModeTab = seg;
            buttons.Add(seg);
            seg.Tapped += (_, _) =>
            {
                if(options[idx] == "Auto" && _shell.Capabilities?.Features.AutoDeclaredLayout != true) return;
                ApplySelection(idx);
                onSelect?.Invoke(idx); // le mutateur (OnModeSelected) porte lui-même la garde _suspend.
            };
            inner.Children.Add(seg);
        }

        // État visuel initial, puis exposition du sélecteur à l'hôte (pré-sélection au Loaded).
        ApplySelection(selectedIndex);
        captureSelector?.Invoke(ApplySelection);

        return new Border
        {
            Background = B("CockpitSurfaceInsetBrush"),
            BorderBrush = B("CockpitBorderSubtleBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(5),
            HorizontalAlignment = HorizontalAlignment.Left,
            Child = inner,
        };
    }

    private Button IconButton(string glyph, string text)
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9 };
        // Icône optionnelle : omise si le glyphe est vide (bouton texte, comme la page Égaliseur).
        if (!string.IsNullOrEmpty(glyph))
        {
            row.Children.Add(new FontIcon { Glyph = glyph, FontSize = 14 });
        }
        row.Children.Add(new TextBlock { Text = text, VerticalAlignment = VerticalAlignment.Center });
        return new Button { Style = S("CockpitGhostButton"), Content = row };
    }

    // Bouton de preset (style fantôme) câblé à un gestionnaire d'application de preset.
    private Button PresetButton(string text, Action onClick)
    {
        var button = IconButton("", text);
        button.Click += (_, _) => onClick();
        return button;
    }

    private Border SelectedSpeakerPanel()
    {
        _selLabel.Text = "Avant gauche (FL)";
        _selLabel.Style = S("CockpitH2");
        _selGain.Value = -1.5; _selGain.SmallChange = 0.5; _selGain.Minimum = -60; _selGain.Maximum = 6; _selGain.SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact; _selGain.Width = 116;
        _selDelay.Value = 2.0; _selDelay.SmallChange = 0.5; _selDelay.Minimum = 0; _selDelay.Maximum = 50; _selDelay.SpinButtonPlacementMode = NumberBoxSpinButtonPlacementMode.Compact; _selDelay.Width = 116;

        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        grid.RowSpacing = 14; grid.ColumnSpacing = 10;

        AddRow(grid, 0, "Gain", _selGain);
        AddRow(grid, 1, "Délai", _selDelay);
        var muteLbl = new TextBlock { Text = "Muet", Style = S("CockpitLabel"), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetRow(muteLbl, 2); grid.Children.Add(muteLbl);
        Grid.SetRow(_selMute, 2); Grid.SetColumn(_selMute, 1); _selMute.OnContent = null; _selMute.OffContent = null; grid.Children.Add(_selMute);

        var stack = new StackPanel { Spacing = 14, Width = 232 };
        stack.Children.Add(_selLabel);
        stack.Children.Add(grid);
        stack.Children.Add(new Border { Height = 1, Background = B("CockpitBorderSubtleBrush"), Margin = new Thickness(0, 4, 0, 4) });
        var test = new StackPanel();
        test.Children.Add(new TextBlock { Text = "Niveau de test", Style = S("CockpitLabel") });
        test.Children.Add(new TextBlock { Text = "-20 dBFS", Style = S("CockpitBody"), Margin = new Thickness(0, 2, 0, 0) });
        stack.Children.Add(test);

        // --- Câblage des contrôles vers l'éditeur : ils agissent sur le canal sélectionné. ---
        _selGain.ValueChanged += (_, args) =>
        {
            // NumberBox lève aussi ValueChanged lors de nos écritures (d'où _suspend) et
            // peut rendre NaN quand le champ est vidé (à ignorer, sinon stocké tel quel).
            if (_suspend || double.IsNaN(args.NewValue))
            {
                return;
            }
            _shell.Editor.SetChannelGainDb(DisplayToChannel[_selectedChannel], args.NewValue);
        };
        _selDelay.ValueChanged += (_, args) =>
        {
            if (_suspend || double.IsNaN(args.NewValue))
            {
                return;
            }
            _shell.Editor.SetChannelDelayMs(DisplayToChannel[_selectedChannel], args.NewValue);
        };
        _selMute.Toggled += (_, _) =>
        {
            if (_suspend)
            {
                return;
            }
            bool muted = _selMute.IsOn;
            _shell.Editor.SetChannelMute(DisplayToChannel[_selectedChannel], muted);
            _scene.SetMuted(Channels[_selectedChannel], muted); // reflet visuel dans la scène (position d'affichage).
        };

        return new Border
        {
            Background = B("CockpitSurfaceInsetBrush"),
            BorderBrush = B("CockpitBorderSubtleBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(18),
            Margin = new Thickness(16, 0, 0, 0),
            VerticalAlignment = VerticalAlignment.Top,
            Child = stack,
        };
    }

    private void AddRow(Grid grid, int row, string label, FrameworkElement control)
    {
        var lbl = new TextBlock { Text = label, Style = S("CockpitLabel"), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetRow(lbl, row); grid.Children.Add(lbl);
        Grid.SetRow(control, row); Grid.SetColumn(control, 1); grid.Children.Add(control);
    }

    private Border VolumeCard()
    {
        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(new TextBlock { Text = "Volume de sortie", Style = S("CockpitH2") });
        stack.Children.Add(DeviceChip("Haut-parleurs"));
        _knob.MinHeight = 190;
        _knob.ValueChanged += (_, value) =>
        {
            if (_suspend)
            {
                return;
            }
            // Mapping linéaire molette 0..100 -> -60..0 dB (gain maître).
            _shell.Editor.SetMasterGainDb(-60.0 + value * 0.6);
        };
        stack.Children.Add(_knob);

        var info = new StackPanel { Spacing = 10, Margin = new Thickness(0, 6, 0, 0) };
        info.Children.Add(InfoLine("Format Windows", _formatValue));
        info.Children.Add(InfoLine("Traitement", "DSP live · modes sur transport 6 canaux"));
        info.Children.Add(InfoLine("Protection", _protectionValue));
        info.Children.Add(InfoLine("Marge DSP", _headroomValue));
        _headroomApply.Style = S("CockpitGhostButton");
        _headroomApply.Click += (_, _) => {
            if(_shell.Client is not LiveApoBroker live || _shell.Editor.State != ProfileEditorState.Applied || !live.IsLastProfileApplied()) return;
            var advice = HeadroomAdvice.From(live.Telemetry);
            if(advice.AdjustmentDb < 0) {
                _headroomApply.IsEnabled = false;
                _shell.Editor.SetMasterGainDb(advice.ApplyTo(_shell.Editor.Draft.Output.MasterGainDb));
            }
        };
        info.Children.Add(_headroomApply);
        stack.Children.Add(info);

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private StackPanel InfoLine(string label, string value)
    {
        var s = new StackPanel { Spacing = 1 };
        s.Children.Add(new TextBlock { Text = label, Style = S("CockpitLabel") });
        s.Children.Add(new TextBlock { Text = value, Style = S("CockpitBody") });
        return s;
    }

    // Variante avec TextBlock fourni (valeur mise à jour dynamiquement, ex. format réel).
    private StackPanel InfoLine(string label, TextBlock value)
    {
        var s = new StackPanel { Spacing = 1 };
        s.Children.Add(new TextBlock { Text = label, Style = S("CockpitLabel") });
        s.Children.Add(value);
        return s;
    }

    // ===========================================================================
    // Niveaux de sortie réels (vumètre par canal, post-DSP, via l'endpoint de rendu)
    // ===========================================================================
    private void StartMeterIfPossible()
    {
        if (_render is null || _render.MeteringChannelCount <= 0)
        {
            StopMeter();
            return;
        }

        _meterTimer ??= CreateMeterTimer();
        if (!_meterTimer.IsRunning)
        {
            _meterTimer.Start();
        }
    }

    private void StopMeter()
    {
        try
        {
            _meterTimer?.Stop();
        }
        catch
        {
            // Arrêt best-effort : sans conséquence si le minuteur n'était pas démarré.
        }
    }

    private DispatcherQueueTimer CreateMeterTimer()
    {
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(MeterIntervalMs);
        timer.IsRepeating = true;
        timer.Tick += (_, _) => OnMeterTick();
        return timer;
    }

    // Un tick : lit les crêtes par canal, convertit en dBFS (20·log10, borné -60..0), lisse
    // le relâchement et rafraîchit les barres (ordre physique FL FR FC LFE SL SR).
    private void OnMeterTick()
    {
        var dsp = (_shell.Client as LiveApoBroker)?.Telemetry;
        var advice = HeadroomAdvice.From(dsp);
        _headroomValue.Text = advice.Message;
        _headroomValue.Foreground = B(advice.AdjustmentDb < 0 ? "CockpitWarningBrush" : "CockpitTextSecondaryBrush");
        _headroomApply.IsEnabled = advice.AdjustmentDb < 0 && _shell.Editor.State == ProfileEditorState.Applied &&
            (_shell.Client as LiveApoBroker)?.IsLastProfileApplied() == true &&
            advice.ApplyTo(_shell.Editor.Draft.Output.MasterGainDb) < _shell.Editor.Draft.Output.MasterGainDb;
        int n = Math.Min(dsp?.OutputRms.Length ?? 0, _peaks.Length);
        if (n > 0) Array.Copy(dsp!.OutputRms, _peaks, n);
        for (int i = 0; i < _vuDb.Length; i++)
        {
            double amplitude = i < n ? _peaks[i] : 0.0;
            double target = amplitude > 0.0
                ? Math.Clamp(20.0 * Math.Log10(amplitude), VuFloorDbfs, VuCeilDbfs)
                : VuFloorDbfs;

            // Attaque instantanée (montée), relâchement progressif (descente plus lisible).
            _vuDb[i] = target >= _vuDb[i]
                ? target
                : _vuDb[i] + (target - _vuDb[i]) * VuReleaseFactor;
        }

        _vu.SetLevels(Channels, _vuDb);
    }

    public void RefreshAudioEndpoint()
    {
        if(_testRunning) return;
        _render?.Dispose();
        _render = RenderAudioEndpoint.Resolve(XonarNameFragment);
        if(!ApplyRenderFormat()) _formatValue.Text = "Sortie Xonar indisponible";
        _meterTimer ??= CreateMeterTimer();
        if(IsLoaded) _meterTimer.Start();
    }

    // ===========================================================================
    // Format Windows réel (format de mélange partagé de l'endpoint de rendu)
    // ===========================================================================
    private bool ApplyRenderFormat()
    {
        if (_render is null)
        {
            return false;
        }

        if (_render.TryGetMixFormat(out int channels, out int bits, out uint rate, out bool isFloat))
        {
            _mixSampleRate = rate;
            _formatValue.Text = DescribeFormat(channels, bits, rate, isFloat);
            RefreshEqCurve(); // recalcule la courbe EQ à la fréquence d'échantillonnage réelle.
            return true;
        }

        return false;
    }

    private static string DescribeFormat(int channels, int bits, uint sampleRate, bool isFloat)
    {
        string bitsText = bits.ToString(CultureInfo.InvariantCulture) + " bits" + (isFloat ? " (flottant)" : string.Empty);
        double khz = sampleRate / 1000.0;
        string rateText = khz.ToString("0.###", FrenchNumbers) + " kHz";
        return channels.ToString(CultureInfo.InvariantCulture) + " canaux · " + bitsText + " · " + rateText;
    }

    // ===========================================================================
    // Courbes réelles : Égaliseur (somme des bandes peaking) et Crossover (passe-haut)
    // ===========================================================================

    // Recalcule la courbe EQ du canal sélectionné = somme (en dB) des réponses des bandes
    // peaking ACTIVES, évaluée sur la grille de fréquences, bornée à [-12, +12] dB.
    private void RefreshEqCurve()
    {
        double fs = _mixSampleRate > 0 ? _mixSampleRate : 48000.0;
        int channel = DisplayToChannel[Math.Clamp(_selectedChannel, 0, DisplayToChannel.Length - 1)];
        EqBand[] bands = _shell.Editor.Draft.Output.Eq[channel];

        int active = 0;
        var values = new double[CurveFreqs.Length];
        for (int i = 0; i < CurveFreqs.Length; i++)
        {
            double sumDb = 0.0;
            foreach (EqBand band in bands)
            {
                if (band.Enabled && band.GainDb != 0.0)
                {
                    sumDb += PeakingResponseDb(CurveFreqs[i], band.FrequencyHz, band.Q, band.GainDb, fs);
                }
            }
            values[i] = Math.Clamp(sumDb, -12.0, 12.0);
        }

        foreach (EqBand band in bands)
        {
            if (band.Enabled)
            {
                active++;
            }
        }

        _eq.SetCurve(CurveFreqs, values);
        _eqSubtitle.Text = active > 0
            ? ChannelLabel(Channels[Math.Clamp(_selectedChannel, 0, Channels.Length - 1)]) + " · "
                + active.ToString(CultureInfo.InvariantCulture) + (active > 1 ? " bandes actives." : " bande active.")
            : "Plat (aucune bande active).";
    }

    // Réponse en dB d'un filtre peaking (cloche) biquad RBJ à la fréquence f (Hz).
    private static double PeakingResponseDb(double f, double f0, double q, double gainDb, double fs)
    {
        if (q <= 0.0 || f0 <= 0.0 || fs <= 0.0)
        {
            return 0.0;
        }

        double a = Math.Pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * Math.PI * f0 / fs;
        double cosw0 = Math.Cos(w0);
        double alpha = Math.Sin(w0) / (2.0 * q);

        double b0 = 1.0 + alpha * a;
        double b1 = -2.0 * cosw0;
        double b2 = 1.0 - alpha * a;
        double a0 = 1.0 + alpha / a;
        double a1 = -2.0 * cosw0;
        double a2 = 1.0 - alpha / a;

        // |H(e^jw)| évalué via la partie réelle/imaginaire du numérateur et du dénominateur.
        double w = 2.0 * Math.PI * f / fs;
        double cosw = Math.Cos(w);
        double cos2w = Math.Cos(2.0 * w);
        double sinw = Math.Sin(w);
        double sin2w = Math.Sin(2.0 * w);

        double numRe = b0 + b1 * cosw + b2 * cos2w;
        double numIm = -(b1 * sinw + b2 * sin2w);
        double denRe = a0 + a1 * cosw + a2 * cos2w;
        double denIm = -(a1 * sinw + a2 * sin2w);

        double numMagSq = numRe * numRe + numIm * numIm;
        double denMagSq = denRe * denRe + denIm * denIm;
        if (denMagSq <= 0.0 || numMagSq <= 0.0)
        {
            return 0.0;
        }

        return 10.0 * Math.Log10(numMagSq / denMagSq);
    }

    // Recalcule la courbe de crossover = passe-haut Butterworth 2e ordre des satellites à la
    // fréquence de coupure du brouillon (si le bass management et le passe-haut sont actifs).
    private void RefreshCrossoverCurve()
    {
        BassManagementSettings bm = _shell.Editor.Draft.Output.BassManagement;
        double fc = Math.Clamp(bm.CrossoverHz, 40.0, 200.0);
        bool active = bm.Enabled && bm.HighPassSatellites;
        const double floorDb = -24.0;

        var values = new double[CurveFreqs.Length];
        for (int i = 0; i < CurveFreqs.Length; i++)
        {
            if (!active)
            {
                values[i] = 0.0; // satellites pleine bande
                continue;
            }

            // LR4 : deux Butterworth du 2e ordre, -6 dB à fc, 24 dB/octave.
            double ratio = CurveFreqs[i] / fc;
            double r4 = ratio * ratio * ratio * ratio;
            double db = 20.0 * Math.Log10(r4 / (1.0 + r4));
            values[i] = db < floorDb ? floorDb : db;
        }

        _xover.SetCurve(CurveFreqs, values);
        _xoverSubtitle.Text = active
            ? "Passe-haut satellites · " + ((long)Math.Round(fc)).ToString(CultureInfo.InvariantCulture) + " Hz."
            : "Satellites pleine bande (passe-haut inactif).";
    }

    // ===========================================================================
    // Presets Musique / Cinéma / Jeux (appliqués au brouillon partagé)
    // ===========================================================================
    private enum Preset
    {
        Musique,
        Cinema,
        Jeux,
    }

    // Applique un preset sobre et cohérent à l'éditeur. Chaque preset règle le mode d'écoute,
    // quelques bandes EQ (canal avant G/D) et le bass management ; rien de destructif, tout
    // passe par les mutateurs coalescés de l'éditeur. Documenté canal par canal ci-dessous.
    private void ApplyPreset(Preset preset)
    {
        _shell.Editor.LoadProfile(ListeningPresets.Create(_shell.Editor.Draft, (int)preset));

        // Resynchronise les contrôles et les courbes sur le nouveau brouillon.
        InitializeFromDraft();
        RefreshCurves();
    }

    // Désactive et remet à plat toutes les bandes EQ de TOUS les canaux (0..7), pas seulement
    // 0..5 : sinon une EQ posée sur SL/SR (enum 6/7) survivrait à l'application d'un préréglage.
    private void ClearEq()
    {
        var editor = _shell.Editor;
        for (int ch = 0; ch < (int)Constants.MaxChannels; ch++)
        {
            for (int b = 0; b < (int)Constants.EqBandsPerChannel; b++)
            {
                editor.SetEqEnabled(ch, b, false);
                editor.SetEqGain(ch, b, 0.0);
            }
        }
    }

    // Active et règle une bande EQ (fréquence, Q, gain) sur un canal donné.
    private void SetEqBand(int channel, int band, double frequencyHz, double q, double gainDb)
    {
        var editor = _shell.Editor;
        editor.SetEqEnabled(channel, band, true);
        editor.SetEqFrequency(channel, band, frequencyHz);
        editor.SetEqQ(channel, band, q);
        editor.SetEqGain(channel, band, gainDb);
    }

    // ===========================================================================
    // Tester les enceintes (brève tonalité par canal via WASAPI, -20 dBFS)
    // ===========================================================================
    private async void OnTestSpeakers()
    {
        await RunSpeakerTestAsync();
    }

    public async Task<string> RunSpeakerTestAsync()
    {
        if (_testRunning)
        {
            return "Test déjà en cours.";
        }

        _render ??= RenderAudioEndpoint.Resolve(XonarNameFragment);
        if (_render is null)
        {
            SetTestStatus("Endpoint de rendu Xonar introuvable.");
            return "Endpoint de rendu Xonar introuvable.";
        }

        _testRunning = true;
        _testBtn.IsEnabled = false;
        SetTestStatus("Test en cours…");
        _testCts?.Dispose();
        _testCts = new CancellationTokenSource();

        bool ok = false;
        var previousMode = _shell.Editor.Draft.ListeningMode;
        try
        {
            // Localiser les enceintes indépendamment du downmix/upmix d'écoute.
            // Le profil utilisateur est restauré dans finally, même après annulation.
            if (previousMode != ListeningMode.Surround51) {
                _shell.Editor.SetListeningMode(ListeningMode.Surround51);
                await _shell.Editor.ApplyNowAsync();
            }
            ok = await _render.PlayChannelTestSequenceAsync(
                TestChannelOrder, OnTestChannel, TestSecondsPerChannel, TestFrequencyHz, TestLevelDbfs, _testCts.Token);
        }
        catch
        {
            ok = false;
        }
        finally
        {
            if (previousMode != ListeningMode.Surround51) {
                _shell.Editor.SetListeningMode(previousMode);
                await _shell.Editor.ApplyNowAsync();
            }
        }

        _testRunning = false;
        _testBtn.IsEnabled = true;
        // Restaure la mise en valeur de l'enceinte réellement sélectionnée par l'utilisateur.
        _scene.SelectedChannel = Channels[Math.Clamp(_selectedChannel, 0, Channels.Length - 1)];
        string result = ok ? "Test terminé. Mode d’écoute restauré."
            : "Test indisponible : " + _render.LastTestError;
        SetTestStatus(result);
        return result;
    }

    // Annonce du canal en cours (appelé depuis le thread de rendu : marshalage UI requis).
    private void OnTestChannel(int index)
    {
        void Apply()
        {
            if (index >= 0 && index < Channels.Length)
            {
                SetTestStatus("Test : " + ChannelLabel(Channels[index]));
                _scene.SelectedChannel = Channels[index]; // met en valeur l'enceinte testée
            }
        }

        var q = DispatcherQueue;
        if (q is null || q.HasThreadAccess)
        {
            Apply();
        }
        else
        {
            q.TryEnqueue(Apply);
        }
    }

    private void SetTestStatus(string text)
    {
        _testStatus.Text = text;
        _testStatus.Visibility = string.IsNullOrEmpty(text) ? Visibility.Collapsed : Visibility.Visible;
    }

    private void CancelTest()
    {
        try
        {
            _testCts?.Cancel();
        }
        catch
        {
            // Annulation best-effort : la séquence s'arrête au prochain réveil.
        }
    }

    // ---------------------------------------------------------------------------
    private void UpdateSelectedSpeaker(string code)
    {
        _scene.SelectedChannel = code;
        _selectedChannel = ChannelIndex(code);
        _selLabel.Text = ChannelLabel(code);

        // Recharge Gain/Délai/Muet du canal nouvellement sélectionné depuis le brouillon.
        LoadChannelControls(_selectedChannel);

        // La courbe EQ reflète le canal sélectionné.
        RefreshEqCurve();
    }

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

    // Code de canal -> POSITION D'AFFICHAGE dans Channels (FL0 FR1 FC2 LFE3 SL4 SR5), pas l'index
    // de l'enum Channel : passer par DisplayToChannel avant d'indexer les tableaux OutputSettings.
    private static int ChannelIndex(string code) => code switch
    {
        "FL" => 0,
        "FR" => 1,
        "FC" => 2,
        "LFE" => 3,
        "SL" => 4,
        "SR" => 5,
        _ => 0,
    };

    // Onglet de mode -> mode d'écoute du contrat et réciproque.
    private int ModeToIndex(ListeningMode mode) =>
        _shell.Editor.Draft.SourcePolicy == SourcePolicy.AutoDeclaredLayout && mode == ListeningMode.Surround51 ? 4 : mode switch
    {
        ListeningMode.Surround51 => 0,
        ListeningMode.Upmix => 1,
        ListeningMode.TwoPointOne => 2,
        ListeningMode.Stereo20 => 3,
        _ => 0,
    };

    private void OnModeSelected(int index)
    {
        if (_suspend)
        {
            return;
        }
        _shell.Editor.SetSourcePolicy(index == 4 ? SourcePolicy.AutoDeclaredLayout : SourcePolicy.Preserve);
        _shell.Editor.SetListeningMode(index switch
        {
            0 => ListeningMode.Surround51,
            1 => ListeningMode.Upmix,
            2 => ListeningMode.TwoPointOne,
            3 => ListeningMode.Stereo20,
            _ => ListeningMode.Surround51,
        });
    }

    // Recopie Gain/Délai/Muet du canal donné depuis le brouillon vers les contrôles.
    // Sauvegarde/restaure _suspend : reste correct même appelé depuis une init déjà suspendue.
    private void LoadChannelControls(int displayPos)
    {
        var output = _shell.Editor.Draft.Output;
        int channel = DisplayToChannel[Math.Clamp(displayPos, 0, DisplayToChannel.Length - 1)];
        bool previous = _suspend;
        _suspend = true;
        try
        {
            _selGain.Value = output.ChannelGainDb[channel];
            _selDelay.Value = output.ChannelDelayMs[channel];
            _selMute.IsOn = output.ChannelMute[channel];
        }
        finally
        {
            _suspend = previous;
        }
    }

    // Initialise tous les contrôles câblés depuis le brouillon partagé, sous _suspend
    // pour qu'aucune écriture de valeur ne rappelle un mutateur.
    private void InitializeFromDraft()
    {
        var output = _shell.Editor.Draft.Output;
        _suspend = true;
        try
        {
            // Onglet de mode d'écoute.
            _applyModeTab?.Invoke(ModeToIndex(_shell.Editor.Draft.ListeningMode));
            if(_autoModeTab is not null) _autoModeTab.Opacity = _shell.Capabilities?.Features.AutoDeclaredLayout == true ? 1 : .4;
            string mode = ModeToIndex(_shell.Editor.Draft.ListeningMode) == 4 ? "Auto (stéréo / 5.1 natif)" :
                _shell.Editor.Draft.ListeningMode == ListeningMode.Upmix ? "Upmix" : Wire.ToWire(_shell.Editor.Draft.ListeningMode);
            _modeDescription.Text = "Haut-parleurs · Écoute " + mode + " · Sortie Windows 5.1";
            _protectionValue.Text = output.Limiter.Enabled ? "Limiteur activé" : "Limiteur désactivé";

            // Molette de volume : dB -> 0..100 (inverse du mapping d'écriture, borné).
            _knob.Value = Math.Clamp((output.MasterGainDb + 60.0) / 0.6, 0.0, 100.0);

            // Panneau enceinte pour le canal sélectionné (FL au démarrage).
            LoadChannelControls(_selectedChannel);

            // Reflet des coupures de chaque canal dans la scène : Channels[i] est une position
            // d'affichage, ChannelMute est indexé par valeur d'enum -> passer par DisplayToChannel.
            for (int i = 0; i < Channels.Length; i++)
            {
                _scene.SetMuted(Channels[i], output.ChannelMute[DisplayToChannel[i]]);
            }
        }
        finally
        {
            _suspend = false;
        }
    }

    // Chargement réel optionnel depuis l'agent : repli pour les niveaux (si pas de vumètre de
    // rendu) et/ou le format Windows (si le format de rendu n'a pas pu être lu). En dev, sans
    // agent, les valeurs d'exemple restent affichées.
    private async System.Threading.Tasks.Task LoadStateAsync(bool applyMeters, bool applyFormat)
    {
        if (!applyMeters && !applyFormat)
        {
            return;
        }

        try
        {
            if (_shell.SelectedEndpointKey is not string key)
            {
                return;
            }

            var state = await _shell.Client.GetStateAsync(key);

            if (applyMeters)
            {
                var meters = state.State?.Meters?.OutputPeakDbfs;
                if (meters is not null && meters.Length >= 6)
                {
                    for (int i = 0; i < _vuDb.Length; i++)
                    {
                        _vuDb[i] = meters[i];
                    }
                    _vu.SetLevels(Channels, _vuDb);
                }
            }

            if (applyFormat)
            {
                var format = state.State?.NegotiatedFormat;
                if (format is not null && format.ChannelCount > 0 && format.SampleRate > 0)
                {
                    _formatValue.Text = DescribeFormat(
                        (int)format.ChannelCount, BitsOf(format.SampleFormat), format.SampleRate, format.SampleFormat == SampleFormat.Float32);
                    if (_mixSampleRate <= 0)
                    {
                        _mixSampleRate = format.SampleRate;
                        RefreshEqCurve();
                    }
                }
            }
        }
        catch
        {
            // En dev, l'absence d'état réel n'empêche pas l'affichage des valeurs d'exemple.
        }
    }

    // Bits de conteneur associés à un SampleFormat du contrat (pour l'affichage du format).
    private static int BitsOf(SampleFormat format) => format switch
    {
        SampleFormat.Pcm16 => 16,
        SampleFormat.Pcm24 => 24,
        SampleFormat.Pcm32 => 32,
        SampleFormat.Float32 => 32,
        _ => 0,
    };
}
