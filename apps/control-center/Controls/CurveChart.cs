// SPDX-License-Identifier: Apache-2.0
// Contrôle « CurveChart » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Graphe de réponse en fréquence tracé entièrement en code (pas de XAML) :
//   - axe X logarithmique 20 Hz – 20 kHz avec graduations étiquetées ;
//   - axe Y linéaire en dB (lignes horizontales étiquetées, 0 dB accentué) ;
//   - courbe turquoise lissée passant par les nœuds, zone sous la courbe translucide,
//     grille discrète.
// Deux usages : courbe d'égaliseur (Y -12…+12 dB) et courbe de crossover (Y -24…0 dB).
// Affichage seul (aucune édition interactive des points). Licence Apache-2.0, comme le dépôt.

using System;
using System.Collections.Generic;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

// « Path » existe aussi sous System.IO (tiré par ImplicitUsings) : on lève l'ambiguïté
// avec un alias vers la forme vectorielle WinUI.
using ShapePath = Microsoft.UI.Xaml.Shapes.Path;

namespace Xonar.ControlCenter.Controls;

/// <summary>
/// Graphe de courbe (réponse en fréquence) dessiné en code pour le centre de contrôle.
/// Axe X logarithmique de 20 Hz à 20 kHz, axe Y linéaire en dB bornée par
/// <see cref="YMin"/>/<see cref="YMax"/>. Appeler <see cref="SetCurve"/> pour tracer.
/// Le rendu est recalculé à chaque appel de <see cref="SetCurve"/>, à chaque
/// redimensionnement et lorsque <see cref="YMin"/> ou <see cref="YMax"/> changent.
/// </summary>
public sealed class CurveChart : UserControl
{
    // ----------------------------- Constantes de tracé -----------------------------

    private const double FreqMin = 20.0;        // borne gauche de l'axe X (Hz)
    private const double FreqMax = 20000.0;     // borne droite de l'axe X (Hz)

    // Marges internes réservées aux étiquettes (gauche pour les dB, bas pour les Hz).
    private const double MarginLeft = 34.0;
    private const double MarginRight = 12.0;
    private const double MarginTop = 12.0;
    private const double MarginBottom = 20.0;

    private const double AxisFontSize = 10.5;

    // Graduations de l'axe X (fréquences) et leurs étiquettes compactes.
    private static readonly double[] GridFreqs =
        { 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000 };
    private static readonly string[] GridFreqLabels =
        { "20", "50", "100", "200", "500", "1k", "2k", "5k", "10k", "20k" };

    // ----------------------------- Repli couleurs (#RRGGBB du thème) ----------------
    // Utilisés si la ressource du thème « cockpit » est introuvable.
    private static readonly Color AccentFallback = Color.FromArgb(0xFF, 0x35, 0xD6, 0xCE); // #35D6CE
    private static readonly Color GridFallback = Color.FromArgb(0xFF, 0x17, 0x20, 0x2C);   // #17202C
    private static readonly Color LabelFallback = Color.FromArgb(0xFF, 0x5D, 0x6B, 0x7B);  // #5D6B7B
    private static readonly Color InsetFallback = Color.FromArgb(0xFF, 0x0B, 0x0F, 0x17);  // #0B0F17

    // ----------------------------- État interne -------------------------------------

    private readonly Canvas _canvas = new();      // surface racine (grille + étiquettes)
    private readonly Canvas _plotLayer = new();   // couche écrêtée (remplissage + courbe + nœuds)
    private Size _size;                            // dernière taille connue du contrôle
    private double[] _freqs = Array.Empty<double>();
    private double[] _values = Array.Empty<double>();

    public CurveChart()
    {
        // Taille minimale raisonnable pour rester visible dans une mise en page « auto ».
        MinWidth = 260;
        MinHeight = 160;

        Content = _canvas;
        SizeChanged += OnSizeChanged;
        Loaded += OnLoaded;
    }

    // ----------------------------- Propriétés de dépendance -------------------------

    /// <summary>Valeur minimale de l'axe Y, en dB (défaut -12).</summary>
    public static readonly DependencyProperty YMinProperty = DependencyProperty.Register(
        nameof(YMin), typeof(double), typeof(CurveChart), new PropertyMetadata(-12.0, OnRangeChanged));

    public double YMin
    {
        get => (double)GetValue(YMinProperty);
        set => SetValue(YMinProperty, value);
    }

    /// <summary>Valeur maximale de l'axe Y, en dB (défaut +12).</summary>
    public static readonly DependencyProperty YMaxProperty = DependencyProperty.Register(
        nameof(YMax), typeof(double), typeof(CurveChart), new PropertyMetadata(12.0, OnRangeChanged));

    public double YMax
    {
        get => (double)GetValue(YMaxProperty);
        set => SetValue(YMaxProperty, value);
    }

    private static void OnRangeChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is CurveChart chart)
        {
            chart.Redraw();
        }
    }

    // ----------------------------- API publique -------------------------------------

    /// <summary>
    /// Trace la courbe. <paramref name="freqsHz"/> en Hz (croissantes, dans [20, 20000])
    /// et <paramref name="values"/> en dB, de même longueur. Les données sont copiées ;
    /// en cas de longueurs différentes, la longueur commune la plus courte est utilisée.
    /// </summary>
    public void SetCurve(IReadOnlyList<double> freqsHz, IReadOnlyList<double> values)
    {
        if (freqsHz is null || values is null)
        {
            _freqs = Array.Empty<double>();
            _values = Array.Empty<double>();
        }
        else
        {
            int n = Math.Min(freqsHz.Count, values.Count);
            var f = new double[n];
            var v = new double[n];
            for (int i = 0; i < n; i++)
            {
                f[i] = freqsHz[i];
                v[i] = values[i];
            }

            _freqs = f;
            _values = v;
        }

        Redraw();
    }

    // ----------------------------- Événements de cycle de vie -----------------------

    private void OnLoaded(object sender, RoutedEventArgs e) => Redraw();

    private void OnSizeChanged(object sender, SizeChangedEventArgs e)
    {
        _size = e.NewSize;
        Redraw();
    }

    // ----------------------------- Rendu --------------------------------------------

    private void Redraw()
    {
        _canvas.Children.Clear();
        _plotLayer.Children.Clear();

        double w = _size.Width > 0 ? _size.Width : ActualWidth;
        double h = _size.Height > 0 ? _size.Height : ActualHeight;
        if (w <= 0 || h <= 0)
        {
            return; // pas encore mesuré : un futur SizeChanged/Loaded relancera le tracé.
        }

        // Pinceaux : ressources du thème « cockpit » avec repli codé en dur.
        Brush gridBrush = Res("CockpitBorderSubtleBrush", GridFallback);
        Brush labelBrush = Res("CockpitTextTertiaryBrush", LabelFallback);
        Brush accentBrush = Res("CockpitAccentBrush", AccentFallback);
        Brush insetBrush = Res("CockpitSurfaceInsetBrush", InsetFallback);
        Color accentColor = (accentBrush as SolidColorBrush)?.Color ?? AccentFallback;

        // Fond optionnel (surface « inset ») sous toute la zone.
        _canvas.Children.Add(new Rectangle
        {
            Width = w,
            Height = h,
            Fill = insetBrush,
            RadiusX = 8,
            RadiusY = 8,
            IsHitTestVisible = false,
        });

        double left = MarginLeft;
        double top = MarginTop;
        double plotW = w - MarginLeft - MarginRight;
        double plotH = h - MarginTop - MarginBottom;
        if (plotW <= 4 || plotH <= 4)
        {
            return; // trop petit pour un tracé lisible.
        }

        double span = YMax - YMin;
        if (span <= 0)
        {
            return; // plage Y invalide : rien à projeter.
        }

        double baseline = top + plotH;
        double logMin = Math.Log10(FreqMin);
        double logSpan = Math.Log10(FreqMax) - logMin;

        // Projections : X logarithmique, Y linéaire (YMax en haut, YMin en bas).
        double XOf(double f)
        {
            double fc = f < FreqMin ? FreqMin : (f > FreqMax ? FreqMax : f);
            return left + (Math.Log10(fc) - logMin) / logSpan * plotW;
        }

        double YOf(double db) => top + (YMax - db) / span * plotH;

        // --- Grille verticale + étiquettes de fréquence (en bas) ---
        for (int i = 0; i < GridFreqs.Length; i++)
        {
            double x = XOf(GridFreqs[i]);
            _canvas.Children.Add(MakeLine(x, top, x, baseline, gridBrush, 1));

            TextBlock label = MakeLabel(GridFreqLabels[i], labelBrush);
            double lx = x - (label.DesiredSize.Width / 2.0);
            lx = Math.Max(0, Math.Min(lx, w - label.DesiredSize.Width));
            Canvas.SetLeft(label, lx);
            Canvas.SetTop(label, baseline + 4);
            _canvas.Children.Add(label);
        }

        // --- Grille horizontale + étiquettes dB (à gauche) ---
        double step = ChooseStep(span);
        double firstLine = Math.Ceiling((YMin - 1e-6) / step) * step;
        for (double db = firstLine; db <= YMax + 1e-6; db += step)
        {
            double y = YOf(db);
            bool isZero = Math.Abs(db) < 1e-6 && YMin < -1e-6 && YMax > 1e-6;

            // La ligne du 0 dB (si dans la plage) est un peu plus visible.
            _canvas.Children.Add(MakeLine(
                left, y, left + plotW, y,
                isZero ? labelBrush : gridBrush,
                isZero ? 1.2 : 1));

            TextBlock label = MakeLabel(FormatDb(db), labelBrush);
            double ly = Math.Max(0, left - 6 - label.DesiredSize.Width);
            Canvas.SetLeft(label, ly);
            Canvas.SetTop(label, y - (label.DesiredSize.Height / 2.0));
            _canvas.Children.Add(label);
        }

        // --- Courbe, remplissage et nœuds (couche écrêtée à la zone de tracé) ---
        // UIElement.Clip est de type RectangleGeometry en WinUI : écrêtage rectangulaire.
        _plotLayer.Clip = new RectangleGeometry { Rect = new Rect(left, top, plotW, plotH) };

        int count = Math.Min(_freqs.Length, _values.Length);
        if (count >= 1)
        {
            var pts = new Point[count];
            for (int i = 0; i < count; i++)
            {
                pts[i] = new Point(XOf(_freqs[i]), YOf(_values[i]));
            }

            if (count >= 2)
            {
                (Point start, List<(Point c1, Point c2, Point end)> segs) = BuildSpline(pts);

                // Zone translucide sous la courbe : bas-gauche -> premier point -> courbe
                // -> bas-droite, puis fermeture automatique vers le point de départ.
                var fillFigure = new PathFigure
                {
                    StartPoint = new Point(start.X, baseline),
                    IsClosed = true,
                    IsFilled = true,
                    Segments = new PathSegmentCollection(),
                };
                fillFigure.Segments.Add(new LineSegment { Point = start });
                foreach ((Point c1, Point c2, Point end) in segs)
                {
                    fillFigure.Segments.Add(new BezierSegment { Point1 = c1, Point2 = c2, Point3 = end });
                }
                fillFigure.Segments.Add(new LineSegment { Point = new Point(pts[count - 1].X, baseline) });

                var fillGeometry = new PathGeometry { Figures = new PathFigureCollection() };
                fillGeometry.Figures.Add(fillFigure);
                _plotLayer.Children.Add(new ShapePath
                {
                    Data = fillGeometry,
                    Fill = new SolidColorBrush(accentColor) { Opacity = 0.12 },
                    IsHitTestVisible = false,
                });

                // Courbe turquoise lissée (spline Catmull-Rom convertie en cubiques de Bézier).
                var curveFigure = new PathFigure
                {
                    StartPoint = start,
                    IsClosed = false,
                    IsFilled = false,
                    Segments = new PathSegmentCollection(),
                };
                foreach ((Point c1, Point c2, Point end) in segs)
                {
                    curveFigure.Segments.Add(new BezierSegment { Point1 = c1, Point2 = c2, Point3 = end });
                }

                var curveGeometry = new PathGeometry { Figures = new PathFigureCollection() };
                curveGeometry.Figures.Add(curveFigure);
                _plotLayer.Children.Add(new ShapePath
                {
                    Data = curveGeometry,
                    Stroke = accentBrush,
                    StrokeThickness = 2,
                    StrokeLineJoin = PenLineJoin.Round,
                    StrokeStartLineCap = PenLineCap.Round,
                    StrokeEndLineCap = PenLineCap.Round,
                    IsHitTestVisible = false,
                });
            }

            // Petits nœuds turquoise à chaque point (cerclés de la couleur du fond).
            foreach (Point p in pts)
            {
                var dot = new Ellipse
                {
                    Width = 7,
                    Height = 7,
                    Fill = accentBrush,
                    Stroke = insetBrush,
                    StrokeThickness = 1,
                    IsHitTestVisible = false,
                };
                Canvas.SetLeft(dot, p.X - 3.5);
                Canvas.SetTop(dot, p.Y - 3.5);
                _plotLayer.Children.Add(dot);
            }
        }

        _canvas.Children.Add(_plotLayer);
    }

    // ----------------------------- Aides au tracé -----------------------------------

    /// <summary>Construit une spline Catmull-Rom uniforme sous forme de segments de Bézier.</summary>
    private static (Point start, List<(Point c1, Point c2, Point end)> segs) BuildSpline(Point[] p)
    {
        var segs = new List<(Point c1, Point c2, Point end)>();
        int n = p.Length;
        for (int i = 0; i < n - 1; i++)
        {
            Point p0 = p[i == 0 ? 0 : i - 1];
            Point p1 = p[i];
            Point p2 = p[i + 1];
            Point p3 = p[(i + 2) >= n ? n - 1 : i + 2];

            var c1 = new Point(p1.X + ((p2.X - p0.X) / 6.0), p1.Y + ((p2.Y - p0.Y) / 6.0));
            var c2 = new Point(p2.X - ((p3.X - p1.X) / 6.0), p2.Y - ((p3.Y - p1.Y) / 6.0));
            segs.Add((c1, c2, p2));
        }

        return (p[0], segs);
    }

    /// <summary>Choisit un pas « rond » en dB visant ~5 intervalles (6 dB pour une plage de 24).</summary>
    private static double ChooseStep(double range)
    {
        double[] candidates = { 1, 2, 3, 6, 12, 24, 48 };
        double target = range / 5.0;
        foreach (double c in candidates)
        {
            if (c >= target)
            {
                return c;
            }
        }

        return candidates[candidates.Length - 1];
    }

    /// <summary>Met en forme une valeur dB : « +6 », « 0 », « -12 » (point décimal invariant).</summary>
    private static string FormatDb(double v)
    {
        double r = Math.Round(v, 1);
        string num = r == Math.Round(r)
            ? ((long)Math.Round(r)).ToString(CultureInfo.InvariantCulture)
            : r.ToString("0.#", CultureInfo.InvariantCulture);
        return r > 0 ? "+" + num : num;
    }

    private static Line MakeLine(double x1, double y1, double x2, double y2, Brush brush, double thickness) =>
        new()
        {
            X1 = x1,
            Y1 = y1,
            X2 = x2,
            Y2 = y2,
            Stroke = brush,
            StrokeThickness = thickness,
            IsHitTestVisible = false,
        };

    private static TextBlock MakeLabel(string text, Brush brush)
    {
        var label = new TextBlock
        {
            Text = text,
            FontSize = AxisFontSize,
            Foreground = brush,
            IsHitTestVisible = false,
        };

        // Mesure hors arbre visuel pour pouvoir centrer/aligner via DesiredSize.
        label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return label;
    }

    /// <summary>
    /// Résout un pinceau du thème via <c>Application.Current.Resources</c> (l'indexeur
    /// traverse les dictionnaires fusionnés), avec repli codé en dur si la clé est absente.
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
            }
        }
        catch
        {
            // Clé absente ou ressources indisponibles : on retombe sur le repli.
        }

        return new SolidColorBrush(fallback);
    }
}
