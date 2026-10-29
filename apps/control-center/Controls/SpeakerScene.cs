// SPDX-License-Identifier: Apache-2.0
// Contrôle « SpeakerScene » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Scène sonore 5.1 vue de dessus, entièrement vectorielle et dessinée en code (pas de XAML) :
//   - sol (tapis) arrondi sombre et champ sonore (cercles concentriques très discrets) ;
//   - au centre, une position d'écoute stylisée (fauteuil vu de dessus) tournée vers l'avant ;
//   - 6 enceintes positionnées (FL FC FR à l'avant, LFE à droite, SL SR à l'arrière), chacune
//     avec sa boîte, son driver, son libellé et 2-3 arcs d'ondes turquoise orientés vers l'auditeur ;
//   - des liaisons fines en pointillés relient l'auditeur à chaque enceinte.
// Les enceintes sont cliquables (survol = léger surlignage, clic = sélection + événement).
// Une enceinte sélectionnée reçoit un halo turquoise ; une enceinte coupée est atténuée et sans ondes.
// Les couleurs proviennent du thème Cockpit (ressources d'application) avec repli codé en dur.
// Licence Apache-2.0, comme le dépôt.

using System;
using System.Collections.Generic;
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
/// Scène sonore 5.1 interactive dessinée en code pour le centre de contrôle.
/// Vue de dessus d'une pièce d'écoute : auditeur au centre (légèrement vers le bas) et
/// 6 enceintes autour. Contrôle code-only (aucun XAML) : tout est repositionné depuis
/// ActualWidth / ActualHeight à chaque SizeChanged (positions proportionnelles), au
/// chargement, lorsque <see cref="SelectedChannel"/> change et à chaque <see cref="SetMuted"/>.
/// Cliquer une enceinte lève <see cref="SpeakerSelected"/> et la marque comme sélectionnée.
/// </summary>
public sealed class SpeakerScene : UserControl
{
    // ----------------------------- Disposition (fractions de la taille du contrôle) -----------------------------
    // Vue de dessus : X de 0 (gauche) à 1 (droite), Y de 0 (haut) à 1 (bas).
    private const double ListenerFx = 0.500;
    private const double ListenerFy = 0.595; // auditeur au centre, légèrement vers le bas

    // Un libellé par code de canal attendu par le contrat (FL FC FR LFE SL SR).
    private static readonly SpeakerSpec[] Speakers =
    {
        new("FL", 0.205, 0.195),  // avant gauche
        new("FC", 0.500, 0.150),  // avant centre
        new("FR", 0.795, 0.195),  // avant droit
        new("LFE", 0.860, 0.500), // caisson : à droite, entre FR et SR
        new("SL", 0.175, 0.830),  // surround gauche (arrière)
        new("SR", 0.825, 0.830),  // surround droit (arrière)
    };

    // ----------------------------- Repli couleurs (#RRGGBB du thème « cockpit ») -----------------------------
    // Utilisés si la ressource du thème est introuvable (ordre = clés du brief).
    private static readonly Color AccentFallback = Color.FromArgb(0xFF, 0x35, 0xD6, 0xCE);     // #35D6CE (ondes/halo)
    private static readonly Color AccentGlowFallback = Color.FromArgb(0xFF, 0x2A, 0x9D, 0x98); // #2A9D98
    private static readonly Color FloorFallback = Color.FromArgb(0xFF, 0x0B, 0x0F, 0x17);      // #0B0F17 (sol)
    private static readonly Color BoxFallback = Color.FromArgb(0xFF, 0x16, 0x1F, 0x2D);        // #161F2D (boîtes)
    private static readonly Color BorderFallback = Color.FromArgb(0xFF, 0x1F, 0x2A, 0x3A);     // #1F2A3A
    private static readonly Color LabelFallback = Color.FromArgb(0xFF, 0x93, 0xA2, 0xB2);      // #93A2B2 (libellés)
    private static readonly Color TertiaryFallback = Color.FromArgb(0xFF, 0x5D, 0x6B, 0x7B);   // #5D6B7B

    // ----------------------------- État interne -----------------------------
    private readonly Canvas _canvas;                 // surface racine (tout est positionné en absolu)
    private readonly HashSet<string> _muted = new(StringComparer.Ordinal); // canaux coupés

    public SpeakerScene()
    {
        _canvas = new Canvas
        {
            HorizontalAlignment = HorizontalAlignment.Stretch,
            VerticalAlignment = VerticalAlignment.Stretch,
        };
        Content = _canvas;

        // Un Canvas seul a une taille désirée nulle : on fixe des minimales pour rester
        // visible dans un conteneur qui s'ajuste au contenu.
        MinWidth = 280;
        MinHeight = 240;

        Loaded += (_, _) => Redraw();
        SizeChanged += (_, _) => Redraw();
    }

    // ----------------------------- Propriété de dépendance : sélection -----------------------------

    /// <summary>
    /// Code du canal sélectionné : « FL », « FC », « FR », « LFE », « SL », « SR » (ou null).
    /// L'enceinte correspondante est mise en valeur (halo/contour turquoise).
    /// </summary>
    public static readonly DependencyProperty SelectedChannelProperty = DependencyProperty.Register(
        nameof(SelectedChannel), typeof(string), typeof(SpeakerScene),
        new PropertyMetadata(null, OnSelectedChannelChanged));

    public string? SelectedChannel
    {
        get => (string?)GetValue(SelectedChannelProperty);
        set => SetValue(SelectedChannelProperty, value);
    }

    private static void OnSelectedChannelChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
    {
        if (d is SpeakerScene scene)
        {
            scene.Redraw();
        }
    }

    // ----------------------------- Événement de sélection -----------------------------

    /// <summary>Levé au clic sur une enceinte ; l'argument est le code du canal.</summary>
    public event System.EventHandler<string>? SpeakerSelected;

    // ----------------------------- API publique -----------------------------

    /// <summary>
    /// Marque une enceinte comme coupée (rendu atténué, ondes masquées) ou non.
    /// <paramref name="channelCode"/> hors de la liste 5.1 est ignoré silencieusement.
    /// </summary>
    public void SetMuted(string channelCode, bool muted)
    {
        if (string.IsNullOrEmpty(channelCode) || !IsKnownChannel(channelCode))
        {
            return;
        }

        bool changed = muted ? _muted.Add(channelCode) : _muted.Remove(channelCode);
        if (!changed)
        {
            return;
        }

        // Les contrôles WinUI ne se touchent que sur le thread UI. Si l'appel provient
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

    private static bool IsKnownChannel(string code)
    {
        foreach (SpeakerSpec s in Speakers)
        {
            if (string.Equals(s.Code, code, StringComparison.Ordinal))
            {
                return true;
            }
        }

        return false;
    }

    // ----------------------------- Rendu -----------------------------

    /// <summary>Reconstruit intégralement la scène à partir de la taille actuelle.</summary>
    private void Redraw()
    {
        _canvas.Children.Clear();

        double w = ActualWidth;
        double h = ActualHeight;
        if (w <= 1.0 || h <= 1.0)
        {
            return; // pas encore mesuré : un futur SizeChanged/Loaded relancera le rendu.
        }

        double s = Math.Min(w, h); // échelle de référence (les tailles suivent le plus petit côté)
        double margin = s * 0.03;

        // Pinceaux du thème (résolus une fois), avec repli codé en dur.
        Brush floorBrush = Res("CockpitSurfaceInsetBrush", FloorFallback);
        Brush boxBrush = Res("CockpitSurface2Brush", BoxFallback);
        Brush borderBrush = Res("CockpitBorderBrush", BorderFallback);
        Brush labelBrush = Res("CockpitTextSecondaryBrush", LabelFallback);
        Brush tertiaryBrush = Res("CockpitTextTertiaryBrush", TertiaryFallback);
        Brush accentBrush = Res("CockpitAccentBrush", AccentFallback);
        Color accentColor = (accentBrush as SolidColorBrush)?.Color ?? AccentFallback;
        Color glowColor = (Res("CockpitAccentGlow", AccentGlowFallback) as SolidColorBrush)?.Color ?? AccentGlowFallback;

        // --- Sol (tapis) : rectangle arrondi sombre couvrant la scène ---
        var floor = new Rectangle
        {
            Width = Math.Max(0, w - 2 * margin),
            Height = Math.Max(0, h - 2 * margin),
            RadiusX = s * 0.045,
            RadiusY = s * 0.045,
            Fill = floorBrush,
            Stroke = borderBrush,
            StrokeThickness = 1,
            IsHitTestVisible = false,
        };
        Canvas.SetLeft(floor, margin);
        Canvas.SetTop(floor, margin);
        _canvas.Children.Add(floor);

        var listener = new Point(ListenerFx * w, ListenerFy * h);

        // --- Champ sonore : cercles concentriques très discrets autour de l'auditeur ---
        double[] fieldRadii = { s * 0.12, s * 0.175, s * 0.23 };
        double[] fieldOpacity = { 0.08, 0.055, 0.035 };
        for (int i = 0; i < fieldRadii.Length; i++)
        {
            AddEllipse(listener, fieldRadii[i], null,
                new SolidColorBrush(accentColor) { Opacity = fieldOpacity[i] }, 1.0);
        }

        // --- Liaisons auditeur -> enceintes (pointillés), sous le fauteuil et les enceintes ---
        foreach (SpeakerSpec spec in Speakers)
        {
            var c = new Point(spec.Fx * w, spec.Fy * h);
            bool muted = _muted.Contains(spec.Code);
            var link = new Line
            {
                X1 = listener.X,
                Y1 = listener.Y,
                X2 = c.X,
                Y2 = c.Y,
                Stroke = accentBrush,
                StrokeThickness = Math.Max(1.0, s * 0.0035),
                Opacity = muted ? 0.10 : 0.28,
                StrokeDashArray = new DoubleCollection { 2.5, 3.5 },
                StrokeDashCap = PenLineCap.Round,
                IsHitTestVisible = false,
            };
            _canvas.Children.Add(link);
        }

        // --- Fauteuil / position d'écoute (tourné vers l'avant, c.-à-d. vers le haut) ---
        DrawChair(listener, s, boxBrush, floorBrush, borderBrush, accentColor);

        // --- Enceintes ---
        foreach (SpeakerSpec spec in Speakers)
        {
            DrawSpeaker(spec, listener, w, h, s, margin,
                boxBrush, borderBrush, labelBrush, tertiaryBrush, accentBrush, accentColor, glowColor);
        }
    }

    /// <summary>Fauteuil vu de dessus : corps arrondi + assise ouverte vers l'avant (haut).</summary>
    private void DrawChair(Point listener, double s, Brush body, Brush seatFill, Brush border, Color accentColor)
    {
        double cw = s * 0.17;  // largeur
        double cd = s * 0.15;  // profondeur
        double x0 = listener.X - cw / 2;
        double y0 = listener.Y - cd / 2;

        // Corps (accoudoirs + dossier) : rectangle arrondi plein.
        _canvas.Children.Add(new ShapePath
        {
            Data = RoundedRectGeometry(x0, y0, cw, cd, cd * 0.30),
            Fill = body,
            Stroke = border,
            StrokeThickness = 1,
            IsHitTestVisible = false,
        });

        // Assise : encoche inset, mince à l'avant (haut), épaisse à l'arrière (dossier) et sur les côtés.
        double arm = cw * 0.16;
        double back = cd * 0.30;
        double front = cd * 0.12;
        double seatW = Math.Max(0, cw - 2 * arm);
        double seatH = Math.Max(0, cd - front - back);
        _canvas.Children.Add(new ShapePath
        {
            Data = RoundedRectGeometry(x0 + arm, y0 + front, seatW, seatH, cd * 0.14),
            Fill = seatFill,
            Stroke = border,
            StrokeThickness = 1,
            IsHitTestVisible = false,
        });

        // Liseré turquoise discret sur le bord avant de l'assise : indique l'orientation d'écoute.
        _canvas.Children.Add(new Line
        {
            X1 = x0 + arm + 2,
            Y1 = y0 + front + 1,
            X2 = x0 + cw - arm - 2,
            Y2 = y0 + front + 1,
            Stroke = new SolidColorBrush(accentColor) { Opacity = 0.45 },
            StrokeThickness = Math.Max(1.0, s * 0.004),
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            IsHitTestVisible = false,
        });
    }

    /// <summary>Dessine une enceinte : halo (si sélectionnée), ondes (si non coupée), boîte, driver, libellé, zone cliquable.</summary>
    private void DrawSpeaker(
        SpeakerSpec spec, Point listener, double w, double h, double s, double margin,
        Brush boxBrush, Brush borderBrush, Brush labelBrush, Brush tertiaryBrush,
        Brush accentBrush, Color accentColor, Color glowColor)
    {
        var c = new Point(spec.Fx * w, spec.Fy * h);
        double ang = Math.Atan2(listener.Y - c.Y, listener.X - c.X); // radians, vers l'auditeur (Y vers le bas)
        double deg = ang * 180.0 / Math.PI;
        double cos = Math.Cos(ang);
        double sin = Math.Sin(ang);

        bool muted = _muted.Contains(spec.Code);
        bool selected = string.Equals(spec.Code, SelectedChannel, StringComparison.Ordinal);

        // Dimensions proportionnelles.
        double depth = s * 0.060;  // profondeur (axe orienté vers l'auditeur)
        double face = s * 0.088;   // largeur de façade (perpendiculaire)
        double driverR = Math.Min(depth, face) * 0.30;

        // --- Halo de sélection (sous la boîte) ---
        if (selected)
        {
            double hr = Math.Max(depth, face) * 0.98;
            AddEllipse(c, hr * 1.30, null, new SolidColorBrush(accentColor) { Opacity = 0.30 }, Math.Max(1.0, s * 0.003));
            AddEllipse(c, hr, new SolidColorBrush(glowColor) { Opacity = 0.16 }, accentBrush, Math.Max(1.6, s * 0.0055));
        }

        // --- Surlignage de survol (sous la boîte, invisible au repos) ---
        Ellipse hover = AddEllipse(c, Math.Max(depth, face) * 0.82,
            new SolidColorBrush(accentColor) { Opacity = 0.18 }, null, 0);
        hover.Opacity = 0;

        // --- Ondes concentriques turquoise orientées vers l'auditeur (masquées si coupée) ---
        if (!muted)
        {
            double thick = Math.Max(1.4, s * 0.006);
            double half = 32.0 * Math.PI / 180.0; // demi-ouverture angulaire des arcs
            AddArc(c, s * 0.055, ang, half, accentBrush, thick, 0.90);
            AddArc(c, s * 0.078, ang, half, accentBrush, thick * 0.9, 0.55);
            AddArc(c, s * 0.101, ang, half, accentBrush, thick * 0.8, 0.32);
        }

        // --- Boîte d'enceinte : rectangle arrondi orienté vers l'auditeur ---
        Brush boxStroke = selected ? accentBrush : borderBrush;
        double boxStrokeThickness = selected ? Math.Max(1.6, s * 0.006) : 1.0;
        var box = new Rectangle
        {
            Width = depth,
            Height = face,
            RadiusX = s * 0.014,
            RadiusY = s * 0.014,
            Fill = boxBrush,
            Stroke = boxStroke,
            StrokeThickness = boxStrokeThickness,
            Opacity = muted ? 0.45 : 1.0,
            IsHitTestVisible = false,
            RenderTransform = new RotateTransform { Angle = deg, CenterX = depth / 2, CenterY = face / 2 },
        };
        Canvas.SetLeft(box, c.X - depth / 2);
        Canvas.SetTop(box, c.Y - face / 2);
        _canvas.Children.Add(box);

        // --- Driver (cône) près de la façade, côté auditeur ---
        var driverCenter = new Point(c.X + cos * depth * 0.17, c.Y + sin * depth * 0.17);
        Brush driverStroke = muted ? tertiaryBrush : accentBrush;
        AddEllipse(driverCenter, driverR, Res("CockpitSurfaceInsetBrush", FloorFallback), driverStroke, 1.0)
            .Opacity = muted ? 0.5 : 1.0;
        AddEllipse(driverCenter, driverR * 0.45,
            muted ? tertiaryBrush : new SolidColorBrush(accentColor) { Opacity = 0.55 }, null, 0)
            .Opacity = muted ? 0.5 : 1.0;

        // --- Libellé du canal, à l'extérieur (opposé à l'auditeur) ---
        double fs = Math.Clamp(s * 0.034, 10.5, 18.0);
        var label = new TextBlock
        {
            Text = spec.Code,
            FontSize = fs,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = muted ? tertiaryBrush : labelBrush,
            Opacity = muted ? 0.7 : 1.0,
            IsHitTestVisible = false,
        };
        label.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        double lw = label.DesiredSize.Width;
        double lh = label.DesiredSize.Height;
        double dist = depth * 0.6 + lh * 0.9;
        double lx = c.X - cos * dist - lw / 2;
        double ly = c.Y - sin * dist - lh / 2;
        lx = Math.Clamp(lx, margin, Math.Max(margin, w - lw - margin));
        ly = Math.Clamp(ly, margin, Math.Max(margin, h - lh - margin));
        Canvas.SetLeft(label, lx);
        Canvas.SetTop(label, ly);
        _canvas.Children.Add(label);

        // --- Zone cliquable (par-dessus) : capture survol et clic pour toute l'enceinte ---
        double hit = Math.Max(depth, face) * 1.5;
        var target = new Border
        {
            Width = hit,
            Height = hit,
            // Pinceau transparent (A=0) mais « hit-testable » : capture survol et clic.
            Background = new SolidColorBrush(Color.FromArgb(0x00, 0x00, 0x00, 0x00)),
            CornerRadius = new CornerRadius(hit / 2),
        };
        Canvas.SetLeft(target, c.X - hit / 2);
        Canvas.SetTop(target, c.Y - hit / 2);

        string state = (muted ? ", coupée" : string.Empty) + (selected ? ", sélectionnée" : string.Empty);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(target, $"Enceinte {spec.Code}{state}");

        string code = spec.Code;
        Brush restoreStroke = boxStroke;
        double restoreThickness = boxStrokeThickness;
        Rectangle boxRef = box;
        Ellipse hoverRef = hover;

        target.PointerEntered += (_, _) =>
        {
            hoverRef.Opacity = 1.0;
            boxRef.Stroke = accentBrush;
            boxRef.StrokeThickness = Math.Max(1.4, s * 0.005);
        };
        target.PointerExited += (_, _) =>
        {
            hoverRef.Opacity = 0.0;
            boxRef.Stroke = restoreStroke;
            boxRef.StrokeThickness = restoreThickness;
        };
        target.Tapped += (_, _) =>
        {
            SelectedChannel = code;              // met en valeur (déclenche un rendu via le callback)
            SpeakerSelected?.Invoke(this, code); // notifie l'hôte
        };

        _canvas.Children.Add(target);
    }

    // ----------------------------- Aides au tracé -----------------------------

    /// <summary>Ajoute une ellipse centrée sur <paramref name="center"/> de rayon <paramref name="radius"/>.</summary>
    private Ellipse AddEllipse(Point center, double radius, Brush? fill, Brush? stroke, double strokeThickness)
    {
        var e = new Ellipse
        {
            Width = radius * 2,
            Height = radius * 2,
            Fill = fill,
            Stroke = stroke,
            StrokeThickness = strokeThickness,
            IsHitTestVisible = false,
        };
        Canvas.SetLeft(e, center.X - radius);
        Canvas.SetTop(e, center.Y - radius);
        _canvas.Children.Add(e);
        return e;
    }

    /// <summary>
    /// Ajoute un arc d'onde : portion de cercle centrée sur <paramref name="center"/>, de rayon
    /// <paramref name="radius"/>, centrée sur l'angle <paramref name="angCenter"/> (radians) et
    /// couvrant +/- <paramref name="halfSpan"/>. L'arc s'ouvre donc vers l'auditeur.
    /// </summary>
    private void AddArc(Point center, double radius, double angCenter, double halfSpan,
        Brush stroke, double thickness, double opacity)
    {
        var start = new Point(
            center.X + radius * Math.Cos(angCenter - halfSpan),
            center.Y + radius * Math.Sin(angCenter - halfSpan));
        var end = new Point(
            center.X + radius * Math.Cos(angCenter + halfSpan),
            center.Y + radius * Math.Sin(angCenter + halfSpan));

        var figure = new PathFigure
        {
            StartPoint = start,
            IsClosed = false,
            IsFilled = false,
            Segments = new PathSegmentCollection(),
        };
        // Angle croissant en repère écran (Y vers le bas) => sens horaire ; arc mineur (ouverture < 180°).
        figure.Segments.Add(new ArcSegment
        {
            Point = end,
            Size = new Size(radius, radius),
            SweepDirection = SweepDirection.Clockwise,
            IsLargeArc = false,
            RotationAngle = 0,
        });

        var geometry = new PathGeometry { Figures = new PathFigureCollection() };
        geometry.Figures.Add(figure);

        _canvas.Children.Add(new ShapePath
        {
            Data = geometry,
            Stroke = stroke,
            StrokeThickness = thickness,
            Opacity = opacity,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            IsHitTestVisible = false,
        });
    }

    /// <summary>Construit la géométrie d'un rectangle arrondi (figure fermée) utilisable par un Path.</summary>
    private static PathGeometry RoundedRectGeometry(double x, double y, double width, double height, double radius)
    {
        double r = Math.Max(0.0, Math.Min(radius, Math.Min(width, height) / 2.0));

        var figure = new PathFigure
        {
            StartPoint = new Point(x + r, y),
            IsClosed = true,
            IsFilled = true,
            Segments = new PathSegmentCollection(),
        };

        void LineTo(double px, double py) =>
            figure.Segments.Add(new LineSegment { Point = new Point(px, py) });

        void ArcTo(double px, double py) =>
            figure.Segments.Add(new ArcSegment
            {
                Point = new Point(px, py),
                Size = new Size(r, r),
                SweepDirection = SweepDirection.Clockwise, // coins convexes (repère Y vers le bas)
                IsLargeArc = false,
                RotationAngle = 0,
            });

        LineTo(x + width - r, y);
        ArcTo(x + width, y + r);
        LineTo(x + width, y + height - r);
        ArcTo(x + width - r, y + height);
        LineTo(x + r, y + height);
        ArcTo(x, y + height - r);
        LineTo(x, y + r);
        ArcTo(x + r, y);

        var geometry = new PathGeometry { Figures = new PathFigureCollection() };
        geometry.Figures.Add(figure);
        return geometry;
    }

    /// <summary>
    /// Résout un pinceau du thème « cockpit ». L'indexeur de ressources traverse les
    /// dictionnaires fusionnés (Themes/Cockpit.xaml). Repli sur <paramref name="fallback"/>
    /// si le thème n'est pas chargé, si la clé est absente, ou si elle désigne une
    /// Color (ex. « CockpitAccent », « CockpitAccentGlow ») plutôt qu'un Brush.
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
            // Clé absente ou ressources indisponibles : on retombe sur le repli.
        }

        return new SolidColorBrush(fallback);
    }

    // ----------------------------- Spécification d'une enceinte -----------------------------
    private readonly struct SpeakerSpec
    {
        public SpeakerSpec(string code, double fx, double fy)
        {
            Code = code;
            Fx = fx;
            Fy = fy;
        }

        public string Code { get; }   // code du canal (FL, FC, ...)
        public double Fx { get; }      // position X proportionnelle (0..1)
        public double Fy { get; }      // position Y proportionnelle (0..1)
    }
}
