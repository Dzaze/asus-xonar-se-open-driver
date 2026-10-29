// SPDX-License-Identifier: Apache-2.0
// Page « Réglages » : préférences de l'application et du centre de contrôle.
// Apparence, langue, démarrage, confidentialité et « À propos ». Page sobre,
// construite en code comme le reste du cockpit. Aucune écriture système ici :
// les bascules sont illustratives ou passent par l'agent de contrôle (C4).

using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Xonar.ControlCenter.Shell;

namespace Xonar.ControlCenter.Pages;

public sealed class SettingsPage : Microsoft.UI.Xaml.Controls.UserControl
{
    private readonly AppShellViewModel _shell;
    private readonly TextBlock _versionValue = new();

    private Brush B(string key) => (Brush)Application.Current.Resources[key];
    private Style S(string key) => (Style)Application.Current.Resources[key];

    public SettingsPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;
        Content = Build();
        // La version réelle peut arriver après l'initialisation de l'agent : on
        // la réévalue au chargement de la page (sinon valeur de repli).
        Loaded += (_, _) => _versionValue.Text = DriverVersion();
    }

    private UIElement Build()
    {
        // ---------- En-tête ----------
        var header = new StackPanel { Spacing = 2 };
        header.Children.Add(new TextBlock { Text = "Réglages", Style = S("CockpitH1") });
        header.Children.Add(new TextBlock
        {
            Text = "Préférences de l'application et du centre de contrôle.",
            Style = S("CockpitLabel"),
        });

        // ---------- Cartes ----------
        var cards = new StackPanel { Spacing = 16, Width = 820, HorizontalAlignment = HorizontalAlignment.Left };
        cards.Children.Add(AppearanceCard());
        cards.Children.Add(LanguageCard());
        cards.Children.Add(StartupCard());
        cards.Children.Add(PrivacyCard());
        cards.Children.Add(AboutCard());

        var container = new StackPanel
        {
            Spacing = 22,
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
            Margin = new Thickness(32, 26, 32, 28),
        };
        container.Children.Add(header);
        container.Children.Add(cards);

        return new ScrollViewer
        {
            Content = container,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ---------------------------------------------------------------------------
    // Cartes
    // ---------------------------------------------------------------------------
    private Border AppearanceCard()
    {
        var body = new StackPanel { Spacing = 14 };
        body.Children.Add(Row("Thème", Value("Sombre"), "Seul thème disponible pour l'instant."));
        body.Children.Add(Divider());
        body.Children.Add(Row("Accent", AccentValue()));
        return Card("Apparence", body);
    }

    private Border LanguageCard()
    {
        var combo = new ComboBox { MinWidth = 180 };
        combo.Items.Add("Français");
        combo.SelectedIndex = 0;
        return Card("Langue", Row("Langue de l'interface", combo));
    }

    private Border StartupCard()
    {
        var autostart = new ToggleSwitch { OnContent = null, OffContent = null };
        autostart.IsOn = Services.StartupManager.IsEnabled();
        autostart.Toggled += (_, _) => Services.StartupManager.SetEnabled(autostart.IsOn);
        var formatRepair = new ToggleSwitch { IsOn = false, IsEnabled = false, OnContent = null, OffContent = null };

        var body = new StackPanel { Spacing = 14 };
        body.Children.Add(Row(
            "Lancer au démarrage de Windows",
            autostart,
            "Enregistre une entrée de démarrage utilisateur (HKCU\\…\\Run) ; réversible. Ne touche pas au pilote ni au format audio."));
        body.Children.Add(Divider());
        body.Children.Add(Row(
            "Rétablir le format 6 canaux si Windows le change",
            formatRepair,
            "Réparation automatique indisponible. Les modes d’écoute actuels conservent le format Windows à 6 canaux."));
        return Card("Démarrage", body);
    }

    private Border PrivacyCard()
    {
        var note = new TextBlock
        {
            Text = "Aucune télémétrie. L'application ne touche ni au pilote, ni au format audio, "
                 + "ni au test-signing ; elle passe par l'agent de contrôle. La seule écriture système "
                 + "optionnelle est l'entrée de démarrage (HKCU). Les profils sont enregistrés localement et les réglages DSP dans ProgramData\\XonarSE.",
            Style = S("CockpitBody"),
        };
        return Card("Confidentialité", note);
    }

    private Border AboutCard()
    {
        _versionValue.Style = S("CockpitBody");
        _versionValue.Foreground = B("CockpitTextSecondaryBrush");
        _versionValue.Text = DriverVersion();

        var body = new StackPanel { Spacing = 12 };
        body.Children.Add(new TextBlock
        {
            Text = "Xonar SE Open Driver — Centre de contrôle",
            Style = S("CockpitBody"),
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        });
        body.Children.Add(Divider());
        body.Children.Add(Row("Version du pilote", _versionValue));
        body.Children.Add(Row("Licence", Value("Apache-2.0")));
        body.Children.Add(new TextBlock
        {
            Text = "Projet open source.",
            Style = S("CockpitCaption"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        return Card("À propos", body);
    }

    // ---------------------------------------------------------------------------
    // Fabriques communes
    // ---------------------------------------------------------------------------
    private Border Card(string title, UIElement body)
    {
        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(new TextBlock { Text = title, Style = S("CockpitH2") });
        stack.Children.Add(body);
        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // Ligne « libellé (gauche) + contrôle (droite) », centrée verticalement,
    // avec une légende facultative sous le libellé.
    private Grid Row(string label, FrameworkElement control, string? caption = null)
    {
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var left = new StackPanel
        {
            Spacing = 3,
            VerticalAlignment = VerticalAlignment.Center,
            Margin = new Thickness(0, 0, 16, 0),
        };
        left.Children.Add(new TextBlock { Text = label, Style = S("CockpitBody") });
        if (caption is not null)
        {
            left.Children.Add(new TextBlock { Text = caption, Style = S("CockpitCaption") });
        }
        grid.Children.Add(left);

        control.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(control, 1);
        grid.Children.Add(control);
        return grid;
    }

    private TextBlock Value(string text) => new TextBlock
    {
        Text = text,
        Style = S("CockpitBody"),
        Foreground = B("CockpitTextSecondaryBrush"),
    };

    // Valeur « accent » : pastille turquoise + libellé.
    private StackPanel AccentValue()
    {
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
        row.Children.Add(new Border
        {
            Width = 12,
            Height = 12,
            CornerRadius = new CornerRadius(6),
            Background = B("CockpitAccentBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        });
        row.Children.Add(new TextBlock
        {
            Text = "Turquoise",
            Style = S("CockpitBody"),
            Foreground = B("CockpitTextSecondaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        });
        return row;
    }

    private Border Divider() => new Border { Height = 1, Background = B("CockpitBorderSubtleBrush") };

    // ---------------------------------------------------------------------------
    // Version du pilote : relevée via les capacités déclarées par l'agent, en
    // préférant l'endpoint sélectionné ; valeur de repli si aucune n'est connue.
    // ---------------------------------------------------------------------------
    private string DriverVersion()
    {
        var caps = _shell.Capabilities;
        if (caps is not null)
        {
            string? fallback = null;
            foreach (var endpoint in caps.Endpoints)
            {
                if (endpoint?.Driver?.Version is string version && !string.IsNullOrWhiteSpace(version))
                {
                    if (endpoint.EndpointKey == _shell.SelectedEndpointKey)
                    {
                        return version;
                    }
                    fallback ??= version;
                }
            }
            if (fallback is not null)
            {
                return fallback;
            }
        }
        return Services.InstalledAudioDriver.Read().Version;
    }
}
