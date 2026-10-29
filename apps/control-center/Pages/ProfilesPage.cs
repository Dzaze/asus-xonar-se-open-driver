// SPDX-License-Identifier: Apache-2.0
// Page « Profils » : réglages personnels (Musique, Cinéma, Jeux), profils
// enregistrés et import/export au format JSON. Interface construite en code,
// thème « cockpit » sombre turquoise. Cliquer une carte de preset applique un
// mode d'écoute via l'éditeur partagé (shell.Editor) et un indicateur reflète
// l'état d'application. Les profils personnels et les fichiers JSON sont persistants.

using System.Collections.Generic;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.Windows.Storage.Pickers;
using Xonar.ControlCenter.Services;
using Xonar.Contracts.V1;

// Alias ciblés : on garde les identifiants techniques exacts sans importer les
// espaces de noms entiers (évite toute ambiguïté avec Profiles.ProfilesPage).
using ListeningMode = Xonar.Contracts.V1.ListeningMode;
using ProfileEditorState = Xonar.ControlCenter.Profiles.ProfileEditorState;

namespace Xonar.ControlCenter.Pages;

public sealed class ProfilesPage : Microsoft.UI.Xaml.Controls.UserControl
{
    private readonly Xonar.ControlCenter.Shell.AppShellViewModel _shell;
    private readonly List<Border> _presetCards = new();
    private readonly List<FontIcon> _presetIcons = new();
    private int _selectedPreset = 1; // Cinéma sélectionné par défaut.

    private bool _suspend;                 // garde anti-réentrance (init / MAJ programmatiques)
    private Border _stateDot = null!;      // pastille colorée de l'indicateur d'état
    private TextBlock _stateText = null!;  // libellé « État : … »
    private readonly ProfileLibrary _library = new();
    private StackPanel _savedProfiles = null!;
    private readonly TextBlock _profileNotice = new() { TextWrapping = TextWrapping.Wrap };

    private Brush B(string key) => (Brush)Application.Current.Resources[key];
    private Style S(string key) => (Style)Application.Current.Resources[key];

    public ProfilesPage(Xonar.ControlCenter.Shell.AppShellViewModel shell)
    {
        _shell = shell;

        // Phase d'init : aucune écriture ne doit être déclenchée par la construction.
        _suspend = true;
        Content = Build();
        _suspend = false;

        // L'éditeur partagé (toujours non-null) lève Changed depuis le coalesceur
        // d'applications (thread hors-UI) : on s'abonne APRÈS avoir construit l'UI, on
        // marshale toujours vers le thread UI (voir OnEditorChanged) et on se désabonne
        // au déchargement (l'éditeur est un singleton, la page est recréée à chaque
        // navigation : on évite ainsi une fuite d'abonnement).
        Loaded += (_, _) => {
            _shell.Editor.Changed -= OnEditorChanged;
            _shell.Editor.Changed += OnEditorChanged;
            RefreshState();RefreshSavedProfiles();
        };
        Unloaded += (_, _) => _shell.Editor.Changed -= OnEditorChanged;
        RefreshState();
    }

    private UIElement Build()
    {
        var root = new StackPanel
        {
            Margin = new Thickness(32, 26, 32, 28),
            MaxWidth = 1480,
            HorizontalAlignment = HorizontalAlignment.Left,
            Spacing = 20,
        };

        root.Children.Add(BuildHeader());
        root.Children.Add(BuildPresetRow());
        root.Children.Add(BuildStateIndicator());
        root.Children.Add(BuildBottomRow());

        return new ScrollViewer
        {
            Content = root,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Disabled,
        };
    }

    // ---------- En-tête ----------
    private UIElement BuildHeader()
    {
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        var titleStack = new StackPanel();
        titleStack.Children.Add(new TextBlock { Text = "Profils", Style = S("CockpitH1") });
        titleStack.Children.Add(new TextBlock
        {
            Text = "Réglages personnels Musique, Cinéma, Jeux ; import/export.",
            Style = S("CockpitLabel"),
            Margin = new Thickness(0, 2, 0, 0),
        });
        head.Children.Add(titleStack);

        var status = StatusChip();
        Grid.SetColumn(status, 1);
        head.Children.Add(status);

        return head;
    }

    private Border StatusChip()
    {
        bool connected = _shell.IsConnected;
        var dot = new Border
        {
            Width = 9,
            Height = 9,
            CornerRadius = new CornerRadius(5),
            Background = connected ? B("CockpitAccentBrush") : B("CockpitTextTertiaryBrush"),
            VerticalAlignment = VerticalAlignment.Center,
        };
        var text = new TextBlock
        {
            Text = connected ? "Agent connecté" : "Agent non détecté",
            Foreground = B("CockpitTextSecondaryBrush"),
            FontSize = 13,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var row = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 9, VerticalAlignment = VerticalAlignment.Center };
        row.Children.Add(dot);
        row.Children.Add(text);
        return new Border
        {
            Style = S("CockpitBadge"),
            Padding = new Thickness(12, 6, 12, 6),
            VerticalAlignment = VerticalAlignment.Center,
            Child = row,
        };
    }

    // ---------- Rangée de presets ----------
    private UIElement BuildPresetRow()
    {
        var grid = new Grid { ColumnSpacing = 16 };
        for (int i = 0; i < 3; i++)
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        }

        var presets = new (string Glyph, string Name, string Desc)[]
        {
            ("", "Musique", "Stéréo fidèle, grave maîtrisé, scène large"),
            ("", "Cinéma", "Upmix 5.1, dialogues renforcés, LFE dosé"),
            ("", "Jeux", "Surround réactif, pas directionnels, basse latence"),
        };

        for (int i = 0; i < presets.Length; i++)
        {
            int idx = i;
            var card = PresetCard(presets[i].Glyph, presets[i].Name, presets[i].Desc, i == _selectedPreset);
            card.Tapped += (_, _) => SelectPreset(idx);
            Grid.SetColumn(card, i);
            grid.Children.Add(card);
        }

        return grid;
    }

    private Border PresetCard(string glyph, string name, string description, bool selected)
    {
        var icon = new FontIcon
        {
            Glyph = glyph,
            FontSize = 30,
            HorizontalAlignment = HorizontalAlignment.Left,
        };
        var stack = new StackPanel { Spacing = 12 };
        stack.Children.Add(icon);
        stack.Children.Add(new TextBlock { Text = name, Style = S("CockpitH2") });
        stack.Children.Add(new TextBlock { Text = description, Style = S("CockpitCaption") });

        var card = new Border
        {
            CornerRadius = new CornerRadius(14),
            Padding = new Thickness(22),
            MinHeight = 158,
            Child = stack,
        };

        _presetCards.Add(card);
        _presetIcons.Add(icon);
        ApplyPresetVisual(card, icon, selected);
        return card;
    }

    private void SelectPreset(int index)
    {
        _selectedPreset = index;
        for (int i = 0; i < _presetCards.Count; i++)
        {
            ApplyPresetVisual(_presetCards[i], _presetIcons[i], i == index);
        }

        if (_suspend)
        {
            return; // sélection programmatique / init : on ne touche pas l'éditeur.
        }

        // Câblage : un clic applique le mode d'écoute du preset via l'éditeur partagé.
        // Application coalescée (≤ 10 cmd/s) côté ProfileEditorViewModel ; lève Changed,
        // ce qui rafraîchit l'indicateur d'état via l'abonnement marshalé.
        _shell.Editor.LoadProfile(ListeningPresets.Create(_shell.Editor.Draft, index));
    }

    private void ApplyPresetVisual(Border card, FontIcon icon, bool selected)
    {
        card.Background = selected ? B("CockpitAccentSoftBrush") : B("CockpitSurfaceBrush");
        card.BorderBrush = selected ? B("CockpitAccentBrush") : B("CockpitBorderSubtleBrush");
        card.BorderThickness = new Thickness(selected ? 1.5 : 1);
        icon.Foreground = selected ? B("CockpitAccentBrightBrush") : B("CockpitTextSecondaryBrush");
    }

    // Mapping preset -> mode d'écoute : Musique = 5.1, Cinéma et Jeux = Upmix.
    private static ListeningMode ModeForPreset(int index) =>
        index == 0 ? ListeningMode.Surround51 : ListeningMode.Upmix;

    // ---------- Indicateur d'état d'application ----------
    // Petit badge sous les cartes reflétant shell.Editor.State.
    private UIElement BuildStateIndicator()
    {
        _stateDot = new Border
        {
            Width = 9,
            Height = 9,
            CornerRadius = new CornerRadius(5),
            VerticalAlignment = VerticalAlignment.Center,
        };
        _stateText = new TextBlock
        {
            Foreground = B("CockpitTextSecondaryBrush"),
            FontSize = 13,
            VerticalAlignment = VerticalAlignment.Center,
        };

        var row = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 9,
            VerticalAlignment = VerticalAlignment.Center,
        };
        row.Children.Add(_stateDot);
        row.Children.Add(_stateText);

        return new Border
        {
            Style = S("CockpitBadge"),
            Padding = new Thickness(12, 6, 12, 6),
            HorizontalAlignment = HorizontalAlignment.Left,
            Child = row,
        };
    }

    // Changed peut venir d'un thread hors-UI (coalesceur d'applications) : on marshale
    // TOUJOURS vers le thread UI avant de toucher aux contrôles WinUI, sinon
    // RPC_E_WRONG_THREAD (conf#0 volet 2 / part#1).
    private void OnEditorChanged()
    {
        var q = DispatcherQueue;
        if (q is null || q.HasThreadAccess)
        {
            RefreshState();
        }
        else
        {
            q.TryEnqueue(RefreshState);
        }
    }

    private void RefreshState()
    {
        var (label, brush) = _shell.Editor.State switch
        {
            ProfileEditorState.Applied => ("Appliqué", B("CockpitAccentBrush")),
            ProfileEditorState.Applying => ("Application…", B("CockpitAccentBrightBrush")),
            ProfileEditorState.PartiallyApplied => ("Partiel", B("CockpitWarningBrush")),
            ProfileEditorState.Error => ("Erreur", B("CockpitDangerBrush")),
            _ => ("Brouillon", B("CockpitTextTertiaryBrush")),
        };
        _stateDot.Background = brush;
        _stateText.Text = $"État : {label}";
    }

    // ---------- Bas : « Mes profils » + « Import / Export » ----------
    private UIElement BuildBottomRow()
    {
        var grid = new Grid { ColumnSpacing = 18 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1.6, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });

        var mine = MyProfilesCard();
        grid.Children.Add(mine);

        var io = ImportExportCard();
        Grid.SetColumn(io, 1);
        grid.Children.Add(io);

        return grid;
    }

    private Border MyProfilesCard()
    {
        var head = new Grid();
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        head.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        head.Children.Add(new TextBlock { Text = "Mes profils", Style = S("CockpitH2"), VerticalAlignment = VerticalAlignment.Center });
        var nouveau = new Button { Style = S("CockpitAccentButton"), Content = "Enregistrer le profil courant" };
        // « Nouveau profil » : repart d'un preset conservateur (limiteur actif). Lève
        // Changed -> l'indicateur d'état se met à jour via l'abonnement marshalé.
        nouveau.Click += async (_, _) => await RunProfileActionAsync(async () => {
            var name = new TextBox { Text = "Mon profil", MaxLength = 48, PlaceholderText = "Nom du profil" };
            var dialog = new ContentDialog { XamlRoot = XamlRoot, Title = "Enregistrer les réglages actuels", Content = name,
                PrimaryButtonText = "Enregistrer", CloseButtonText = "Annuler", DefaultButton = ContentDialogButton.Primary };
            if(await dialog.ShowAsync() != ContentDialogResult.Primary) return;
            if(string.IsNullOrWhiteSpace(name.Text)) throw new FormatException("Donnez un nom au profil.");
            var profile = ProfileDefaults.Clone(_shell.Editor.Draft);
            profile.ProfileId = Guid.NewGuid().ToString("D");profile.Name = name.Text.Trim();
            _library.Save(profile);RefreshSavedProfiles();_profileNotice.Text = "Profil enregistré.";
        });
        Grid.SetColumn(nouveau, 1);
        head.Children.Add(nouveau);

        _savedProfiles = new StackPanel { Spacing = 10 };

        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(head);
        stack.Children.Add(_savedProfiles);
        stack.Children.Add(_profileNotice);

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private Border ProfileRow(ProfileDocument profile)
    {
        var grid = new Grid { ColumnSpacing = 8 };
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

        grid.Children.Add(new TextBlock { Text = profile.Name, Style = S("CockpitBody"), VerticalAlignment = VerticalAlignment.Center });

        var actions = new StackPanel
        {
            Orientation = Orientation.Horizontal,
            Spacing = 8,
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Center,
        };
        var apply = GhostButton("Appliquer");
        apply.Click += async (_, _) => await RunProfileActionAsync(async () => {
            _shell.Editor.LoadProfile(profile);await _shell.Editor.ApplyNowAsync();
            _profileNotice.Text = _shell.Editor.LastMessage;
        });
        actions.Children.Add(apply);
        var duplicate = GhostButton("Dupliquer");
        duplicate.Click += async (_, _) => await RunProfileActionAsync(() => {
            var copy = ProfileDefaults.Clone(profile);copy.ProfileId = Guid.NewGuid().ToString("D");
            copy.Name = profile.Name + " (copie)";_library.Save(copy);RefreshSavedProfiles();
            return Task.CompletedTask;
        });
        actions.Children.Add(duplicate);
        var supprimer = GhostButton("Supprimer");
        supprimer.Foreground = B("CockpitDangerBrush");
        supprimer.Click += async (_, _) => await RunProfileActionAsync(async () => {
            var dialog = new ContentDialog { XamlRoot = XamlRoot, Title = "Supprimer ce profil ?", Content = profile.Name,
                PrimaryButtonText = "Supprimer", CloseButtonText = "Annuler", DefaultButton = ContentDialogButton.Close };
            if(await dialog.ShowAsync() == ContentDialogResult.Primary) { _library.Delete(profile.ProfileId);RefreshSavedProfiles(); }
        });
        actions.Children.Add(supprimer);
        Grid.SetColumn(actions, 1);
        grid.Children.Add(actions);

        return new Border
        {
            Background = B("CockpitSurfaceInsetBrush"),
            BorderBrush = B("CockpitBorderSubtleBrush"),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(10),
            Padding = new Thickness(14, 12, 14, 12),
            Child = grid,
        };
    }

    private Border ImportExportCard()
    {
        var import = new Button
        {
            Style = S("CockpitGhostButton"),
            Content = "Importer un profil…",
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        var export = new Button
        {
            Style = S("CockpitGhostButton"),
            Content = "Exporter le profil courant…",
            HorizontalAlignment = HorizontalAlignment.Stretch,
        };
        import.Click += async (_, _) => await RunProfileActionAsync(async () => {
            var picker = new FileOpenPicker(((App)Application.Current).MainWindowId);
            picker.FileTypeFilter.Add(".json");
            var file = await picker.PickSingleFileAsync();
            if(file is null) return;
            var profile = ProfileLibrary.Read(file.Path);
            profile.ProfileId = Guid.NewGuid().ToString("D");
            _library.Save(profile);RefreshSavedProfiles();_profileNotice.Text = "Profil importé et validé.";
        });
        export.Click += async (_, _) => await RunProfileActionAsync(async () => {
            var picker = new FileSavePicker(((App)Application.Current).MainWindowId) {
                SuggestedFileName = "xonar-profil", DefaultFileExtension = ".json"
            };
            picker.FileTypeChoices.Add("Profil Xonar SE", new List<string> { ".json" });
            var file = await picker.PickSaveFileAsync();
            if(file is null) return;
            ProfileLibrary.Export(_shell.Editor.Draft, file.Path);_profileNotice.Text = "Profil exporté.";
        });

        var buttons = new StackPanel { Spacing = 10 };
        buttons.Children.Add(import);
        buttons.Children.Add(export);

        var stack = new StackPanel { Spacing = 14 };
        stack.Children.Add(new TextBlock { Text = "Import / Export", Style = S("CockpitH2") });
        stack.Children.Add(buttons);
        stack.Children.Add(new TextBlock { Text = "Fichiers JSON de profil v1.", Style = S("CockpitCaption") });

        return new Border { Style = S("CockpitCard"), Child = stack };
    }

    private Button GhostButton(string text) => new Button { Style = S("CockpitGhostButton"), Content = text };

    private void RefreshSavedProfiles()
    {
        try {
            _savedProfiles.Children.Clear();
            foreach(var profile in _library.List()) _savedProfiles.Children.Add(ProfileRow(profile));
            if(_savedProfiles.Children.Count == 0)
                _savedProfiles.Children.Add(new TextBlock { Text = "Aucun profil enregistré. Conservez vos réglages avec le bouton ci-dessus.", TextWrapping = TextWrapping.Wrap });
        } catch(Exception ex) { _profileNotice.Text = "Lecture des profils impossible : " + ex.Message; }
    }

    private async Task RunProfileActionAsync(Func<Task> action)
    {
        try { await action(); }
        catch(Exception ex) { _profileNotice.Text = "Opération impossible : " + ex.Message; }
    }
}
