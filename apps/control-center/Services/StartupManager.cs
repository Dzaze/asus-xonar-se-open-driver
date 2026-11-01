// SPDX-License-Identifier: Apache-2.0
// Démarrage automatique de l'application avec Windows (opt-in).
//
// PORTÉE ET SÛRETÉ : écrit UNIQUEMENT une entrée de démarrage PROPRE À CETTE
// APPLICATION dans la ruche UTILISATEUR (HKCU\...\Run). C'est le mécanisme
// standard et réversible d'un panneau de contrôle qui se lance au démarrage.
// Cela ne touche NI au pilote, NI au format audio, NI au test-signing, NI à la
// configuration de démarrage de Windows — aucune mutation de la pile audio. La
// désactivation supprime simplement l'entrée.

using System;
using Microsoft.Win32;

namespace Xonar.ControlCenter.Services;

public static class StartupManager
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "XonarControlCenter";

    /// <summary>Vrai si l'application est enregistrée pour démarrer avec la session.</summary>
    public static bool IsEnabled()
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.OpenSubKey(RunKeyPath, writable: false);
            return key?.GetValue(ValueName) is not null;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Active ou désactive le démarrage automatique (entrée HKCU\Run).</summary>
    public static bool SetEnabled(bool enabled)
    {
        try
        {
            using RegistryKey? key = Registry.CurrentUser.CreateSubKey(RunKeyPath, writable: true);
            if (key is null)
            {
                return false;
            }
            if (enabled)
            {
                string? exe = Environment.ProcessPath;
                if (string.IsNullOrEmpty(exe))
                {
                    return false;
                }
                key.SetValue(ValueName, $"\"{exe}\" --background");
            }
            else
            {
                key.DeleteValue(ValueName, throwOnMissingValue: false);
            }
            return true;
        }
        catch
        {
            return false;
        }
    }
}
