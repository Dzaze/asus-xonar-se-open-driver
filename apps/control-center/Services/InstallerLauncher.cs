using System.Diagnostics;

namespace Xonar.ControlCenter.Services;

public static class InstallerLauncher
{
    public static string? FindScript()
    {
        var directory = new DirectoryInfo(AppContext.BaseDirectory);
        for(int i = 0; i < 9 && directory is not null; ++i, directory = directory.Parent) {
            string repair = Path.Combine(directory.FullName, "scripts", "apo", "repair-speaker-effects.ps1");
            if(File.Exists(repair) && File.Exists(@"C:\Program Files\XonarSE\XonarApo.dll")) return repair;
            string script = Path.Combine(directory.FullName, "scripts", "install", "install-xonar-apo.ps1");
            if(File.Exists(script)) return script;
        }
        return null;
    }

    public static async Task<(bool Success, string Message)> RunAsync()
    {
        string? script = FindScript();
        if(script is null) return (false, "Installateur introuvable. Utiliser scripts/install/Xonar-APO.cmd du paquet complet.");
        try {
            string shell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "PowerShell", "7", "pwsh.exe");
            if(!File.Exists(shell)) shell = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe");
            using var process = Process.Start(new ProcessStartInfo {
                FileName = shell,
                Arguments = "-NoProfile -ExecutionPolicy Bypass -File \"" + script + "\"",
                UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden
            });
            if(process is null) return (false, "Le processus d’installation n’a pas démarré.");
            await process.WaitForExitAsync();
            return process.ExitCode == 0 ? (true, "Installation terminée.") :
                (false, "Installation en échec. Consulter le journal xonar-apo-install dans %TEMP% pour vérifier le résultat de la restauration.");
        } catch(Exception ex) { return (false, "Installation non lancée : " + ex.Message); }
    }
}
