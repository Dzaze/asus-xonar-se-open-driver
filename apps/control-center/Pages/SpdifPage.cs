// SPDX-License-Identifier: Apache-2.0
// Page « S/PDIF » : disposition CIBLE de la sortie optique numérique (PCM et
// passthrough compressé). Fonction PRÉVUE, non encore validée : la page montre
// la mise en page visée mais toutes les commandes sont illustratives (ComboBox
// désactivé, pastilles « À valider »). Aucune écriture système n'est effectuée
// ici, et aucun format compressé n'est jamais annoncé tant qu'il n'est pas
// réellement validé au sens IEC 61937.

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace Xonar.ControlCenter.Pages;

public sealed class SpdifPage : Microsoft.UI.Xaml.Controls.UserControl
{
    private readonly Xonar.ControlCenter.Shell.AppShellViewModel _shell;

    private Brush B(string key) => (Brush)Application.Current.Resources[key];
    private Style S(string key) => (Style)Application.Current.Resources[key];

    public SpdifPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;
        Content = Build();
    }

    private UIElement Build()
    {
        var root = new Grid
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
        };
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); // en-tête
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); // bannière « Prévu »
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); // cartes du haut
        root.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto }); // carte Horloge

        // ---------- En-tête ----------
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var titleStack = new StackPanel();
        titleStack.Children.Add(new TextBlock { Text = "S/PDIF", Style = S("CockpitH1") });
        titleStack.Children.Add(new TextBlock
        {
            Text = "PCM et passthrough compressé lorsque validé.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        head.Children.Add(titleStack);
        var chip = StatusChip();
        Grid.SetColumn(chip, 1);
        head.Children.Add(chip);
        root.Children.Add(head);

        // ---------- Bannière « Prévu » ----------
        var banner = PlannedBanner();
        banner.Margin = new Thickness(0, 20, 0, 0);
        Grid.SetRow(banner, 1);
        root.Children.Add(banner);

        // ---------- Cartes du haut : Sortie optique + Passthrough ----------
        var top = new Grid { Margin = new Thickness(0, 18, 0, 0), ColumnSpacing = 18 };
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.15, GridUnitType.Star) });
        top.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var optical = OpticalCard();
        top.Children.Add(optical);
        var passthrough = PassthroughCard();
        Grid.SetColumn(passthrough, 1);
        top.Children.Add(passthrough);
        Grid.SetRow(top, 2);
        root.Children.Add(top);

        // ---------- Carte Horloge ----------
        var clock = ClockCard();
        clock.Margin = new Thickness(0, 18, 0, 0);
        Grid.SetRow(clock, 3);
        root.Children.Add(clock);

        return new ScrollViewer
        {
            Content = root,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ---------------------------------------------------------------------------
    // Puce d'état de l'agent (reflet honnête de l'état du shell, pas de la liaison
    // S/PDIF qui, elle, reste prévue).
    // ---------------------------------------------------------------------------
    private Border StatusChip()
    {
        bool connected = _shell.IsConnected;
        var text = new TextBlock
        {
            Text = connected ? "Agent connecté" : "Agent non détecté",
            FontSize = 13,
            VerticalAlignment = VerticalAlignment.Center,
            Foreground = connected ? B("CockpitAccentBrightBrush") : B("CockpitTextTertiaryBrush"),
        };
        return new Border
        {
            Background = B("CockpitSurface2Brush"),
            BorderBrush = B("CockpitBorderBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(9),
            Padding = new Thickness(14, 8, 14, 8),
            VerticalAlignment = VerticalAlignment.Center,
            Child = text,
        };
    }

    private Border PlannedBanner()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 14 };
        row.Children.Add(Badge("Prévu", B("CockpitAccentBrightBrush")));
        row.Children.Add(new TextBlock
        {
            Text = "Prévu — la sortie S/PDIF optique sera pilotée une fois validée. Commandes illustratives.",
            Style = S("CockpitBody"),
            Foreground = B("CockpitTextSecondaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        });
        return new Border
        {
            Background = B("CockpitAccentSoftBrush"),
            BorderBrush = B("CockpitAccentDeepBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(12),
            Padding = new Thickness(18, 14, 18, 14),
            Child = row,
        };
    }

    // ---------------------------------------------------------------------------
    // Carte « Sortie optique » : état PCM / Passthrough, sélecteur de mode (illustratif,
    // désactivé) et format déclaré.
    // ---------------------------------------------------------------------------
    private Border OpticalCard()
    {
        var content = new StackPanel { Spacing = 16 };

        // État courant (illustratif) : PCM stéréo actif, Passthrough proposé.
        var stateBlock = new StackPanel { Spacing = 8 };
        stateBlock.Children.Add(new TextBlock { Text = "État", Style = S("CockpitLabel") });
        var states = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        states.Children.Add(Pill("PCM stéréo", true));
        states.Children.Add(Pill("Passthrough compressé", false));
        stateBlock.Children.Add(states);
        content.Children.Add(stateBlock);

        content.Children.Add(Divider());

        // Sélecteur de mode (ComboBox désactivé) + format déclaré.
        var combo = new ComboBox { IsEnabled = false, MinWidth = 220 };
        combo.Items.Add(new ComboBoxItem { Content = "PCM" });
        combo.Items.Add(new ComboBoxItem { Content = "Passthrough" });
        combo.SelectedIndex = 0;

        var format = new TextBlock { Text = "48 kHz · 16/24 bits", Style = S("CockpitBody") };

        var grid = RowsGrid();
        AddRow(grid, 0, "Mode", combo);
        AddRow(grid, 1, "Format", format);
        content.Children.Add(grid);

        return Card("Sortie optique", content, "Liaison optique — PCM stéréo par défaut.");
    }

    // ---------------------------------------------------------------------------
    // Carte « Passthrough compressé » : formats candidats avec pastille « À valider »
    // et note honnête sur l'annonce conditionnée à une vraie validation (IEC 61937).
    // ---------------------------------------------------------------------------
    private Border PassthroughCard()
    {
        var content = new StackPanel { Spacing = 14 };

        var formats = new StackPanel { Spacing = 10 };
        formats.Children.Add(FormatRow("Dolby Digital"));
        formats.Children.Add(FormatRow("DTS"));
        content.Children.Add(formats);

        content.Children.Add(Divider());

        content.Children.Add(new TextBlock
        {
            Text = "Le passthrough n'est annoncé à Windows que lorsqu'il est réellement validé (IEC 61937). "
                 + "Tant qu'un format n'est pas confirmé, il n'est jamais proposé comme flux binaire.",
            Style = S("CockpitBody"),
            Foreground = B("CockpitTextSecondaryBrush"),
        });

        return Card("Passthrough compressé", content, "Flux binaire compressé (bitstream) vers un décodeur externe.");
    }

    // ---------------------------------------------------------------------------
    // Carte « Horloge » : sources d'horloge illustratives.
    // ---------------------------------------------------------------------------
    private Border ClockCard()
    {
        var content = new StackPanel { Spacing = 14 };

        var sources = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10 };
        sources.Children.Add(Pill("44,1 kHz", false));
        sources.Children.Add(Pill("48 kHz", true));
        sources.Children.Add(Pill("96 kHz", false));
        sources.Children.Add(Pill("192 kHz", false));
        content.Children.Add(sources);

        content.Children.Add(new TextBlock
        {
            Text = "Illustratif — la source d'horloge réelle dépendra de la validation du pilote. "
                 + "48 kHz est présenté comme réglage par défaut.",
            Style = S("CockpitCaption"),
        });

        return Card("Horloge", content, "Fréquence d'échantillonnage de la liaison numérique.");
    }

    // ---------------------------------------------------------------------------
    // Fabriques partagées.
    // ---------------------------------------------------------------------------
    private Border Card(string title, UIElement content, string? subtitle = null)
    {
        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(new TextBlock { Text = title, Style = S("CockpitH2") });
        if (subtitle is not null)
        {
            stack.Children.Add(new TextBlock { Text = subtitle, Style = S("CockpitCaption"), Margin = new Thickness(0, -6, 0, 2) });
        }
        stack.Children.Add(content);
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private Grid RowsGrid()
    {
        var grid = new Grid { RowSpacing = 12, ColumnSpacing = 14 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(96) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        return grid;
    }

    private void AddRow(Grid grid, int row, string label, FrameworkElement value)
    {
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var lbl = new TextBlock { Text = label, Style = S("CockpitLabel"), VerticalAlignment = VerticalAlignment.Center };
        Grid.SetRow(lbl, row);
        grid.Children.Add(lbl);
        value.HorizontalAlignment = HorizontalAlignment.Left;
        value.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetRow(value, row);
        Grid.SetColumn(value, 1);
        grid.Children.Add(value);
    }

    private Border FormatRow(string name)
    {
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var label = new TextBlock { Text = name, Style = S("CockpitBody"), VerticalAlignment = VerticalAlignment.Center };
        grid.Children.Add(label);
        var badge = Badge("À valider", B("CockpitWarningBrush"));
        Grid.SetColumn(badge, 1);
        grid.Children.Add(badge);
        return new Border
        {
            Background = B("CockpitSurfaceInsetBrush"),
            BorderBrush = B("CockpitBorderSubtleBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(9),
            Padding = new Thickness(14, 10, 14, 10),
            Child = grid,
        };
    }

    private Border Pill(string text, bool active)
    {
        var label = new TextBlock
        {
            Text = text,
            FontSize = 13,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = active ? B("CockpitAccentBrightBrush") : B("CockpitTextSecondaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        return new Border
        {
            Background = active ? B("CockpitAccentSelBrush") : B("CockpitSurfaceInsetBrush"),
            BorderBrush = active ? B("CockpitAccentDeepBrush") : B("CockpitBorderSubtleBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(14, 7, 14, 7),
            Child = label,
        };
    }

    private Border Badge(string text, Brush foreground)
    {
        return new Border
        {
            Style = S("CockpitBadge"),
            Child = new TextBlock
            {
                Text = text,
                FontSize = 11,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = foreground,
                VerticalAlignment = VerticalAlignment.Center,
            },
        };
    }

    private Border Divider() => new Border { Height = 1, Background = B("CockpitBorderSubtleBrush") };
}
