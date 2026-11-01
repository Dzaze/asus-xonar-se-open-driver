# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([string]$Version='0.5.0-preview.2',[string]$DriverPackage,[string]$ApoDll,[string]$PublishedApp,[string]$SourceCommit='unpublished',[string]$OutputDirectory,[string]$RuntimeRedistributable)
$ErrorActionPreference='Stop';Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot 'ReleaseValidation.psm1') -Force
$repo=(Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$output=if($OutputDirectory){[IO.Path]::GetFullPath($OutputDirectory)}else{Join-Path $repo ('build\releases\'+$Version)}
$package=Join-Path $output 'package'
if(Test-Path -LiteralPath (Join-Path $package 'manifest.json')){throw 'Paquet deja construit. Choisir une nouvelle version ou un dossier propre.'}
if(-not $DriverPackage){$DriverPackage=Join-Path $repo 'build\driver-line-mic2\driver-package'}
if(-not $ApoDll){$ApoDll=Join-Path $repo 'build-apo\src\windows-apo\XonarApo.dll'}
if(-not $PublishedApp){$PublishedApp=Join-Path $output 'payload\App'}
$thumb='FF68A5C11C614F5E45AD64594D5A978BD9E5D321'
$signer=Get-Item ('Cert:\CurrentUser\My\'+$thumb)
if(-not $signer.HasPrivateKey){throw 'Certificat de signature de test absent du magasin personnel. Aucune cle privee ne doit etre placee dans le depot.'}
$sdk=@(Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory|Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'x64\signtool.exe')}|Sort-Object Name -Descending)[0].FullName
$signtool=Join-Path $sdk 'x64\signtool.exe'
foreach($file in $ApoDll,(Join-Path $DriverPackage 'xonarse.sys'),(Join-Path $DriverPackage 'xonarse.cat')){
    $sig=Get-AuthenticodeSignature -LiteralPath $file
    if($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Thumbprint -ne $thumb){throw "Binaire non signe en test par le projet : $file"}
}
$driverInf=[IO.File]::ReadAllText((Join-Path $DriverPackage 'xonarse.inf'))
$packagedDriverVersion=Get-XonarReleaseDriverVersion $driverInf
& $signtool verify /pa /c (Join-Path $DriverPackage 'xonarse.cat') (Join-Path $DriverPackage 'xonarse.sys')
if($LASTEXITCODE -ne 0){throw 'Catalogue et pilote non conformes.'}
& $signtool verify /pa /c (Join-Path $DriverPackage 'xonarse.cat') (Join-Path $DriverPackage 'xonarse.inf')
if($LASTEXITCODE -ne 0){throw 'Le catalogue signe ne couvre pas l INF fourni.'}
New-Item -ItemType Directory -Path $output,$package -Force|Out-Null
function Copy-ReleaseFile([string]$Source,[string]$Relative){
    $target=Join-Path $package $Relative;New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force|Out-Null
    Copy-Item -LiteralPath $Source -Destination $target
}
$app=(Resolve-Path -LiteralPath $PublishedApp).Path.TrimEnd('\')
foreach($required in 'Xonar.ControlCenter.exe','Xonar.ControlCenter.pri','App.xbf','Shell\MainWindow.xbf','Microsoft.UI.Xaml.dll','coreclr.dll'){
    if(-not(Test-Path -LiteralPath (Join-Path $app $required))){throw "Application publiee incomplete : $required"}
}
foreach($file in Get-ChildItem -LiteralPath $app -Recurse -File){
    if($file.Extension -eq '.pdb'){continue}
    Copy-ReleaseFile $file.FullName ('App\'+$file.FullName.Substring($app.Length).TrimStart('\'))
}
foreach($name in 'xonarse.sys','xonarse.inf','xonarse.cat'){Copy-ReleaseFile (Join-Path $DriverPackage $name) ('Driver\'+$name)}
Copy-ReleaseFile $ApoDll 'bin\XonarApo.dll'
New-Item -ItemType Directory -Path (Join-Path $package 'Certificates') -Force|Out-Null
Export-Certificate -Cert $signer -FilePath (Join-Path $package 'Certificates\XonarSeOpenDriver-TEST.cer') -Force|Out-Null
foreach($file in Get-ChildItem (Join-Path $repo 'scripts\install') -File){
    if($file.Extension -notin '.ps1','.psm1','.cmd','.cs'){continue}
    Copy-ReleaseFile $file.FullName ('Scripts\install\'+$file.Name)
}
foreach($name in 'ApoRegistry.psm1','register-apo.ps1','unregister-apo.ps1','repair-speaker-effects.ps1','protect-speaker-effects.ps1'){Copy-ReleaseFile (Join-Path $repo ('scripts\apo\'+$name)) ('Scripts\apo\'+$name)}
# Windows PowerShell 5.1 interprete correctement le texte UTF-8 avec BOM.
foreach($file in Get-ChildItem -LiteralPath (Join-Path $package 'Scripts') -Recurse -File|Where-Object {$_.Extension -in '.ps1','.psm1'}){
    $text=[IO.File]::ReadAllText($file.FullName);[IO.File]::WriteAllText($file.FullName,$text,[Text.UTF8Encoding]::new($true))
}
Copy-ReleaseFile (Join-Path $repo 'packaging\defaults\apo-efx.cfg') 'Defaults\apo-efx.cfg'
Copy-ReleaseFile (Join-Path $repo 'packaging\public\docs\INSTALLATION.fr.md') 'README-INSTALLATION.md'
Copy-ReleaseFile (Join-Path $repo 'docs\APO-PROTECTION.fr.md') 'APO-PROTECTION.fr.md'
Copy-ReleaseFile (Join-Path $repo 'LICENSE') 'Licenses\XonarSE-Apache-2.0.txt'
$nuget=Join-Path $env:USERPROFILE '.nuget\packages'
Copy-ReleaseFile (Join-Path $nuget 'microsoft.windowsappsdk\1.8.260921001\license.txt') 'Licenses\WindowsAppSDK-license.txt'
Copy-ReleaseFile (Join-Path $nuget 'microsoft.windowsappsdk\1.8.260921001\NOTICE.txt') 'Licenses\WindowsAppSDK-NOTICE.txt'
Copy-ReleaseFile (Join-Path $repo 'packaging\windows\ThirdPartyNotices.txt') 'Licenses\ThirdPartyNotices.txt'
foreach($name in 'dotnet-LICENSE.txt','dotnet-THIRD-PARTY-NOTICES.txt','H.NotifyIcon-LICENSE.txt'){
    Copy-ReleaseFile (Join-Path $repo ('packaging\licenses\'+$name)) ('Licenses\'+$name)
}
$redistFile=$RuntimeRedistributable
if(-not $redistFile){
    $vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $visualStudio=& $vswhere -latest -prerelease -products * -property installationPath
    if(-not $visualStudio){throw 'Visual Studio introuvable. Fournir -RuntimeRedistributable.'}
    $redist=@(Get-ChildItem (Join-Path $visualStudio 'VC\Redist\MSVC') -Directory|Where-Object {Test-Path -LiteralPath (Join-Path $_.FullName 'vc_redist.x64.exe')}|Sort-Object Name -Descending)[0]
    $redistFile=Join-Path $redist.FullName 'vc_redist.x64.exe'
}
$sig=Get-AuthenticodeSignature -LiteralPath $redistFile
if($sig.Status -ne 'Valid' -or $sig.SignerCertificate.Subject -notmatch 'Microsoft Corporation'){throw 'Runtime Microsoft non valide.'}
Copy-ReleaseFile $redistFile 'Prerequisites\vc_redist.x64.exe'
$generated=Join-Path $output 'BuildInfo.cs'
$csc=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$source=Join-Path $repo 'packaging\windows\Setup.cs';$manifest=Join-Path $repo 'packaging\windows\setup.manifest';$icon=Join-Path $repo 'apps\control-center\Assets\app.ico'
function Write-BuildInfo([string]$Hash){
    [IO.File]::WriteAllText($generated,('using System.Reflection; [assembly:AssemblyTitle("Xonar SE Setup")] [assembly:AssemblyCompany("Benoist Pirolley")] [assembly:AssemblyVersion("0.5.0.0")] [assembly:AssemblyFileVersion("0.5.0.0")] namespace XonarSE.Setup {internal static class BuildInfo {internal const string Version="'+$Version+'";internal const string ManifestHash="'+$Hash+'";}}'),[Text.UTF8Encoding]::new($false))
}
$refs=@('/r:System.Windows.Forms.dll','/r:System.Drawing.dll','/r:System.IO.Compression.dll','/r:System.IO.Compression.FileSystem.dll','/r:System.Web.Extensions.dll')
foreach($name in 'Xonar.ControlCenter.exe','Xonar.ControlCenter.dll'){
    & $signtool sign /s My /sha1 $thumb /fd SHA256 (Join-Path $package ('App\'+$name))
    if($LASTEXITCODE -ne 0){throw 'Signature application echouee.'}
}
Write-BuildInfo ''
$uninstall=Join-Path $package 'Uninstall-XonarSE.exe'
& $csc /nologo /target:winexe /platform:x64 /optimize+ /codepage:65001 /langversion:5 /define:UNINSTALL @refs ('/win32manifest:'+$manifest) ('/win32icon:'+$icon) ('/out:'+$uninstall) $source $generated
if($LASTEXITCODE -ne 0){throw 'Compilation de la desinstallation echouee.'}
& $signtool sign /s My /sha1 $thumb /fd SHA256 $uninstall
if($LASTEXITCODE -ne 0){throw 'Signature desinstallation echouee.'}
$files=@(Get-ChildItem -LiteralPath $package -Recurse -File|Sort-Object FullName|ForEach-Object {
    [pscustomobject]@{Path=$_.FullName.Substring($package.Length).TrimStart('\').Replace('\','/');Sha256=(Get-FileHash -LiteralPath $_.FullName).Hash}
})
$metadata=[ordered]@{Schema=1;Version=$Version;DriverVersion=$packagedDriverVersion;HardwareId='USB\VID_0B05&PID_189D&MI_00';SignerThumbprint=$thumb;SourceCommit=$SourceCommit;Files=$files}
[IO.File]::WriteAllText((Join-Path $package 'manifest.json'),($metadata|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
$hash=(Get-FileHash -LiteralPath (Join-Path $package 'manifest.json')).Hash
Write-BuildInfo $hash
$zip=Join-Path $output 'payload.zip'
if(Test-Path -LiteralPath $zip){throw 'Archive de build deja presente.'}
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($package,$zip,[IO.Compression.CompressionLevel]::Optimal,$false)
$setup=Join-Path $output ('XonarSE-Setup-'+$Version+'-x64.exe')
& $csc /nologo /target:winexe /platform:x64 /optimize+ /codepage:65001 /langversion:5 @refs ('/win32manifest:'+$manifest) ('/win32icon:'+$icon) ('/resource:'+$zip+',Xonar.Payload.zip') ('/out:'+$setup) $source $generated
if($LASTEXITCODE -ne 0){throw 'Compilation du Setup echouee.'}
& $signtool sign /s My /sha1 $thumb /fd SHA256 $setup
if($LASTEXITCODE -ne 0){throw 'Signature Setup echouee.'}
Copy-Item -LiteralPath (Join-Path $package 'manifest.json') -Destination (Join-Path $output 'release-manifest.json')
$sums=@(foreach($file in $setup,(Join-Path $output 'release-manifest.json')){(Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant()+'  '+[IO.Path]::GetFileName($file)})
$sums|Set-Content -LiteralPath (Join-Path $output 'SHA256SUMS.txt') -Encoding ascii
[pscustomobject]@{Setup=$setup;Package=$package;ManifestHash=$hash;Bytes=(Get-Item -LiteralPath $setup).Length}|ConvertTo-Json|Set-Content (Join-Path $output 'build-result.json')
Write-Host "Setup construit : $setup"
