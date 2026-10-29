// SPDX-License-Identifier: Apache-2.0
// Page « Entrées » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
//
// Cette page est FONCTIONNELLE : elle pilote l'endpoint de CAPTURE Windows de la
// Xonar SE via les API Core Audio (WASAPI), SANS élévation et SANS dépendance au
// pilote. Elle offre :
//   - un vumètre temps réel du niveau d'entrée (IAudioMeterInformation.GetPeakValue,
//     rafraîchi ~30 ms via un minuteur de la file du dispatcher) ;
//   - un curseur de volume micro lié à IAudioEndpointVolume (lecture/écriture) ;
//   - un bouton de sourdine lié au mute de IAudioEndpointVolume ;
//   - un sélecteur de source (Micro / Ligne / Mixage) volontairement DÉSACTIVÉ
//     (aperçu « Prévu ») : le pilote n'expose pas encore de commutation matérielle.
// Si l'endpoint de capture Xonar est absent, la page le signale honnêtement et
// propose une nouvelle tentative de détection. Interface construite en code, sur le
// modèle d'OverviewPage ; styles et pinceaux résolus depuis le thème Cockpit.

using System;
using System.Globalization;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Xonar.ControlCenter.Controls;
using Xonar.ControlCenter.Services;

namespace Xonar.ControlCenter.Pages;

/// <summary>
/// Page « Entrées » : capture microphone (niveau, volume, sourdine) via WASAPI, plus
/// aperçu « Prévu » du sélecteur de source matériel.
/// </summary>
public sealed class InputsPage : Microsoft.UI.Xaml.Controls.UserControl
{
    // ----------------------------- Réglages du vumètre -----------------------------
    private const double FloorDb = -60.0;         // plancher de l'échelle (dBFS)
    private const double CeilDb = 0.0;            // plafond de l'échelle (dBFS)
    private const double ReleaseFactor = 0.28;    // relâchement par tick (attaque instantanée)
    private const int TimerIntervalMs = 30;       // ~33 rafraîchissements/seconde
    private const int PollEveryTicks = 16;        // resynchro volume/sourdine (~0,5 s)
    private static readonly string[] LevelLabels = { "Entrée" };
    private const string XonarNameFragment = "Xonar";

    // ----------------------------- Endpoint de capture Core Audio ------------------
    private CaptureAudioEndpoint? _endpoint;

    // ----------------------------- Contrôles persistants ---------------------------
    private readonly Border _bodyHost = new();
    private readonly VuMeterBars _vu = new();
    private readonly Slider _volume = new();
    private readonly TextBlock _volumeValue = new();
    private readonly Button _muteButton = new();
    private readonly FontIcon _muteIcon = new();
    private readonly TextBlock _muteText = new();

    // ----------------------------- Minuteur de rafraîchissement --------------------
    private DispatcherQueueTimer? _timer;

    // ----------------------------- État de synchronisation -------------------------
    private bool _suspend;                        // garde anti-réentrance (écriture -> contrôle)
    private bool _muted;                          // dernier état de sourdine connu
    private double _displayDb = FloorDb;          // niveau lissé affiché
    private DateTime _lastUserVolumeChangeUtc = DateTime.MinValue;
    private int _pollCounter;

    // ----------------------------- Accès au thème ----------------------------------
    private Brush B(string key) => (Brush)Application.Current.Resources[key];
    private Style S(string key) => (Style)Application.Current.Resources[key];

    // Le shell est accepté pour la parité de signature avec les autres pages ; cette
    // page dialogue directement avec Core Audio (WASAPI) et n'a pas besoin de l'agent.
    public InputsPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _ = shell;

        BuildPersistentControls();
        BuildRoot();

        // Résolution initiale de l'endpoint + construction du corps adapté.
        RebuildBody();

        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    // ===========================================================================
    // Cycle de vie : le minuteur ne tourne que lorsque la page est affichée.
    // ===========================================================================
    private void OnLoaded(object sender, RoutedEventArgs e) => StartMeterIfPossible();

    private void OnUnloaded(object sender, RoutedEventArgs e) => StopMeter();

    private void StartMeterIfPossible()
    {
        if (_endpoint is null)
        {
            StopMeter();
            return;
        }

        _timer ??= CreateTimer();
        SyncVolumeAndMute(force: true);
        if (!_timer.IsRunning)
        {
            _timer.Start();
        }
    }

    private void StopMeter()
    {
        try
        {
            _timer?.Stop();
        }
        catch
        {
            // Arrêt best-effort : sans conséquence si le minuteur n'était pas démarré.
        }
    }

    private DispatcherQueueTimer CreateTimer()
    {
        var timer = DispatcherQueue.CreateTimer();
        timer.Interval = TimeSpan.FromMilliseconds(TimerIntervalMs);
        timer.IsRepeating = true;
        timer.Tick += (_, _) => OnTick();
        return timer;
    }

    // Un tick : met à jour le niveau (lissé) et, périodiquement, resynchronise
    // volume et sourdine depuis le matériel (changements externes).
    private void OnTick()
    {
        double target = FloorDb;
        if (_endpoint is not null && _endpoint.TryGetPeak(out float peak) && peak > 0f)
        {
            double db = 20.0 * Math.Log10(peak);
            target = double.IsNaN(db) || double.IsInfinity(db)
                ? FloorDb
                : Math.Clamp(db, FloorDb, CeilDb);
        }

        // Attaque instantanée (montée), relâchement progressif (descente plus lisible).
        _displayDb = target >= _displayDb
            ? target
            : _displayDb + (target - _displayDb) * ReleaseFactor;

        _vu.SetLevels(LevelLabels, new[] { _displayDb });

        if (++_pollCounter >= PollEveryTicks)
        {
            _pollCounter = 0;
            SyncVolumeAndMute(force: false);
        }
    }

    // ===========================================================================
    // Construction de l'ossature (en-tête + hôte de corps dans un ScrollViewer)
    // ===========================================================================
    private void BuildRoot()
    {
        var header = new StackPanel();
        header.Children.Add(new TextBlock { Text = "Entrées", Style = S("CockpitH1") });
        header.Children.Add(new TextBlock
        {
            Text = "Microphone, niveau d'entrée et sélection de source.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 2, 0, 0),
        });

        var column = new StackPanel { Spacing = 18 };
        column.Children.Add(header);
        column.Children.Add(_bodyHost);

        var root = new Grid
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
        };
        root.Children.Add(column);

        Content = new ScrollViewer
        {
            Content = root,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // Prépare les contrôles réutilisés et câble leurs gestionnaires une seule fois
    // (le corps peut être reconstruit sans ré-abonner les évènements).
    private void BuildPersistentControls()
    {
        // --- Curseur de volume d'entrée (0..100 %) ---
        _volume.Minimum = 0;
        _volume.Maximum = 100;
        _volume.StepFrequency = 1;
        _volume.HorizontalAlignment = HorizontalAlignment.Stretch;
        _volume.ValueChanged += (_, e) =>
        {
            if (_suspend)
            {
                return;
            }
            _lastUserVolumeChangeUtc = DateTime.UtcNow;
            _endpoint?.SetVolumeScalar((float)(e.NewValue / 100.0));
            _volumeValue.Text = FormatPercent(e.NewValue);
        };

        _volumeValue.Style = S("CockpitBody");
        _volumeValue.VerticalAlignment = VerticalAlignment.Center;
        _volumeValue.MinWidth = 46;
        _volumeValue.TextAlignment = TextAlignment.Right;
        _volumeValue.Text = FormatPercent(0);

        // --- Bouton de sourdine ---
        _muteIcon.Glyph = ""; // Microphone (Segoe Fluent/MDL2)
        _muteIcon.FontSize = 15;
        _muteIcon.VerticalAlignment = VerticalAlignment.Center;
        _muteText.FontSize = 13;
        _muteText.VerticalAlignment = VerticalAlignment.Center;

        var muteRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9 };
        muteRow.Children.Add(_muteIcon);
        muteRow.Children.Add(_muteText);
        _muteButton.Style = S("CockpitGhostButton");
        _muteButton.Content = muteRow;
        _muteButton.HorizontalAlignment = HorizontalAlignment.Left;
        _muteButton.Click += (_, _) => ToggleMute();
        ToolTipService.SetToolTip(_muteButton, "Activer ou couper la sourdine du micro");
        UpdateMuteVisual(false);

        // --- Vumètre ---
        _vu.MinHeight = 240;
        _vu.HorizontalAlignment = HorizontalAlignment.Stretch;
        _vu.SetLevels(LevelLabels, new[] { FloorDb });

        // --- Hôte de corps : s'étire à la largeur disponible ---
        _bodyHost.Background = new SolidColorBrush(Microsoft.UI.Colors.Transparent);
    }

    // Résout l'endpoint (si besoin) et installe le corps adapté (détecté / absent).
    private void RebuildBody()
    {
        _endpoint ??= CaptureAudioEndpoint.Resolve(XonarNameFragment);
        _bodyHost.Child = _endpoint is null ? BuildNotFoundBody() : BuildFoundBody();
    }

    // ===========================================================================
    // Corps « détecté » : Microphone + Niveau, puis Source d'entrée (aperçu).
    // ===========================================================================
    private UIElement BuildFoundBody()
    {
        var row = new Grid { ColumnSpacing = 18 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(360) });

        var mic = MicrophoneCard();
        row.Children.Add(mic);

        var level = LevelCard();
        Grid.SetColumn(level, 1);
        row.Children.Add(level);

        var column = new StackPanel { Spacing = 18 };
        column.Children.Add(row);
        column.Children.Add(SourceCard());
        return column;
    }

    // Carte « Microphone » : périphérique résolu, volume et sourdine (Core Audio).
    private Border MicrophoneCard()
    {
        bool hasVolume = _endpoint!.HasVolumeControl;

        var stack = new StackPanel { Spacing = 16 };
        stack.Children.Add(new TextBlock { Text = "Microphone", Style = S("CockpitH2") });

        // Périphérique résolu (nom convivial Windows).
        var device = new StackPanel { Spacing = 2 };
        device.Children.Add(new TextBlock { Text = "Périphérique", Style = S("CockpitLabel") });
        device.Children.Add(new TextBlock { Text = _endpoint.FriendlyName, Style = S("CockpitBody") });
        stack.Children.Add(device);

        // Volume d'entrée : curseur + pourcentage.
        _volume.IsEnabled = hasVolume;
        var sliderCell = new Grid { ColumnSpacing = 12 };
        sliderCell.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        sliderCell.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        Grid.SetColumn(_volume, 0);
        sliderCell.Children.Add(_volume);
        Grid.SetColumn(_volumeValue, 1);
        sliderCell.Children.Add(_volumeValue);
        stack.Children.Add(Field("Volume d'entrée", sliderCell));

        // Bouton de sourdine.
        _muteButton.IsEnabled = hasVolume;
        stack.Children.Add(Field("Sourdine", _muteButton));

        if (!hasVolume)
        {
            stack.Children.Add(new TextBlock
            {
                Text = "Ce périphérique n'expose pas de réglage de volume via Windows.",
                Style = S("CockpitCaption"),
            });
        }

        stack.Children.Add(new TextBlock
        {
            Text = "Volume et sourdine agissent sur l'endpoint de capture Windows (WASAPI), "
                 + "sans passer par le pilote et sans élévation.",
            Style = S("CockpitCaption"),
        });

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    // Carte « Niveau d'entrée » : vumètre temps réel (crête master de l'endpoint).
    private Border LevelCard()
    {
        var content = new StackPanel { Spacing = 12 };
        content.Children.Add(new TextBlock { Text = "Niveau d'entrée", Style = S("CockpitH2") });
        content.Children.Add(new TextBlock
        {
            Text = "Crête temps réel (dBFS) mesurée sur l'endpoint de capture.",
            Style = S("CockpitCaption"),
            Margin = new Thickness(0, -4, 0, 2),
        });
        content.Children.Add(_vu);
        return new Border { Style = S("CockpitCard"), Child = content };
    }

    // ===========================================================================
    // Corps « absent » : message honnête + nouvelle tentative, puis Source d'entrée.
    // ===========================================================================
    private UIElement BuildNotFoundBody()
    {
        var column = new StackPanel { Spacing = 18 };
        column.Children.Add(NotFoundCard());
        column.Children.Add(SourceCard());
        return column;
    }

    private Border NotFoundCard()
    {
        var grid = new Grid { ColumnSpacing = 14 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var icon = new FontIcon
        {
            Glyph = "", // Microphone
            FontSize = 20,
            Foreground = B("CockpitWarningBrush"),
            VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(0, 2, 0, 0),
        };
        grid.Children.Add(icon);

        var text = new StackPanel { Spacing = 6 };
        text.Children.Add(new TextBlock { Text = "Microphone Xonar non détecté", Style = S("CockpitH2") });
        text.Children.Add(new TextBlock
        {
            Text = "Aucun périphérique de capture Windows dont le nom contient « Xonar » n'a été trouvé. "
                 + "Vérifiez que la carte est présente et que l'entrée micro est activée dans Windows, "
                 + "puis relancez la détection.",
            Style = S("CockpitBody"),
            Foreground = B("CockpitTextSecondaryBrush"),
        });

        var retry = new Button { Style = S("CockpitGhostButton"), Content = "Réessayer la détection", HorizontalAlignment = HorizontalAlignment.Left };
        retry.Click += (_, _) =>
        {
            _endpoint = CaptureAudioEndpoint.Resolve(XonarNameFragment);
            RebuildBody();
            StartMeterIfPossible();
        };
        text.Children.Add(retry);

        Grid.SetColumn(text, 1);
        grid.Children.Add(text);

        return new Border { Style = S("CockpitCard"), Child = grid };
    }

    // ===========================================================================
    // Carte « Source d'entrée » : sélecteur visuel DÉSACTIVÉ (aperçu « Prévu »).
    // ===========================================================================
    private Border SourceCard()
    {
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(new TextBlock { Text = "Source d'entrée", Style = S("CockpitH2"), VerticalAlignment = VerticalAlignment.Center });
        var badge = PlannedBadge();
        Grid.SetColumn(badge, 1);
        head.Children.Add(badge);

        var content = new StackPanel { Spacing = 14 };
        content.Children.Add(head);
        content.Children.Add(new TextBlock
        {
            Text = "Sélection matérielle de la source captée (micro, entrée ligne ou mixage). "
                 + "Le pilote n'expose pas encore de commande de commutation : cet aperçu est inactif.",
            Style = S("CockpitCaption"),
        });
        content.Children.Add(SourceSelector());

        return new Border { Style = S("CockpitCard"), Child = content };
    }

    // Sélecteur segmenté (Micro / Ligne / Mixage) : « Micro » marqué comme actif,
    // ensemble grisé et non interactif, infobulle explicative. Reste testable au
    // survol (hit-test actif) pour afficher l'infobulle, mais n'agit sur rien.
    private FrameworkElement SourceSelector()
    {
        string[] options = { "Micro", "Ligne", "Mixage" };
        var inner = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 4 };

        for (int i = 0; i < options.Length; i++)
        {
            bool on = i == 0;
            var label = new TextBlock
            {
                Text = options[i],
                FontSize = 14,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = on
                    ? new SolidColorBrush(Microsoft.UI.ColorHelper.FromArgb(255, 7, 23, 26))
                    : B("CockpitTextSecondaryBrush"),
                VerticalAlignment = VerticalAlignment.Center,
                HorizontalAlignment = HorizontalAlignment.Center,
            };
            var seg = new Border
            {
                CornerRadius = new CornerRadius(8),
                Padding = new Thickness(24, 8, 24, 8),
                Background = on ? B("CockpitAccentBrush") : new SolidColorBrush(Microsoft.UI.Colors.Transparent),
                Child = label,
            };
            inner.Children.Add(seg);
        }

        var container = new Border
        {
            Background = B("CockpitSurfaceInsetBrush"),
            BorderBrush = B("CockpitBorderSubtleBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(5),
            HorizontalAlignment = HorizontalAlignment.Left,
            Opacity = 0.55, // lecture « désactivé »
            Child = inner,
        };
        ToolTipService.SetToolTip(container, "Prévu (sélecteur matériel)");
        return container;
    }

    // ===========================================================================
    // Interaction sourdine
    // ===========================================================================
    private void ToggleMute()
    {
        if (_endpoint is null || !_endpoint.HasVolumeControl)
        {
            return;
        }

        _endpoint.SetMute(!_muted);

        // Relit l'état réel (le matériel/Windows fait foi) avant de rafraîchir le visuel.
        bool actual = !_muted;
        if (_endpoint.TryGetMute(out bool read))
        {
            actual = read;
        }
        UpdateMuteVisual(actual);
    }

    private void UpdateMuteVisual(bool muted)
    {
        _muted = muted;
        _muteText.Text = muted ? "Micro coupé" : "Micro actif";
        Brush brush = muted ? B("CockpitWarningBrush") : B("CockpitAccentBrightBrush");
        _muteIcon.Foreground = brush;
        _muteText.Foreground = brush;
    }

    // Resynchronise curseur de volume et sourdine depuis le matériel. Le volume n'est
    // pas réécrit si l'utilisateur vient d'agir sur le curseur (évite la lutte).
    private void SyncVolumeAndMute(bool force)
    {
        if (_endpoint is null)
        {
            return;
        }

        if (_endpoint.HasVolumeControl
            && (force || (DateTime.UtcNow - _lastUserVolumeChangeUtc).TotalMilliseconds > 750))
        {
            if (_endpoint.TryGetVolumeScalar(out float scalar))
            {
                double percent = Math.Clamp(scalar * 100.0, 0.0, 100.0);
                if (force || Math.Abs(percent - _volume.Value) > 0.8)
                {
                    _suspend = true;
                    try
                    {
                        _volume.Value = percent;
                    }
                    finally
                    {
                        _suspend = false;
                    }
                    _volumeValue.Text = FormatPercent(percent);
                }
            }
        }

        if (_endpoint.TryGetMute(out bool muted) && (force || muted != _muted))
        {
            UpdateMuteVisual(muted);
        }
    }

    // ===========================================================================
    // Aides
    // ===========================================================================
    private StackPanel Field(string label, FrameworkElement control)
    {
        var stack = new StackPanel { Spacing = 7 };
        stack.Children.Add(new TextBlock { Text = label, Style = S("CockpitLabel") });
        stack.Children.Add(control);
        return stack;
    }

    private Border PlannedBadge()
    {
        return new Border
        {
            Style = S("CockpitBadge"),
            HorizontalAlignment = HorizontalAlignment.Right,
            Child = new TextBlock
            {
                Text = "Prévu",
                FontSize = 11,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                Foreground = B("CockpitTextTertiaryBrush"),
            },
        };
    }

    private static string FormatPercent(double value)
    {
        int rounded = (int)Math.Round(value, MidpointRounding.AwayFromZero);
        return rounded.ToString(CultureInfo.InvariantCulture) + " %";
    }
}
