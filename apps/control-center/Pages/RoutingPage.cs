// SPDX-License-Identifier: Apache-2.0
// Page « Mixage & routage » du centre de contrôle Xonar SE (WinUI 3, thème « cockpit »).
// Trois cartes, entièrement construites en code (pas de XAML) :
//   - « Politique de source » : choix entre préservation, upmix selon le layout déclaré
//     et réduction à la paire frontale (chaque option décrite ; l'option active et le badge
//     « Actif » reflètent la politique du brouillon partagé via shell.Editor, et la
//     sélection pilote réellement cet éditeur) ;
//   - « Schéma de routage » : petit diagramme vectoriel ENTRÉE → DSP → SORTIE 5.1 illustrant
//     l'upmix stéréo (FL=L, FR=R, FC=(L+R)/2, LFE=graves (L+R), SL=L, SR=R) ;
//   - « Préservation du 5.1 natif » : un flux déclaré 5.1 passe en 6→6 sans upmix.
// Valeurs d'exemple (page vitrine). Styles et pinceaux proviennent du thème Cockpit via
// Application.Current.Resources. Textes et commentaires en français. Licence Apache-2.0.

using System.Collections.Generic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Shapes;
using Xonar.Contracts.V1;
using Xonar.ControlCenter.Shell;

namespace Xonar.ControlCenter.Pages;

public sealed class RoutingPage : Microsoft.UI.Xaml.Controls.UserControl
{
    // Ordre physique des canaux 5.1 (identique au contrat et à la page « Vue d'ensemble »).
    private static readonly string[] Channels = { "FL", "FR", "FC", "LFE", "SL", "SR" };

    // Formules d'affectation pour l'upmix stéréo (une par canal, dans l'ordre de Channels).
    private static readonly string[] UpmixFormulas = { "L", "R", "(L + R) / 2", "graves (L + R)", "L", "R" };

    private readonly AppShellViewModel _shell;

    // Boutons radio de politique, chacun associé à sa valeur de contrat et à son badge
    // « Actif » (présent mais masqué tant que l'option n'est pas la politique courante).
    private readonly List<(RadioButton Radio, SourcePolicy Policy, Border Badge)> _policyRows = new();

    // Garde anti-réentrance : vrai pendant les mises à jour programmatiques de l'UI
    // (init au Loaded, synchronisation sur Changed) pour ne pas réécrire l'éditeur.
    private bool _suspend;

    private Brush B(string key) => (Brush)Application.Current.Resources[key];

    private Style S(string key) => (Style)Application.Current.Resources[key];

    public RoutingPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;
        Content = Build();

        // Initialisation et abonnement différés au chargement : l'état initial des
        // boutons radio reflète le brouillon partagé, jamais une valeur codée en dur.
        Loaded += OnLoaded;
        Unloaded += OnUnloaded;
    }

    // ---------------------------------------------------------------------------
    // Cycle de vie : au chargement on reflète le brouillon et on s'abonne à Changed ;
    // au déchargement on se désabonne (l'éditeur du shell survit à la page).
    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        SyncFromDraft();
        _shell.CapabilitiesChanged -= OnEditorChanged;
        _shell.CapabilitiesChanged += OnEditorChanged;
        _shell.Editor.Changed -= OnEditorChanged; // idempotent si Loaded est rejoué
        _shell.Editor.Changed += OnEditorChanged;
    }

    private void OnUnloaded(object sender, RoutedEventArgs e)
    {
        _shell.CapabilitiesChanged -= OnEditorChanged;
        _shell.Editor.Changed -= OnEditorChanged;
    }

    // Les notifications Changed peuvent provenir d'un thread hors-UI (coalesceur
    // d'applications) ; on marshale toujours sur le thread UI avant de toucher aux
    // contrôles WinUI, comme ProfilesPage (sinon RPC_E_WRONG_THREAD).
    private void OnEditorChanged()
    {
        var q = DispatcherQueue;
        if (q is null || q.HasThreadAccess)
        {
            SyncFromDraft();
        }
        else
        {
            q.TryEnqueue(SyncFromDraft);
        }
    }

    // Reflète la politique du brouillon dans les boutons radio et le badge « Actif ».
    // Encadré par _suspend : les événements Checked déclenchés par ces écritures ne
    // doivent pas réécrire l'éditeur.
    private void SyncFromDraft()
    {
        _suspend = true;
        try
        {
            var policy = _shell.Editor.Draft.SourcePolicy;
            foreach (var row in _policyRows)
            {
                row.Radio.IsEnabled = _shell.CanSelectSourcePolicy(row.Policy);
                row.Radio.IsChecked = row.Policy == policy;
            }
            ShowActiveBadge(policy);
        }
        finally
        {
            _suspend = false;
        }
    }

    // Déplace le badge « Actif » sur l'option correspondant à la politique donnée
    // (un seul badge visible à la fois).
    private void ShowActiveBadge(SourcePolicy policy)
    {
        foreach (var row in _policyRows)
        {
            row.Badge.Visibility = row.Policy == policy ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    // Câblage d'un bouton radio : la sélection pilote l'éditeur partagé puis déplace
    // le badge. Ignoré pendant une synchronisation programmatique (_suspend).
    private void OnPolicyChecked(SourcePolicy policy)
    {
        if (_suspend)
        {
            return;
        }
        _shell.Editor.SetSourcePolicy(policy);
        if(policy == SourcePolicy.AutoDeclaredLayout) _shell.Editor.SetListeningMode(ListeningMode.Surround51);
        ShowActiveBadge(policy);
    }

    // ---------------------------------------------------------------------------
    private UIElement Build()
    {
        var container = new StackPanel
        {
            Spacing = 18,
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
            Margin = new Thickness(32, 26, 32, 28),
        };

        // Le modèle du shell est conservé sur l'élément racine pour un accès éventuel
        // (la page reste purement informative : aucune écriture système n'est tentée).
        container.Tag = _shell;

        // ---------- En-tête ----------
        var header = new StackPanel();
        header.Children.Add(new TextBlock { Text = "Mixage & routage", Style = S("CockpitH1") });
        header.Children.Add(new TextBlock
        {
            Text = "Affectation des canaux, upmix automatique selon le format déclaré, préservation du vrai 5.1.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 4, 0, 0),
        });
        container.Children.Add(header);

        // ---------- Carte « Politique de source » ----------
        container.Children.Add(Card(
            "Politique de source",
            BuildPolicy(),
            "Comment l'entrée est transformée avant le routage vers les six sorties."));

        // ---------- Carte « Schéma de routage » ----------
        var diagContent = new StackPanel { Spacing = 12 };
        diagContent.Children.Add(BuildDiagram());
        diagContent.Children.Add(new TextBlock
        {
            Style = S("CockpitCaption"),
            Text = "Exemple d'upmix stéréo (politique active). Pour un flux déclaré 5.1, le routage reste direct en 6→6 (voir ci-dessous).",
        });
        container.Children.Add(Card(
            "Schéma de routage",
            diagContent,
            "Entrée → DSP → sortie 5.1, dans l'ordre physique FL FR FC LFE SL SR."));

        // ---------- Carte « Préservation du 5.1 natif » ----------
        container.Children.Add(Card(
            "Préservation du 5.1 natif",
            BuildNative(),
            "Le mode 5.1 préserve les canaux natifs. Upmix, 2.0 et 2.1 traitent le mix Windows global."));

        return new ScrollViewer
        {
            Content = container,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ---------------------------------------------------------------------------
    // Carte générique : titre (H2), sous-titre optionnel (légende) puis contenu.
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

    // ---------------------------------------------------------------------------
    // Carte 1 : sélecteur de politique de source (groupe de boutons radio).
    private UIElement BuildPolicy()
    {
        var group = new StackPanel { Spacing = 6 };
        if (_shell.Client is Xonar.ControlCenter.Services.LiveApoBroker)
            group.Children.Add(new TextBlock {
                Text = "Auto utilise le format déclaré de chaque flux avant mixage : une source stéréo est étendue, une source 5.1 reste indépendante. Les réglages de sortie sont appliqués une seule fois après le mixage.",
                TextWrapping = TextWrapping.Wrap, Style = S("CockpitCaption")
            });
        group.Children.Add(PolicyOption(
            SourcePolicy.Preserve,
            "Préserver (vrai 5.1 intact)",
            "Le flux est routé tel quel : aucun mélange n'est appliqué, les six canaux d'un vrai 5.1 restent indépendants."));
        group.Children.Add(PolicyOption(
            SourcePolicy.AutoDeclaredLayout,
            "Upmix selon le layout déclaré (stéréo → 5.1)",
            "Une source stéréo est étendue vers 5.1 d'après le format annoncé par Windows ; un flux déjà 5.1 est laissé intact."));
        group.Children.Add(PolicyOption(
            SourcePolicy.ForceStereoFrontPair,
            "Forcer la paire frontale",
            "Tout est ramené aux deux enceintes avant (FL/FR) ; le centre, le caisson et les surrounds ne reçoivent rien."));
        return group;
    }

    private RadioButton PolicyOption(SourcePolicy policy, string title, string description)
    {
        var head = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        head.Children.Add(new TextBlock
        {
            Text = title,
            Foreground = B("CockpitTextPrimaryBrush"),
            FontSize = 14,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            VerticalAlignment = VerticalAlignment.Center,
        });

        // Badge présent sur chaque option mais masqué : SyncFromDraft / ShowActiveBadge
        // le révèle sur la seule politique active (il « se déplace » par visibilité).
        var badge = Badge("Actif");
        badge.Visibility = Visibility.Collapsed;
        head.Children.Add(badge);

        var body = new TextBlock
        {
            Text = description,
            Foreground = B("CockpitTextSecondaryBrush"),
            FontSize = 13,
            TextWrapping = TextWrapping.Wrap,
            MaxWidth = 900,
            Margin = new Thickness(0, 3, 0, 0),
        };

        var content = new StackPanel { Spacing = 0 };
        content.Children.Add(head);
        content.Children.Add(body);

        // Pas d'IsChecked codé en dur : l'état initial est posé au Loaded par
        // SyncFromDraft, d'après le brouillon partagé.
        var radio = new RadioButton
        {
            IsEnabled = _shell.CanSelectSourcePolicy(policy),
            GroupName = "SourcePolicy",
            Content = content,
            VerticalContentAlignment = VerticalAlignment.Top,
            MinHeight = 0,
            Margin = new Thickness(0, 2, 0, 2),
            Padding = new Thickness(8, 6, 8, 6),
        };
        // Checked peut être différé par WinUI après SyncFromDraft. Seul un clic
        // utilisateur doit modifier le profil, jamais la synchronisation visuelle.
        radio.Click += (_, _) => { if(radio.IsChecked == true) OnPolicyChecked(policy); };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(radio, title);

        _policyRows.Add((radio, policy, badge));
        return radio;
    }

    private Border Badge(string text)
    {
        var label = new TextBlock
        {
            Text = text,
            FontSize = 11,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            Foreground = B("CockpitAccentBrightBrush"),
        };
        return new Border { Style = S("CockpitBadge"), Child = label };
    }

    // ---------------------------------------------------------------------------
    // Carte 2 : diagramme vectoriel ENTRÉE (2.0) → DSP → SORTIE 5.1 (upmix stéréo).
    // Repère écran en pixels (Canvas de taille fixe), aligné à gauche.
    private Canvas BuildDiagram()
    {
        var cv = new Canvas { Width = 820, Height = 330, HorizontalAlignment = HorizontalAlignment.Left };

        // Centres verticaux des six sorties et géométrie des colonnes.
        double[] cy = { 64, 107, 150, 193, 236, 279 };
        const double dspRight = 435; // bord droit de la boîte DSP
        const double dspCy = 171;    // centre vertical du DSP
        const double outLeft = 624;  // bord gauche des boîtes de sortie

        // --- Liaisons (sous les boîtes) : entrées L/R vers le DSP ---
        cv.Children.Add(Conn(94, 130, 285, 150));
        cv.Children.Add(Conn(94, 212, 285, 192));

        // --- Liaisons DSP vers chacune des six sorties ---
        foreach (double y in cy)
        {
            cv.Children.Add(Conn(dspRight, dspCy, outLeft, y));
        }

        // --- Entrées stéréo L / R ---
        cv.Children.Add(DiagramBox(30, 112, 64, 36, B("CockpitSurface2Brush"), B("CockpitBorderBrush"),
            Centered("L", B("CockpitTextPrimaryBrush"), 15, semibold: true)));
        cv.Children.Add(DiagramBox(30, 194, 64, 36, B("CockpitSurface2Brush"), B("CockpitBorderBrush"),
            Centered("R", B("CockpitTextPrimaryBrush"), 15, semibold: true)));

        // --- Bloc DSP central ---
        var dspInner = new StackPanel { Spacing = 2, HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center };
        dspInner.Children.Add(new TextBlock
        {
            Text = "DSP",
            Foreground = B("CockpitTextPrimaryBrush"),
            FontSize = 16,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
            HorizontalAlignment = HorizontalAlignment.Center,
        });
        dspInner.Children.Add(new TextBlock
        {
            Text = "Upmix 2.0 → 5.1",
            Style = S("CockpitCaption"),
            HorizontalAlignment = HorizontalAlignment.Center,
            TextAlignment = TextAlignment.Center,
        });
        cv.Children.Add(DiagramBox(285, 111, 150, 120, B("CockpitSurfaceInsetBrush"), B("CockpitAccentDeepBrush"), dspInner));

        // --- Sorties 5.1 : un cartouche par canal, avec sa formule d'affectation en turquoise ---
        for (int i = 0; i < Channels.Length; i++)
        {
            var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 7, VerticalAlignment = VerticalAlignment.Center };
            row.Children.Add(new TextBlock
            {
                Text = Channels[i],
                Foreground = B("CockpitAccentBrush"),
                FontSize = 13,
                FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
                VerticalAlignment = VerticalAlignment.Center,
                Width = 34,
            });
            row.Children.Add(new TextBlock
            {
                Text = "=",
                Foreground = B("CockpitTextTertiaryBrush"),
                FontSize = 13,
                VerticalAlignment = VerticalAlignment.Center,
            });
            row.Children.Add(new TextBlock
            {
                Text = UpmixFormulas[i],
                Foreground = B("CockpitAccentBrush"),
                FontSize = 13,
                VerticalAlignment = VerticalAlignment.Center,
            });

            var box = DiagramBox(outLeft, cy[i] - 17, 190, 34, B("CockpitSurfaceInsetBrush"), B("CockpitBorderSubtleBrush"), row);
            box.Padding = new Thickness(12, 0, 12, 0);
            cv.Children.Add(box);
        }

        // --- Intitulés de colonnes ---
        var entree = ColumnHead("ENTRÉE", "2.0 — stéréo (exemple)");
        Canvas.SetLeft(entree, 20);
        Canvas.SetTop(entree, 8);
        cv.Children.Add(entree);

        var sortie = ColumnHead("SORTIE", "5.1 — 6 canaux");
        Canvas.SetLeft(sortie, outLeft);
        Canvas.SetTop(sortie, 8);
        cv.Children.Add(sortie);

        return cv;
    }

    // Boîte du diagramme : bordure arrondie positionnée en absolu sur le Canvas.
    private static Border DiagramBox(double left, double top, double width, double height, Brush background, Brush border, UIElement child)
    {
        var b = new Border
        {
            Width = width,
            Height = height,
            Background = background,
            BorderBrush = border,
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Child = child,
        };
        Canvas.SetLeft(b, left);
        Canvas.SetTop(b, top);
        return b;
    }

    // Liaison (segment) turquoise discrète entre deux points du diagramme.
    private Line Conn(double x1, double y1, double x2, double y2, double opacity = 0.5)
    {
        return new Line
        {
            X1 = x1,
            Y1 = y1,
            X2 = x2,
            Y2 = y2,
            Stroke = B("CockpitAccentBrush"),
            StrokeThickness = 1.5,
            Opacity = opacity,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
        };
    }

    private static TextBlock Centered(string text, Brush foreground, double size, bool semibold = false)
    {
        return new TextBlock
        {
            Text = text,
            Foreground = foreground,
            FontSize = size,
            FontWeight = semibold ? Microsoft.UI.Text.FontWeights.SemiBold : Microsoft.UI.Text.FontWeights.Normal,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            TextAlignment = TextAlignment.Center,
        };
    }

    private StackPanel ColumnHead(string title, string subtitle)
    {
        var s = new StackPanel { Spacing = 1 };
        s.Children.Add(new TextBlock
        {
            Text = title,
            Foreground = B("CockpitTextSecondaryBrush"),
            FontSize = 12,
            FontWeight = Microsoft.UI.Text.FontWeights.SemiBold,
        });
        s.Children.Add(new TextBlock { Text = subtitle, Style = S("CockpitCaption") });
        return s;
    }

    // ---------------------------------------------------------------------------
    // Carte 3 : préservation du 5.1 natif (routage identité 6→6).
    private UIElement BuildNative()
    {
        var stack = new StackPanel { Spacing = 12 };

        stack.Children.Add(new TextBlock
        {
            Style = S("CockpitBody"),
            Text = "Lorsqu'un flux est déclaré en 5.1 par Windows, les six canaux entrants sont acheminés tels quels vers les six sorties, "
                 + "sans upmix ni downmix : FL→FL, FR→FR, FC→FC, LFE→LFE, SL→SL, SR→SR.",
        });

        // Indicateur « 6 → 6 · identité ».
        var chipRow = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        chipRow.Children.Add(new TextBlock { Text = "6 canaux", Foreground = B("CockpitTextPrimaryBrush"), FontSize = 13, VerticalAlignment = VerticalAlignment.Center });
        chipRow.Children.Add(new TextBlock { Text = "→", Foreground = B("CockpitAccentBrush"), FontSize = 14, FontWeight = Microsoft.UI.Text.FontWeights.SemiBold, VerticalAlignment = VerticalAlignment.Center });
        chipRow.Children.Add(new TextBlock { Text = "6 canaux", Foreground = B("CockpitTextPrimaryBrush"), FontSize = 13, VerticalAlignment = VerticalAlignment.Center });
        chipRow.Children.Add(new TextBlock { Text = "· identité (aucun mixage)", Foreground = B("CockpitAccentBrush"), FontSize = 13, VerticalAlignment = VerticalAlignment.Center });
        stack.Children.Add(new Border
        {
            Background = B("CockpitSurfaceInsetBrush"),
            BorderBrush = B("CockpitAccentDeepBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(12, 8, 12, 8),
            HorizontalAlignment = HorizontalAlignment.Left,
            Child = chipRow,
        });

        stack.Children.Add(new TextBlock
        {
            Style = S("CockpitBody"),
            Text = "La décision repose sur le format déclaré, et non sur l'analyse du signal : un canal surround momentanément silencieux "
                 + "reste un vrai canal et n'est jamais confondu avec de la stéréo ni replié sur la paire frontale.",
        });

        stack.Children.Add(new TextBlock
        {
            Style = S("CockpitCaption"),
            Text = "Note honnête : la préservation dépend de l'exactitude du format annoncé par l'application et par Windows. "
                 + "Si une source étiquette mal son flux, le DSP suit la déclaration ; aucune détection de contenu n'est réalisée ici.",
        });

        return stack;
    }
}
