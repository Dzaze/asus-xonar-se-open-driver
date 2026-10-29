// SPDX-License-Identifier: Apache-2.0
// Fenêtre principale « cockpit » : barre latérale de navigation (construite en code),
// en-tête XONAR SE et hôte de contenu. Aucune vue n'écrit dans KS/CAPX/registre :
// tout passe par le client C4 injecté (faux agent en développement).

using System.Collections.Generic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Xonar.ControlCenter.Services;

namespace Xonar.ControlCenter.Shell;

public sealed partial class MainWindow : Window
{
    private sealed class NavItem
    {
        public string Tag = "";
        public Border Highlight = null!;
        public Border Bar = null!;
        public FontIcon Icon = null!;
        public TextBlock Label = null!;
    }

    private readonly IControlClient _client;
    private readonly AppShellViewModel _shell;
    private readonly Dictionary<string, FrameworkElement> _cache = new();
    private readonly List<NavItem> _nav = new();
    private string _selected = "";
    private TestControlPipe? _testPipe; // pilotage automatisé des tests (si XONAR_TEST_PIPE=1)
    private readonly DispatcherTimer _dspStateTimer = new() { Interval = System.TimeSpan.FromMilliseconds(500) };
    private bool _refreshingAudio;
    private int _hardwareTicks;

    private Brush Res(string key) => (Brush)Application.Current.Resources[key];

    public MainWindow(IControlClient client)
    {
        InitializeComponent();
        TrySetDefaultSize();
        // Fermer la fenêtre la RANGE dans la zone de notification au lieu de quitter ;
        // le vrai arrêt passe par « Quitter » dans le menu du tray (App.IsExiting).
        AppWindow.Closing += OnAppWindowClosing;
        _client = client;
        _shell = new AppShellViewModel(client);
        _dspStateTimer.Tick += async (_, _) => {
            _shell.Editor.RefreshLiveState();
            if(_refreshingAudio || ++_hardwareTicks % 2 != 0 || _client is not LiveApoBroker live) return;
            _refreshingAudio = true;
            try {
                bool recover = live.RefreshHardware();
                await _shell.InitializeAsync();
                if(recover) {
                    if(_cache.TryGetValue("overview", out var page) && page is Pages.OverviewPage overview) overview.RefreshAudioEndpoint();
                    if(_shell.CanWrite) await _shell.Editor.ApplyNowAsync();
                }
                if(live.Device?.Driver.Version is string version) DriverFooter.Text = "Pilote " + version;
            } catch { } finally { _refreshingAudio = false; }
        };
        _dspStateTimer.Start();
        // Pilotage automatisé pour les tests (serveur named pipe), désactivé par défaut :
        // n'est actif que si XONAR_TEST_PIPE=1 au lancement.
        if (System.Environment.GetEnvironmentVariable("XONAR_TEST_PIPE") == "1")
        {
            _testPipe = new TestControlPipe(_shell.Editor, DispatcherQueue, async () => {
                Select("overview");
                return await ((Xonar.ControlCenter.Pages.OverviewPage)_cache["overview"]).RunSpeakerTestAsync();
            }, Select, async () => DiagnosticsExporter.Export((await _client.GetStateAsync("MI_00")).State, (_client as LiveApoBroker)?.Telemetry));
        }
        BuildNav();
        // Page de départ : « overview » par défaut ; surchargée par XONAR_START_PAGE
        // (utile pour les captures de développement). Un tag inconnu tombe sur le placeholder.
        Select(System.Environment.GetEnvironmentVariable("XONAR_START_PAGE") ?? "overview");
        _ = InitAsync();
    }

    // Taille par défaut adaptée au cockpit (large). Best-effort : si l'API AppWindow
    // n'est pas disponible, on laisse la taille par défaut.
    private void TrySetDefaultSize()
    {
        try
        {
            var appWindow = AppWindow;
            if (appWindow is null)
            {
                return;
            }

            // AppWindow attend des pixels physiques, WinUI des pixels effectifs.
            // À 150 %, 1480 pixels physiques ne laissaient que 987 pixels de layout.
            uint dpi = GetDpiForWindow(WinRT.Interop.WindowNative.GetWindowHandle(this));
            double scale = dpi == 0 ? 1 : dpi / 96.0;
            int width = (int)System.Math.Round(1480 * scale);
            int height = (int)System.Math.Round(940 * scale);

            // Centre la fenêtre sur la zone de travail de l'écran courant (hors barre
            // des tâches) plutôt que de la laisser dans le coin par défaut du système.
            // Best-effort : si l'API de fenêtrage n'est pas disponible, on conserve la
            // position par défaut.
            var area = Microsoft.UI.Windowing.DisplayArea.GetFromWindowId(
                appWindow.Id, Microsoft.UI.Windowing.DisplayAreaFallback.Primary);
            if (area is not null)
            {
                var work = area.WorkArea;
                width = System.Math.Min(width, System.Math.Max(320, work.Width - 24));
                height = System.Math.Min(height, System.Math.Max(320, work.Height - 24));
                appWindow.Resize(new Windows.Graphics.SizeInt32(width, height));
                int x = work.X + System.Math.Max(0, (work.Width - width) / 2);
                int y = work.Y + System.Math.Max(0, (work.Height - height) / 2);
                appWindow.Move(new Windows.Graphics.PointInt32(x, y));
            }
            else appWindow.Resize(new Windows.Graphics.SizeInt32(width, height));
        }
        catch
        {
            // Ignoré : taille et position de la fenêtre non critiques.
        }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(System.IntPtr window);

    private void OnAppWindowClosing(Microsoft.UI.Windowing.AppWindow sender,
                                    Microsoft.UI.Windowing.AppWindowClosingEventArgs args)
    {
        if (Application.Current is App app && app.IsExiting)
        {
            return; // arrêt réel demandé via « Quitter »
        }
        args.Cancel = true;
        sender.Hide();
    }

    private async System.Threading.Tasks.Task InitAsync()
    {
        await _shell.InitializeAsync();
        if (_shell.Capabilities is not null)
        {
            var ep = System.Linq.Enumerable.FirstOrDefault(_shell.Capabilities.Endpoints);
            if (ep?.Driver?.Version is string v && !string.IsNullOrEmpty(v))
            {
                DriverFooter.Text = $"Pilote {v}";
            }
        }
        if(_client is LiveApoBroker) await _shell.Editor.ApplyNowAsync();
    }

    // ---------------------------------------------------------------------------
    // Navigation (code)
    // ---------------------------------------------------------------------------
    private void BuildNav()
    {
        AddNav("overview", "", "Vue d'ensemble", false);
        AddNav("outputs", "", "Sorties", false);
        AddNav("routing", "", "Mixage & routage", false);
        AddNav("eq", "", "Égaliseur", false);
        AddNav("bass", "", "Basses & crossover", false);
        AddNav("profiles", "", "Profils", false);
        AddSeparator();
        AddNav("inputs", "", "Entrées", false);
        AddNav("spdif", "", "S/PDIF", true);
        AddSeparator();
        AddNav("diagnostic", "", "Diagnostic", false);
        AddNav("settings", "", "Réglages", false);
    }

    private void AddSeparator()
    {
        NavPanel.Children.Add(new Border
        {
            Height = 1,
            Background = Res("CockpitBorderSubtleBrush"),
            Margin = new Thickness(20, 12, 20, 12),
        });
    }

    private void AddNav(string tag, string glyph, string label, bool planned)
    {
        var highlight = new Border
        {
            Background = Res("CockpitAccentSelBrush"),
            CornerRadius = new CornerRadius(10),
            Opacity = 0,
        };
        var bar = new Border
        {
            Width = 3,
            Height = 20,
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center,
            Background = Res("CockpitAccentBrush"),
            CornerRadius = new CornerRadius(2),
            Opacity = 0,
        };
        var icon = new FontIcon
        {
            Glyph = glyph,
            FontSize = 16,
            Foreground = Res("CockpitTextSecondaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var text = new TextBlock
        {
            Text = label,
            FontSize = 14,
            Foreground = Res("CockpitTextSecondaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        };

        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 14,
            Margin = new Thickness(16, 9, 14, 9),
            VerticalAlignment = VerticalAlignment.Center,
        };
        row.Children.Add(icon);
        row.Children.Add(text);
        if (planned)
        {
            var badge = new Border
            {
                Style = (Style)Application.Current.Resources["CockpitBadge"],
                Child = new TextBlock { Text = "Prévu", FontSize = 10, Foreground = Res("CockpitTextTertiaryBrush") },
            };
            row.Children.Add(badge);
        }

        var grid = new Grid { Tag = tag, Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent), Margin = new Thickness(10, 2, 10, 2) };
        grid.Children.Add(highlight);
        grid.Children.Add(bar);
        grid.Children.Add(row);
        grid.Tapped += OnNavTapped;
        grid.PointerEntered += (s, e) => { if (_selected != tag) highlight.Opacity = 0.5; };
        grid.PointerExited += (s, e) => { if (_selected != tag) highlight.Opacity = 0; };

        NavPanel.Children.Add(grid);
        _nav.Add(new NavItem { Tag = tag, Highlight = highlight, Bar = bar, Icon = icon, Label = text });
    }

    private void OnNavTapped(object sender, TappedRoutedEventArgs e)
    {
        if (sender is FrameworkElement fe && fe.Tag is string tag)
        {
            Select(tag);
        }
    }

    private void Select(string tag)
    {
        if (_selected == tag && Host is not null && Host.Children.Count != 0) return;
        _selected = tag;
        var accent = Res("CockpitAccentBrush");
        var secondary = Res("CockpitTextSecondaryBrush");
        foreach (var item in _nav)
        {
            bool on = item.Tag == tag;
            item.Highlight.Opacity = on ? 1 : 0;
            item.Bar.Opacity = on ? 1 : 0;
            item.Icon.Foreground = on ? accent : secondary;
            item.Label.Foreground = on ? accent : secondary;
        }

        if (Host is null)
        {
            return;
        }
        Host.Children.Clear();
        Host.Children.Add(GetPage(tag));
    }

    // ---------------------------------------------------------------------------
    // Pages
    // ---------------------------------------------------------------------------
    private FrameworkElement GetPage(string tag)
    {
        if (_cache.TryGetValue(tag, out FrameworkElement? cached))
        {
            return cached;
        }

        FrameworkElement page = tag switch
        {
            "overview" => new Pages.OverviewPage(_shell),
            "outputs" => new Pages.SortiesPage(_shell),
            "routing" => new Pages.RoutingPage(_shell),
            "eq" => new Pages.EqualizerPage(_shell),
            "bass" => new Pages.BassPage(_shell),
            "profiles" => new Pages.ProfilesPage(_shell),
            "inputs" => new Pages.InputsPage(_shell),
            "spdif" => new Pages.SpdifPage(_shell),
            "diagnostic" => new Pages.DiagnosticPage(_shell),
            "settings" => new Pages.SettingsPage(_shell),
            _ => Placeholder(tag),
        };
        _cache[tag] = page;
        return page;
    }

    private FrameworkElement Placeholder(string tag)
    {
        (string title, string subtitle, bool planned) = tag switch
        {
            "outputs" => ("Sorties", "Haut-parleurs, casque, volumes et état des connecteurs.", false),
            "routing" => ("Mixage & routage", "Affectation des canaux, upmix automatique, préservation du vrai 5.1.", false),
            "eq" => ("Égaliseur", "Dix bandes par canal, courbes superposables, comparaison avant/après.", false),
            "bass" => ("Basses & crossover", "Crossover, bass management, dosage du LFE et protection contre la saturation.", false),
            "profiles" => ("Profils", "Réglages Musique, Cinéma, Jeux ; import/export.", false),
            "inputs" => ("Entrées", "Micro, ligne, sélection de source, niveaux et vumètres.", false),
            "spdif" => ("S/PDIF", "PCM et passthrough compressé lorsque validé.", true),
            "diagnostic" => ("Diagnostic", "Format Windows réel, traitement actif, état du pilote et récupération.", false),
            "settings" => ("Réglages", "Préférences de l'application et du centre de contrôle.", false),
            _ => ("Espace", "", false),
        };

        var header = new TextBlock { Text = title, Style = (Style)Application.Current.Resources["CockpitH1"] };
        var sub = new TextBlock
        {
            Text = subtitle,
            Style = (Style)Application.Current.Resources["CockpitLabel"],
            Margin = new Thickness(0, 4, 0, 0),
        };
        var note = new Border
        {
            Style = (Style)Application.Current.Resources["CockpitCard"],
            Margin = new Thickness(0, 26, 0, 0),
            HorizontalAlignment = HorizontalAlignment.Left,
        };
        note.Child = new TextBlock
        {
            Text = planned
                ? "Espace prévu : disponible une fois la fonction du pilote activée."
                : "En cours de construction dans le nouveau cockpit.",
            Style = (Style)Application.Current.Resources["CockpitBody"],
            Foreground = Res("CockpitTextSecondaryBrush"),
        };

        var stack = new StackPanel { Margin = new Thickness(40, 34, 40, 40) };
        stack.Children.Add(header);
        stack.Children.Add(sub);
        stack.Children.Add(note);
        return stack;
    }
}
