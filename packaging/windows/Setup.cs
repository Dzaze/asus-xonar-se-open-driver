// SPDX-License-Identifier: Apache-2.0
// Compilable avec le compilateur .NET Framework fourni par Windows 11.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

namespace XonarSE.Setup {
    internal static class Program {
        internal static readonly string InstallRoot=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),"XonarSE");
        internal static readonly JavaScriptSerializer Json=new JavaScriptSerializer {MaxJsonLength=16777216};
        internal static string NewStage(){string parent=Path.Combine(InstallRoot,"Installer");Directory.CreateDirectory(parent);string path=Path.Combine(parent,"stage-"+Guid.NewGuid().ToString("N"));Directory.CreateDirectory(path);return path;}
        internal static string Digest(Stream stream){using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-","");}
        internal static string SafePath(string root,string relative){
            string prefix=Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar;
            if(Path.IsPathRooted(relative)||relative.Contains(":"))throw new InvalidDataException("Chemin de paquet invalide.");
            string path=Path.GetFullPath(Path.Combine(prefix,relative.Replace('/',Path.DirectorySeparatorChar)));
            if(!path.StartsWith(prefix,StringComparison.OrdinalIgnoreCase))throw new InvalidDataException("Chemin hors paquet.");
            return path;
        }
        internal static void Cleanup(string stage){
            if(String.IsNullOrEmpty(stage)||!Directory.Exists(stage))return;
            string allowed=Path.GetFullPath(Path.Combine(InstallRoot,"Installer")).TrimEnd('\\')+"\\";
            string full=Path.GetFullPath(stage);
            if(!full.StartsWith(allowed,StringComparison.OrdinalIgnoreCase)||!Path.GetFileName(full).StartsWith("stage-",StringComparison.Ordinal))throw new InvalidOperationException("Nettoyage hors dossier installation refuse.");
            Directory.Delete(full,true);
        }
        internal static void Extract(string stage){
            using(var resource=Assembly.GetExecutingAssembly().GetManifestResourceStream("Xonar.Payload.zip")){
                if(resource==null)throw new InvalidDataException("Paquet embarque absent.");
                using(var zip=new ZipArchive(resource,ZipArchiveMode.Read)){
                    foreach(var entry in zip.Entries){
                        string target=SafePath(stage,entry.FullName);
                        if(String.IsNullOrEmpty(entry.Name)){Directory.CreateDirectory(target);continue;}
                        Directory.CreateDirectory(Path.GetDirectoryName(target));
                        using(var input=entry.Open())using(var output=new FileStream(target,FileMode.CreateNew,FileAccess.Write))input.CopyTo(output);
                    }
                }
            }
            Verify(stage);
        }
        internal static int Verify(string stage){
            string manifest=Path.Combine(stage,"manifest.json");
            using(var stream=File.OpenRead(manifest))if(Digest(stream)!=BuildInfo.ManifestHash)throw new InvalidDataException("Manifeste altere.");
            var root=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(manifest));
            var files=(System.Collections.IEnumerable)root["Files"];
            var seen=new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach(Dictionary<string,object> item in files){
                string target=SafePath(stage,(string)item["Path"]);
                if(!seen.Add(target))throw new InvalidDataException("Fichier duplique.");
                using(var stream=File.OpenRead(target))if(Digest(stream)!=(string)item["Sha256"])throw new InvalidDataException("Fichier altere : "+item["Path"]);
            }
            if(Directory.GetFiles(stage,"*",SearchOption.AllDirectories).Length!=seen.Count+1)throw new InvalidDataException("Fichier non declare dans le paquet.");
            return seen.Count;
        }
        internal static string Quote(string value){if(value.Contains("\""))throw new ArgumentException("Chemin invalide.");return "\""+value+"\"";}
        internal static int Execute(string script,string arguments,Action<string> log){
            var info=new ProcessStartInfo {
                FileName=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),"WindowsPowerShell","v1.0","powershell.exe"),
                Arguments="-NoProfile -ExecutionPolicy Bypass -File "+Quote(script)+" "+arguments,
                UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true,
                StandardOutputEncoding=Encoding.UTF8,StandardErrorEncoding=Encoding.UTF8
            };
            using(var process=new Process {StartInfo=info}){
                process.OutputDataReceived+=(sender,e)=>{if(e.Data!=null)log(e.Data);};
                process.ErrorDataReceived+=(sender,e)=>{if(e.Data!=null)log(e.Data);};
                process.Start();process.BeginOutputReadLine();process.BeginErrorReadLine();process.WaitForExit();return process.ExitCode;
            }
        }
        internal static void CopyScripts(string stage){
            string source=Path.Combine(InstallRoot,"Scripts");
            foreach(string file in Directory.GetFiles(source,"*",SearchOption.AllDirectories)){
                string relative=file.Substring(source.Length).TrimStart('\\');string target=SafePath(Path.Combine(stage,"Scripts"),relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target));File.Copy(file,target);
            }
        }
        [STAThread] public static int Main(string[] args){
            bool uninstall=args.Contains("/uninstall");
#if UNINSTALL
            uninstall=true;
            // Le programme lance une copie protegee pour pouvoir retirer son executable installe.
            if(!args.Contains("/worker")){
                string worker=NewStage();string path=Path.Combine(worker,"Uninstall.exe");File.Copy(Assembly.GetExecutingAssembly().Location,path);
                Process.Start(new ProcessStartInfo(path,"/uninstall /worker") {UseShellExecute=true});return 0;
            }
#endif
            if(args.Length>=2&&args[0]=="/verify"){
                string stage=null;
                try{stage=NewStage();Extract(stage);int count=Verify(stage);File.WriteAllText(args[1],Json.Serialize(new {Passed=true,Files=count,Version=BuildInfo.Version}),Encoding.UTF8);return 0;}
                catch(Exception ex){File.WriteAllText(args[1],Json.Serialize(new {Passed=false,Error=ex.Message}),Encoding.UTF8);return 1;}
                finally{if(stage!=null)Cleanup(stage);}
            }
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            using(var window=new SetupForm(uninstall)){Application.Run(window);return window.ResultCode;}
        }
    }
    internal sealed class SetupForm:Form {
        readonly bool uninstall;
        readonly TextBox output=new TextBox {Multiline=true,ReadOnly=true,ScrollBars=ScrollBars.Vertical,BackColor=Color.FromArgb(16,23,33),ForeColor=Color.FromArgb(205,220,231),BorderStyle=BorderStyle.FixedSingle};
        readonly CheckBox consent=new CheckBox {AutoSize=false};
        readonly Button action=new Button {FlatStyle=FlatStyle.Flat};
        readonly Button open=new Button {Text="Ouvrir le centre de contrôle",Visible=false,FlatStyle=FlatStyle.Flat};
        readonly ProgressBar progress=new ProgressBar {Style=ProgressBarStyle.Marquee,Visible=false};
        readonly Label status=new Label {Text="Prêt",AutoSize=false};
        bool working,finished;
        internal int ResultCode=2;
        public SetupForm(bool remove){
            uninstall=remove;Text=remove?"Désinstallation Xonar SE":"Installation Xonar SE";
            AutoScaleMode=AutoScaleMode.None;
            ClientSize=new Size(780,620);MinimumSize=new Size(720,640);StartPosition=FormStartPosition.CenterScreen;
            BackColor=Color.FromArgb(10,15,23);ForeColor=Color.FromArgb(225,235,242);Font=new Font("Segoe UI",10);
            var title=new Label {Text="XONAR SE",Font=new Font("Segoe UI",24,FontStyle.Bold),Location=new Point(28,20),Size=new Size(720,48)};Controls.Add(title);
            var description=new Label {Text=remove?"Retrouver l’état audio précédent":"Pilote ouvert · Centre de contrôle · Version "+BuildInfo.Version,Location=new Point(30,72),Size=new Size(710,26)};Controls.Add(description);
            var info=new Label {Location=new Point(30,114),Size=new Size(710,88),Text=remove?
                "L’installation sauvegardée sera restaurée. Les profils sont conservés par défaut. Les sauvegardes et journaux restent disponibles.":
                "Windows 11 x64 · ASUS Xonar SE uniquement\nPilote expérimental signé avec un certificat de test, sans certification Microsoft WHQL.\nLe mode test Windows doit déjà être activé. L’installateur ne change pas le démarrage Windows ni Secure Boot."};Controls.Add(info);
            var help=new LinkLabel {Text="Instructions du mode test et installation",Location=new Point(30,207),Size=new Size(690,25),LinkColor=Color.Turquoise};
            help.LinkClicked+=(sender,e)=>Process.Start(new ProcessStartInfo("https://github.com/Dzaze/asus-xonar-se-open-driver/blob/main/docs/INSTALLATION.fr.md") {UseShellExecute=true});Controls.Add(help);
            consent.Text=remove?"Supprimer également mes profils (facultatif)":"J’accepte l’installation du pilote de test et de son certificat public de confiance.";
            consent.Location=new Point(30,243);consent.Size=new Size(710,45);Controls.Add(consent);
            output.Location=new Point(30,300);output.Size=new Size(720,190);output.Anchor=AnchorStyles.Top|AnchorStyles.Bottom|AnchorStyles.Left|AnchorStyles.Right;Controls.Add(output);
            status.Location=new Point(30,505);status.Size=new Size(720,25);status.Anchor=AnchorStyles.Bottom|AnchorStyles.Left|AnchorStyles.Right;Controls.Add(status);
            progress.Location=new Point(30,538);progress.Size=new Size(720,6);progress.Anchor=AnchorStyles.Bottom|AnchorStyles.Left|AnchorStyles.Right;Controls.Add(progress);
            action.Text=remove?"Désinstaller":"Installer / mettre à jour";action.Location=new Point(30,563);action.Size=new Size(245,38);
            action.BackColor=Color.Turquoise;action.ForeColor=Color.FromArgb(8,27,30);action.Anchor=AnchorStyles.Bottom|AnchorStyles.Left;action.Enabled=remove;Controls.Add(action);
            consent.CheckedChanged+=(sender,e)=>{if(!working)action.Enabled=uninstall||consent.Checked;};
            open.Location=new Point(300,563);open.Size=new Size(300,38);open.Anchor=AnchorStyles.Bottom|AnchorStyles.Left;Controls.Add(open);
            open.Click+=(sender,e)=>Process.Start(new ProcessStartInfo("explorer.exe",Program.Quote(Path.Combine(Program.InstallRoot,"App","Xonar.ControlCenter.exe"))) {UseShellExecute=true});
            action.Click+=async(sender,e)=>{if(finished)Close();else await Run();};
            FormClosing+=(sender,e)=>{if(working){e.Cancel=true;status.Text="Veuillez attendre la fin de l’opération et de sa restauration éventuelle.";}};
            Icon=Icon.ExtractAssociatedIcon(Assembly.GetExecutingAssembly().Location);
            using(var graphics=Graphics.FromHwnd(IntPtr.Zero)){
                float scale=graphics.DpiX/96f;
                var layout=Controls.Cast<Control>().Select(c=>new {Control=c,Bounds=c.Bounds,Anchor=c.Anchor}).ToArray();
                SuspendLayout();
                foreach(var item in layout)item.Control.Anchor=AnchorStyles.Top|AnchorStyles.Left;
                ClientSize=new Size((int)(780*scale),(int)(620*scale));MinimumSize=new Size((int)(720*scale),(int)(640*scale));
                foreach(var item in layout){var b=item.Bounds;item.Control.Bounds=new Rectangle((int)(b.Left*scale),(int)(b.Top*scale),(int)(b.Width*scale),(int)(b.Height*scale));item.Control.Anchor=item.Anchor;}
                ResumeLayout(true);
            }
        }
        void Log(string line){if(IsDisposed)return;BeginInvoke((Action)(()=>output.AppendText(line+Environment.NewLine)));}
        async Task Run(){
            working=true;action.Enabled=false;consent.Enabled=false;progress.Visible=true;status.Text=uninstall?"Désinstallation en cours…":"Vérification et installation en cours…";
            string stage=null;
            bool removeProfiles=consent.Checked;
            try {
                int code=await Task.Run(()=>{
                    stage=Program.NewStage();
                    if(uninstall){Program.CopyScripts(stage);return Program.Execute(Path.Combine(stage,"Scripts","install","Uninstall-XonarProduct.ps1"),"-InstallRoot "+Program.Quote(Program.InstallRoot)+(removeProfiles?" -RemoveProfiles":""),Log);}
                    Program.Extract(stage);Log("Paquet vérifié. Préparation de la carte Xonar…");
                    return Program.Execute(Path.Combine(stage,"Scripts","install","Install-XonarProduct.ps1"),"-PackageRoot "+Program.Quote(stage)+" -ManifestHash "+Program.Quote(BuildInfo.ManifestHash),Log);
                });
                ResultCode=code;
                if(code==0||code==3010){status.Text=code==3010?"Installation terminée. Redémarrez Windows pour terminer.":uninstall?"Désinstallation terminée. Profils conservés sauf choix contraire.":"Installation terminée.";open.Visible=!uninstall;action.Text="Fermer";finished=true;action.Enabled=true;}
                else{status.Text="L’opération a échoué. Consultez le journal ci-dessus et les sauvegardes indiquées.";action.Enabled=true;}
            }catch(Exception ex){ResultCode=1;Log(ex.Message);status.Text="L’opération n’a pas abouti.";action.Enabled=true;}
            finally {working=false;progress.Visible=false;consent.Enabled=true;if(stage!=null){try{Program.Cleanup(stage);}catch(Exception ex){Log("Dossier temporaire conservé : "+ex.Message);}}}
        }
    }
}
