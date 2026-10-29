// SPDX-License-Identifier: Apache-2.0
// Contrôle « VolumeKnob » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Molette de volume circulaire, réutilisable et autonome, dessinée entièrement en code
// (aucun XAML, aucun x:Class) :
//   - corps : disque sombre « cockpit » ;
//   - anneau de fond sombre encastré (arc d'environ 270°, ouverture centrée en bas) ;
//   - arc de progression turquoise (dégradé du thème) proportionnel à la valeur ;
//   - pastille (thumb) claire à l'extrémité de l'arc ;
//   - pourcentage affiché en grand au centre, avec un libellé discret dessous.
// Interaction : glisser vertical à la souris (vers le haut = augmente) ; une course
// d'environ 200 px couvre toute la plage. Licence Apache-2.0, comme le dépôt.

using System;
using System.Globalization;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Windows.Foundation;
using Windows.UI;

// « Path » existe aussi sous System.IO (tiré par ImplicitUsings) : on lève l'ambiguïté
// avec un alias vers la forme vectorielle WinUI (même convention que CurveChart).
using ShapePath = Microsoft.UI.Xaml.Shapes.Path;

namespace Xonar.ControlCenter.Controls;

/// <summary>
/// Molette de volume circulaire « cockpit », construite en code dans le constructeur.
/// La valeur courante est exposée par <see cref="Value"/> (bornée par
/// <see cref="Minimum"/>/<see cref="Maximum"/>). L'arc et la pastille sont redessinés
/// lorsque la valeur change et à chaque redimensionnement, à partir de
/// ActualWidth/ActualHeight. <see cref="ValueChanged"/> est levé uniquement lorsque
/// l'utilisateur agit sur la molette (glisser souris).
/// </summary>
public sealed class VolumeKnob : UserControl
{
    // ------------------------- Géométrie de l'arc -------------------------
    // Angles mesurés en degrés depuis le haut (12 h = 0°), sens horaire.
    private const double ArcSweepDegrees = 270.0;   // amplitude totale de l'anneau
    private const double StartAngleDegrees = 225.0; // départ bas-gauche (ouverture centrée en bas)
    private const double DefaultDimension = 200.0;  // taille carrée par défaut si non contrainte
    private const double DragRangePixels = 200.0;   // course verticale couvrant toute la plage

    // ------------------------- Replis de couleurs (thème Cockpit) -------------------------
    private static readonly Color BodyFallback = Color.FromArgb(0xFF, 0x11, 0x18, 0x24);        // #111824 (surface)
    private static readonly Color InsetFallback = Color.FromArgb(0xFF, 0x0B, 0x0F, 0x17);       // #0B0F17 (anneau)
    private static readonly Color AccentDeepFallback = Color.FromArgb(0xFF, 0x17, 0x80, 0x7B);  // #17807B
    private static readonly Color AccentBrightFallback = Color.FromArgb(0xFF, 0x5B, 0xEE, 0xE6);// #5BEEE6 (thumb)
    private static readonly Color TextPrimaryFallback = Color.FromArgb(0xFF, 0xE9, 0xF0, 0xF6); // #E9F0F6
    private static readonly Color TextSecondaryFallback = Color.FromArgb(0xFF, 0x93, 0xA2, 0xB2);// #93A2B2

    // ------------------------- Éléments visuels (persistants, bâtis au constructeur) -------------------------
    private readonly Ellipse _body;
    private readonly ShapePath _track;
    private readonly ShapePath _progress;
    private readonly Ellipse _thumb;
    private readonly TextBlock _valueText;
    private readonly TextBlock _labelText;

    // ------------------------- État du glisser -------------------------
    private bool _dragging;
    private double _dragStartY;
    private double _dragStartValue;
    private bool _updatingValue; // garde-fou contre la réentrance lors du bornage

    public VolumeKnob()
    {
        // --- Pinceaux du thème (repli codé en dur si le thème n'est pas chargé) ---
        Brush bodyBrush = Res("CockpitSurfaceBrush", BodyFallback);
        Brush trackBrush = Res("CockpitSurfaceInsetBrush", InsetFallback);
        Brush arcBrush = AccentArcBrush();
        Brush thumbBrush = Res("CockpitAccentBrightBrush", AccentBrightFallback);
        Brush thumbStroke = Res("CockpitSurfaceInsetBrush", InsetFallback);
        Brush textBrush = Res("CockpitTextPrimaryBrush", TextPrimaryFallback);
        Brush labelBrush = Res("CockpitTextSecondaryBrush", TextSecondaryFallback);

        // --- Corps : disque sombre ---
        _body = new Ellipse { Fill = bodyBrush, IsHitTestVisible = false };

        // --- Anneau de fond : arc complet (~270°) ---
        _track = new ShapePath
        {
            Stroke = trackBrush,
            StrokeThickness = 14.0,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            IsHitTestVisible = false,
        };

        // --- Arc de progression turquoise ---
        _progress = new ShapePath
        {
            Stroke = arcBrush,
            StrokeThickness = 14.0,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            IsHitTestVisible = false,
        };

        // --- Pastille (thumb) claire ---
        _thumb = new Ellipse
        {
            Fill = thumbBrush,
            Stroke = thumbStroke,
            StrokeThickness = 2.0,
            IsHitTestVisible = false,
        };

        // --- Toile qui porte corps, arcs et pastille (ordre = profondeur) ---
        var canvas = new Canvas
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
            IsHitTestVisible = false,
        };
        canvas.Children.Add(_body);
        canvas.Children.Add(_track);
        canvas.Children.Add(_progress);
        canvas.Children.Add(_thumb);

        // --- Texte central : pourcentage (grand) + libellé discret ---
        _valueText = new TextBlock
        {
            Foreground = textBrush,
            FontSize = 44.0,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            HorizontalAlignment = HorizontalAlignment.Center,
            TextAlignment = TextAlignment.Center,
            IsHitTestVisible = false,
        };
        _labelText = new TextBlock
        {
            Text = "VOLUME",
            Foreground = labelBrush,
            FontSize = 12.0,
            HorizontalAlignment = HorizontalAlignment.Center,
            TextAlignment = TextAlignment.Center,
            Margin = new Thickness(0, 2, 0, 0),
            IsHitTestVisible = false,
        };

        var textPanel = new StackPanel
        {
            Orientation = Orientation.Vertical,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            IsHitTestVisible = false,
        };
        textPanel.Children.Add(_valueText);
        textPanel.Children.Add(_labelText);

        // --- Racine : la toile puis le texte par-dessus ; fond transparent (A=0)
        //     pour rester cliquable/glissable sur toute la surface du contrôle. ---
        var root = new Grid
        {
            Background = new SolidColorBrush(Color.FromArgb(0x00, 0x00, 0x00, 0x00)),
        };
        root.Children.Add(canvas);
        root.Children.Add(textPanel);

        Content = root;

        // Floor modeste : évite une molette dégénérée dans un conteneur « auto ».
        MinWidth = 48;
        MinHeight = 48;

        // --- Abonnements : rendu initial + redimensionnement, et entrée pointeur. ---
        Loaded += OnLoaded;
        SizeChanged += OnSizeChanged;
        PointerPressed += OnPointerPressed;
        PointerMoved += OnPointerMoved;
        PointerReleased += OnPointerReleased;
        PointerCanceled += OnPointerReleased;
        PointerCaptureLost += OnPointerCaptureLost;

        UpdateText();
    }

    // ============================ Propriétés de dépendance ============================

    /// <summary>Valeur courante, bornée à [<see cref="Minimum"/>, <see cref="Maximum"/>] (défaut 62).</summary>
    public static readonly DependencyProperty ValueProperty = DependencyProperty.Register(
        nameof(Value), typeof(double), typeof(VolumeKnob), new PropertyMetadata(62.0, OnValuePropertyChanged));

    public double Value
    {
        get => (double)GetValue(ValueProperty);
        set => SetValue(ValueProperty, value);
    }

    /// <summary>Valeur minimale (défaut 0).</summary>
    public static readonly DependencyProperty MinimumProperty = DependencyProperty.Register(
        nameof(Minimum), typeof(double), typeof(VolumeKnob), new PropertyMetadata(0.0, OnRangePropertyChanged));

    public double Minimum
    {
        get => (double)GetValue(MinimumProperty);
        set => SetValue(MinimumProperty, value);
    }

    /// <summary>Valeur maximale (défaut 100).</summary>
    public static readonly DependencyProperty MaximumProperty = DependencyProperty.Register(
        nameof(Maximum), typeof(double), typeof(VolumeKnob), new PropertyMetadata(100.0, OnRangePropertyChanged));

    public double Maximum
    {
        get => (double)GetValue(MaximumProperty);
        set => SetValue(MaximumProperty, value);
    }

    /// <summary>Levé lorsque l'utilisateur modifie la valeur via la molette (glisser souris).</summary>
    public event System.EventHandler<double>? ValueChanged;

    // ============================ Réactions aux changements ============================

    private static void OnValuePropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is VolumeKnob knob)
        {
            knob.HandleValueChanged((double)e.NewValue);
        }
    }

    private static void OnRangePropertyChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is VolumeKnob knob)
        {
            // La plage a changé : reborne la valeur et redessine.
            knob.HandleValueChanged(knob.Value);
        }
    }

    private void HandleValueChanged(double newValue)
    {
        if (_updatingValue)
        {
            return;
        }

        double clamped = Clamp(newValue, Minimum, Maximum);
        if (clamped != newValue)
        {
            // Réécrit la propriété avec la valeur bornée (sans relancer le traitement).
            _updatingValue = true;
            try
            {
                Value = clamped;
            }
            finally
            {
                _updatingValue = false;
            }
        }

        UpdateText();
        Redraw();
    }

    // ============================ Mise en page / rendu ============================

    protected override Size MeasureOverride(Size availableSize)
    {
        // Mesure le contenu (texte) via l'implémentation UserControl, puis impose une taille :
        //  - si le parent contraint (dimension finie), on remplit l'espace imposé ;
        //  - sinon (dimension infinie), on retombe sur une taille carrée par défaut (~200).
        base.MeasureOverride(availableSize);

        double w = availableSize.Width;
        double h = availableSize.Height;
        if (double.IsInfinity(w) || double.IsNaN(w) || w <= 0.0)
        {
            w = DefaultDimension;
        }
        if (double.IsInfinity(h) || double.IsNaN(h) || h <= 0.0)
        {
            h = DefaultDimension;
        }

        return new Size(w, h);
    }

    private void OnLoaded(object sender, RoutedEventArgs e) => Redraw();

    private void OnSizeChanged(object sender, SizeChangedEventArgs e) => Redraw();

    private void Redraw()
    {
        double w = ActualWidth;
        double h = ActualHeight;
        if (w <= 0.0 || h <= 0.0)
        {
            return; // pas encore mesuré : un futur Loaded/SizeChanged relancera le rendu.
        }

        double size = Math.Min(w, h);
        double cx = w / 2.0;
        double cy = h / 2.0;

        // Épaisseurs proportionnelles à la taille.
        double stroke = Math.Max(6.0, size * 0.075);
        double thumbRadius = Math.Max(5.0, stroke * 0.75);
        double pad = thumbRadius + 3.0;
        double radius = Math.Max(1.0, size / 2.0 - pad);

        _track.StrokeThickness = stroke;
        _progress.StrokeThickness = stroke;

        // Corps : disque sombre presque plein.
        double bodyDiameter = Math.Max(0.0, size - 2.0);
        _body.Width = bodyDiameter;
        _body.Height = bodyDiameter;
        Canvas.SetLeft(_body, cx - bodyDiameter / 2.0);
        Canvas.SetTop(_body, cy - bodyDiameter / 2.0);

        double fraction = FractionFromValue();

        // Anneau de fond : arc complet.
        _track.Data = BuildArcGeometry(cx, cy, radius, StartAngleDegrees, ArcSweepDegrees);

        // Arc de progression : de 0 à la fraction courante.
        double progressSweep = ArcSweepDegrees * fraction;
        if (progressSweep <= 0.5)
        {
            _progress.Visibility = Visibility.Collapsed;
        }
        else
        {
            _progress.Visibility = Visibility.Visible;
            _progress.Data = BuildArcGeometry(cx, cy, radius, StartAngleDegrees, progressSweep);
        }

        // Pastille à l'extrémité de l'arc de progression.
        double thumbAngle = StartAngleDegrees + progressSweep;
        Point thumbCenter = PointOnCircle(cx, cy, radius, thumbAngle);
        double thumbDiameter = thumbRadius * 2.0;
        _thumb.Width = thumbDiameter;
        _thumb.Height = thumbDiameter;
        _thumb.StrokeThickness = Math.Max(1.0, stroke * 0.14);
        Canvas.SetLeft(_thumb, thumbCenter.X - thumbRadius);
        Canvas.SetTop(_thumb, thumbCenter.Y - thumbRadius);

        // Taille de police adaptée à la molette (base ~44 pour une molette de 200).
        _valueText.FontSize = Clamp(size * 0.22, 14.0, 64.0);
        _labelText.FontSize = Clamp(size * 0.07, 8.0, 18.0);
    }

    private double FractionFromValue()
    {
        double range = Maximum - Minimum;
        if (range <= 0.0)
        {
            return 0.0;
        }

        return Clamp((Value - Minimum) / range, 0.0, 1.0);
    }

    private void UpdateText()
    {
        double pct = FractionFromValue() * 100.0;
        int rounded = (int)Math.Round(pct, MidpointRounding.AwayFromZero);
        _valueText.Text = rounded.ToString(CultureInfo.InvariantCulture) + " %";
    }

    // ============================ Interaction pointeur ============================

    private void OnPointerPressed(object sender, PointerRoutedEventArgs e)
    {
        _dragStartY = e.GetCurrentPoint(this).Position.Y;
        _dragStartValue = Value;
        _dragging = CapturePointer(e.Pointer);
        e.Handled = true;
    }

    private void OnPointerMoved(object sender, PointerRoutedEventArgs e)
    {
        if (!_dragging)
        {
            return;
        }

        double currentY = e.GetCurrentPoint(this).Position.Y;
        double deltaY = _dragStartY - currentY; // vers le haut => positif => augmente
        double range = Maximum - Minimum;
        double deltaValue = (deltaY / DragRangePixels) * range;
        double newValue = Clamp(_dragStartValue + deltaValue, Minimum, Maximum);

        if (newValue != Value)
        {
            Value = newValue;                     // déclenche le rebornage, le texte et le rendu
            ValueChanged?.Invoke(this, newValue); // notifie : changement initié par l'utilisateur
        }

        e.Handled = true;
    }

    private void OnPointerReleased(object sender, PointerRoutedEventArgs e)
    {
        if (_dragging)
        {
            _dragging = false;
            ReleasePointerCapture(e.Pointer);
        }

        e.Handled = true;
    }

    private void OnPointerCaptureLost(object sender, PointerRoutedEventArgs e)
    {
        _dragging = false;
    }

    // ============================ Utilitaires de dessin et de thème ============================

    /// <summary>Construit un arc (figure non fermée, non remplie) de <paramref name="sweepDeg"/>° sens horaire.</summary>
    private static PathGeometry BuildArcGeometry(double cx, double cy, double radius, double startAngleDeg, double sweepDeg)
    {
        Point start = PointOnCircle(cx, cy, radius, startAngleDeg);
        Point end = PointOnCircle(cx, cy, radius, startAngleDeg + sweepDeg);

        var arc = new ArcSegment
        {
            Point = end,
            Size = new Size(radius, radius),
            RotationAngle = 0.0,
            SweepDirection = SweepDirection.Clockwise,
            IsLargeArc = sweepDeg > 180.0,
        };

        var figure = new PathFigure
        {
            StartPoint = start,
            IsClosed = false,
            IsFilled = false,
            Segments = new PathSegmentCollection(),
        };
        figure.Segments.Add(arc);

        var geometry = new PathGeometry { Figures = new PathFigureCollection() };
        geometry.Figures.Add(figure);
        return geometry;
    }

    /// <summary>Point du cercle à l'angle donné (degrés depuis le haut, sens horaire ; Y vers le bas).</summary>
    private static Point PointOnCircle(double cx, double cy, double radius, double angleDeg)
    {
        double rad = angleDeg * Math.PI / 180.0;
        double x = cx + radius * Math.Sin(rad);
        double y = cy - radius * Math.Cos(rad);
        return new Point(x, y);
    }

    private static double Clamp(double value, double min, double max)
    {
        if (max < min)
        {
            (min, max) = (max, min);
        }
        if (value < min)
        {
            return min;
        }
        if (value > max)
        {
            return max;
        }
        return value;
    }

    /// <summary>
    /// Résout un pinceau du thème « cockpit ». L'indexeur de ressources traverse les
    /// dictionnaires fusionnés (Themes/Cockpit.xaml). Repli sur <paramref name="fallback"/>
    /// si le thème n'est pas chargé ou si la clé est absente.
    /// </summary>
    private static Brush Res(string key, Color fallback)
    {
        try
        {
            if (Application.Current?.Resources is { } resources)
            {
                object? value = resources[key];
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
            // Clé absente ou ressources indisponibles : repli codé en dur ci-dessous.
        }

        return new SolidColorBrush(fallback);
    }

    /// <summary>
    /// Pinceau de l'arc : dégradé turquoise du thème (CockpitAccentGradient) ; repli sur
    /// un dégradé équivalent (sombre -> clair) construit en code si la ressource est absente.
    /// </summary>
    private static Brush AccentArcBrush()
    {
        try
        {
            if (Application.Current?.Resources is { } resources &&
                resources["CockpitAccentGradient"] is Brush themed)
            {
                return themed;
            }
        }
        catch
        {
            // Ressource absente : dégradé de repli ci-dessous.
        }

        var gradient = new LinearGradientBrush
        {
            StartPoint = new Point(0.0, 1.0), // bas
            EndPoint = new Point(0.0, 0.0),   // haut
        };
        gradient.GradientStops.Add(new GradientStop { Offset = 0.0, Color = AccentDeepFallback });
        gradient.GradientStops.Add(new GradientStop { Offset = 1.0, Color = AccentBrightFallback });
        return gradient;
    }
}
