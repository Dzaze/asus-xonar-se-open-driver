// SPDX-License-Identifier: Apache-2.0
// Point d'entrée de l'application. Crée la fenêtre principale et l'icône de la zone
// de notification (tray) : fermer la fenêtre la RANGE dans le tray plutôt que de
// quitter ; « Quitter » arrête réellement l’application. Le client local réel est injecté ici.

using System;
using H.NotifyIcon;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;
using Xonar.ControlCenter.Services;
using Xonar.ControlCenter.Shell;

namespace Xonar.ControlCenter;

public partial class App : Application
{
    private MainWindow? _window;
    private System.Threading.Mutex? _instanceMutex;
    public Microsoft.UI.WindowId MainWindowId => _window?.AppWindow.Id ?? default;
    private TaskbarIcon? _tray;

    /// <summary>Vrai pendant un arrêt réel (via « Quitter ») : la fenêtre ne se range alors plus dans le tray.</summary>
    public bool IsExiting { get; private set; }

    public App()
    {
        UnhandledException += (_, e) => LogStartupFailure(e.Exception);
        InitializeComponent();
    }

    private static void LogStartupFailure(Exception error)
    {
        try {
            string directory = System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData), "XonarSE");
            System.IO.Directory.CreateDirectory(directory);
            System.IO.File.AppendAllText(System.IO.Path.Combine(directory, "control-center-error.log"), DateTimeOffset.Now + " " + error + Environment.NewLine);
        } catch { }
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        bool background = Array.Exists(Environment.GetCommandLineArgs(), a => a == "--background");
        _instanceMutex = new System.Threading.Mutex(false, @"Local\XonarSE.ControlCenter", out bool firstInstance);
        if(!firstInstance) {
            var window = FindWindow(null, "Centre de contrôle Xonar SE");
            if(!background && window != IntPtr.Zero) { ShowWindow(window, 9);SetForegroundWindow(window); }
            _instanceMutex.Dispose();_instanceMutex = null;Exit();return;
        }
        // Matériel résolu par identité USB ; état observé dans audiodg et profil persistant.
        IControlClient client = new LiveApoBroker();

        _window = new MainWindow(client);
        _window.Activate();

        CreateTrayIcon();
        if(background) _window.AppWindow.Hide();
    }

    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    private static extern IntPtr FindWindow(string? className, string title);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr window, int command);
    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr window);

    private void CreateTrayIcon()
    {
        var menu = new MenuFlyout();
        var open = new MenuFlyoutItem { Text = "Ouvrir le centre de contrôle" };
        open.Click += (_, _) => ShowMainWindow();
        var quit = new MenuFlyoutItem { Text = "Quitter" };
        quit.Click += (_, _) => ExitApplication();
        menu.Items.Add(open);
        menu.Items.Add(new MenuFlyoutSeparator());
        menu.Items.Add(quit);

        _tray = new TaskbarIcon
        {
            ToolTipText = "Centre de contrôle Xonar SE",
            ContextFlyout = menu,
            LeftClickCommand = new RelayCommand(ShowMainWindow),
        };
        try
        {
            _tray.IconSource = new BitmapImage(new Uri("ms-appx:///Assets/app.ico"));
        }
        catch
        {
            // Icône non critique : le tray reste fonctionnel même sans visuel.
        }
        _tray.ForceCreate();
    }

    /// <summary>Affiche et met au premier plan la fenêtre principale (depuis le tray).</summary>
    public void ShowMainWindow()
    {
        if (_window is null)
        {
            return;
        }
        _window.AppWindow.Show();
        _window.Activate();
    }

    /// <summary>Arrêt réel de l'application (menu « Quitter » du tray).</summary>
    public void ExitApplication()
    {
        IsExiting = true;
        try { _tray?.Dispose(); } catch { /* ignoré */ }
        _window?.Close();
        Exit();
    }
}
