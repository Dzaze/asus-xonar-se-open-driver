# SPDX-License-Identifier: Apache-2.0
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '..\apo\ApoRegistry.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'ApoInstallTransaction.psm1') -Force
$script:ProductHardwareId='USB\VID_0B05&PID_189D&MI_00'
$script:ProductSigner='FF68A5C11C614F5E45AD64594D5A978BD9E5D321'
function Initialize-XonarProductApi {
    if('XonarProductNative' -as [type]){return}
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class XonarProductNative {
 [StructLayout(LayoutKind.Sequential)] public struct Integrity {public uint Length;public uint Options;}
 [DllImport("ntdll.dll")] static extern int NtQuerySystemInformation(int kind,ref Integrity info,uint size,out uint returned);
 public static uint IntegrityOptions(){var i=new Integrity {Length=8};uint returned;int result=NtQuerySystemInformation(103,ref i,8,out returned);if(result<0)throw new InvalidOperationException("Code integrity query failed: "+result);return i.Options;}
 [DllImport("newdev.dll",CharSet=CharSet.Unicode,SetLastError=true)]
 [return:MarshalAs(UnmanagedType.Bool)] public static extern bool UpdateDriverForPlugAndPlayDevices(IntPtr window,string hardwareId,string inf,uint flags,[MarshalAs(UnmanagedType.Bool)]out bool reboot);
}
'@
}
function Test-XonarProductPrerequisites {
    Initialize-XonarProductApi
    if(-not [Environment]::Is64BitOperatingSystem -or $env:PROCESSOR_ARCHITECTURE -ne 'AMD64'){throw 'Windows x64 sur processeur Intel/AMD requis.'}
    $build=[int](Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion').CurrentBuildNumber
    if($build -lt 22000){throw 'Windows 11 x64 requis.'}
    if(([XonarProductNative]::IntegrityOptions() -band 2) -eq 0){throw 'Le mode test Windows est desactive. Lire INSTALLATION.fr.md, activer le mode test puis redemarrer avant de relancer cet installateur. Aucun reglage de demarrage ne sera modifie automatiquement.'}
    $devices=@(Get-PnpDevice -PresentOnly -Class Media | Where-Object {$_.InstanceId.StartsWith($script:ProductHardwareId+'\',[StringComparison]::OrdinalIgnoreCase)})
    if($devices.Count -ne 1){throw "Une seule interface audio Xonar SE doit etre presente ($($devices.Count) detectee)."}
    $devices[0]
}
function Assert-XonarPackage {
    param([string]$PackageRoot,[string]$ManifestHash)
    $root=[IO.Path]::GetFullPath($PackageRoot).TrimEnd('\')+'\'
    $manifestPath=Join-Path $root 'manifest.json'
    if((Get-FileHash -LiteralPath $manifestPath).Hash -ne $ManifestHash){throw 'Manifeste du paquet altere.'}
    $manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    if($manifest.Schema -ne 1 -or $manifest.HardwareId -ne $script:ProductHardwareId -or $manifest.SignerThumbprint -ne $script:ProductSigner){throw 'Paquet non reconnu.'}
    $seen=@{}
    foreach($entry in $manifest.Files){
        if($entry.Path -match ':' -or [IO.Path]::IsPathRooted($entry.Path)){throw 'Chemin de paquet invalide.'}
        $path=[IO.Path]::GetFullPath((Join-Path $root $entry.Path))
        if(-not $path.StartsWith($root,[StringComparison]::OrdinalIgnoreCase) -or $seen.ContainsKey($path)){throw 'Chemin hors paquet ou duplique.'}
        $seen[$path]=$true
        if((Get-FileHash -LiteralPath $path).Hash -ne $entry.Sha256){throw "Fichier altere : $($entry.Path)"}
    }
    $manifest
}
function Get-XonarProductDriver {
    param([string]$InstanceId)
    $device=Get-ItemProperty -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Enum\'+$InstanceId)
    $driver=Get-ItemProperty -LiteralPath ('HKLM:\SYSTEM\CurrentControlSet\Control\Class\'+$device.Driver)
    $path=$null
    if($device.Service -eq 'xonarse'){$path=(Get-CimInstance Win32_SystemDriver -Filter "Name='xonarse'").PathName}
    [pscustomobject]@{InstanceId=$InstanceId;InfName=$driver.InfPath;Version=$driver.DriverVersion;Service=$device.Service;RuntimePath=$path}
}
function Invoke-XonarPnpUtil {
    param([string[]]$Arguments)
    $output=& (Join-Path $env:WINDIR 'System32\pnputil.exe') @Arguments 2>&1
    $result=$LASTEXITCODE
    $output|ForEach-Object {Write-Host $_}
    if($result -notin 0,3010){throw "PnPUtil a echoue : $result"}
    return $result
}
function Set-XonarProductDriver {
    param([string]$Inf,[switch]$AlreadyStaged)
    Initialize-XonarProductApi
    if(-not $AlreadyStaged){[void](Invoke-XonarPnpUtil -Arguments @('/add-driver',$Inf))}
    $reboot=$false
    if(-not [XonarProductNative]::UpdateDriverForPlugAndPlayDevices([IntPtr]::Zero,$script:ProductHardwareId,$Inf,1,[ref]$reboot)){
        throw "Installation de l'interface Xonar refusee (Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error()))."
    }
    return $reboot
}
function Assert-XonarProductPaths {
    param([string]$InstallRoot,[string]$ConfigRoot)
    foreach($path in $InstallRoot,$ConfigRoot){
        $full=[IO.Path]::GetFullPath($path).TrimEnd('\')
        if($full -in @([IO.Path]::GetPathRoot($full).TrimEnd('\'),$env:WINDIR,$env:ProgramFiles,$env:ProgramData,$env:USERPROFILE)){throw 'Dossier trop large.'}
        if(Test-Path -LiteralPath $full){if((Get-Item -LiteralPath $full).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Une jonction ne peut servir de dossier installation.'}}
    }
}
function Remove-XonarProductDirectory {
    param([string]$Path,[string]$InstallRoot)
    $full=[IO.Path]::GetFullPath($Path).TrimEnd('\');$root=[IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')+'\'
    if(-not $full.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Suppression hors installation refusee.'}
    if(Test-Path -LiteralPath $full){Remove-Item -LiteralPath $full -Recurse -Force}
}
function Move-XonarProductDirectory {
    param([string]$Source,[string]$Destination,[string]$InstallRoot)
    $root=[IO.Path]::GetFullPath($InstallRoot).TrimEnd('\')+'\'
    foreach($path in $Source,$Destination){if(-not [IO.Path]::GetFullPath($path).StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Deplacement hors installation refuse.'}}
    Move-Item -LiteralPath $Source -Destination $Destination
}
function Test-XonarApoCurrent {
    param([string]$Source,[string]$InstallRoot)
    $target=Join-Path $InstallRoot 'XonarApo.dll'
    if(-not(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $Source).Hash -ne (Get-FileHash -LiteralPath $target).Hash){return $false}
    try {
        $endpoint=Get-XonarSpeakerTarget;$key=Open-ApoEffectsKey $endpoint.Subkey
        try {
            $sfx=$key.GetValue('{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},1');$efx=$key.GetValue('{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},2')
            $com=Get-Item -LiteralPath 'Registry::HKEY_LOCAL_MACHINE\SOFTWARE\Classes\CLSID\{D2FBC737-00C3-4482-B3DD-02D92C06003E}\InprocServer32'
            return $sfx -eq '{C243A8CB-D535-4BA0-BCC5-CC4338833E7A}' -and $efx -eq '{D2FBC737-00C3-4482-B3DD-02D92C06003E}' -and $com.GetValue('') -eq $target
        }finally{$key.Dispose()}
    }catch{return $false}
}
function Save-XonarProductManifest {param($Manifest,[string]$Directory) $Manifest|Export-Clixml -LiteralPath (Join-Path $Directory 'product-manifest.clixml') -Encoding utf8}
function Assert-XonarProductAdministrator {
    $admin=([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if(-not $admin){throw 'Execution en administrateur requise.'}
}
function Get-XonarProductCertificate {param([string]$Path) [Security.Cryptography.X509Certificates.X509Certificate2]::new($Path)}
function Install-XonarProductRuntime {
    param([string]$Redist)
    $vc=Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64' -ErrorAction SilentlyContinue
    $required=[version]([regex]::Match((Get-Item -LiteralPath $Redist).VersionInfo.FileVersion,'\d+\.\d+\.\d+(\.\d+)?').Value)
    if($vc -and $vc.Installed -and [version]$vc.Version.TrimStart('v') -ge $required){return $false}
    $signature=Get-AuthenticodeSignature -LiteralPath $Redist
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'Microsoft Corporation'){throw 'Runtime Microsoft non reconnu.'}
    Write-Host 'Installation du runtime Microsoft Visual C++...'
    $p=Start-Process -FilePath $Redist -ArgumentList '/install /quiet /norestart' -WindowStyle Hidden -Wait -PassThru
    if($p.ExitCode -notin 0,3010,1638){throw "Runtime Microsoft : code $($p.ExitCode)"}
    return $p.ExitCode -eq 3010
}
function Install-XonarProductShell {
    param([string]$InstallRoot,[string]$Version)
    $shortcut=Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'Xonar SE.lnk'
    $shell=New-Object -ComObject WScript.Shell;$link=$shell.CreateShortcut($shortcut)
    $link.TargetPath=Join-Path $InstallRoot 'App\Xonar.ControlCenter.exe';$link.WorkingDirectory=Join-Path $InstallRoot 'App';$link.Save()
    $key='HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\XonarSEOpenDriver'
    New-Item -Path $key -Force|Out-Null
    foreach($entry in @{DisplayName='ASUS Xonar SE Open Driver';DisplayVersion=$Version;Publisher='Benoist Pirolley';InstallLocation=$InstallRoot;DisplayIcon=(Join-Path $InstallRoot 'App\Xonar.ControlCenter.exe');UninstallString=('"'+(Join-Path $InstallRoot 'Uninstall-XonarSE.exe')+'" /uninstall');URLInfoAbout='https://benoist-pirolley.web.app/';URLUpdateInfo='https://github.com/Dzaze/asus-xonar-se-open-driver'}.GetEnumerator()){
        New-ItemProperty -LiteralPath $key -Name $entry.Key -Value $entry.Value -PropertyType String -Force|Out-Null
    }
}
function Get-XonarProductShell {
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\XonarSEOpenDriver')
    try {$values=if($key){@(foreach($name in $key.GetValueNames()){Get-ApoValue $key $name})}else{@()}}
    finally{if($key){$key.Dispose()}}
    $shortcut=Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'Xonar SE.lnk'
    [pscustomobject]@{RegistryExisted=$null -ne $key;Values=$values;Shortcut=if(Test-Path -LiteralPath $shortcut){[IO.File]::ReadAllBytes($shortcut)}else{$null}}
}
function Restore-XonarProductShell {
    param($State)
    $path='SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\XonarSEOpenDriver'
    [Microsoft.Win32.Registry]::LocalMachine.DeleteSubKeyTree($path,$false)
    if($State.RegistryExisted){
        $key=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($path)
        try{foreach($value in $State.Values){Set-ApoValue $key $value}}finally{$key.Dispose()}
    }
    $shortcut=Join-Path ([Environment]::GetFolderPath('CommonPrograms')) 'Xonar SE.lnk'
    if($State.Shortcut){[IO.File]::WriteAllBytes($shortcut,[byte[]]$State.Shortcut)}
    elseif(Test-Path -LiteralPath $shortcut){Remove-Item -LiteralPath $shortcut -Force}
}
function Restore-XonarProductDriver {
    param($Manifest)
    if(-not $Manifest.DriverChanged){return}
    $old=$Manifest.OriginalDriver
    $oldInf=if($Manifest.DriverExport){@(Get-ChildItem -LiteralPath $Manifest.DriverExport -Recurse -Filter '*.inf')[0].FullName}else{Join-Path $env:WINDIR ('INF\'+$old.InfName)}
    if(-not(Test-Path -LiteralPath $oldInf)){throw 'Paquet pilote precedent introuvable.'}
    [void](Set-XonarProductDriver $oldInf -AlreadyStaged:(-not [bool]$Manifest.DriverExport))
    if($Manifest.InstalledInf -and $Manifest.InstalledInf -ne $old.InfName -and $Manifest.InstalledInf -match '^oem\d+\.inf$'){
        [void](Invoke-XonarPnpUtil -Arguments @('/delete-driver',$Manifest.InstalledInf))
    }
}
function Restore-XonarProduct {
    param([string]$BackupDirectory)
    $m=Import-Clixml -LiteralPath (Join-Path $BackupDirectory 'product-manifest.clixml')
    if($m.Schema -ne 1){throw 'Version de sauvegarde produit inconnue.'}
    Assert-XonarProductPaths $m.InstallRoot $m.ConfigRoot
    if($m.ApoBackup -and (Test-Path -LiteralPath (Join-Path $m.ApoBackup 'install-manifest.clixml'))){Restore-ApoInstallation -BackupDirectory $m.ApoBackup}
    Restore-XonarProductDriver $m
    foreach($name in @($m.ReplacedDirectories)){
        Remove-XonarProductDirectory -Path (Join-Path $m.InstallRoot $name) -InstallRoot $m.InstallRoot
        $previous=Join-Path $BackupDirectory ('Files\'+$name)
        if(Test-Path -LiteralPath $previous){Move-XonarProductDirectory $previous (Join-Path $m.InstallRoot $name) $m.InstallRoot}
    }
    foreach($file in $m.TopFiles){
        $target=Join-Path $m.InstallRoot $file.Name
        if($file.Existed){Copy-Item -LiteralPath (Join-Path $BackupDirectory $file.Backup) -Destination $target -Force}
        elseif(Test-Path -LiteralPath $target){Remove-Item -LiteralPath $target -Force}
    }
    Restore-XonarProductShell $m.Shell
    foreach($store in $m.AddedCertificateStores){
        $cert=Join-Path ('Cert:\LocalMachine\'+$store) $script:ProductSigner
        if(Test-Path -LiteralPath $cert){Remove-Item -LiteralPath $cert}
    }
    $m.Outcome='Restored';Save-XonarProductManifest $m $BackupDirectory
}
function Install-XonarProduct {
    [CmdletBinding()]
    param([string]$PackageRoot,[string]$ManifestHash,[string]$InstallRoot='C:\Program Files\XonarSE',[string]$ConfigRoot='C:\ProgramData\XonarSE',[switch]$Preview)
    Assert-XonarProductPaths $InstallRoot $ConfigRoot
    $package=Assert-XonarPackage $PackageRoot $ManifestHash
    $device=Test-XonarProductPrerequisites
    $original=Get-XonarProductDriver $device.InstanceId
    $driverFile=Join-Path $PackageRoot 'Driver\xonarse.sys'
    $driverNeeded=$original.Service -ne 'xonarse' -or -not $original.RuntimePath -or (Get-FileHash -LiteralPath $original.RuntimePath).Hash -ne (Get-FileHash -LiteralPath $driverFile).Hash
    $apo=Join-Path $PackageRoot 'bin\XonarApo.dll'
    $apoNeeded=$driverNeeded -or -not(Test-XonarApoCurrent $apo $InstallRoot)
    $certificate=Get-XonarProductCertificate (Join-Path $PackageRoot 'Certificates\XonarSeOpenDriver-TEST.cer')
    if($certificate.Thumbprint -ne $script:ProductSigner -or $certificate.HasPrivateKey){throw 'Certificat public inattendu.'}
    foreach($file in $driverFile,(Join-Path $PackageRoot 'Driver\xonarse.cat'),$apo){
        $sig=Get-AuthenticodeSignature -LiteralPath $file
        if($sig.SignerCertificate.Thumbprint -ne $script:ProductSigner -or $sig.Status -notin 'Valid','NotTrusted','UnknownError'){throw "Signature de paquet refusee : $file"}
    }
    Write-Host "Version $($package.Version)"
    if($driverNeeded){Write-Host 'Le pilote de test va etre installe.'}else{Write-Host 'Pilote deja conforme : conserve.'}
    if($apoNeeded){Write-Host 'Les effets audio vont etre installes.'}else{Write-Host 'Effets audio deja conformes : conserves.'}
    if($Preview){Write-Host 'Previsualisation terminee : aucune modification du systeme audio.';return}
    Assert-XonarProductAdministrator
    $backup=Join-Path $InstallRoot ('Backups\Product-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $backup -Force|Out-Null
    $oldStatePath=Join-Path $InstallRoot 'ProductState.clixml'
    $oldState=if(Test-Path -LiteralPath $oldStatePath){Import-Clixml -LiteralPath $oldStatePath}else{$null}
    $topFiles=@(foreach($name in 'manifest.json','Uninstall-XonarSE.exe','README-INSTALLATION.md','ProductState.clixml'){
        $path=Join-Path $InstallRoot $name;$exists=Test-Path -LiteralPath $path
        if($exists){Copy-Item -LiteralPath $path -Destination (Join-Path $backup $name)}
        [pscustomobject]@{Name=$name;Existed=$exists;Backup=$name}
    })
    $m=[pscustomobject]@{Schema=1;Version=$package.Version;InstallRoot=$InstallRoot;ConfigRoot=$ConfigRoot;OriginalDriver=$original;DriverExport=$null;
        DriverChanged=$false;InstalledInf=$null;ApoBackup=$null;AddedCertificateStores=@();ReplacedDirectories=@();TopFiles=$topFiles;Outcome='Prepared';NeedsRestart=$false;
        BaselineManifest=if($oldState){$oldState.BaselineManifest}else{Join-Path $backup 'product-manifest.clixml'};Shell=(Get-XonarProductShell)}
    Save-XonarProductManifest $m $backup
    try {
        foreach($store in 'Root','TrustedPublisher'){
            $target='Cert:\LocalMachine\'+$store+'\'+$script:ProductSigner
            if(-not(Test-Path -LiteralPath $target)){
                $m.AddedCertificateStores+=,$store;Save-XonarProductManifest $m $backup
                Import-Certificate -FilePath (Join-Path $PackageRoot 'Certificates\XonarSeOpenDriver-TEST.cer') -CertStoreLocation ('Cert:\LocalMachine\'+$store)|Out-Null
            }
        }
        foreach($file in $driverFile,(Join-Path $PackageRoot 'Driver\xonarse.cat'),$apo){if((Get-AuthenticodeSignature -LiteralPath $file).Status -ne 'Valid'){throw 'Signature non valide apres import du certificat de test.'}}
        $redist=Join-Path $PackageRoot 'Prerequisites\vc_redist.x64.exe'
        if(Install-XonarProductRuntime $redist){$m.NeedsRestart=$true}
        if($driverNeeded){
            if($original.InfName -match '^oem\d+\.inf$'){
                $export=Join-Path $backup 'PreviousDriver';New-Item -ItemType Directory -Path $export|Out-Null
                [void](Invoke-XonarPnpUtil -Arguments @('/export-driver',$original.InfName,$export));$m.DriverExport=$export
            }
            $m.DriverChanged=$true;Save-XonarProductManifest $m $backup
            $m.NeedsRestart=(Set-XonarProductDriver (Join-Path $PackageRoot 'Driver\xonarse.inf')) -or $m.NeedsRestart
            $current=Get-XonarProductDriver $device.InstanceId;$m.InstalledInf=$current.InfName;Save-XonarProductManifest $m $backup
            if($current.Service -ne 'xonarse' -or $current.Version -ne $package.DriverVersion){throw 'Le pilote attendu ne pilote pas la carte.'}
        }
        if($apoNeeded){
            $ready=$false
            for($retry=0;$retry -lt 30;$retry++){try{$null=Get-XonarSpeakerTarget;$ready=$true;break}catch{Start-Sleep -Milliseconds 500}}
            if(-not $ready){throw 'Endpoint 5.1 indisponible. Redemarrer Windows puis relancer la preparation si necessaire.'}
            $m.ApoBackup=Join-Path $backup 'Apo';Save-XonarProductManifest $m $backup
            Install-ApoTransactional -SourceDll $apo -InstallDir $InstallRoot -ConfigDir $ConfigRoot -BackupDirectory $m.ApoBackup|Out-Null
        }
        # Ne fermer que notre application deja installee ; aucun autre lecteur audio.
        $appPrefix=[IO.Path]::GetFullPath((Join-Path $InstallRoot 'App')).TrimEnd('\')+'\'
        foreach($process in @(Get-Process -Name Xonar.ControlCenter -ErrorAction SilentlyContinue)){
            if($process.Path -and $process.Path.StartsWith($appPrefix,[StringComparison]::OrdinalIgnoreCase)){Stop-Process -Id $process.Id}
        }
        New-Item -ItemType Directory -Path (Join-Path $backup 'Files') -Force|Out-Null
        foreach($name in 'App','Scripts','bin','Driver','Certificates','Defaults','Licenses','Prerequisites'){
            $source=Join-Path $PackageRoot $name;$target=Join-Path $InstallRoot $name
            if(-not(Test-Path -LiteralPath $source)){throw "Dossier de paquet absent : $name"}
            if(Test-Path -LiteralPath $target){Move-XonarProductDirectory $target (Join-Path $backup ('Files\'+$name)) $InstallRoot}
            $m.ReplacedDirectories+=,$name;Save-XonarProductManifest $m $backup
            Copy-Item -LiteralPath $source -Destination $target -Recurse -Force
        }
        foreach($name in 'manifest.json','Uninstall-XonarSE.exe','README-INSTALLATION.md'){Copy-Item -LiteralPath (Join-Path $PackageRoot $name) -Destination (Join-Path $InstallRoot $name) -Force}
        New-Item -ItemType Directory -Path $ConfigRoot -Force|Out-Null
        $config=Join-Path $ConfigRoot 'apo-efx.cfg'
        if(-not(Test-Path -LiteralPath $config)){Copy-Item -LiteralPath (Join-Path $PackageRoot 'Defaults\apo-efx.cfg') -Destination $config}
        Install-XonarProductShell $InstallRoot $package.Version
        [pscustomobject]@{Schema=1;Version=$package.Version;BaselineManifest=$m.BaselineManifest;LastManifest=(Join-Path $backup 'product-manifest.clixml')}|Export-Clixml -LiteralPath $oldStatePath -Encoding utf8
        $m.Outcome='Installed';Save-XonarProductManifest $m $backup
        Write-Host "Installation terminee. Sauvegarde : $backup"
        [pscustomobject]@{Version=$package.Version;NeedsRestart=$m.NeedsRestart;BackupDirectory=$backup;DriverChanged=$driverNeeded;ApoChanged=$apoNeeded}
    }catch{
        $errorRecord=$_
        try{Restore-XonarProduct $backup}catch{throw "Installation : $($errorRecord.Exception.Message). Restauration incomplete : $($_.Exception.Message). Sauvegarde : $backup"}
        throw "Installation annulee, etat precedent restaure : $($errorRecord.Exception.Message). Sauvegarde : $backup"
    }
}
Export-ModuleMember -Function Install-XonarProduct,Restore-XonarProduct,Test-XonarProductPrerequisites,Assert-XonarPackage,Get-XonarProductDriver,Restore-XonarProductDriver,Remove-XonarProductDirectory
