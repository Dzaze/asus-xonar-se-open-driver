# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([string]$InstallRoot='C:\Program Files\XonarSE',[switch]$RemoveProfiles)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
Import-Module (Join-Path $PSScriptRoot 'XonarProduct.psm1') -Force
$state=Import-Clixml -LiteralPath (Join-Path $InstallRoot 'ProductState.clixml')
if($state.Schema -ne 1){throw 'Version installation inconnue.'}
$m=Import-Clixml -LiteralPath $state.BaselineManifest
if([IO.Path]::GetFullPath($InstallRoot) -ne [IO.Path]::GetFullPath($m.InstallRoot)){throw 'Sauvegarde d une autre installation.'}
$log=Join-Path $InstallRoot ('Logs\uninstall-'+(Get-Date -Format yyyyMMdd-HHmmss)+'.log')
Start-Transcript -LiteralPath $log|Out-Null
$code=0
try {
    $prefix=[IO.Path]::GetFullPath((Join-Path $InstallRoot 'App')).TrimEnd('\')+'\'
    foreach($p in @(Get-Process -Name Xonar.ControlCenter -ErrorAction SilentlyContinue)){
        if($p.Path -and $p.Path.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)){Stop-Process -Id $p.Id}
    }
    Restore-XonarProduct -BackupDirectory (Split-Path -Parent $state.BaselineManifest)
    if($RemoveProfiles){
        $expected=[IO.Path]::GetFullPath((Join-Path $env:ProgramData 'XonarSE'))
        if([IO.Path]::GetFullPath($m.ConfigRoot) -ne $expected){throw 'Dossier profils inattendu : suppression refusee.'}
        if(Test-Path -LiteralPath $expected){
            if((Get-Item -LiteralPath $expected).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Jonction dans le dossier profils : suppression refusee.'}
            Remove-Item -LiteralPath $expected -Recurse -Force
        }
    }
    Write-Host 'Desinstallation terminee. Sauvegardes et journaux conserves.'
}catch{Write-Host ('ECHEC : '+$_.Exception.Message);$code=1}
finally{Stop-Transcript|Out-Null}
exit $code
