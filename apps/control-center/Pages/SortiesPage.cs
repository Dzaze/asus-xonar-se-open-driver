// SPDX-License-Identifier: Apache-2.0
// Page « Sorties » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Interface construite entièrement en code (aucun XAML, aucun x:Class), sur le
// modèle de OverviewPage :
//   - carte « Haut-parleurs (Surround 5.1) » : molette de volume maître, format
//     négocié, masque de canaux 0x60F et interrupteur « Muet » ;
//   - carte « Casque » : curseur de volume, format stéréo, note illustrative ;
//   - carte « Interface S/PDIF » : état PCM et mention du passthrough « Prévu » ;
//   - carte « État des connecteurs » : liste HONNÊTE des jacks. Le pilote force
//     l'état « connecté » (détection de jack désactivée) : une pastille
//     « connecté » ne prouve JAMAIS une présence physique. Les pastilles reflètent
//     cette incertitude (accent = publié, gris = non sondé, warning = forcé).
// Les valeurs affichées sont des exemples représentatifs ; un chargement réel,
// optionnel et gardé, affine le format depuis l'état rapporté par l'agent.

using System;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Xonar.ControlCenter.Controls;
using Xonar.ControlCenter.Shell;

namespace Xonar.ControlCenter.Pages;

public sealed class SortiesPage : Microsoft.UI.Xaml.Controls.UserControl
{
    // ------------------------- Nature d'une pastille d'état -------------------------
    // Accent  : état actif réellement publié (ex. flux PCM S/PDIF).
    // Neutral : état non sondé / inconnu (honnête : aucune mesure fiable).
    // Warning : état forcé par le pilote, détection non fiable.
    private enum PillKind
    {
        Accent,
        Neutral,
        Warning,
    }

    // ------------------------- Modèle du shell (connexion + client C4) -------------------------
    private readonly AppShellViewModel _shell;

    // ------------------------- Contrôles persistants -------------------------
    private readonly VolumeKnob _speakerVolume = new() { Value = 58 };
    private readonly ToggleSwitch _speakerMute = new();
    private readonly Slider _headphoneVolume = new();
    private readonly TextBlock _speakerFormatValue = new();
    private readonly TextBlock _speakerMaskValue = new();

    // ------------------------- Garde de ré-entrance -------------------------
    // Vrai pendant que l'on règle les contrôles depuis le brouillon (Draft) :
    // neutralise les gestionnaires pour ne pas réécrire dans l'éditeur (anti-boucle).
    private bool _suspend;

    // ------------------------- Accès aux ressources du thème -------------------------
    private Brush B(string key) => (Brush)Application.Current.Resources[key];
    private Style S(string key) => (Style)Application.Current.Resources[key];

    public SortiesPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;
        Content = Build();
        // Chargement réel optionnel : en dev (sans agent), les valeurs d'exemple restent.
        Loaded += async (_, _) => await LoadStateAsync();
        // Volume maître et Muet « vivants » : liaison au brouillon partagé de l'éditeur.
        Loaded += OnEditorBindingLoaded;
        Unloaded += OnEditorBindingUnloaded;
    }

    // ===========================================================================
    // Construction de l'arbre visuel
    // ===========================================================================
    private UIElement Build()
    {
        var root = new StackPanel
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
            Spacing = 22,
        };

        // ---------- En-tête ----------
        var header = new StackPanel();
        header.Children.Add(new TextBlock { Text = "Sorties", Style = S("CockpitH1") });
        header.Children.Add(new TextBlock
        {
            Text = "Haut-parleurs, casque et état des connecteurs.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        root.Children.Add(header);

        // ---------- Grille de contenu : 2 colonnes, 2 lignes ----------
        var content = new Grid { ColumnSpacing = 18, RowSpacing = 18 };
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.35, GridUnitType.Star) });
        content.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        content.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        content.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

        // Haut-parleurs (colonne large, ligne 0)
        var speakers = SpeakersCard();
        Grid.SetColumn(speakers, 0);
        Grid.SetRow(speakers, 0);
        content.Children.Add(speakers);

        // Colonne droite (ligne 0) : Casque + S/PDIF empilés
        var rightColumn = new StackPanel { Spacing = 18 };
        rightColumn.Children.Add(HeadphoneCard());
        rightColumn.Children.Add(SpdifCard());
        Grid.SetColumn(rightColumn, 1);
        Grid.SetRow(rightColumn, 0);
        content.Children.Add(rightColumn);

        // État des connecteurs (ligne 1, sur toute la largeur)
        var connectors = ConnectorsCard();
        Grid.SetColumn(connectors, 0);
        Grid.SetColumnSpan(connectors, 2);
        Grid.SetRow(connectors, 1);
        content.Children.Add(connectors);

        root.Children.Add(content);

        return new ScrollViewer
        {
            Content = root,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ===========================================================================
    // Carte « Haut-parleurs (Surround 5.1) »
    // ===========================================================================
    private Border SpeakersCard()
    {
        var stack = new StackPanel { Spacing = 16 };
        stack.Children.Add(new TextBlock { Text = "Haut-parleurs (Surround 5.1)", Style = S("CockpitH2") });

        // Corps : molette à gauche, informations à droite.
        var body = new Grid { ColumnSpacing = 24 };
        body.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        body.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        // Molette de volume maître (taille fixe). Pilote le gain maître du brouillon
        // de l'éditeur (shell.Editor) ; aucune écriture matérielle directe.
        _speakerVolume.Width = 190;
        _speakerVolume.Height = 190;
        _speakerVolume.ValueChanged += OnSpeakerVolumeChanged;
        var knobHost = new StackPanel { VerticalAlignment = VerticalAlignment.Center };
        knobHost.Children.Add(_speakerVolume);
        Grid.SetColumn(knobHost, 0);
        body.Children.Add(knobHost);

        // Informations (grille 2 colonnes libellé / valeur).
        _speakerFormatValue.Text = "6 canaux · 24 bits · 48 kHz";
        _speakerFormatValue.Style = S("CockpitBody");
        _speakerMaskValue.Text = "0x60F (FL FR FC LFE SL SR)";
        _speakerMaskValue.Style = S("CockpitBody");

        // Interrupteur « Muet » sans libellé anglais par défaut (comme OverviewPage).
        _speakerMute.OnContent = null;
        _speakerMute.OffContent = null;
        // Bascule « vivante » : mute/démute les 6 canaux 5.1 du brouillon de l'éditeur.
        _speakerMute.Toggled += OnSpeakerMuteToggled;

        var info = KeyValueGrid(
            ("Format", _speakerFormatValue),
            ("Masque de canaux", _speakerMaskValue),
            ("Muet", _speakerMute));
        info.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(info, 1);
        body.Children.Add(info);

        stack.Children.Add(body);

        // Note honnête : la sortie Windows reste fixée à 6 canaux quel que soit le mode.
        stack.Children.Add(new TextBlock
        {
            Text = "La sortie Windows reste en 6 canaux (masque 0x60F) quel que soit le mode d'écoute ; le mixage est appliqué en amont.",
            Style = S("CockpitCaption"),
        });

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // ===========================================================================
    // Carte « Casque »
    // ===========================================================================
    private Border HeadphoneCard()
    {
        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(new TextBlock { Text = "Casque", Style = S("CockpitH2") });

        // Curseur de volume + valeur en pourcentage à droite.
        _headphoneVolume.Minimum = 0;
        _headphoneVolume.Maximum = 100;
        _headphoneVolume.Value = 40;
        _headphoneVolume.StepFrequency = 1;
        _headphoneVolume.HorizontalAlignment = HorizontalAlignment.Stretch;

        var volValue = new TextBlock
        {
            Text = FormatPercent(_headphoneVolume.Value),
            Style = S("CockpitBody"),
            VerticalAlignment = VerticalAlignment.Center,
            MinWidth = 46,
            TextAlignment = TextAlignment.Right,
        };
        _headphoneVolume.ValueChanged += (_, e) => volValue.Text = FormatPercent(e.NewValue);

        var sliderCell = new Grid { ColumnSpacing = 12 };
        sliderCell.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        sliderCell.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(_headphoneVolume, 0);
        sliderCell.Children.Add(_headphoneVolume);
        Grid.SetColumn(volValue, 1);
        sliderCell.Children.Add(volValue);

        var info = KeyValueGrid(
            ("Volume", sliderCell),
            ("Format", new TextBlock { Text = "Stéréo · 24 bits · 48 kHz", Style = S("CockpitBody") }));
        stack.Children.Add(info);

        stack.Children.Add(new TextBlock
        {
            Text = "Bascule automatique sur le casque s'il est branché (illustratif).",
            Style = S("CockpitCaption"),
        });

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // ===========================================================================
    // Carte « Interface S/PDIF »
    // ===========================================================================
    private Border SpdifCard()
    {
        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(new TextBlock { Text = "Interface S/PDIF", Style = S("CockpitH2") });

        var info = KeyValueGrid(
            ("État", PillWithLabel(PillKind.Accent, "PCM (stéréo)")),
            ("Passthrough", Badge("Prévu", PillKind.Warning)));
        stack.Children.Add(info);

        stack.Children.Add(new TextBlock
        {
            Text = "Le passthrough compressé (IEC 61937 · Dolby / DTS) n'est pas encore pris en charge.",
            Style = S("CockpitCaption"),
        });

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // ===========================================================================
    // Carte « État des connecteurs » (honnête : détection forcée par le pilote)
    // ===========================================================================
    private Border ConnectorsCard()
    {
        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(new TextBlock { Text = "État des connecteurs", Style = S("CockpitH2") });

        // Avertissement explicite : aucune présence physique n'est prouvée.
        stack.Children.Add(new TextBlock
        {
            Text = "Le pilote publie en permanence l'état « connecté » (détection de jack désactivée, "
                 + "HonorJackDetection = 0). Une pastille « connecté » ne prouve donc pas qu'une fiche "
                 + "est réellement branchée.",
            Style = S("CockpitBody"),
        });

        // Liste des jacks : grille 2 colonnes (libellé / état). États volontairement prudents.
        var connectors = new (string Name, PillKind Kind, string State)[]
        {
            ("Avant (Front)", PillKind.Warning, "détection non fiable (forcée connecté)"),
            ("Latéral (Side)", PillKind.Warning, "détection non fiable (forcée connecté)"),
            ("Arrière (Rear)", PillKind.Warning, "détection non fiable (forcée connecté)"),
            ("Ligne / Micro", PillKind.Neutral, "état inconnu (non sondé)"),
            ("SPDIF optique", PillKind.Neutral, "sortie numérique · état non sondé"),
        };

        var grid = new Grid { ColumnSpacing = 18, RowSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        for (int i = 0; i < connectors.Length; i++)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var name = new TextBlock
            {
                Text = connectors[i].Name,
                Style = S("CockpitBody"),
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetRow(name, i);
            Grid.SetColumn(name, 0);
            grid.Children.Add(name);

            var statePanel = PillWithLabel(connectors[i].Kind, connectors[i].State);
            statePanel.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetRow(statePanel, i);
            Grid.SetColumn(statePanel, 1);
            grid.Children.Add(statePanel);
        }

        stack.Children.Add(grid);

        // Séparateur discret + légende des pastilles.
        stack.Children.Add(new Border
        {
            Height = 1,
            Background = B("CockpitBorderSubtleBrush"),
            Margin = new Thickness(0, 4, 0, 4),
        });

        var legend = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 22 };
        legend.Children.Add(PillWithLabel(PillKind.Accent, "publié « connecté »", "CockpitCaption"));
        legend.Children.Add(PillWithLabel(PillKind.Warning, "forcé · détection non fiable", "CockpitCaption"));
        legend.Children.Add(PillWithLabel(PillKind.Neutral, "non sondé · inconnu", "CockpitCaption"));
        stack.Children.Add(legend);

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // ===========================================================================
    // Aides de construction
    // ===========================================================================

    /// <summary>
    /// Grille 2 colonnes « libellé / valeur » : colonne de gauche dimensionnée au
    /// plus large libellé (alignement constant des valeurs), colonne de droite
    /// élastique. Chaque valeur est un élément arbitraire (texte, curseur, bascule…).
    /// </summary>
    private Grid KeyValueGrid(params (string Label, FrameworkElement Value)[] rows)
    {
        var grid = new Grid { ColumnSpacing = 18, RowSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        for (int i = 0; i < rows.Length; i++)
        {
            grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });

            var label = new TextBlock
            {
                Text = rows[i].Label,
                Style = S("CockpitLabel"),
                VerticalAlignment = VerticalAlignment.Center,
            };
            Grid.SetRow(label, i);
            Grid.SetColumn(label, 0);
            grid.Children.Add(label);

            FrameworkElement value = rows[i].Value;
            value.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetRow(value, i);
            Grid.SetColumn(value, 1);
            grid.Children.Add(value);
        }

        return grid;
    }

    /// <summary>Petite pastille ronde + libellé, alignés horizontalement.</summary>
    private FrameworkElement PillWithLabel(PillKind kind, string label, string styleKey = "CockpitBody")
    {
        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 9,
            VerticalAlignment = VerticalAlignment.Center,
        };
        row.Children.Add(Pill(kind));
        row.Children.Add(new TextBlock
        {
            Text = label,
            Style = S(styleKey),
            VerticalAlignment = VerticalAlignment.Center,
        });
        return row;
    }

    /// <summary>Pastille ronde colorée selon la nature de l'état.</summary>
    private Ellipse Pill(PillKind kind)
    {
        Brush fill = kind switch
        {
            PillKind.Accent => B("CockpitAccentBrush"),
            PillKind.Warning => B("CockpitWarningBrush"),
            _ => B("CockpitTextTertiaryBrush"),
        };
        return new Ellipse
        {
            Width = 10,
            Height = 10,
            Fill = fill,
            VerticalAlignment = VerticalAlignment.Center,
        };
    }

    /// <summary>Badge compact (style du thème) avec une teinte de texte selon l'état.</summary>
    private Border Badge(string text, PillKind tint)
    {
        Brush foreground = tint switch
        {
            PillKind.Accent => B("CockpitAccentBrightBrush"),
            PillKind.Warning => B("CockpitWarningBrush"),
            _ => B("CockpitTextSecondaryBrush"),
        };
        return new Border
        {
            Style = S("CockpitBadge"),
            HorizontalAlignment = HorizontalAlignment.Left,
            Child = new TextBlock { Text = text, Foreground = foreground, FontSize = 12 },
        };
    }

    // ===========================================================================
    // Formatage (séparateur décimal français pour les fréquences)
    // ===========================================================================
    private static string FormatPercent(double value)
    {
        int rounded = (int)Math.Round(value, MidpointRounding.AwayFromZero);
        return rounded.ToString(CultureInfo.InvariantCulture) + " %";
    }

    private static string FormatKilohertz(uint hz)
    {
        var french = new NumberFormatInfo { NumberDecimalSeparator = "," };
        double khz = hz / 1000.0;
        return khz.ToString("0.###", french) + " kHz";
    }

    private static string DescribeFormat(uint channels, Xonar.Contracts.V1.SampleFormat format, uint sampleRate)
    {
        string bits = format switch
        {
            Xonar.Contracts.V1.SampleFormat.Pcm16 => "16 bits",
            Xonar.Contracts.V1.SampleFormat.Pcm24 => "24 bits",
            Xonar.Contracts.V1.SampleFormat.Pcm32 => "32 bits",
            Xonar.Contracts.V1.SampleFormat.Float32 => "32 bits (flottant)",
            _ => "format inconnu",
        };
        return channels.ToString(CultureInfo.InvariantCulture) + " canaux · " + bits + " · " + FormatKilohertz(sampleRate);
    }

    private static string DescribeMask(uint mask)
    {
        string hex = "0x" + mask.ToString("X", CultureInfo.InvariantCulture);
        // N'ajoute la correspondance des canaux que pour le masque 5.1 attendu (honnêteté).
        return mask == 0x60F ? hex + " (FL FR FC LFE SL SR)" : hex;
    }

    // ===========================================================================
    // Liaison « vivante » au brouillon de l'éditeur (volume maître + Muet)
    // ===========================================================================
    // Le volume maître et le Muet des haut-parleurs pilotent le brouillon partagé
    // shell.Editor (mutateurs coalescés) et se (re)synchronisent depuis ce brouillon.
    // Casque et S/PDIF restent purement visuels (aucun mutateur dédié).

    // Bornes du mappage molette <-> dB : 0..100 % <-> -60..0 dB (0,6 dB par %).
    private const double MasterGainDbAtZeroPercent = -60.0;
    private const double MasterGainDbPerPercent = 0.6;

    // Valeurs de l'enum xonar::dsp::Channel pour les 6 enceintes du 5.1-côté (0x60F) :
    // FL=0 FR=1 FC=2 LFE=3 SL=6 SR=7. ATTENTION : ce ne sont PAS 0..5 — les index 4/5 sont
    // BL/BR, absents de cette disposition. Boucler naïvement sur 0..5 couperait BL/BR et
    // LAISSERAIT les surrounds SL/SR actifs.
    private static readonly int[] SpeakerChannels = { 0, 1, 2, 3, 6, 7 };

    private void OnEditorBindingLoaded(object sender, RoutedEventArgs e)
    {
        // Abonnement idempotent : Loaded peut se redéclencher (ré-insertion dans l'arbre).
        _shell.Editor.Changed -= OnEditorChanged;
        _shell.Editor.Changed += OnEditorChanged;
        SyncControlsFromDraft();
    }

    private void OnEditorBindingUnloaded(object sender, RoutedEventArgs e)
    {
        // L'éditeur vit aussi longtemps que le shell : se désabonner évite de retenir la page.
        _shell.Editor.Changed -= OnEditorChanged;
    }

    /// <summary>
    /// Resynchronise les contrôles depuis le brouillon. Marshalé vers le thread UI :
    /// l'évènement Changed peut être levé depuis l'application coalescée (hors thread UI).
    /// </summary>
    private void OnEditorChanged()
    {
        var queue = DispatcherQueue;
        if (queue is not null && !queue.HasThreadAccess)
        {
            queue.TryEnqueue(SyncControlsFromDraft);
            return;
        }
        SyncControlsFromDraft();
    }

    /// <summary>Règle molette et bascule depuis Draft, sous garde de ré-entrance.</summary>
    private void SyncControlsFromDraft()
    {
        _suspend = true;
        try
        {
            var output = _shell.Editor.Draft.Output;
            _speakerVolume.Value = KnobValueFromDb(output.MasterGainDb);
            _speakerMute.IsOn = AllSpeakerChannelsMuted(output.ChannelMute);
        }
        finally
        {
            _suspend = false;
        }
    }

    // --- Gestionnaires d'interaction utilisateur (écrivent dans l'éditeur) ---

    private void OnSpeakerVolumeChanged(object? sender, double value)
    {
        if (_suspend)
        {
            return;
        }
        _shell.Editor.SetMasterGainDb(DbFromKnobValue(value));
    }

    private void OnSpeakerMuteToggled(object sender, RoutedEventArgs e)
    {
        if (_suspend)
        {
            return;
        }
        bool muted = _speakerMute.IsOn;
        // Les 6 enceintes 5.1 suivent le Muet maître. Index d'enum (pas 0..5) : voir SpeakerChannels.
        foreach (int channel in SpeakerChannels)
        {
            _shell.Editor.SetChannelMute(channel, muted);
        }
    }

    // --- Mappage molette (0..100 %) <-> gain maître (dB) ---

    private static double DbFromKnobValue(double value) =>
        MasterGainDbAtZeroPercent + value * MasterGainDbPerPercent;

    private static double KnobValueFromDb(double db)
    {
        double value = (db - MasterGainDbAtZeroPercent) / MasterGainDbPerPercent;
        return value < 0.0 ? 0.0 : (value > 100.0 ? 100.0 : value);
    }

    /// <summary>Vrai si les 6 enceintes 5.1 (valeurs d'enum SpeakerChannels) sont toutes muettes.</summary>
    private static bool AllSpeakerChannelsMuted(bool[] channelMute)
    {
        foreach (int channel in SpeakerChannels)
        {
            if (channel >= channelMute.Length || !channelMute[channel])
            {
                return false;
            }
        }
        return true;
    }

    // ===========================================================================
    // Chargement réel optionnel : affine le format depuis l'état de l'agent.
    // ===========================================================================
    private async System.Threading.Tasks.Task LoadStateAsync()
    {
        try
        {
            if (_shell.SelectedEndpointKey is not string key)
            {
                return;
            }

            var state = await _shell.Client.GetStateAsync(key);
            var format = state.State?.NegotiatedFormat;
            if (format is not null)
            {
                _speakerFormatValue.Text = DescribeFormat(format.ChannelCount, format.SampleFormat, format.SampleRate);
                _speakerMaskValue.Text = DescribeMask(format.ChannelMask);
            }
        }
        catch
        {
            // En dev, l'absence d'agent réel n'empêche pas l'affichage des valeurs d'exemple.
        }
    }
}
