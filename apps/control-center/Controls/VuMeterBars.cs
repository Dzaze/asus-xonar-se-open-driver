// SPDX-License-Identifier: Apache-2.0
// Contrôle « VuMeterBars » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Bloc de vumètres verticaux (barres) tracé entièrement en code (pas de XAML) :
//   - une barre par canal (ex. FL FR FC LFE SL SR) : rail sombre à coins arrondis
//     + remplissage turquoise dégradé proportionnel au niveau (dBFS, -60 .. 0) ;
//   - libellé du canal au-dessus, valeur en dBFS en dessous ;
//   - échelle à gauche (0, -10, -20, -30, -40, -60).
// Affichage seul : l'arbre visuel est reconstruit à chaque SetLevels et à chaque
// redimensionnement. Licence Apache-2.0, comme le dépôt.

using System;
using System.Collections.Generic;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

namespace Xonar.ControlCenter.Controls;

/// <summary>
/// Vumètres verticaux « cockpit » (une barre par canal). Contrôle code-only : le
/// rendu est recalculé depuis ActualWidth/ActualHeight à chaque <see cref="SetLevels"/>,
/// à chaque SizeChanged et au chargement. Les couleurs proviennent du thème Cockpit
/// (ressources d'application) avec repli codé en dur. La barre est bornée à l'échelle
/// -60..0 dBFS, mais la valeur affichée reste la mesure fournie (aucune valeur inventée).
/// </summary>
public sealed class VuMeterBars : UserControl
{
    // ----------------------------- Échelle et plage ---------------------------------
    private const double FloorDbfs = -60.0; // bas de l'échelle (dBFS)
    private const double CeilDbfs = 0.0;    // haut de l'échelle (dBFS)

    // Graduations affichées à gauche (volontairement sans -50, conforme au brief).
    private static readonly int[] ScaleTicksDbfs = { 0, -10, -20, -30, -40, -60 };

    // ----------------------------- Métriques de mise en page (px) -------------------
    private const double LabelBandH = 20.0; // bande des libellés (haut)
    private const double ValueBandH = 20.0; // bande des valeurs (bas)
    private const double ScaleW = 40.0;     // colonne d'échelle (gauche)
    private const double BandGap = 8.0;      // marge entre bandes et rails
    private const double RightPad = 4.0;     // marge droite
    private const double ColGap = 10.0;      // espace horizontal entre colonnes
    private const double MinBarW = 6.0;
    private const double MaxBarW = 46.0;
    private const double RailRadius = 4.0;
    private const double CapH = 2.0;         // épaisseur de la crête turquoise
    private const double LabelFontSize = 12.0;
    private const double ValueFontSize = 12.0;
    private const double ScaleFontSize = 10.0;

    // ----------------------------- Replis couleurs (#RRGGBB du thème) ---------------
    // Utilisés si la ressource du thème « cockpit » est introuvable.
    private static readonly Color AccentFallback = Color.FromArgb(0xFF, 0x35, 0xD6, 0xCE);        // #35D6CE
    private static readonly Color AccentDeepFallback = Color.FromArgb(0xFF, 0x17, 0x80, 0x7B);    // #17807B (bas)
    private static readonly Color AccentBrightFallback = Color.FromArgb(0xFF, 0x5B, 0xEE, 0xE6);  // #5BEEE6 (haut)
    private static readonly Color SurfaceInsetFallback = Color.FromArgb(0xFF, 0x0B, 0x0F, 0x17);  // #0B0F17
    private static readonly Color TextPrimaryFallback = Color.FromArgb(0xFF, 0xE9, 0xF0, 0xF6);   // #E9F0F6
    private static readonly Color TextSecondaryFallback = Color.FromArgb(0xFF, 0x93, 0xA2, 0xB2); // #93A2B2
    private static readonly Color TextTertiaryFallback = Color.FromArgb(0xFF, 0x5D, 0x6B, 0x7B);  // #5D6B7B
    private static readonly Color BorderSubtleFallback = Color.FromArgb(0xFF, 0x17, 0x20, 0x2C);  // #17202C

    // Format français : séparateur décimal virgule (ex. « -12,4 »).
    private static readonly NumberFormatInfo FrenchNumbers = new NumberFormatInfo { NumberDecimalSeparator = "," };

    // ----------------------------- État interne -------------------------------------
    private readonly Canvas _canvas = new();
    private Size _size;                              // dernière taille connue du contrôle
    private string[] _labels = Array.Empty<string>();
    private double[] _dbfs = Array.Empty<double>();

    public VuMeterBars()
    {
        // Un Canvas seul a une taille désirée nulle : on fixe des minimales pour
        // rester visible dans un conteneur qui s'ajuste au contenu (ex. StackPanel).
        MinWidth = 180;
        MinHeight = 160;

        _canvas.HorizontalAlignment = HorizontalAlignment.Stretch;
        _canvas.VerticalAlignment = VerticalAlignment.Stretch;
        Content = _canvas;

        SizeChanged += OnSizeChanged;
        Loaded += OnLoaded;
    }

    /// <summary>
    /// Définit/rafraîchit les canaux affichés. <paramref name="labels"/> et
    /// <paramref name="dbfs"/> doivent avoir la même longueur ; sinon la longueur
    /// commune la plus courte est utilisée. Les données sont copiées.
    /// </summary>
    public void SetLevels(IReadOnlyList<string> labels, IReadOnlyList<double> dbfs)
    {
        int count = Math.Min(labels?.Count ?? 0, dbfs?.Count ?? 0);
        var newLabels = new string[count];
        var newDbfs = new double[count];
        for (int i = 0; i < count; i++)
        {
            newLabels[i] = labels![i] ?? string.Empty;
            newDbfs[i] = dbfs![i];
        }

        _labels = newLabels;
        _dbfs = newDbfs;

        // Les contrôles WinUI ne se touchent que sur le thread UI. Si l'appel vient
        // d'un autre thread (notification d'agent), on marshale le rendu.
        var queue = DispatcherQueue;
        if (queue is null || queue.HasThreadAccess)
        {
            Redraw();
        }
        else
        {
            queue.TryEnqueue(Redraw);
        }
    }

    // ----------------------------- Événements de cycle de vie -----------------------

    private void OnLoaded(object sender, RoutedEventArgs e) => Redraw();

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        _size = e.NewSize;
        Redraw();
    }

    // ----------------------------- Rendu --------------------------------------------

    /// <summary>Reconstruit intégralement l'arbre visuel à partir de la taille actuelle.</summary>
    private void Redraw()
    {
        _canvas.Children.Clear();

        double w = _size.Width > 0 ? _size.Width : ActualWidth;
        double h = _size.Height > 0 ? _size.Height : ActualHeight;
        if (w <= 1.0 || h <= 1.0)
        {
            return; // pas encore mesuré : un futur SizeChanged/Loaded relancera le tracé.
        }

        double railTop = LabelBandH + BandGap;
        double railBottom = h - ValueBandH - BandGap;
        double railHeight = railBottom - railTop;
        if (railHeight <= 2.0)
        {
            return; // trop court pour dessiner.
        }

        // ------------------------- Échelle à gauche -------------------------
        Brush scaleBrush = Res("CockpitTextTertiaryBrush", TextTertiaryFallback);
        Brush tickBrush = Res("CockpitBorderSubtleBrush", BorderSubtleFallback);

        foreach (int tick in ScaleTicksDbfs)
        {
            double frac = (tick - FloorDbfs) / (CeilDbfs - FloorDbfs); // 0..1
            double y = railBottom - frac * railHeight;

            TextBlock scaleLabel = MakeText(
                tick.ToString(CultureInfo.InvariantCulture),
                ScaleFontSize,
                Microsoft.UI.Text.FontWeights.Normal,
                scaleBrush,
                ScaleW - 10.0,
                TextAlignment.Right);
            Canvas.SetLeft(scaleLabel, 0.0);
            Canvas.SetTop(scaleLabel, y - (scaleLabel.DesiredSize.Height / 2.0));
            _canvas.Children.Add(scaleLabel);

            var tickMark = new Rectangle
            {
                Width = 6.0,
                Height = 1.0,
                Fill = tickBrush,
                Opacity = 0.7,
                IsHitTestVisible = false,
            };
            Canvas.SetLeft(tickMark, ScaleW - 8.0);
            Canvas.SetTop(tickMark, y);
            _canvas.Children.Add(tickMark);
        }

        int n = _labels.Length; // == _dbfs.Length (garanti par SetLevels)
        if (n == 0)
        {
            return;
        }

        double barsLeft = ScaleW + BandGap;
        double barsWidth = w - barsLeft - RightPad;
        if (barsWidth <= 2.0)
        {
            return;
        }

        double colW = barsWidth / n;
        double barW = Math.Clamp(colW - ColGap, MinBarW, MaxBarW);

        // ------------------------- Pinceaux (résolus une fois) -------------------------
        Brush railBg = Res("CockpitSurfaceInsetBrush", SurfaceInsetFallback);
        Brush railStroke = Res("CockpitBorderSubtleBrush", BorderSubtleFallback);
        Brush capBrush = Res("CockpitAccent", AccentFallback);
        Brush labelBrush = Res("CockpitTextSecondaryBrush", TextSecondaryFallback);
        Brush valueBrush = Res("CockpitTextPrimaryBrush", TextPrimaryFallback);
        Brush fillBrush = AccentFillBrush();

        // ------------------------- Barres -------------------------
        for (int i = 0; i < n; i++)
        {
            double colX = barsLeft + i * colW;
            double barX = colX + (colW - barW) / 2.0;

            // Rail de fond sombre, coins légèrement arrondis.
            var rail = new Rectangle
            {
                Width = barW,
                Height = railHeight,
                RadiusX = RailRadius,
                RadiusY = RailRadius,
                Fill = railBg,
                Stroke = railStroke,
                StrokeThickness = 1.0,
                IsHitTestVisible = false,
            };
            Canvas.SetLeft(rail, barX);
            Canvas.SetTop(rail, railTop);
            _canvas.Children.Add(rail);

            // Remplissage proportionnel au niveau, ancré en bas (valeur bornée à l'échelle).
            double db = _dbfs[i];
            double level = double.IsNaN(db) || double.IsInfinity(db) ? FloorDbfs : db;
            double fillFrac = Math.Clamp((level - FloorDbfs) / (CeilDbfs - FloorDbfs), 0.0, 1.0);
            double fillH = fillFrac * railHeight;

            if (fillH > 0.5)
            {
                double innerW = Math.Max(0.0, barW - 2.0);
                double fillY = railTop + (railHeight - fillH);

                var fill = new Rectangle
                {
                    Width = innerW,
                    Height = fillH,
                    RadiusX = Math.Max(0.0, RailRadius - 1.0),
                    RadiusY = Math.Max(0.0, RailRadius - 1.0),
                    Fill = fillBrush,
                    IsHitTestVisible = false,
                };
                Canvas.SetLeft(fill, barX + 1.0);
                Canvas.SetTop(fill, fillY);
                _canvas.Children.Add(fill);

                // Crête turquoise claire au sommet du remplissage (repère de niveau).
                var cap = new Rectangle
                {
                    Width = innerW,
                    Height = CapH,
                    RadiusX = 1.0,
                    RadiusY = 1.0,
                    Fill = capBrush,
                    IsHitTestVisible = false,
                };
                Canvas.SetLeft(cap, barX + 1.0);
                Canvas.SetTop(cap, fillY);
                _canvas.Children.Add(cap);
            }

            // Libellé du canal au-dessus de la barre.
            TextBlock channelLabel = MakeText(
                _labels[i],
                LabelFontSize,
                Microsoft.UI.Text.FontWeights.SemiBold,
                labelBrush,
                colW,
                TextAlignment.Center);
            Canvas.SetLeft(channelLabel, colX);
            Canvas.SetTop(channelLabel, (LabelBandH - channelLabel.DesiredSize.Height) / 2.0);
            _canvas.Children.Add(channelLabel);

            // Valeur en dBFS sous la barre (séparateur décimal français).
            TextBlock value = MakeText(
                FormatDbfs(db),
                ValueFontSize,
                Microsoft.UI.Text.FontWeights.Normal,
                valueBrush,
                colW,
                TextAlignment.Center);
            Canvas.SetLeft(value, colX);
            Canvas.SetTop(value, (h - ValueBandH) + ((ValueBandH - value.DesiredSize.Height) / 2.0));
            _canvas.Children.Add(value);
        }
    }

    // ----------------------------- Aides ---------------------------------------------

    /// <summary>Crée un TextBlock (largeur fixe + alignement) et le mesure pour permettre un centrage exact.</summary>
    private static TextBlock MakeText(
        string text,
        double fontSize,
        Windows.UI.Text.FontWeight weight,
        Brush brush,
        double width,
        TextAlignment align)
    {
        var tb = new TextBlock
        {
            Text = text,
            FontSize = fontSize,
            FontWeight = weight,
            Foreground = brush,
            TextAlignment = align,
            TextTrimming = TextTrimming.CharacterEllipsis,
            IsHitTestVisible = false,
        };

        if (width > 0.0)
        {
            tb.Width = width;
        }

        // Mesure hors arbre visuel : DesiredSize.Height sert au centrage vertical.
        tb.Measure(new Size(width > 0.0 ? width : double.PositiveInfinity, double.PositiveInfinity));
        return tb;
    }

    /// <summary>
    /// Pinceau de remplissage : dégradé turquoise vertical du thème
    /// (CockpitAccentGradient, sombre en bas -> clair en haut) ; repli sur un dégradé
    /// équivalent construit en code si la ressource est absente.
    /// </summary>
    private static Brush AccentFillBrush()
    {
        try
        {
            ResourceDictionary? res = Application.Current?.Resources;
            if (res is not null && res["CockpitAccentGradient"] is Brush themed)
            {
                return themed;
            }
        }
        catch
        {
            // Ressource absente : dégradé de repli ci-dessous.
        }

        var brush = new LinearGradientBrush
        {
            StartPoint = new Point(0, 1), // bas
            EndPoint = new Point(0, 0),   // haut
        };
        brush.GradientStops.Add(new GradientStop { Offset = 0.0, Color = AccentDeepFallback });
        brush.GradientStops.Add(new GradientStop { Offset = 1.0, Color = AccentBrightFallback });
        return brush;
    }

    /// <summary>
    /// Résout un pinceau du thème via <c>Application.Current.Resources</c> (l'indexeur
    /// traverse les dictionnaires fusionnés, Themes/Cockpit.xaml). Repli codé en dur si
    /// la clé est absente, et conversion Color -> SolidColorBrush si la clé désigne une
    /// Color (ex. « CockpitAccent ») plutôt qu'un Brush.
    /// </summary>
    private static Brush Res(string key, Color fallback)
    {
        try
        {
            ResourceDictionary? res = Application.Current?.Resources;
            if (res is not null)
            {
                object? value = res[key];
                if (value is Brush brush)
                {
                    return brush;
                }
                if (value is Color color)
                {
                    return new SolidColorBrush(color);
                }
            }
        }
        catch
        {
            // Clé absente ou ressources indisponibles : on retombe sur le repli.
        }

        return new SolidColorBrush(fallback);
    }

    /// <summary>Met en forme une valeur dBFS en français (« -12,4 ») ; silence/non fini -> plancher.</summary>
    private static string FormatDbfs(double db)
    {
        if (double.IsNaN(db) || double.IsInfinity(db))
        {
            db = FloorDbfs; // silence numérique représenté au plancher
        }

        return db.ToString("0.0", FrenchNumbers);
    }
}
